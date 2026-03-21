//! Performance Matrix Test Suite
//!
//! Reproduces the microsoft/duroxide team's performance benchmarking methodology.
//! Creates ~200 independent test functions that exercise realistic SQLite operations
//! across three backends: in-memory, local file, and Azure blob storage.
//!
//! ## Usage
//!
//! ```bash
//! # In-memory (default) - fastest, baseline performance
//! cd rust && cargo nextest run --test perf_matrix -j 14
//!
//! # Local file - tests file I/O overhead
//! cd rust && PERF_MODE=file cargo nextest run --test perf_matrix -j 14
//!
//! # Azure blob storage - tests network latency and cloud storage overhead
//! cd rust && PERF_MODE=azure cargo nextest run --test perf_matrix -j 14
//!
//! # Quick performance comparison across concurrency levels
//! for j in 1 4 14 28; do
//!   echo "--- j=$j ---"
//!   time PERF_MODE=file cargo nextest run --test perf_matrix -j $j 2>&1 | tail -1
//! done
//! ```
//!
//! ## Configuration
//!
//! - `PERF_MODE` - Backend: `memory` (default), `file`, `azure`
//! - `PERF_ITERATIONS` - Work loop iterations per test (default: 6000, set to 0 to disable)
//!
//! ## Test Categories
//!
//! - **Schema CRUD** (~30 tests) - CREATE TABLE, INDEX, DROP, ALTER operations
//! - **Single-row ops** (~40 tests) - INSERT, SELECT, UPDATE, DELETE across data types
//! - **Bulk operations** (~40 tests) - Batch inserts, complex queries, aggregates
//! - **Transactions** (~30 tests) - BEGIN/COMMIT, ROLLBACK, SAVEPOINT patterns
//! - **Complex queries** (~30 tests) - JOINs, subqueries, CTEs, UNION
//! - **WAL mode** (~15 tests) - WAL enable, checkpoint, concurrent read/write
//! - **Provider patterns** (~17 tests) - Work-item lifecycle, peek-lock semantics
//!
//! Each test creates a fresh database with unique name (UUID for Azure blobs).
//! All tests are independent and safe for parallel execution via nextest.

use rusqlite::{Connection, OpenFlags, Result};
use std::env;
use std::path::Path;
use std::sync::OnceLock;
use tempfile::TempDir;
use uuid::Uuid;

/// Lazy VFS registration for Azure mode
static VFS_REGISTERED: OnceLock<()> = OnceLock::new();

/// Lazy .env loading for Azure credentials
static DOTENV_LOADED: OnceLock<()> = OnceLock::new();

fn load_dotenv() {
    DOTENV_LOADED.get_or_init(|| {
        let manifest_dir = Path::new(env!("CARGO_MANIFEST_DIR"));
        // Try crate-level .env first, then repo root
        let crate_env = manifest_dir.join(".env");
        let repo_env = manifest_dir.join("../../.env");
        if dotenvy::from_path(&crate_env).is_err() {
            dotenvy::from_path(&repo_env).ok();
        }
    });
}

/// Performance test mode controlled by PERF_MODE environment variable
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum PerfMode {
    Memory, // :memory: - fastest, no I/O
    File,   // Local temp file - file I/O overhead
    Azure,  // Azure blob storage - network + cloud overhead
}

impl PerfMode {
    fn from_env() -> Self {
        match env::var("PERF_MODE").as_deref() {
            Ok("file") => PerfMode::File,
            Ok("azure") => PerfMode::Azure,
            _ => PerfMode::Memory,
        }
    }
}

/// Test database handle with cleanup
struct TestDb {
    conn: Connection,
    #[allow(dead_code)]
    temp_dir: Option<TempDir>,
}

impl TestDb {
    fn new() -> Result<Self> {
        let mode = PerfMode::from_env();

        match mode {
            PerfMode::Memory => {
                let conn = Connection::open_in_memory()?;
                Self::configure_pragmas(&conn, mode)?;
                Ok(TestDb {
                    conn,
                    temp_dir: None,
                })
            }
            PerfMode::File => {
                let temp_dir = TempDir::new().unwrap();
                let db_path = temp_dir.path().join("test.db");
                let conn = Connection::open_with_flags(
                    &db_path,
                    OpenFlags::SQLITE_OPEN_READ_WRITE | OpenFlags::SQLITE_OPEN_CREATE,
                )?;
                Self::configure_pragmas(&conn, mode)?;
                Ok(TestDb {
                    conn,
                    temp_dir: Some(temp_dir),
                })
            }
            PerfMode::Azure => {
                load_dotenv();
                Self::ensure_vfs_registered();

                // Load Azure credentials from environment
                let account = env::var("AZURE_STORAGE_ACCOUNT")
                    .expect("AZURE_STORAGE_ACCOUNT not set for azure mode");
                let container = env::var("AZURE_STORAGE_CONTAINER")
                    .expect("AZURE_STORAGE_CONTAINER not set for azure mode");
                let sas = env::var("AZURE_STORAGE_SAS")
                    .expect("AZURE_STORAGE_SAS not set for azure mode");

                // Unique blob name per test
                let blob_name = format!("perf-{}.db", Uuid::new_v4());
                eprintln!(
                    "[perf_matrix] AZURE: account={account} container={container} blob={blob_name}"
                );

                // Build URI with proper encoding
                let uri = sqlite_objs::UriBuilder::new(&blob_name, &account, &container)
                    .sas_token(&sas)
                    .build();

                let conn = Connection::open_with_flags_and_vfs(
                    &uri,
                    OpenFlags::SQLITE_OPEN_READ_WRITE
                        | OpenFlags::SQLITE_OPEN_CREATE
                        | OpenFlags::SQLITE_OPEN_URI,
                    "sqlite-objs",
                )?;
                Self::configure_pragmas(&conn, mode)?;
                Ok(TestDb {
                    conn,
                    temp_dir: None,
                })
            }
        }
    }

    fn configure_pragmas(conn: &Connection, mode: PerfMode) -> Result<()> {
        // Azure VFS requires exclusive locking for WAL shared-memory
        if mode == PerfMode::Azure {
            conn.execute_batch("PRAGMA locking_mode=EXCLUSIVE;")?;
        }
        // In-memory databases don't support WAL mode - they use MEMORY journal mode automatically
        // For file/azure, we try to enable WAL but don't fail if it's not supported
        let _ = conn.execute_batch(
            "PRAGMA journal_mode=WAL;
             PRAGMA synchronous=NORMAL;
             PRAGMA busy_timeout=60000;",
        );
        Ok(())
    }

    fn ensure_vfs_registered() {
        VFS_REGISTERED.get_or_init(|| {
            sqlite_objs::SqliteObjsVfs::register_uri(false)
                .expect("Failed to register sqlite-objs VFS in URI mode");
        });
    }
}

impl std::ops::Deref for TestDb {
    type Target = Connection;
    fn deref(&self) -> &Self::Target {
        &self.conn
    }
}

const DEFAULT_WORK_ITERATIONS: usize = 6000;

fn work_iterations() -> usize {
    env::var("PERF_ITERATIONS")
        .ok()
        .and_then(|v| v.parse().ok())
        .unwrap_or(DEFAULT_WORK_ITERATIONS)
}

impl TestDb {
    fn run_work_loop(&self) {
        let n = work_iterations();
        if n == 0 {
            return;
        }

        // Create work table
        self.execute(
            "CREATE TABLE IF NOT EXISTS _perf_work (
            id INTEGER PRIMARY KEY,
            category TEXT NOT NULL,
            value REAL NOT NULL,
            payload TEXT
        )",
            [],
        )
        .unwrap();

        // Batch insert in a transaction
        self.execute("BEGIN", []).unwrap();
        for i in 0..n {
            self.execute(
                "INSERT INTO _perf_work VALUES (?, ?, ?, ?)",
                rusqlite::params![
                    i as i64,
                    format!("cat-{}", i % 10),
                    (i as f64) * 1.5,
                    format!("payload-data-for-iteration-{}", i)
                ],
            )
            .unwrap();
        }
        self.execute("COMMIT", []).unwrap();

        // Aggregate query
        let count: i64 = self
            .query_row("SELECT COUNT(*) FROM _perf_work", [], |r| r.get(0))
            .unwrap();
        assert_eq!(count, n as i64);

        // Group-by query
        let cat_count: i64 = self
            .query_row("SELECT COUNT(DISTINCT category) FROM _perf_work", [], |r| {
                r.get(0)
            })
            .unwrap();
        assert!(cat_count > 0);

        // Update with computation
        self.execute(
            "UPDATE _perf_work SET value = value * 2.0 WHERE category = 'cat-0'",
            [],
        )
        .unwrap();

        // Range query
        let sum: f64 = self
            .query_row(
                "SELECT SUM(value) FROM _perf_work WHERE id BETWEEN 10 AND 100",
                [],
                |r| r.get(0),
            )
            .unwrap_or(0.0);
        assert!(sum >= 0.0);

        // Delete subset
        self.execute("DELETE FROM _perf_work WHERE id < ?", [n as i64 / 4])
            .unwrap();

        // Verify remaining
        let remaining: i64 = self
            .query_row("SELECT COUNT(*) FROM _perf_work", [], |r| r.get(0))
            .unwrap();
        assert!(remaining > 0);
    }
}

//
// SCHEMA CRUD TESTS (~30 tests)
//

macro_rules! schema_test {
    ($name:ident, $schema:expr, $verify:expr) => {
        #[test]
        fn $name() {
            let db = TestDb::new().unwrap();
            db.execute_batch($schema).unwrap();
            $verify(&db);
            db.run_work_loop();
        }
    };
}

schema_test!(
    schema_simple_table,
    "CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT);",
    |db: &TestDb| {
        let count: i64 = db
            .query_row(
                "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='users'",
                [],
                |r| r.get(0),
            )
            .unwrap();
        assert_eq!(count, 1);
    }
);

schema_test!(
    schema_multi_column,
    "CREATE TABLE products (
        id INTEGER PRIMARY KEY,
        sku TEXT NOT NULL,
        name TEXT,
        price REAL,
        quantity INTEGER,
        created_at TEXT
    );",
    |db: &TestDb| {
        db.execute(
            "INSERT INTO products (sku, name, price, quantity) VALUES (?, ?, ?, ?)",
            ["ABC123", "Widget", "9.99", "100"],
        )
        .unwrap();
        let count: i64 = db
            .query_row("SELECT COUNT(*) FROM products", [], |r| r.get(0))
            .unwrap();
        assert_eq!(count, 1);
    }
);

schema_test!(
    schema_with_index,
    "CREATE TABLE orders (id INTEGER PRIMARY KEY, customer_id INTEGER, total REAL);
     CREATE INDEX idx_customer ON orders(customer_id);",
    |db: &TestDb| {
        let count: i64 = db
            .query_row(
                "SELECT COUNT(*) FROM sqlite_master WHERE type='index' AND name='idx_customer'",
                [],
                |r| r.get(0),
            )
            .unwrap();
        assert_eq!(count, 1);
    }
);

schema_test!(
    schema_unique_constraint,
    "CREATE TABLE emails (id INTEGER PRIMARY KEY, address TEXT UNIQUE);",
    |db: &TestDb| {
        db.execute(
            "INSERT INTO emails (address) VALUES (?)",
            ["test@example.com"],
        )
        .unwrap();
        let result = db.execute(
            "INSERT INTO emails (address) VALUES (?)",
            ["test@example.com"],
        );
        assert!(result.is_err()); // Unique constraint violation
    }
);

schema_test!(schema_foreign_key,
    "PRAGMA foreign_keys=ON;
     CREATE TABLE authors (id INTEGER PRIMARY KEY, name TEXT);
     CREATE TABLE books (id INTEGER PRIMARY KEY, title TEXT, author_id INTEGER REFERENCES authors(id));",
    |db: &TestDb| {
        db.execute("INSERT INTO authors (name) VALUES (?)", ["Tolkien"]).unwrap();
        db.execute("INSERT INTO books (title, author_id) VALUES (?, 1)", ["The Hobbit"]).unwrap();
        let count: i64 = db.query_row("SELECT COUNT(*) FROM books", [], |r| r.get(0)).unwrap();
        assert_eq!(count, 1);
    }
);

schema_test!(schema_composite_key,
    "CREATE TABLE enrollment (student_id INTEGER, course_id INTEGER, grade TEXT, PRIMARY KEY (student_id, course_id));",
    |db: &TestDb| {
        db.execute("INSERT INTO enrollment VALUES (1, 101, 'A')", []).unwrap();
        db.execute("INSERT INTO enrollment VALUES (1, 102, 'B')", []).unwrap();
        let count: i64 = db.query_row("SELECT COUNT(*) FROM enrollment", [], |r| r.get(0)).unwrap();
        assert_eq!(count, 2);
    }
);

schema_test!(
    schema_check_constraint,
    "CREATE TABLE inventory (id INTEGER PRIMARY KEY, quantity INTEGER CHECK(quantity >= 0));",
    |db: &TestDb| {
        db.execute("INSERT INTO inventory (quantity) VALUES (10)", [])
            .unwrap();
        let result = db.execute("INSERT INTO inventory (quantity) VALUES (-5)", []);
        assert!(result.is_err()); // Check constraint violation
    }
);

schema_test!(
    schema_default_values,
    "CREATE TABLE settings (key TEXT PRIMARY KEY, value TEXT, enabled INTEGER DEFAULT 1);",
    |db: &TestDb| {
        db.execute(
            "INSERT INTO settings (key, value) VALUES ('timeout', '30')",
            [],
        )
        .unwrap();
        let enabled: i64 = db
            .query_row(
                "SELECT enabled FROM settings WHERE key='timeout'",
                [],
                |r| r.get(0),
            )
            .unwrap();
        assert_eq!(enabled, 1);
    }
);

schema_test!(
    schema_drop_table,
    "CREATE TABLE temp_data (id INTEGER);
     INSERT INTO temp_data VALUES (1), (2), (3);
     DROP TABLE temp_data;",
    |db: &TestDb| {
        let count: i64 = db
            .query_row(
                "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name='temp_data'",
                [],
                |r| r.get(0),
            )
            .unwrap();
        assert_eq!(count, 0);
    }
);

schema_test!(
    schema_multiple_indexes,
    "CREATE TABLE logs (id INTEGER PRIMARY KEY, timestamp TEXT, level TEXT, message TEXT);
     CREATE INDEX idx_timestamp ON logs(timestamp);
     CREATE INDEX idx_level ON logs(level);
     CREATE INDEX idx_ts_level ON logs(timestamp, level);",
    |db: &TestDb| {
        let count: i64 = db
            .query_row(
                "SELECT COUNT(*) FROM sqlite_master WHERE type='index' AND tbl_name='logs'",
                [],
                |r| r.get(0),
            )
            .unwrap();
        assert_eq!(count, 3);
    }
);

// Additional schema tests (20 more for ~30 total)
schema_test!(
    schema_blob_column,
    "CREATE TABLE files (id INTEGER PRIMARY KEY, data BLOB);",
    |_db: &TestDb| {}
);
schema_test!(
    schema_text_affinity,
    "CREATE TABLE docs (id INTEGER PRIMARY KEY, content TEXT);",
    |_db: &TestDb| {}
);
schema_test!(
    schema_real_affinity,
    "CREATE TABLE measurements (id INTEGER PRIMARY KEY, value REAL);",
    |_db: &TestDb| {}
);
schema_test!(
    schema_integer_affinity,
    "CREATE TABLE counters (id INTEGER PRIMARY KEY, count INTEGER);",
    |_db: &TestDb| {}
);
schema_test!(
    schema_numeric_affinity,
    "CREATE TABLE stats (id INTEGER PRIMARY KEY, amount NUMERIC);",
    |_db: &TestDb| {}
);
schema_test!(
    schema_not_null,
    "CREATE TABLE required (id INTEGER PRIMARY KEY, name TEXT NOT NULL);",
    |_db: &TestDb| {}
);
schema_test!(
    schema_autoincrement,
    "CREATE TABLE seq (id INTEGER PRIMARY KEY AUTOINCREMENT, val TEXT);",
    |_db: &TestDb| {}
);
schema_test!(
    schema_without_rowid,
    "CREATE TABLE kvstore (key TEXT PRIMARY KEY, value TEXT) WITHOUT ROWID;",
    |_db: &TestDb| {}
);
schema_test!(
    schema_view,
    "CREATE TABLE base (id INTEGER); CREATE VIEW v AS SELECT * FROM base;",
    |_db: &TestDb| {}
);
schema_test!(
    schema_trigger,
    "CREATE TABLE audit (id INTEGER); CREATE TRIGGER t AFTER INSERT ON audit BEGIN SELECT 1; END;",
    |_db: &TestDb| {}
);
schema_test!(schema_partial_index, "CREATE TABLE items (id INTEGER, active INTEGER); CREATE INDEX idx_active ON items(id) WHERE active=1;", |_db: &TestDb| {});
schema_test!(
    schema_expression_index,
    "CREATE TABLE names (id INTEGER, name TEXT); CREATE INDEX idx_lower ON names(LOWER(name));",
    |_db: &TestDb| {}
);
schema_test!(
    schema_drop_index,
    "CREATE TABLE t (id INTEGER); CREATE INDEX i ON t(id); DROP INDEX i;",
    |_db: &TestDb| {}
);
schema_test!(
    schema_temp_table,
    "CREATE TEMP TABLE session (key TEXT, value TEXT);",
    |_db: &TestDb| {}
);
schema_test!(
    schema_if_not_exists,
    "CREATE TABLE IF NOT EXISTS users (id INTEGER); CREATE TABLE IF NOT EXISTS users (id INTEGER);",
    |_db: &TestDb| {}
);
schema_test!(
    schema_multiple_tables,
    "CREATE TABLE t1 (id INTEGER); CREATE TABLE t2 (id INTEGER); CREATE TABLE t3 (id INTEGER);",
    |_db: &TestDb| {}
);
schema_test!(schema_cascading_fk, "PRAGMA foreign_keys=ON; CREATE TABLE p (id INTEGER PRIMARY KEY); CREATE TABLE c (id INTEGER PRIMARY KEY, pid INTEGER REFERENCES p(id) ON DELETE CASCADE);", |_db: &TestDb| {});
schema_test!(
    schema_collate,
    "CREATE TABLE names (id INTEGER, name TEXT COLLATE NOCASE);",
    |_db: &TestDb| {}
);
schema_test!(
    schema_generated_column,
    "CREATE TABLE calc (a INTEGER, b INTEGER, sum INTEGER GENERATED ALWAYS AS (a+b));",
    |_db: &TestDb| {}
);
schema_test!(
    schema_strict_table,
    "CREATE TABLE strict_data (id INTEGER PRIMARY KEY, val TEXT) STRICT;",
    |_db: &TestDb| {}
);

//
// SINGLE-ROW OPERATIONS (~40 tests)
//

macro_rules! single_row_test {
    ($name:ident, $test:expr) => {
        #[test]
        fn $name() {
            let db = TestDb::new().unwrap();
            $test(&db);
            db.run_work_loop();
        }
    };
}

single_row_test!(row_insert_text, |db: &TestDb| {
    db.execute("CREATE TABLE t (val TEXT)", []).unwrap();
    db.execute("INSERT INTO t VALUES (?)", ["hello"]).unwrap();
    let val: String = db.query_row("SELECT val FROM t", [], |r| r.get(0)).unwrap();
    assert_eq!(val, "hello");
});

single_row_test!(row_insert_integer, |db: &TestDb| {
    db.execute("CREATE TABLE t (val INTEGER)", []).unwrap();
    db.execute("INSERT INTO t VALUES (42)", []).unwrap();
    let val: i64 = db.query_row("SELECT val FROM t", [], |r| r.get(0)).unwrap();
    assert_eq!(val, 42);
});

single_row_test!(row_insert_real, |db: &TestDb| {
    db.execute("CREATE TABLE t (val REAL)", []).unwrap();
    db.execute("INSERT INTO t VALUES (3.14159)", []).unwrap();
    let val: f64 = db.query_row("SELECT val FROM t", [], |r| r.get(0)).unwrap();
    assert!((val - std::f64::consts::PI).abs() < 0.0001);
});

single_row_test!(row_insert_blob, |db: &TestDb| {
    db.execute("CREATE TABLE t (val BLOB)", []).unwrap();
    let data = vec![1u8, 2, 3, 4, 5];
    db.execute("INSERT INTO t VALUES (?)", [&data]).unwrap();
    let retrieved: Vec<u8> = db.query_row("SELECT val FROM t", [], |r| r.get(0)).unwrap();
    assert_eq!(retrieved, data);
});

single_row_test!(row_insert_null, |db: &TestDb| {
    db.execute("CREATE TABLE t (val TEXT)", []).unwrap();
    db.execute("INSERT INTO t VALUES (NULL)", []).unwrap();
    let val: Option<String> = db.query_row("SELECT val FROM t", [], |r| r.get(0)).unwrap();
    assert!(val.is_none());
});

single_row_test!(row_update, |db: &TestDb| {
    db.execute("CREATE TABLE t (id INTEGER PRIMARY KEY, val TEXT)", [])
        .unwrap();
    db.execute("INSERT INTO t (val) VALUES ('old')", [])
        .unwrap();
    db.execute("UPDATE t SET val='new' WHERE id=1", []).unwrap();
    let val: String = db
        .query_row("SELECT val FROM t WHERE id=1", [], |r| r.get(0))
        .unwrap();
    assert_eq!(val, "new");
});

single_row_test!(row_delete, |db: &TestDb| {
    db.execute("CREATE TABLE t (id INTEGER PRIMARY KEY, val TEXT)", [])
        .unwrap();
    db.execute("INSERT INTO t (val) VALUES ('data')", [])
        .unwrap();
    db.execute("DELETE FROM t WHERE id=1", []).unwrap();
    let count: i64 = db
        .query_row("SELECT COUNT(*) FROM t", [], |r| r.get(0))
        .unwrap();
    assert_eq!(count, 0);
});

single_row_test!(row_select_by_id, |db: &TestDb| {
    db.execute("CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT)", [])
        .unwrap();
    db.execute(
        "INSERT INTO t VALUES (1, 'Alice'), (2, 'Bob'), (3, 'Charlie')",
        [],
    )
    .unwrap();
    let name: String = db
        .query_row("SELECT name FROM t WHERE id=2", [], |r| r.get(0))
        .unwrap();
    assert_eq!(name, "Bob");
});

single_row_test!(row_update_multiple_columns, |db: &TestDb| {
    db.execute(
        "CREATE TABLE t (id INTEGER PRIMARY KEY, a TEXT, b INTEGER)",
        [],
    )
    .unwrap();
    db.execute("INSERT INTO t VALUES (1, 'x', 10)", []).unwrap();
    db.execute("UPDATE t SET a='y', b=20 WHERE id=1", [])
        .unwrap();
    let (a, b): (String, i64) = db
        .query_row("SELECT a, b FROM t WHERE id=1", [], |r| {
            Ok((r.get(0)?, r.get(1)?))
        })
        .unwrap();
    assert_eq!((a.as_str(), b), ("y", 20));
});

single_row_test!(row_insert_returning, |db: &TestDb| {
    db.execute("CREATE TABLE t (id INTEGER PRIMARY KEY, val TEXT)", [])
        .unwrap();
    db.execute("INSERT INTO t (val) VALUES ('test')", [])
        .unwrap();
    let last_id = db.last_insert_rowid();
    assert_eq!(last_id, 1);
});

// Additional single-row tests (30 more for ~40 total)
single_row_test!(row_insert_unicode, |db: &TestDb| {
    db.execute("CREATE TABLE t (val TEXT)", []).unwrap();
    db.execute("INSERT INTO t VALUES ('Hello 世界 🌍')", [])
        .unwrap();
    let val: String = db.query_row("SELECT val FROM t", [], |r| r.get(0)).unwrap();
    assert_eq!(val, "Hello 世界 🌍");
});

single_row_test!(row_insert_empty_string, |db: &TestDb| {
    db.execute("CREATE TABLE t (val TEXT)", []).unwrap();
    db.execute("INSERT INTO t VALUES ('')", []).unwrap();
    let val: String = db.query_row("SELECT val FROM t", [], |r| r.get(0)).unwrap();
    assert_eq!(val, "");
});

single_row_test!(row_insert_large_integer, |db: &TestDb| {
    db.execute("CREATE TABLE t (val INTEGER)", []).unwrap();
    let large = 9223372036854775807i64; // i64::MAX
    db.execute("INSERT INTO t VALUES (?)", [large]).unwrap();
    let val: i64 = db.query_row("SELECT val FROM t", [], |r| r.get(0)).unwrap();
    assert_eq!(val, large);
});

single_row_test!(row_insert_negative, |db: &TestDb| {
    db.execute("CREATE TABLE t (val INTEGER)", []).unwrap();
    db.execute("INSERT INTO t VALUES (-12345)", []).unwrap();
    let val: i64 = db.query_row("SELECT val FROM t", [], |r| r.get(0)).unwrap();
    assert_eq!(val, -12345);
});

single_row_test!(row_insert_zero, |db: &TestDb| {
    db.execute("CREATE TABLE t (val INTEGER)", []).unwrap();
    db.execute("INSERT INTO t VALUES (0)", []).unwrap();
    let val: i64 = db.query_row("SELECT val FROM t", [], |r| r.get(0)).unwrap();
    assert_eq!(val, 0);
});

single_row_test!(row_insert_scientific_notation, |db: &TestDb| {
    db.execute("CREATE TABLE t (val REAL)", []).unwrap();
    db.execute("INSERT INTO t VALUES (1.23e-10)", []).unwrap();
    let val: f64 = db.query_row("SELECT val FROM t", [], |r| r.get(0)).unwrap();
    assert!((val - 1.23e-10).abs() < 1e-15);
});

single_row_test!(row_insert_empty_blob, |db: &TestDb| {
    db.execute("CREATE TABLE t (val BLOB)", []).unwrap();
    let data: Vec<u8> = vec![];
    db.execute("INSERT INTO t VALUES (?)", [&data]).unwrap();
    let retrieved: Vec<u8> = db.query_row("SELECT val FROM t", [], |r| r.get(0)).unwrap();
    assert_eq!(retrieved.len(), 0);
});

single_row_test!(row_insert_large_blob, |db: &TestDb| {
    db.execute("CREATE TABLE t (val BLOB)", []).unwrap();
    let data = vec![42u8; 10000];
    db.execute("INSERT INTO t VALUES (?)", [&data]).unwrap();
    let retrieved: Vec<u8> = db.query_row("SELECT val FROM t", [], |r| r.get(0)).unwrap();
    assert_eq!(retrieved.len(), 10000);
});

single_row_test!(row_update_where_clause, |db: &TestDb| {
    db.execute("CREATE TABLE t (id INTEGER, val INTEGER)", [])
        .unwrap();
    db.execute("INSERT INTO t VALUES (1, 10), (2, 20), (3, 30)", [])
        .unwrap();
    db.execute("UPDATE t SET val=25 WHERE id=2", []).unwrap();
    let val: i64 = db
        .query_row("SELECT val FROM t WHERE id=2", [], |r| r.get(0))
        .unwrap();
    assert_eq!(val, 25);
});

single_row_test!(row_update_all, |db: &TestDb| {
    db.execute("CREATE TABLE t (val INTEGER)", []).unwrap();
    db.execute("INSERT INTO t VALUES (1), (2), (3)", [])
        .unwrap();
    db.execute("UPDATE t SET val=99", []).unwrap();
    let sum: i64 = db
        .query_row("SELECT SUM(val) FROM t", [], |r| r.get(0))
        .unwrap();
    assert_eq!(sum, 297);
});

single_row_test!(row_delete_where, |db: &TestDb| {
    db.execute("CREATE TABLE t (id INTEGER, val TEXT)", [])
        .unwrap();
    db.execute("INSERT INTO t VALUES (1, 'a'), (2, 'b'), (3, 'c')", [])
        .unwrap();
    db.execute("DELETE FROM t WHERE id=2", []).unwrap();
    let count: i64 = db
        .query_row("SELECT COUNT(*) FROM t", [], |r| r.get(0))
        .unwrap();
    assert_eq!(count, 2);
});

single_row_test!(row_delete_all, |db: &TestDb| {
    db.execute("CREATE TABLE t (val TEXT)", []).unwrap();
    db.execute("INSERT INTO t VALUES ('a'), ('b'), ('c')", [])
        .unwrap();
    db.execute("DELETE FROM t", []).unwrap();
    let count: i64 = db
        .query_row("SELECT COUNT(*) FROM t", [], |r| r.get(0))
        .unwrap();
    assert_eq!(count, 0);
});

single_row_test!(row_select_star, |db: &TestDb| {
    db.execute("CREATE TABLE t (a INTEGER, b TEXT, c REAL)", [])
        .unwrap();
    db.execute("INSERT INTO t VALUES (1, 'x', 2.5)", [])
        .unwrap();
    let (a, b, c): (i64, String, f64) = db
        .query_row("SELECT * FROM t", [], |r| {
            Ok((r.get(0)?, r.get(1)?, r.get(2)?))
        })
        .unwrap();
    assert_eq!((a, b.as_str(), c), (1, "x", 2.5));
});

single_row_test!(row_select_expressions, |db: &TestDb| {
    db.execute("CREATE TABLE t (a INTEGER, b INTEGER)", [])
        .unwrap();
    db.execute("INSERT INTO t VALUES (5, 10)", []).unwrap();
    let sum: i64 = db
        .query_row("SELECT a + b FROM t", [], |r| r.get(0))
        .unwrap();
    assert_eq!(sum, 15);
});

single_row_test!(row_select_literal, |db: &TestDb| {
    let val: i64 = db.query_row("SELECT 42", [], |r| r.get(0)).unwrap();
    assert_eq!(val, 42);
});

single_row_test!(row_replace, |db: &TestDb| {
    db.execute("CREATE TABLE t (id INTEGER PRIMARY KEY, val TEXT)", [])
        .unwrap();
    db.execute("INSERT INTO t VALUES (1, 'old')", []).unwrap();
    db.execute("REPLACE INTO t VALUES (1, 'new')", []).unwrap();
    let val: String = db
        .query_row("SELECT val FROM t WHERE id=1", [], |r| r.get(0))
        .unwrap();
    assert_eq!(val, "new");
});

single_row_test!(row_insert_or_ignore, |db: &TestDb| {
    db.execute("CREATE TABLE t (id INTEGER PRIMARY KEY)", [])
        .unwrap();
    db.execute("INSERT INTO t VALUES (1)", []).unwrap();
    db.execute("INSERT OR IGNORE INTO t VALUES (1)", [])
        .unwrap();
    let count: i64 = db
        .query_row("SELECT COUNT(*) FROM t", [], |r| r.get(0))
        .unwrap();
    assert_eq!(count, 1);
});

single_row_test!(row_insert_or_replace, |db: &TestDb| {
    db.execute("CREATE TABLE t (id INTEGER PRIMARY KEY, val TEXT)", [])
        .unwrap();
    db.execute("INSERT INTO t VALUES (1, 'first')", []).unwrap();
    db.execute("INSERT OR REPLACE INTO t VALUES (1, 'second')", [])
        .unwrap();
    let val: String = db
        .query_row("SELECT val FROM t WHERE id=1", [], |r| r.get(0))
        .unwrap();
    assert_eq!(val, "second");
});

single_row_test!(row_upsert, |db: &TestDb| {
    db.execute("CREATE TABLE t (id INTEGER PRIMARY KEY, val TEXT)", [])
        .unwrap();
    db.execute(
        "INSERT INTO t VALUES (1, 'initial') ON CONFLICT(id) DO UPDATE SET val='updated'",
        [],
    )
    .unwrap();
    db.execute(
        "INSERT INTO t VALUES (1, 'new') ON CONFLICT(id) DO UPDATE SET val='updated'",
        [],
    )
    .unwrap();
    let val: String = db
        .query_row("SELECT val FROM t WHERE id=1", [], |r| r.get(0))
        .unwrap();
    assert_eq!(val, "updated");
});

single_row_test!(row_select_case, |db: &TestDb| {
    db.execute("CREATE TABLE t (val INTEGER)", []).unwrap();
    db.execute("INSERT INTO t VALUES (5)", []).unwrap();
    let label: String = db
        .query_row(
            "SELECT CASE WHEN val > 3 THEN 'high' ELSE 'low' END FROM t",
            [],
            |r| r.get(0),
        )
        .unwrap();
    assert_eq!(label, "high");
});

single_row_test!(row_select_coalesce, |db: &TestDb| {
    db.execute("CREATE TABLE t (val TEXT)", []).unwrap();
    db.execute("INSERT INTO t VALUES (NULL)", []).unwrap();
    let val: String = db
        .query_row("SELECT COALESCE(val, 'default') FROM t", [], |r| r.get(0))
        .unwrap();
    assert_eq!(val, "default");
});

single_row_test!(row_select_substr, |db: &TestDb| {
    db.execute("CREATE TABLE t (val TEXT)", []).unwrap();
    db.execute("INSERT INTO t VALUES ('hello world')", [])
        .unwrap();
    let sub: String = db
        .query_row("SELECT SUBSTR(val, 1, 5) FROM t", [], |r| r.get(0))
        .unwrap();
    assert_eq!(sub, "hello");
});

single_row_test!(row_select_length, |db: &TestDb| {
    db.execute("CREATE TABLE t (val TEXT)", []).unwrap();
    db.execute("INSERT INTO t VALUES ('test')", []).unwrap();
    let len: i64 = db
        .query_row("SELECT LENGTH(val) FROM t", [], |r| r.get(0))
        .unwrap();
    assert_eq!(len, 4);
});

single_row_test!(row_select_upper, |db: &TestDb| {
    db.execute("CREATE TABLE t (val TEXT)", []).unwrap();
    db.execute("INSERT INTO t VALUES ('hello')", []).unwrap();
    let upper: String = db
        .query_row("SELECT UPPER(val) FROM t", [], |r| r.get(0))
        .unwrap();
    assert_eq!(upper, "HELLO");
});

single_row_test!(row_select_lower, |db: &TestDb| {
    db.execute("CREATE TABLE t (val TEXT)", []).unwrap();
    db.execute("INSERT INTO t VALUES ('WORLD')", []).unwrap();
    let lower: String = db
        .query_row("SELECT LOWER(val) FROM t", [], |r| r.get(0))
        .unwrap();
    assert_eq!(lower, "world");
});

single_row_test!(row_select_trim, |db: &TestDb| {
    db.execute("CREATE TABLE t (val TEXT)", []).unwrap();
    db.execute("INSERT INTO t VALUES ('  spaces  ')", [])
        .unwrap();
    let trimmed: String = db
        .query_row("SELECT TRIM(val) FROM t", [], |r| r.get(0))
        .unwrap();
    assert_eq!(trimmed, "spaces");
});

single_row_test!(row_select_round, |db: &TestDb| {
    db.execute("CREATE TABLE t (val REAL)", []).unwrap();
    db.execute("INSERT INTO t VALUES (3.14159)", []).unwrap();
    let rounded: f64 = db
        .query_row("SELECT ROUND(val, 2) FROM t", [], |r| r.get(0))
        .unwrap();
    assert!((rounded - std::f64::consts::PI).abs() < 0.01);
});

single_row_test!(row_select_abs, |db: &TestDb| {
    db.execute("CREATE TABLE t (val INTEGER)", []).unwrap();
    db.execute("INSERT INTO t VALUES (-42)", []).unwrap();
    let abs_val: i64 = db
        .query_row("SELECT ABS(val) FROM t", [], |r| r.get(0))
        .unwrap();
    assert_eq!(abs_val, 42);
});

single_row_test!(row_select_datetime, |db: &TestDb| {
    let now: String = db
        .query_row("SELECT DATETIME('now')", [], |r| r.get(0))
        .unwrap();
    assert!(now.len() > 10); // Basic sanity check
});

//
// BULK OPERATIONS (~40 tests)
//

macro_rules! bulk_test {
    ($name:ident, $test:expr) => {
        #[test]
        fn $name() {
            let db = TestDb::new().unwrap();
            $test(&db);
            db.run_work_loop();
        }
    };
}

bulk_test!(bulk_insert_100_rows, |db: &TestDb| {
    db.execute("CREATE TABLE t (id INTEGER, val TEXT)", [])
        .unwrap();
    for i in 0..100 {
        db.execute(
            "INSERT INTO t VALUES (?, ?)",
            rusqlite::params![i, format!("row{}", i)],
        )
        .unwrap();
    }
    let count: i64 = db
        .query_row("SELECT COUNT(*) FROM t", [], |r| r.get(0))
        .unwrap();
    assert_eq!(count, 100);
});

bulk_test!(bulk_insert_transaction, |db: &TestDb| {
    db.execute("CREATE TABLE t (id INTEGER)", []).unwrap();
    db.execute("BEGIN", []).unwrap();
    for i in 0..500 {
        db.execute("INSERT INTO t VALUES (?)", [i]).unwrap();
    }
    db.execute("COMMIT", []).unwrap();
    let count: i64 = db
        .query_row("SELECT COUNT(*) FROM t", [], |r| r.get(0))
        .unwrap();
    assert_eq!(count, 500);
});

bulk_test!(bulk_select_where_in, |db: &TestDb| {
    db.execute("CREATE TABLE t (id INTEGER)", []).unwrap();
    db.execute("INSERT INTO t VALUES (1), (2), (3), (4), (5)", [])
        .unwrap();
    let count: i64 = db
        .query_row("SELECT COUNT(*) FROM t WHERE id IN (2, 4, 5)", [], |r| {
            r.get(0)
        })
        .unwrap();
    assert_eq!(count, 3);
});

bulk_test!(bulk_select_like, |db: &TestDb| {
    db.execute("CREATE TABLE t (name TEXT)", []).unwrap();
    db.execute(
        "INSERT INTO t VALUES ('apple'), ('application'), ('banana'), ('apricot')",
        [],
    )
    .unwrap();
    let count: i64 = db
        .query_row("SELECT COUNT(*) FROM t WHERE name LIKE 'app%'", [], |r| {
            r.get(0)
        })
        .unwrap();
    assert_eq!(count, 2);
});

bulk_test!(bulk_aggregate_sum, |db: &TestDb| {
    db.execute("CREATE TABLE t (val INTEGER)", []).unwrap();
    db.execute("INSERT INTO t VALUES (10), (20), (30), (40), (50)", [])
        .unwrap();
    let sum: i64 = db
        .query_row("SELECT SUM(val) FROM t", [], |r| r.get(0))
        .unwrap();
    assert_eq!(sum, 150);
});

bulk_test!(bulk_aggregate_avg, |db: &TestDb| {
    db.execute("CREATE TABLE t (val INTEGER)", []).unwrap();
    db.execute("INSERT INTO t VALUES (10), (20), (30), (40), (50)", [])
        .unwrap();
    let avg: f64 = db
        .query_row("SELECT AVG(val) FROM t", [], |r| r.get(0))
        .unwrap();
    assert!((avg - 30.0).abs() < 0.01);
});

bulk_test!(bulk_aggregate_min_max, |db: &TestDb| {
    db.execute("CREATE TABLE t (val INTEGER)", []).unwrap();
    db.execute("INSERT INTO t VALUES (5), (15), (3), (22), (8)", [])
        .unwrap();
    let (min, max): (i64, i64) = db
        .query_row("SELECT MIN(val), MAX(val) FROM t", [], |r| {
            Ok((r.get(0)?, r.get(1)?))
        })
        .unwrap();
    assert_eq!((min, max), (3, 22));
});

bulk_test!(bulk_group_by, |db: &TestDb| {
    db.execute("CREATE TABLE t (category TEXT, amount INTEGER)", [])
        .unwrap();
    db.execute(
        "INSERT INTO t VALUES ('A', 10), ('B', 20), ('A', 15), ('B', 25), ('A', 5)",
        [],
    )
    .unwrap();
    let sum_a: i64 = db
        .query_row(
            "SELECT SUM(amount) FROM t WHERE category='A' GROUP BY category",
            [],
            |r| r.get(0),
        )
        .unwrap();
    assert_eq!(sum_a, 30);
});

bulk_test!(bulk_order_by_asc, |db: &TestDb| {
    db.execute("CREATE TABLE t (val INTEGER)", []).unwrap();
    db.execute("INSERT INTO t VALUES (30), (10), (20), (50), (40)", [])
        .unwrap();
    let first: i64 = db
        .query_row("SELECT val FROM t ORDER BY val ASC LIMIT 1", [], |r| {
            r.get(0)
        })
        .unwrap();
    assert_eq!(first, 10);
});

bulk_test!(bulk_order_by_desc, |db: &TestDb| {
    db.execute("CREATE TABLE t (val INTEGER)", []).unwrap();
    db.execute("INSERT INTO t VALUES (30), (10), (20), (50), (40)", [])
        .unwrap();
    let first: i64 = db
        .query_row("SELECT val FROM t ORDER BY val DESC LIMIT 1", [], |r| {
            r.get(0)
        })
        .unwrap();
    assert_eq!(first, 50);
});

bulk_test!(bulk_limit_offset, |db: &TestDb| {
    db.execute("CREATE TABLE t (id INTEGER)", []).unwrap();
    for i in 1..=20 {
        db.execute("INSERT INTO t VALUES (?)", [i]).unwrap();
    }
    let val: i64 = db
        .query_row("SELECT id FROM t ORDER BY id LIMIT 1 OFFSET 10", [], |r| {
            r.get(0)
        })
        .unwrap();
    assert_eq!(val, 11);
});

// Additional bulk tests (30 more for ~40 total)
bulk_test!(bulk_insert_1000_rows, |db: &TestDb| {
    db.execute("CREATE TABLE t (id INTEGER)", []).unwrap();
    db.execute("BEGIN", []).unwrap();
    for i in 0..1000 {
        db.execute("INSERT INTO t VALUES (?)", [i]).unwrap();
    }
    db.execute("COMMIT", []).unwrap();
    let count: i64 = db
        .query_row("SELECT COUNT(*) FROM t", [], |r| r.get(0))
        .unwrap();
    assert_eq!(count, 1000);
});

bulk_test!(bulk_update_batch, |db: &TestDb| {
    db.execute("CREATE TABLE t (id INTEGER, val INTEGER)", [])
        .unwrap();
    for i in 0..100 {
        db.execute("INSERT INTO t VALUES (?, 0)", [i]).unwrap();
    }
    db.execute("UPDATE t SET val=1 WHERE id < 50", []).unwrap();
    let count: i64 = db
        .query_row("SELECT COUNT(*) FROM t WHERE val=1", [], |r| r.get(0))
        .unwrap();
    assert_eq!(count, 50);
});

bulk_test!(bulk_delete_batch, |db: &TestDb| {
    db.execute("CREATE TABLE t (id INTEGER)", []).unwrap();
    for i in 0..100 {
        db.execute("INSERT INTO t VALUES (?)", [i]).unwrap();
    }
    db.execute("DELETE FROM t WHERE id >= 80", []).unwrap();
    let count: i64 = db
        .query_row("SELECT COUNT(*) FROM t", [], |r| r.get(0))
        .unwrap();
    assert_eq!(count, 80);
});

bulk_test!(bulk_select_distinct, |db: &TestDb| {
    db.execute("CREATE TABLE t (val TEXT)", []).unwrap();
    db.execute(
        "INSERT INTO t VALUES ('a'), ('b'), ('a'), ('c'), ('b'), ('a')",
        [],
    )
    .unwrap();
    let count: i64 = db
        .query_row("SELECT COUNT(DISTINCT val) FROM t", [], |r| r.get(0))
        .unwrap();
    assert_eq!(count, 3);
});

bulk_test!(bulk_select_having, |db: &TestDb| {
    db.execute("CREATE TABLE t (category TEXT, amount INTEGER)", [])
        .unwrap();
    db.execute(
        "INSERT INTO t VALUES ('A', 10), ('B', 5), ('A', 20), ('C', 3), ('B', 8)",
        [],
    )
    .unwrap();
    let count: i64 = db.query_row("SELECT COUNT(*) FROM (SELECT category, SUM(amount) as total FROM t GROUP BY category HAVING total > 10)", [], |r| r.get(0)).unwrap();
    assert_eq!(count, 2);
});

bulk_test!(bulk_aggregate_count_star, |db: &TestDb| {
    db.execute("CREATE TABLE t (id INTEGER)", []).unwrap();
    for i in 0..250 {
        db.execute("INSERT INTO t VALUES (?)", [i]).unwrap();
    }
    let count: i64 = db
        .query_row("SELECT COUNT(*) FROM t", [], |r| r.get(0))
        .unwrap();
    assert_eq!(count, 250);
});

bulk_test!(bulk_aggregate_group_by_multiple, |db: &TestDb| {
    db.execute("CREATE TABLE t (a TEXT, b TEXT, val INTEGER)", [])
        .unwrap();
    db.execute(
        "INSERT INTO t VALUES ('x', 'y', 10), ('x', 'z', 20), ('x', 'y', 30), ('w', 'y', 40)",
        [],
    )
    .unwrap();
    let sum: i64 = db
        .query_row(
            "SELECT SUM(val) FROM t WHERE a='x' AND b='y' GROUP BY a, b",
            [],
            |r| r.get(0),
        )
        .unwrap();
    assert_eq!(sum, 40);
});

bulk_test!(bulk_select_between, |db: &TestDb| {
    db.execute("CREATE TABLE t (val INTEGER)", []).unwrap();
    for i in 1..=100 {
        db.execute("INSERT INTO t VALUES (?)", [i]).unwrap();
    }
    let count: i64 = db
        .query_row(
            "SELECT COUNT(*) FROM t WHERE val BETWEEN 25 AND 75",
            [],
            |r| r.get(0),
        )
        .unwrap();
    assert_eq!(count, 51);
});

bulk_test!(bulk_select_not_in, |db: &TestDb| {
    db.execute("CREATE TABLE t (id INTEGER)", []).unwrap();
    db.execute("INSERT INTO t VALUES (1), (2), (3), (4), (5)", [])
        .unwrap();
    let count: i64 = db
        .query_row("SELECT COUNT(*) FROM t WHERE id NOT IN (2, 4)", [], |r| {
            r.get(0)
        })
        .unwrap();
    assert_eq!(count, 3);
});

bulk_test!(bulk_select_glob, |db: &TestDb| {
    db.execute("CREATE TABLE t (name TEXT)", []).unwrap();
    db.execute(
        "INSERT INTO t VALUES ('test1'), ('test2'), ('other'), ('test3')",
        [],
    )
    .unwrap();
    let count: i64 = db
        .query_row("SELECT COUNT(*) FROM t WHERE name GLOB 'test*'", [], |r| {
            r.get(0)
        })
        .unwrap();
    assert_eq!(count, 3);
});

bulk_test!(bulk_insert_values_list, |db: &TestDb| {
    db.execute("CREATE TABLE t (id INTEGER, val TEXT)", [])
        .unwrap();
    db.execute(
        "INSERT INTO t VALUES (1, 'a'), (2, 'b'), (3, 'c'), (4, 'd'), (5, 'e')",
        [],
    )
    .unwrap();
    let count: i64 = db
        .query_row("SELECT COUNT(*) FROM t", [], |r| r.get(0))
        .unwrap();
    assert_eq!(count, 5);
});

bulk_test!(bulk_select_order_multiple, |db: &TestDb| {
    db.execute("CREATE TABLE t (a INTEGER, b INTEGER)", [])
        .unwrap();
    db.execute("INSERT INTO t VALUES (1, 20), (2, 10), (1, 30), (2, 5)", [])
        .unwrap();
    let (a, b): (i64, i64) = db
        .query_row(
            "SELECT a, b FROM t ORDER BY a ASC, b DESC LIMIT 1",
            [],
            |r| Ok((r.get(0)?, r.get(1)?)),
        )
        .unwrap();
    assert_eq!((a, b), (1, 30));
});

bulk_test!(bulk_aggregate_total, |db: &TestDb| {
    db.execute("CREATE TABLE t (val REAL)", []).unwrap();
    db.execute(
        "INSERT INTO t VALUES (NULL), (10.5), (20.3), (NULL), (15.2)",
        [],
    )
    .unwrap();
    let total: f64 = db
        .query_row("SELECT TOTAL(val) FROM t", [], |r| r.get(0))
        .unwrap();
    assert!((total - 46.0).abs() < 0.1);
});

bulk_test!(bulk_select_null_handling, |db: &TestDb| {
    db.execute("CREATE TABLE t (val INTEGER)", []).unwrap();
    db.execute("INSERT INTO t VALUES (1), (NULL), (2), (NULL), (3)", [])
        .unwrap();
    let count: i64 = db
        .query_row("SELECT COUNT(*) FROM t WHERE val IS NOT NULL", [], |r| {
            r.get(0)
        })
        .unwrap();
    assert_eq!(count, 3);
});

bulk_test!(bulk_string_concatenation, |db: &TestDb| {
    db.execute("CREATE TABLE t (first TEXT, last TEXT)", [])
        .unwrap();
    db.execute(
        "INSERT INTO t VALUES ('John', 'Doe'), ('Jane', 'Smith')",
        [],
    )
    .unwrap();
    let name: String = db
        .query_row(
            "SELECT first || ' ' || last FROM t WHERE first='John'",
            [],
            |r| r.get(0),
        )
        .unwrap();
    assert_eq!(name, "John Doe");
});

bulk_test!(bulk_math_operations, |db: &TestDb| {
    db.execute("CREATE TABLE t (a INTEGER, b INTEGER)", [])
        .unwrap();
    db.execute("INSERT INTO t VALUES (10, 3), (20, 4), (15, 5)", [])
        .unwrap();
    let result: i64 = db
        .query_row("SELECT SUM(a * b) FROM t", [], |r| r.get(0))
        .unwrap();
    assert_eq!(result, 10 * 3 + 20 * 4 + 15 * 5);
});

bulk_test!(bulk_select_random, |db: &TestDb| {
    db.execute("CREATE TABLE t (id INTEGER)", []).unwrap();
    for i in 0..100 {
        db.execute("INSERT INTO t VALUES (?)", [i]).unwrap();
    }
    let val: i64 = db
        .query_row("SELECT id FROM t ORDER BY RANDOM() LIMIT 1", [], |r| {
            r.get(0)
        })
        .unwrap();
    assert!((0..100).contains(&val));
});

bulk_test!(bulk_insert_select, |db: &TestDb| {
    db.execute("CREATE TABLE source (id INTEGER)", []).unwrap();
    db.execute("CREATE TABLE dest (id INTEGER)", []).unwrap();
    for i in 0..50 {
        db.execute("INSERT INTO source VALUES (?)", [i]).unwrap();
    }
    db.execute("INSERT INTO dest SELECT id FROM source WHERE id < 25", [])
        .unwrap();
    let count: i64 = db
        .query_row("SELECT COUNT(*) FROM dest", [], |r| r.get(0))
        .unwrap();
    assert_eq!(count, 25);
});

bulk_test!(bulk_update_from_select, |db: &TestDb| {
    db.execute("CREATE TABLE t (id INTEGER, val INTEGER)", [])
        .unwrap();
    for i in 0..100 {
        db.execute("INSERT INTO t VALUES (?, 0)", [i]).unwrap();
    }
    db.execute("UPDATE t SET val=(SELECT COUNT(*) FROM t) WHERE id=0", [])
        .unwrap();
    let val: i64 = db
        .query_row("SELECT val FROM t WHERE id=0", [], |r| r.get(0))
        .unwrap();
    assert_eq!(val, 100);
});

bulk_test!(bulk_aggregate_stddev_placeholder, |db: &TestDb| {
    db.execute("CREATE TABLE t (val INTEGER)", []).unwrap();
    db.execute("INSERT INTO t VALUES (10), (20), (30), (40), (50)", [])
        .unwrap();
    // SQLite doesn't have built-in STDDEV, but we can test variance-like computation
    let avg: f64 = db
        .query_row("SELECT AVG(val * val) FROM t", [], |r| r.get(0))
        .unwrap();
    assert!(avg > 0.0);
});

bulk_test!(bulk_json_extract, |db: &TestDb| {
    db.execute("CREATE TABLE t (data TEXT)", []).unwrap();
    db.execute(r#"INSERT INTO t VALUES ('{"name":"Alice","age":30}')"#, [])
        .unwrap();
    let name: String = db
        .query_row("SELECT JSON_EXTRACT(data, '$.name') FROM t", [], |r| {
            r.get(0)
        })
        .unwrap();
    assert_eq!(name, "Alice");
});

bulk_test!(bulk_fts5_search, |db: &TestDb| {
    db.execute("CREATE VIRTUAL TABLE docs USING fts5(content)", [])
        .unwrap();
    db.execute("INSERT INTO docs VALUES ('SQLite is great'), ('PostgreSQL is powerful'), ('SQLite is fast')", []).unwrap();
    let count: i64 = db
        .query_row(
            "SELECT COUNT(*) FROM docs WHERE docs MATCH 'SQLite'",
            [],
            |r| r.get(0),
        )
        .unwrap();
    assert_eq!(count, 2);
});

bulk_test!(bulk_recursive_cte_placeholder, |db: &TestDb| {
    // Simple recursive CTE - generate series 1..10
    let sum: i64 = db.query_row(
        "WITH RECURSIVE cnt(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM cnt WHERE x<10) SELECT SUM(x) FROM cnt",
        [], |r| r.get(0)
    ).unwrap();
    assert_eq!(sum, 55);
});

bulk_test!(bulk_window_function, |db: &TestDb| {
    db.execute("CREATE TABLE t (id INTEGER, val INTEGER)", [])
        .unwrap();
    db.execute("INSERT INTO t VALUES (1, 10), (2, 20), (3, 30)", [])
        .unwrap();
    let sum: i64 = db
        .query_row("SELECT SUM(val) OVER () FROM t LIMIT 1", [], |r| r.get(0))
        .unwrap();
    assert_eq!(sum, 60);
});

bulk_test!(bulk_rank_window, |db: &TestDb| {
    db.execute("CREATE TABLE t (name TEXT, score INTEGER)", [])
        .unwrap();
    db.execute(
        "INSERT INTO t VALUES ('A', 90), ('B', 80), ('C', 90), ('D', 70)",
        [],
    )
    .unwrap();
    let rank: i64 = db
        .query_row(
            "WITH ranked AS (SELECT name, RANK() OVER (ORDER BY score DESC) as rnk FROM t)
         SELECT rnk FROM ranked WHERE name='B'",
            [],
            |r| r.get(0),
        )
        .unwrap();
    assert_eq!(rank, 3);
});

bulk_test!(bulk_row_number, |db: &TestDb| {
    db.execute("CREATE TABLE t (val INTEGER)", []).unwrap();
    db.execute("INSERT INTO t VALUES (100), (200), (300)", [])
        .unwrap();
    let row_num: i64 = db
        .query_row(
            "WITH numbered AS (SELECT val, ROW_NUMBER() OVER (ORDER BY val) as rn FROM t)
         SELECT rn FROM numbered WHERE val=200",
            [],
            |r| r.get(0),
        )
        .unwrap();
    assert_eq!(row_num, 2);
});

bulk_test!(bulk_partition_by, |db: &TestDb| {
    db.execute("CREATE TABLE t (category TEXT, val INTEGER)", [])
        .unwrap();
    db.execute(
        "INSERT INTO t VALUES ('A', 10), ('A', 20), ('B', 30), ('B', 40)",
        [],
    )
    .unwrap();
    let sum: i64 = db
        .query_row(
            "SELECT SUM(val) OVER (PARTITION BY category) FROM t WHERE category='A' LIMIT 1",
            [],
            |r| r.get(0),
        )
        .unwrap();
    assert_eq!(sum, 30);
});

bulk_test!(bulk_lead_lag, |db: &TestDb| {
    db.execute("CREATE TABLE t (val INTEGER)", []).unwrap();
    db.execute("INSERT INTO t VALUES (1), (2), (3), (4), (5)", [])
        .unwrap();
    // Use a CTE to compute window function then filter
    let next: i64 = db
        .query_row(
            "WITH windowed AS (SELECT val, LEAD(val) OVER (ORDER BY val) as next_val FROM t)
         SELECT next_val FROM windowed WHERE val=3",
            [],
            |r| r.get(0),
        )
        .unwrap();
    assert_eq!(next, 4);
});

//
// TRANSACTION TESTS (~30 tests)
//

macro_rules! txn_test {
    ($name:ident, $test:expr) => {
        #[test]
        fn $name() {
            let db = TestDb::new().unwrap();
            $test(&db);
            db.run_work_loop();
        }
    };
}

txn_test!(txn_commit, |db: &TestDb| {
    db.execute("CREATE TABLE t (val INTEGER)", []).unwrap();
    db.execute("BEGIN", []).unwrap();
    db.execute("INSERT INTO t VALUES (1)", []).unwrap();
    db.execute("INSERT INTO t VALUES (2)", []).unwrap();
    db.execute("COMMIT", []).unwrap();
    let count: i64 = db
        .query_row("SELECT COUNT(*) FROM t", [], |r| r.get(0))
        .unwrap();
    assert_eq!(count, 2);
});

txn_test!(txn_rollback, |db: &TestDb| {
    db.execute("CREATE TABLE t (val INTEGER)", []).unwrap();
    db.execute("INSERT INTO t VALUES (1)", []).unwrap();
    db.execute("BEGIN", []).unwrap();
    db.execute("INSERT INTO t VALUES (2)", []).unwrap();
    db.execute("ROLLBACK", []).unwrap();
    let count: i64 = db
        .query_row("SELECT COUNT(*) FROM t", [], |r| r.get(0))
        .unwrap();
    assert_eq!(count, 1);
});

txn_test!(txn_savepoint_release, |db: &TestDb| {
    db.execute("CREATE TABLE t (val INTEGER)", []).unwrap();
    db.execute("BEGIN", []).unwrap();
    db.execute("INSERT INTO t VALUES (1)", []).unwrap();
    db.execute("SAVEPOINT sp1", []).unwrap();
    db.execute("INSERT INTO t VALUES (2)", []).unwrap();
    db.execute("RELEASE sp1", []).unwrap();
    db.execute("COMMIT", []).unwrap();
    let count: i64 = db
        .query_row("SELECT COUNT(*) FROM t", [], |r| r.get(0))
        .unwrap();
    assert_eq!(count, 2);
});

txn_test!(txn_savepoint_rollback, |db: &TestDb| {
    db.execute("CREATE TABLE t (val INTEGER)", []).unwrap();
    db.execute("BEGIN", []).unwrap();
    db.execute("INSERT INTO t VALUES (1)", []).unwrap();
    db.execute("SAVEPOINT sp1", []).unwrap();
    db.execute("INSERT INTO t VALUES (2)", []).unwrap();
    db.execute("ROLLBACK TO sp1", []).unwrap();
    db.execute("COMMIT", []).unwrap();
    let count: i64 = db
        .query_row("SELECT COUNT(*) FROM t", [], |r| r.get(0))
        .unwrap();
    assert_eq!(count, 1);
});

txn_test!(txn_nested_savepoints, |db: &TestDb| {
    db.execute("CREATE TABLE t (val INTEGER)", []).unwrap();
    db.execute("BEGIN", []).unwrap();
    db.execute("INSERT INTO t VALUES (1)", []).unwrap();
    db.execute("SAVEPOINT sp1", []).unwrap();
    db.execute("INSERT INTO t VALUES (2)", []).unwrap();
    db.execute("SAVEPOINT sp2", []).unwrap();
    db.execute("INSERT INTO t VALUES (3)", []).unwrap();
    db.execute("ROLLBACK TO sp2", []).unwrap();
    db.execute("RELEASE sp1", []).unwrap();
    db.execute("COMMIT", []).unwrap();
    let count: i64 = db
        .query_row("SELECT COUNT(*) FROM t", [], |r| r.get(0))
        .unwrap();
    assert_eq!(count, 2);
});

txn_test!(txn_deferred, |db: &TestDb| {
    db.execute("CREATE TABLE t (val INTEGER)", []).unwrap();
    db.execute("BEGIN DEFERRED", []).unwrap();
    db.execute("INSERT INTO t VALUES (1)", []).unwrap();
    db.execute("COMMIT", []).unwrap();
    let count: i64 = db
        .query_row("SELECT COUNT(*) FROM t", [], |r| r.get(0))
        .unwrap();
    assert_eq!(count, 1);
});

txn_test!(txn_immediate, |db: &TestDb| {
    db.execute("CREATE TABLE t (val INTEGER)", []).unwrap();
    db.execute("BEGIN IMMEDIATE", []).unwrap();
    db.execute("INSERT INTO t VALUES (1)", []).unwrap();
    db.execute("COMMIT", []).unwrap();
    let count: i64 = db
        .query_row("SELECT COUNT(*) FROM t", [], |r| r.get(0))
        .unwrap();
    assert_eq!(count, 1);
});

txn_test!(txn_exclusive, |db: &TestDb| {
    db.execute("CREATE TABLE t (val INTEGER)", []).unwrap();
    db.execute("BEGIN EXCLUSIVE", []).unwrap();
    db.execute("INSERT INTO t VALUES (1)", []).unwrap();
    db.execute("COMMIT", []).unwrap();
    let count: i64 = db
        .query_row("SELECT COUNT(*) FROM t", [], |r| r.get(0))
        .unwrap();
    assert_eq!(count, 1);
});

txn_test!(txn_autocommit_insert, |db: &TestDb| {
    db.execute("CREATE TABLE t (val INTEGER)", []).unwrap();
    db.execute("INSERT INTO t VALUES (1)", []).unwrap();
    let count: i64 = db
        .query_row("SELECT COUNT(*) FROM t", [], |r| r.get(0))
        .unwrap();
    assert_eq!(count, 1);
});

txn_test!(txn_multiple_commits, |db: &TestDb| {
    db.execute("CREATE TABLE t (val INTEGER)", []).unwrap();
    db.execute("BEGIN", []).unwrap();
    db.execute("INSERT INTO t VALUES (1)", []).unwrap();
    db.execute("COMMIT", []).unwrap();
    db.execute("BEGIN", []).unwrap();
    db.execute("INSERT INTO t VALUES (2)", []).unwrap();
    db.execute("COMMIT", []).unwrap();
    let count: i64 = db
        .query_row("SELECT COUNT(*) FROM t", [], |r| r.get(0))
        .unwrap();
    assert_eq!(count, 2);
});

// Additional transaction tests (20 more for ~30 total)
txn_test!(txn_rollback_empty, |db: &TestDb| {
    db.execute("CREATE TABLE t (val INTEGER)", []).unwrap();
    db.execute("BEGIN", []).unwrap();
    db.execute("ROLLBACK", []).unwrap();
    let count: i64 = db
        .query_row("SELECT COUNT(*) FROM t", [], |r| r.get(0))
        .unwrap();
    assert_eq!(count, 0);
});

txn_test!(txn_read_uncommitted_pragma, |db: &TestDb| {
    db.execute("PRAGMA read_uncommitted=1", []).unwrap();
    db.execute("CREATE TABLE t (val INTEGER)", []).unwrap();
    db.execute("INSERT INTO t VALUES (1)", []).unwrap();
    let val: i64 = db.query_row("SELECT val FROM t", [], |r| r.get(0)).unwrap();
    assert_eq!(val, 1);
});

txn_test!(txn_isolation_serializable, |db: &TestDb| {
    db.execute("CREATE TABLE t (val INTEGER)", []).unwrap();
    db.execute("BEGIN EXCLUSIVE", []).unwrap();
    db.execute("INSERT INTO t VALUES (1)", []).unwrap();
    db.execute("COMMIT", []).unwrap();
    let count: i64 = db
        .query_row("SELECT COUNT(*) FROM t", [], |r| r.get(0))
        .unwrap();
    assert_eq!(count, 1);
});

txn_test!(txn_conflict_resolution, |db: &TestDb| {
    db.execute("CREATE TABLE t (id INTEGER PRIMARY KEY, val INTEGER)", [])
        .unwrap();
    db.execute("INSERT INTO t VALUES (1, 100)", []).unwrap();
    db.execute("BEGIN", []).unwrap();
    db.execute("INSERT OR REPLACE INTO t VALUES (1, 200)", [])
        .unwrap();
    db.execute("COMMIT", []).unwrap();
    let val: i64 = db
        .query_row("SELECT val FROM t WHERE id=1", [], |r| r.get(0))
        .unwrap();
    assert_eq!(val, 200);
});

txn_test!(txn_long_running, |db: &TestDb| {
    db.execute("CREATE TABLE t (val INTEGER)", []).unwrap();
    db.execute("BEGIN", []).unwrap();
    for i in 0..100 {
        db.execute("INSERT INTO t VALUES (?)", [i]).unwrap();
    }
    db.execute("COMMIT", []).unwrap();
    let count: i64 = db
        .query_row("SELECT COUNT(*) FROM t", [], |r| r.get(0))
        .unwrap();
    assert_eq!(count, 100);
});

txn_test!(txn_read_within_write, |db: &TestDb| {
    db.execute("CREATE TABLE t (val INTEGER)", []).unwrap();
    db.execute("INSERT INTO t VALUES (1)", []).unwrap();
    db.execute("BEGIN", []).unwrap();
    db.execute("INSERT INTO t VALUES (2)", []).unwrap();
    let count: i64 = db
        .query_row("SELECT COUNT(*) FROM t", [], |r| r.get(0))
        .unwrap();
    assert_eq!(count, 2);
    db.execute("COMMIT", []).unwrap();
});

txn_test!(txn_write_after_read, |db: &TestDb| {
    db.execute("CREATE TABLE t (val INTEGER)", []).unwrap();
    db.execute("BEGIN", []).unwrap();
    let count: i64 = db
        .query_row("SELECT COUNT(*) FROM t", [], |r| r.get(0))
        .unwrap();
    assert_eq!(count, 0);
    db.execute("INSERT INTO t VALUES (1)", []).unwrap();
    db.execute("COMMIT", []).unwrap();
});

txn_test!(txn_concurrent_reads, |db: &TestDb| {
    db.execute("CREATE TABLE t (val INTEGER)", []).unwrap();
    db.execute("INSERT INTO t VALUES (1)", []).unwrap();
    let val1: i64 = db.query_row("SELECT val FROM t", [], |r| r.get(0)).unwrap();
    let val2: i64 = db.query_row("SELECT val FROM t", [], |r| r.get(0)).unwrap();
    assert_eq!(val1, val2);
});

txn_test!(txn_commit_after_error_recovery, |db: &TestDb| {
    db.execute("CREATE TABLE t (id INTEGER PRIMARY KEY)", [])
        .unwrap();
    db.execute("BEGIN", []).unwrap();
    db.execute("INSERT INTO t VALUES (1)", []).unwrap();
    let _ = db.execute("INSERT INTO t VALUES (1)", []); // Will fail
    db.execute("ROLLBACK", []).unwrap();
    db.execute("INSERT INTO t VALUES (2)", []).unwrap();
    let count: i64 = db
        .query_row("SELECT COUNT(*) FROM t", [], |r| r.get(0))
        .unwrap();
    assert_eq!(count, 1);
});

txn_test!(txn_implicit_commit_on_ddl, |db: &TestDb| {
    db.execute("CREATE TABLE t1 (val INTEGER)", []).unwrap();
    db.execute("BEGIN", []).unwrap();
    db.execute("INSERT INTO t1 VALUES (1)", []).unwrap();
    // In some databases DDL causes implicit commit, but SQLite allows DDL in transactions
    db.execute("CREATE TABLE t2 (val INTEGER)", []).unwrap();
    db.execute("COMMIT", []).unwrap();
    let count: i64 = db
        .query_row("SELECT COUNT(*) FROM t1", [], |r| r.get(0))
        .unwrap();
    assert_eq!(count, 1);
});

txn_test!(txn_large_batch_commit, |db: &TestDb| {
    db.execute("CREATE TABLE t (val INTEGER)", []).unwrap();
    db.execute("BEGIN", []).unwrap();
    for i in 0..1000 {
        db.execute("INSERT INTO t VALUES (?)", [i]).unwrap();
    }
    db.execute("COMMIT", []).unwrap();
    let count: i64 = db
        .query_row("SELECT COUNT(*) FROM t", [], |r| r.get(0))
        .unwrap();
    assert_eq!(count, 1000);
});

txn_test!(txn_multiple_savepoints_release, |db: &TestDb| {
    db.execute("CREATE TABLE t (val INTEGER)", []).unwrap();
    db.execute("BEGIN", []).unwrap();
    db.execute("SAVEPOINT sp1", []).unwrap();
    db.execute("INSERT INTO t VALUES (1)", []).unwrap();
    db.execute("SAVEPOINT sp2", []).unwrap();
    db.execute("INSERT INTO t VALUES (2)", []).unwrap();
    db.execute("RELEASE sp2", []).unwrap();
    db.execute("RELEASE sp1", []).unwrap();
    db.execute("COMMIT", []).unwrap();
    let count: i64 = db
        .query_row("SELECT COUNT(*) FROM t", [], |r| r.get(0))
        .unwrap();
    assert_eq!(count, 2);
});

txn_test!(txn_rollback_to_multiple_times, |db: &TestDb| {
    db.execute("CREATE TABLE t (val INTEGER)", []).unwrap();
    db.execute("BEGIN", []).unwrap();
    db.execute("SAVEPOINT sp1", []).unwrap();
    db.execute("INSERT INTO t VALUES (1)", []).unwrap();
    db.execute("ROLLBACK TO sp1", []).unwrap();
    db.execute("INSERT INTO t VALUES (2)", []).unwrap();
    db.execute("ROLLBACK TO sp1", []).unwrap();
    db.execute("INSERT INTO t VALUES (3)", []).unwrap();
    db.execute("COMMIT", []).unwrap();
    let count: i64 = db
        .query_row("SELECT COUNT(*) FROM t", [], |r| r.get(0))
        .unwrap();
    assert_eq!(count, 1);
});

txn_test!(txn_vacuum_outside_transaction, |db: &TestDb| {
    db.execute("CREATE TABLE t (val INTEGER)", []).unwrap();
    db.execute("INSERT INTO t VALUES (1), (2), (3)", [])
        .unwrap();
    db.execute("DELETE FROM t WHERE val=2", []).unwrap();
    db.execute("VACUUM", []).unwrap();
    let count: i64 = db
        .query_row("SELECT COUNT(*) FROM t", [], |r| r.get(0))
        .unwrap();
    assert_eq!(count, 2);
});

txn_test!(txn_analyze_within_transaction, |db: &TestDb| {
    db.execute("CREATE TABLE t (val INTEGER)", []).unwrap();
    db.execute("INSERT INTO t VALUES (1), (2), (3)", [])
        .unwrap();
    db.execute("BEGIN", []).unwrap();
    db.execute("ANALYZE", []).unwrap();
    db.execute("COMMIT", []).unwrap();
    let count: i64 = db
        .query_row("SELECT COUNT(*) FROM t", [], |r| r.get(0))
        .unwrap();
    assert_eq!(count, 3);
});

txn_test!(txn_attach_database_behavior, |db: &TestDb| {
    db.execute("CREATE TABLE t (val INTEGER)", []).unwrap();
    db.execute("INSERT INTO t VALUES (1)", []).unwrap();
    // ATTACH typically requires a file path, skip actual attach but verify base behavior
    let count: i64 = db
        .query_row("SELECT COUNT(*) FROM t", [], |r| r.get(0))
        .unwrap();
    assert_eq!(count, 1);
});

txn_test!(txn_pragma_within_transaction, |db: &TestDb| {
    db.execute("CREATE TABLE t (val INTEGER)", []).unwrap();
    db.execute("BEGIN", []).unwrap();
    db.execute("PRAGMA temp_store=MEMORY", []).unwrap();
    db.execute("INSERT INTO t VALUES (1)", []).unwrap();
    db.execute("COMMIT", []).unwrap();
    let count: i64 = db
        .query_row("SELECT COUNT(*) FROM t", [], |r| r.get(0))
        .unwrap();
    assert_eq!(count, 1);
});

txn_test!(txn_integrity_check, |db: &TestDb| {
    db.execute("CREATE TABLE t (val INTEGER)", []).unwrap();
    db.execute("INSERT INTO t VALUES (1), (2), (3)", [])
        .unwrap();
    let result: String = db
        .query_row("PRAGMA integrity_check", [], |r| r.get(0))
        .unwrap();
    assert_eq!(result, "ok");
});

txn_test!(txn_foreign_key_violation_rollback, |db: &TestDb| {
    db.execute("PRAGMA foreign_keys=ON", []).unwrap();
    db.execute("CREATE TABLE p (id INTEGER PRIMARY KEY)", [])
        .unwrap();
    db.execute(
        "CREATE TABLE c (id INTEGER, pid INTEGER REFERENCES p(id))",
        [],
    )
    .unwrap();
    db.execute("BEGIN", []).unwrap();
    db.execute("INSERT INTO p VALUES (1)", []).unwrap();
    let result = db.execute("INSERT INTO c VALUES (1, 999)", []);
    if result.is_err() {
        db.execute("ROLLBACK", []).unwrap();
    }
    let count: i64 = db
        .query_row("SELECT COUNT(*) FROM p", [], |r| r.get(0))
        .unwrap();
    assert_eq!(count, 0);
});

txn_test!(txn_check_constraint_rollback, |db: &TestDb| {
    db.execute("CREATE TABLE t (val INTEGER CHECK(val > 0))", [])
        .unwrap();
    db.execute("BEGIN", []).unwrap();
    db.execute("INSERT INTO t VALUES (5)", []).unwrap();
    let result = db.execute("INSERT INTO t VALUES (-1)", []);
    if result.is_err() {
        db.execute("ROLLBACK", []).unwrap();
    }
    let count: i64 = db
        .query_row("SELECT COUNT(*) FROM t", [], |r| r.get(0))
        .unwrap();
    assert_eq!(count, 0);
});

//
// COMPLEX QUERIES (~30 tests)
//

macro_rules! complex_test {
    ($name:ident, $test:expr) => {
        #[test]
        fn $name() {
            let db = TestDb::new().unwrap();
            $test(&db);
            db.run_work_loop();
        }
    };
}

complex_test!(complex_inner_join, |db: &TestDb| {
    db.execute("CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT)", [])
        .unwrap();
    db.execute(
        "CREATE TABLE orders (id INTEGER, user_id INTEGER, amount REAL)",
        [],
    )
    .unwrap();
    db.execute("INSERT INTO users VALUES (1, 'Alice'), (2, 'Bob')", [])
        .unwrap();
    db.execute(
        "INSERT INTO orders VALUES (1, 1, 100.0), (2, 1, 150.0), (3, 2, 200.0)",
        [],
    )
    .unwrap();
    let total: f64 = db.query_row(
        "SELECT SUM(o.amount) FROM orders o INNER JOIN users u ON o.user_id = u.id WHERE u.name='Alice'",
        [], |r| r.get(0)
    ).unwrap();
    assert!((total - 250.0).abs() < 0.01);
});

complex_test!(complex_left_join, |db: &TestDb| {
    db.execute("CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT)", [])
        .unwrap();
    db.execute("CREATE TABLE orders (id INTEGER, user_id INTEGER)", [])
        .unwrap();
    db.execute(
        "INSERT INTO users VALUES (1, 'Alice'), (2, 'Bob'), (3, 'Charlie')",
        [],
    )
    .unwrap();
    db.execute("INSERT INTO orders VALUES (1, 1), (2, 1), (3, 2)", [])
        .unwrap();
    let count: i64 = db.query_row(
        "SELECT COUNT(*) FROM users u LEFT JOIN orders o ON u.id = o.user_id WHERE o.id IS NULL",
        [], |r| r.get(0)
    ).unwrap();
    assert_eq!(count, 1); // Charlie has no orders
});

complex_test!(complex_subquery_in_select, |db: &TestDb| {
    db.execute("CREATE TABLE products (id INTEGER, price REAL)", [])
        .unwrap();
    db.execute(
        "INSERT INTO products VALUES (1, 10.0), (2, 20.0), (3, 15.0)",
        [],
    )
    .unwrap();
    let _avg: f64 = db
        .query_row("SELECT AVG(price) FROM products", [], |r| r.get(0))
        .unwrap();
    let count: i64 = db
        .query_row(
            "SELECT COUNT(*) FROM products WHERE price > (SELECT AVG(price) FROM products)",
            [],
            |r| r.get(0),
        )
        .unwrap();
    assert_eq!(count, 1); // Only price=20.0 is above avg=15.0
});

complex_test!(complex_subquery_in_from, |db: &TestDb| {
    db.execute("CREATE TABLE sales (product TEXT, amount REAL)", [])
        .unwrap();
    db.execute(
        "INSERT INTO sales VALUES ('A', 100), ('B', 200), ('A', 150), ('C', 50)",
        [],
    )
    .unwrap();
    let max_total: f64 = db.query_row(
        "SELECT MAX(total) FROM (SELECT product, SUM(amount) as total FROM sales GROUP BY product)",
        [], |r| r.get(0)
    ).unwrap();
    assert!((max_total - 250.0).abs() < 0.01); // Product A has 250 total
});

complex_test!(complex_cte, |db: &TestDb| {
    db.execute(
        "CREATE TABLE employees (id INTEGER, name TEXT, manager_id INTEGER)",
        [],
    )
    .unwrap();
    db.execute(
        "INSERT INTO employees VALUES (1, 'CEO', NULL), (2, 'VP', 1), (3, 'Engineer', 2)",
        [],
    )
    .unwrap();
    let count: i64 = db.query_row(
        "WITH managers AS (SELECT id, name FROM employees WHERE manager_id IS NULL) SELECT COUNT(*) FROM managers",
        [], |r| r.get(0)
    ).unwrap();
    assert_eq!(count, 1);
});

complex_test!(complex_recursive_cte, |db: &TestDb| {
    let sum: i64 = db.query_row(
        "WITH RECURSIVE cnt(x) AS (SELECT 1 UNION ALL SELECT x+1 FROM cnt WHERE x<5) SELECT SUM(x) FROM cnt",
        [], |r| r.get(0)
    ).unwrap();
    assert_eq!(sum, 15); // 1+2+3+4+5
});

complex_test!(complex_union, |db: &TestDb| {
    db.execute("CREATE TABLE t1 (val INTEGER)", []).unwrap();
    db.execute("CREATE TABLE t2 (val INTEGER)", []).unwrap();
    db.execute("INSERT INTO t1 VALUES (1), (2), (3)", [])
        .unwrap();
    db.execute("INSERT INTO t2 VALUES (3), (4), (5)", [])
        .unwrap();
    let count: i64 = db
        .query_row(
            "SELECT COUNT(*) FROM (SELECT val FROM t1 UNION SELECT val FROM t2)",
            [],
            |r| r.get(0),
        )
        .unwrap();
    assert_eq!(count, 5); // UNION removes duplicates
});

complex_test!(complex_union_all, |db: &TestDb| {
    db.execute("CREATE TABLE t1 (val INTEGER)", []).unwrap();
    db.execute("CREATE TABLE t2 (val INTEGER)", []).unwrap();
    db.execute("INSERT INTO t1 VALUES (1), (2)", []).unwrap();
    db.execute("INSERT INTO t2 VALUES (2), (3)", []).unwrap();
    let count: i64 = db
        .query_row(
            "SELECT COUNT(*) FROM (SELECT val FROM t1 UNION ALL SELECT val FROM t2)",
            [],
            |r| r.get(0),
        )
        .unwrap();
    assert_eq!(count, 4); // UNION ALL keeps duplicates
});

complex_test!(complex_intersect, |db: &TestDb| {
    db.execute("CREATE TABLE t1 (val INTEGER)", []).unwrap();
    db.execute("CREATE TABLE t2 (val INTEGER)", []).unwrap();
    db.execute("INSERT INTO t1 VALUES (1), (2), (3)", [])
        .unwrap();
    db.execute("INSERT INTO t2 VALUES (2), (3), (4)", [])
        .unwrap();
    let count: i64 = db
        .query_row(
            "SELECT COUNT(*) FROM (SELECT val FROM t1 INTERSECT SELECT val FROM t2)",
            [],
            |r| r.get(0),
        )
        .unwrap();
    assert_eq!(count, 2); // 2 and 3
});

complex_test!(complex_except, |db: &TestDb| {
    db.execute("CREATE TABLE t1 (val INTEGER)", []).unwrap();
    db.execute("CREATE TABLE t2 (val INTEGER)", []).unwrap();
    db.execute("INSERT INTO t1 VALUES (1), (2), (3)", [])
        .unwrap();
    db.execute("INSERT INTO t2 VALUES (2), (3), (4)", [])
        .unwrap();
    let count: i64 = db
        .query_row(
            "SELECT COUNT(*) FROM (SELECT val FROM t1 EXCEPT SELECT val FROM t2)",
            [],
            |r| r.get(0),
        )
        .unwrap();
    assert_eq!(count, 1); // Only 1
});

// Additional complex tests (20 more for ~30 total)
complex_test!(complex_three_table_join, |db: &TestDb| {
    db.execute("CREATE TABLE a (id INTEGER, val TEXT)", [])
        .unwrap();
    db.execute("CREATE TABLE b (id INTEGER, a_id INTEGER, val TEXT)", [])
        .unwrap();
    db.execute("CREATE TABLE c (id INTEGER, b_id INTEGER, val TEXT)", [])
        .unwrap();
    db.execute("INSERT INTO a VALUES (1, 'A1')", []).unwrap();
    db.execute("INSERT INTO b VALUES (1, 1, 'B1')", []).unwrap();
    db.execute("INSERT INTO c VALUES (1, 1, 'C1')", []).unwrap();
    let val: String = db
        .query_row(
            "SELECT c.val FROM a JOIN b ON a.id=b.a_id JOIN c ON b.id=c.b_id WHERE a.id=1",
            [],
            |r| r.get(0),
        )
        .unwrap();
    assert_eq!(val, "C1");
});

complex_test!(complex_cross_join, |db: &TestDb| {
    db.execute("CREATE TABLE t1 (val INTEGER)", []).unwrap();
    db.execute("CREATE TABLE t2 (val INTEGER)", []).unwrap();
    db.execute("INSERT INTO t1 VALUES (1), (2)", []).unwrap();
    db.execute("INSERT INTO t2 VALUES (10), (20)", []).unwrap();
    let count: i64 = db
        .query_row("SELECT COUNT(*) FROM t1 CROSS JOIN t2", [], |r| r.get(0))
        .unwrap();
    assert_eq!(count, 4); // 2x2
});

complex_test!(complex_self_join, |db: &TestDb| {
    db.execute(
        "CREATE TABLE employees (id INTEGER, name TEXT, manager_id INTEGER)",
        [],
    )
    .unwrap();
    db.execute(
        "INSERT INTO employees VALUES (1, 'Alice', NULL), (2, 'Bob', 1), (3, 'Charlie', 1)",
        [],
    )
    .unwrap();
    let count: i64 = db.query_row(
        "SELECT COUNT(*) FROM employees e1 JOIN employees e2 ON e1.id = e2.manager_id WHERE e1.name='Alice'",
        [], |r| r.get(0)
    ).unwrap();
    assert_eq!(count, 2); // Bob and Charlie report to Alice
});

complex_test!(complex_correlated_subquery, |db: &TestDb| {
    db.execute(
        "CREATE TABLE products (id INTEGER, category TEXT, price REAL)",
        [],
    )
    .unwrap();
    db.execute(
        "INSERT INTO products VALUES (1, 'A', 10), (2, 'A', 20), (3, 'B', 15), (4, 'B', 25)",
        [],
    )
    .unwrap();
    let count: i64 = db.query_row(
        "SELECT COUNT(*) FROM products p1 WHERE price = (SELECT MAX(price) FROM products p2 WHERE p2.category = p1.category)",
        [], |r| r.get(0)
    ).unwrap();
    assert_eq!(count, 2); // Highest in each category
});

complex_test!(complex_exists_subquery, |db: &TestDb| {
    db.execute("CREATE TABLE users (id INTEGER, name TEXT)", [])
        .unwrap();
    db.execute("CREATE TABLE orders (user_id INTEGER, amount REAL)", [])
        .unwrap();
    db.execute(
        "INSERT INTO users VALUES (1, 'Alice'), (2, 'Bob'), (3, 'Charlie')",
        [],
    )
    .unwrap();
    db.execute("INSERT INTO orders VALUES (1, 100), (2, 200)", [])
        .unwrap();
    let count: i64 = db.query_row(
        "SELECT COUNT(*) FROM users u WHERE EXISTS (SELECT 1 FROM orders o WHERE o.user_id = u.id)",
        [], |r| r.get(0)
    ).unwrap();
    assert_eq!(count, 2); // Alice and Bob have orders
});

complex_test!(complex_not_exists_subquery, |db: &TestDb| {
    db.execute("CREATE TABLE users (id INTEGER, name TEXT)", [])
        .unwrap();
    db.execute("CREATE TABLE orders (user_id INTEGER, amount REAL)", [])
        .unwrap();
    db.execute(
        "INSERT INTO users VALUES (1, 'Alice'), (2, 'Bob'), (3, 'Charlie')",
        [],
    )
    .unwrap();
    db.execute("INSERT INTO orders VALUES (1, 100)", [])
        .unwrap();
    let count: i64 = db.query_row(
        "SELECT COUNT(*) FROM users u WHERE NOT EXISTS (SELECT 1 FROM orders o WHERE o.user_id = u.id)",
        [], |r| r.get(0)
    ).unwrap();
    assert_eq!(count, 2); // Bob and Charlie have no orders
});

complex_test!(complex_in_subquery, |db: &TestDb| {
    db.execute("CREATE TABLE products (id INTEGER, price REAL)", [])
        .unwrap();
    db.execute("CREATE TABLE featured (product_id INTEGER)", [])
        .unwrap();
    db.execute("INSERT INTO products VALUES (1, 10), (2, 20), (3, 30)", [])
        .unwrap();
    db.execute("INSERT INTO featured VALUES (1), (3)", [])
        .unwrap();
    let sum: f64 = db
        .query_row(
            "SELECT SUM(price) FROM products WHERE id IN (SELECT product_id FROM featured)",
            [],
            |r| r.get(0),
        )
        .unwrap();
    assert!((sum - 40.0).abs() < 0.01);
});

complex_test!(complex_multiple_ctes, |db: &TestDb| {
    db.execute("CREATE TABLE sales (product TEXT, amount REAL)", [])
        .unwrap();
    db.execute(
        "INSERT INTO sales VALUES ('A', 100), ('B', 200), ('A', 50)",
        [],
    )
    .unwrap();
    let result: f64 = db
        .query_row(
            "WITH totals AS (SELECT product, SUM(amount) as total FROM sales GROUP BY product),
              avg_total AS (SELECT AVG(total) as avg FROM totals)
         SELECT avg FROM avg_total",
            [],
            |r| r.get(0),
        )
        .unwrap();
    assert!((result - 175.0).abs() < 0.01); // (150+200)/2
});

complex_test!(complex_window_over_partition, |db: &TestDb| {
    db.execute("CREATE TABLE sales (region TEXT, amount REAL)", [])
        .unwrap();
    db.execute(
        "INSERT INTO sales VALUES ('East', 100), ('East', 200), ('West', 150), ('West', 250)",
        [],
    )
    .unwrap();
    let sum: f64 = db
        .query_row(
            "SELECT SUM(amount) OVER (PARTITION BY region) FROM sales WHERE region='East' LIMIT 1",
            [],
            |r| r.get(0),
        )
        .unwrap();
    assert!((sum - 300.0).abs() < 0.01);
});

complex_test!(complex_case_in_join, |db: &TestDb| {
    db.execute("CREATE TABLE orders (id INTEGER, status TEXT)", [])
        .unwrap();
    db.execute(
        "INSERT INTO orders VALUES (1, 'pending'), (2, 'shipped'), (3, 'pending')",
        [],
    )
    .unwrap();
    let count: i64 = db
        .query_row(
            "SELECT COUNT(*) FROM orders WHERE CASE WHEN status='pending' THEN 1 ELSE 0 END = 1",
            [],
            |r| r.get(0),
        )
        .unwrap();
    assert_eq!(count, 2);
});

complex_test!(complex_nested_subqueries, |db: &TestDb| {
    db.execute("CREATE TABLE t (val INTEGER)", []).unwrap();
    db.execute("INSERT INTO t VALUES (1), (2), (3), (4), (5)", [])
        .unwrap();
    let result: i64 = db
        .query_row(
            "SELECT (SELECT MAX(val) FROM (SELECT val FROM t WHERE val < 5))",
            [],
            |r| r.get(0),
        )
        .unwrap();
    assert_eq!(result, 4);
});

complex_test!(complex_group_by_having_order, |db: &TestDb| {
    db.execute("CREATE TABLE sales (product TEXT, amount REAL)", [])
        .unwrap();
    db.execute(
        "INSERT INTO sales VALUES ('A', 10), ('B', 20), ('A', 15), ('C', 5), ('B', 30)",
        [],
    )
    .unwrap();
    let product: String = db.query_row(
        "SELECT product FROM sales GROUP BY product HAVING SUM(amount) > 20 ORDER BY SUM(amount) DESC LIMIT 1",
        [], |r| r.get(0)
    ).unwrap();
    assert_eq!(product, "B"); // B has total 50
});

complex_test!(complex_multi_column_order, |db: &TestDb| {
    db.execute("CREATE TABLE t (a INTEGER, b INTEGER, c INTEGER)", [])
        .unwrap();
    db.execute(
        "INSERT INTO t VALUES (1, 2, 100), (1, 1, 200), (2, 1, 300)",
        [],
    )
    .unwrap();
    let c: i64 = db
        .query_row("SELECT c FROM t ORDER BY a ASC, b DESC LIMIT 1", [], |r| {
            r.get(0)
        })
        .unwrap();
    assert_eq!(c, 100); // a=1, b=2
});

complex_test!(complex_aggregate_with_filter, |db: &TestDb| {
    db.execute("CREATE TABLE t (category TEXT, val INTEGER)", [])
        .unwrap();
    db.execute(
        "INSERT INTO t VALUES ('A', 10), ('B', 20), ('A', 30), ('B', 40)",
        [],
    )
    .unwrap();
    let sum: i64 = db
        .query_row(
            "SELECT SUM(val) FILTER (WHERE category='A') FROM t",
            [],
            |r| r.get(0),
        )
        .unwrap();
    assert_eq!(sum, 40);
});

complex_test!(complex_join_with_aggregate, |db: &TestDb| {
    db.execute(
        "CREATE TABLE orders (id INTEGER, customer_id INTEGER, amount REAL)",
        [],
    )
    .unwrap();
    db.execute("CREATE TABLE customers (id INTEGER, name TEXT)", [])
        .unwrap();
    db.execute("INSERT INTO customers VALUES (1, 'Alice'), (2, 'Bob')", [])
        .unwrap();
    db.execute(
        "INSERT INTO orders VALUES (1, 1, 100), (2, 1, 200), (3, 2, 150)",
        [],
    )
    .unwrap();
    let total: f64 = db.query_row(
        "SELECT SUM(o.amount) FROM customers c JOIN orders o ON c.id = o.customer_id WHERE c.name='Alice'",
        [], |r| r.get(0)
    ).unwrap();
    assert!((total - 300.0).abs() < 0.01);
});

complex_test!(complex_subquery_scalar, |db: &TestDb| {
    db.execute("CREATE TABLE t (val INTEGER)", []).unwrap();
    db.execute("INSERT INTO t VALUES (1), (2), (3)", [])
        .unwrap();
    let result: i64 = db
        .query_row(
            "SELECT (SELECT COUNT(*) FROM t) + (SELECT MAX(val) FROM t)",
            [],
            |r| r.get(0),
        )
        .unwrap();
    assert_eq!(result, 6); // 3 + 3
});

complex_test!(complex_lateral_join_workaround, |db: &TestDb| {
    // SQLite doesn't support LATERAL, but we can test similar pattern with subquery
    db.execute("CREATE TABLE t1 (id INTEGER, val INTEGER)", [])
        .unwrap();
    db.execute(
        "CREATE TABLE t2 (id INTEGER, t1_id INTEGER, val INTEGER)",
        [],
    )
    .unwrap();
    db.execute("INSERT INTO t1 VALUES (1, 100), (2, 200)", [])
        .unwrap();
    db.execute(
        "INSERT INTO t2 VALUES (1, 1, 10), (2, 1, 20), (3, 2, 30)",
        [],
    )
    .unwrap();
    let sum: i64 = db
        .query_row(
            "SELECT SUM(t2.val) FROM t1 JOIN t2 ON t1.id = t2.t1_id WHERE t1.id=1",
            [],
            |r| r.get(0),
        )
        .unwrap();
    assert_eq!(sum, 30);
});

complex_test!(complex_multiple_aggregates, |db: &TestDb| {
    db.execute("CREATE TABLE t (val INTEGER)", []).unwrap();
    db.execute("INSERT INTO t VALUES (10), (20), (30), (40), (50)", [])
        .unwrap();
    let (count, sum, avg, min, max): (i64, i64, f64, i64, i64) = db
        .query_row(
            "SELECT COUNT(*), SUM(val), AVG(val), MIN(val), MAX(val) FROM t",
            [],
            |r| Ok((r.get(0)?, r.get(1)?, r.get(2)?, r.get(3)?, r.get(4)?)),
        )
        .unwrap();
    assert_eq!((count, sum, min, max), (5, 150, 10, 50));
    assert!((avg - 30.0).abs() < 0.01);
});

complex_test!(complex_string_aggregation, |db: &TestDb| {
    db.execute("CREATE TABLE t (val TEXT)", []).unwrap();
    db.execute("INSERT INTO t VALUES ('a'), ('b'), ('c')", [])
        .unwrap();
    let result: String = db
        .query_row("SELECT GROUP_CONCAT(val, ',') FROM t", [], |r| r.get(0))
        .unwrap();
    assert_eq!(result, "a,b,c");
});

complex_test!(complex_date_functions, |db: &TestDb| {
    db.execute("CREATE TABLE events (id INTEGER, ts TEXT)", [])
        .unwrap();
    db.execute(
        "INSERT INTO events VALUES (1, '2024-01-15 10:30:00'), (2, '2024-01-16 11:45:00')",
        [],
    )
    .unwrap();
    let date: String = db
        .query_row("SELECT DATE(ts) FROM events WHERE id=1", [], |r| r.get(0))
        .unwrap();
    assert_eq!(date, "2024-01-15");
});

//
// WAL MODE TESTS (~15 tests)
//

macro_rules! wal_test {
    ($name:ident, $test:expr) => {
        #[test]
        fn $name() {
            let db = TestDb::new().unwrap();
            $test(&db);
            db.run_work_loop();
        }
    };
}

wal_test!(wal_enable, |db: &TestDb| {
    let mode: String = db
        .query_row("PRAGMA journal_mode", [], |r| r.get(0))
        .unwrap();
    // In-memory databases use MEMORY journal mode, file/azure use WAL
    let mode_upper = mode.to_uppercase();
    assert!(mode_upper == "WAL" || mode_upper == "MEMORY");
});

wal_test!(wal_checkpoint_passive, |db: &TestDb| {
    let mode = PerfMode::from_env();
    if mode == PerfMode::Memory {
        // In-memory databases don't support WAL checkpoints
        return;
    }
    db.execute("CREATE TABLE t (val INTEGER)", []).unwrap();
    db.execute("INSERT INTO t VALUES (1), (2), (3)", [])
        .unwrap();
    let _: (i32, i32, i32) = db
        .query_row("PRAGMA wal_checkpoint(PASSIVE)", [], |r| {
            Ok((r.get(0)?, r.get(1)?, r.get(2)?))
        })
        .unwrap();
    let count: i64 = db
        .query_row("SELECT COUNT(*) FROM t", [], |r| r.get(0))
        .unwrap();
    assert_eq!(count, 3);
});

wal_test!(wal_checkpoint_full, |db: &TestDb| {
    let mode = PerfMode::from_env();
    if mode == PerfMode::Memory {
        return;
    }
    db.execute("CREATE TABLE t (val INTEGER)", []).unwrap();
    for i in 0..100 {
        db.execute("INSERT INTO t VALUES (?)", [i]).unwrap();
    }
    let _: (i32, i32, i32) = db
        .query_row("PRAGMA wal_checkpoint(FULL)", [], |r| {
            Ok((r.get(0)?, r.get(1)?, r.get(2)?))
        })
        .unwrap();
    let count: i64 = db
        .query_row("SELECT COUNT(*) FROM t", [], |r| r.get(0))
        .unwrap();
    assert_eq!(count, 100);
});

wal_test!(wal_checkpoint_restart, |db: &TestDb| {
    let mode = PerfMode::from_env();
    if mode == PerfMode::Memory {
        return;
    }
    db.execute("CREATE TABLE t (val INTEGER)", []).unwrap();
    db.execute("INSERT INTO t VALUES (1)", []).unwrap();
    let _: (i32, i32, i32) = db
        .query_row("PRAGMA wal_checkpoint(RESTART)", [], |r| {
            Ok((r.get(0)?, r.get(1)?, r.get(2)?))
        })
        .unwrap();
    let count: i64 = db
        .query_row("SELECT COUNT(*) FROM t", [], |r| r.get(0))
        .unwrap();
    assert_eq!(count, 1);
});

wal_test!(wal_checkpoint_truncate, |db: &TestDb| {
    let mode = PerfMode::from_env();
    if mode == PerfMode::Memory {
        return;
    }
    db.execute("CREATE TABLE t (val INTEGER)", []).unwrap();
    db.execute("INSERT INTO t VALUES (1)", []).unwrap();
    let _: (i32, i32, i32) = db
        .query_row("PRAGMA wal_checkpoint(TRUNCATE)", [], |r| {
            Ok((r.get(0)?, r.get(1)?, r.get(2)?))
        })
        .unwrap();
    let count: i64 = db
        .query_row("SELECT COUNT(*) FROM t", [], |r| r.get(0))
        .unwrap();
    assert_eq!(count, 1);
});

wal_test!(wal_write_then_read, |db: &TestDb| {
    db.execute("CREATE TABLE t (val INTEGER)", []).unwrap();
    db.execute("INSERT INTO t VALUES (42)", []).unwrap();
    let val: i64 = db.query_row("SELECT val FROM t", [], |r| r.get(0)).unwrap();
    assert_eq!(val, 42);
});

wal_test!(wal_multiple_writes, |db: &TestDb| {
    db.execute("CREATE TABLE t (val INTEGER)", []).unwrap();
    for i in 0..50 {
        db.execute("INSERT INTO t VALUES (?)", [i]).unwrap();
    }
    let count: i64 = db
        .query_row("SELECT COUNT(*) FROM t", [], |r| r.get(0))
        .unwrap();
    assert_eq!(count, 50);
});

wal_test!(wal_autocheckpoint, |db: &TestDb| {
    let threshold: i64 = db
        .query_row("PRAGMA wal_autocheckpoint", [], |r| r.get(0))
        .unwrap();
    assert!(threshold > 0);
});

wal_test!(wal_set_autocheckpoint, |db: &TestDb| {
    let mode = PerfMode::from_env();
    if mode == PerfMode::Memory {
        return;
    }
    let _: i64 = db
        .query_row("PRAGMA wal_autocheckpoint=5000", [], |r| r.get(0))
        .unwrap();
    let threshold: i64 = db
        .query_row("PRAGMA wal_autocheckpoint", [], |r| r.get(0))
        .unwrap();
    assert_eq!(threshold, 5000);
});

wal_test!(wal_read_after_write, |db: &TestDb| {
    db.execute("CREATE TABLE t (id INTEGER PRIMARY KEY, val TEXT)", [])
        .unwrap();
    db.execute("INSERT INTO t VALUES (1, 'initial')", [])
        .unwrap();
    db.execute("UPDATE t SET val='updated' WHERE id=1", [])
        .unwrap();
    let val: String = db
        .query_row("SELECT val FROM t WHERE id=1", [], |r| r.get(0))
        .unwrap();
    assert_eq!(val, "updated");
});

// Additional WAL tests (5 more for ~15 total)
wal_test!(wal_transaction_write_read, |db: &TestDb| {
    db.execute("CREATE TABLE t (val INTEGER)", []).unwrap();
    db.execute("BEGIN", []).unwrap();
    db.execute("INSERT INTO t VALUES (1), (2), (3)", [])
        .unwrap();
    let count: i64 = db
        .query_row("SELECT COUNT(*) FROM t", [], |r| r.get(0))
        .unwrap();
    assert_eq!(count, 3);
    db.execute("COMMIT", []).unwrap();
});

wal_test!(wal_large_transaction, |db: &TestDb| {
    let mode = PerfMode::from_env();
    db.execute("CREATE TABLE t (val INTEGER)", []).unwrap();
    db.execute("BEGIN", []).unwrap();
    for i in 0..500 {
        db.execute("INSERT INTO t VALUES (?)", [i]).unwrap();
    }
    db.execute("COMMIT", []).unwrap();
    if mode != PerfMode::Memory {
        let _: (i32, i32, i32) = db
            .query_row("PRAGMA wal_checkpoint(FULL)", [], |r| {
                Ok((r.get(0)?, r.get(1)?, r.get(2)?))
            })
            .unwrap();
    }
    let count: i64 = db
        .query_row("SELECT COUNT(*) FROM t", [], |r| r.get(0))
        .unwrap();
    assert_eq!(count, 500);
});

wal_test!(wal_synchronous_normal, |db: &TestDb| {
    let sync: i64 = db
        .query_row("PRAGMA synchronous", [], |r| r.get(0))
        .unwrap();
    // 0=OFF, 1=NORMAL, 2=FULL, 3=EXTRA
    assert!((0..=3).contains(&sync));
});

wal_test!(wal_busy_timeout, |db: &TestDb| {
    let timeout: i64 = db
        .query_row("PRAGMA busy_timeout", [], |r| r.get(0))
        .unwrap();
    assert_eq!(timeout, 60000);
});

wal_test!(wal_pragma_query, |db: &TestDb| {
    db.execute("CREATE TABLE t (val INTEGER)", []).unwrap();
    db.execute("INSERT INTO t VALUES (1)", []).unwrap();
    let journal: String = db
        .query_row("PRAGMA journal_mode", [], |r| r.get(0))
        .unwrap();
    let sync: i64 = db
        .query_row("PRAGMA synchronous", [], |r| r.get(0))
        .unwrap();
    let journal_upper = journal.to_uppercase();
    assert!(journal_upper == "WAL" || journal_upper == "MEMORY");
    assert!(sync >= 0); // 0=OFF, 1=NORMAL, 2=FULL, 3=EXTRA
});

//
// PROVIDER-STYLE OPERATIONS (~17 tests)
//
// These mimic work-item queue patterns: create, claim (peek-lock), update, complete, abandon
//

macro_rules! provider_test {
    ($name:ident, $test:expr) => {
        #[test]
        fn $name() {
            let db = TestDb::new().unwrap();
            $test(&db);
            db.run_work_loop();
        }
    };
}

provider_test!(provider_create_item, |db: &TestDb| {
    db.execute(
        "CREATE TABLE work_items (id TEXT PRIMARY KEY, status TEXT, data TEXT)",
        [],
    )
    .unwrap();
    db.execute(
        "INSERT INTO work_items VALUES ('item-1', 'available', 'payload')",
        [],
    )
    .unwrap();
    let count: i64 = db
        .query_row(
            "SELECT COUNT(*) FROM work_items WHERE status='available'",
            [],
            |r| r.get(0),
        )
        .unwrap();
    assert_eq!(count, 1);
});

provider_test!(provider_peek_lock, |db: &TestDb| {
    db.execute(
        "CREATE TABLE work_items (id TEXT PRIMARY KEY, status TEXT, locked_until TEXT)",
        [],
    )
    .unwrap();
    db.execute(
        "INSERT INTO work_items VALUES ('item-1', 'available', NULL)",
        [],
    )
    .unwrap();
    db.execute("UPDATE work_items SET status='locked', locked_until=DATETIME('now', '+5 minutes') WHERE id='item-1' AND status='available'", []).unwrap();
    let status: String = db
        .query_row("SELECT status FROM work_items WHERE id='item-1'", [], |r| {
            r.get(0)
        })
        .unwrap();
    assert_eq!(status, "locked");
});

provider_test!(provider_complete_item, |db: &TestDb| {
    db.execute(
        "CREATE TABLE work_items (id TEXT PRIMARY KEY, status TEXT)",
        [],
    )
    .unwrap();
    db.execute("INSERT INTO work_items VALUES ('item-1', 'locked')", [])
        .unwrap();
    db.execute(
        "UPDATE work_items SET status='completed' WHERE id='item-1' AND status='locked'",
        [],
    )
    .unwrap();
    let status: String = db
        .query_row("SELECT status FROM work_items WHERE id='item-1'", [], |r| {
            r.get(0)
        })
        .unwrap();
    assert_eq!(status, "completed");
});

provider_test!(provider_abandon_item, |db: &TestDb| {
    db.execute(
        "CREATE TABLE work_items (id TEXT PRIMARY KEY, status TEXT)",
        [],
    )
    .unwrap();
    db.execute("INSERT INTO work_items VALUES ('item-1', 'locked')", [])
        .unwrap();
    db.execute(
        "UPDATE work_items SET status='available' WHERE id='item-1' AND status='locked'",
        [],
    )
    .unwrap();
    let status: String = db
        .query_row("SELECT status FROM work_items WHERE id='item-1'", [], |r| {
            r.get(0)
        })
        .unwrap();
    assert_eq!(status, "available");
});

provider_test!(provider_delete_item, |db: &TestDb| {
    db.execute(
        "CREATE TABLE work_items (id TEXT PRIMARY KEY, status TEXT)",
        [],
    )
    .unwrap();
    db.execute("INSERT INTO work_items VALUES ('item-1', 'completed')", [])
        .unwrap();
    db.execute("DELETE FROM work_items WHERE id='item-1'", [])
        .unwrap();
    let count: i64 = db
        .query_row("SELECT COUNT(*) FROM work_items", [], |r| r.get(0))
        .unwrap();
    assert_eq!(count, 0);
});

provider_test!(provider_list_available, |db: &TestDb| {
    db.execute(
        "CREATE TABLE work_items (id TEXT PRIMARY KEY, status TEXT)",
        [],
    )
    .unwrap();
    db.execute("INSERT INTO work_items VALUES ('item-1', 'available'), ('item-2', 'locked'), ('item-3', 'available')", []).unwrap();
    let count: i64 = db
        .query_row(
            "SELECT COUNT(*) FROM work_items WHERE status='available'",
            [],
            |r| r.get(0),
        )
        .unwrap();
    assert_eq!(count, 2);
});

provider_test!(provider_update_data, |db: &TestDb| {
    db.execute(
        "CREATE TABLE work_items (id TEXT PRIMARY KEY, data TEXT)",
        [],
    )
    .unwrap();
    db.execute("INSERT INTO work_items VALUES ('item-1', 'old')", [])
        .unwrap();
    db.execute("UPDATE work_items SET data='new' WHERE id='item-1'", [])
        .unwrap();
    let data: String = db
        .query_row("SELECT data FROM work_items WHERE id='item-1'", [], |r| {
            r.get(0)
        })
        .unwrap();
    assert_eq!(data, "new");
});

provider_test!(provider_expire_locks, |db: &TestDb| {
    db.execute(
        "CREATE TABLE work_items (id TEXT, status TEXT, locked_until TEXT)",
        [],
    )
    .unwrap();
    db.execute("INSERT INTO work_items VALUES ('item-1', 'locked', '2020-01-01'), ('item-2', 'locked', '2099-12-31')", []).unwrap();
    db.execute("UPDATE work_items SET status='available' WHERE status='locked' AND locked_until < DATETIME('now')", []).unwrap();
    let count: i64 = db
        .query_row(
            "SELECT COUNT(*) FROM work_items WHERE status='available'",
            [],
            |r| r.get(0),
        )
        .unwrap();
    assert_eq!(count, 1); // item-1 expired
});

provider_test!(provider_batch_create, |db: &TestDb| {
    db.execute(
        "CREATE TABLE work_items (id TEXT PRIMARY KEY, status TEXT)",
        [],
    )
    .unwrap();
    db.execute("BEGIN", []).unwrap();
    for i in 0..20 {
        db.execute(
            "INSERT INTO work_items VALUES (?, 'available')",
            [format!("item-{}", i)],
        )
        .unwrap();
    }
    db.execute("COMMIT", []).unwrap();
    let count: i64 = db
        .query_row("SELECT COUNT(*) FROM work_items", [], |r| r.get(0))
        .unwrap();
    assert_eq!(count, 20);
});

provider_test!(provider_claim_oldest, |db: &TestDb| {
    db.execute(
        "CREATE TABLE work_items (id TEXT PRIMARY KEY, status TEXT, created_at TEXT)",
        [],
    )
    .unwrap();
    db.execute("INSERT INTO work_items VALUES ('item-1', 'available', '2024-01-01'), ('item-2', 'available', '2024-01-02')", []).unwrap();
    db.execute("UPDATE work_items SET status='locked' WHERE id=(SELECT id FROM work_items WHERE status='available' ORDER BY created_at LIMIT 1)", []).unwrap();
    let id: String = db
        .query_row("SELECT id FROM work_items WHERE status='locked'", [], |r| {
            r.get(0)
        })
        .unwrap();
    assert_eq!(id, "item-1");
});

// Additional provider tests (7 more for ~17 total)
provider_test!(provider_retry_count, |db: &TestDb| {
    db.execute(
        "CREATE TABLE work_items (id TEXT PRIMARY KEY, retry_count INTEGER)",
        [],
    )
    .unwrap();
    db.execute("INSERT INTO work_items VALUES ('item-1', 0)", [])
        .unwrap();
    db.execute(
        "UPDATE work_items SET retry_count=retry_count+1 WHERE id='item-1'",
        [],
    )
    .unwrap();
    let count: i64 = db
        .query_row(
            "SELECT retry_count FROM work_items WHERE id='item-1'",
            [],
            |r| r.get(0),
        )
        .unwrap();
    assert_eq!(count, 1);
});

provider_test!(provider_max_retries, |db: &TestDb| {
    db.execute(
        "CREATE TABLE work_items (id TEXT, status TEXT, retry_count INTEGER)",
        [],
    )
    .unwrap();
    db.execute(
        "INSERT INTO work_items VALUES ('item-1', 'failed', 5), ('item-2', 'failed', 2)",
        [],
    )
    .unwrap();
    let count: i64 = db
        .query_row(
            "SELECT COUNT(*) FROM work_items WHERE retry_count >= 3",
            [],
            |r| r.get(0),
        )
        .unwrap();
    assert_eq!(count, 1);
});

provider_test!(provider_priority_queue, |db: &TestDb| {
    db.execute(
        "CREATE TABLE work_items (id TEXT, priority INTEGER, status TEXT)",
        [],
    )
    .unwrap();
    db.execute("INSERT INTO work_items VALUES ('item-1', 5, 'available'), ('item-2', 10, 'available'), ('item-3', 1, 'available')", []).unwrap();
    let id: String = db
        .query_row(
            "SELECT id FROM work_items WHERE status='available' ORDER BY priority DESC LIMIT 1",
            [],
            |r| r.get(0),
        )
        .unwrap();
    assert_eq!(id, "item-2");
});

provider_test!(provider_bulk_complete, |db: &TestDb| {
    db.execute("CREATE TABLE work_items (id TEXT, status TEXT)", [])
        .unwrap();
    for i in 0..10 {
        db.execute(
            "INSERT INTO work_items VALUES (?, 'locked')",
            [format!("item-{}", i)],
        )
        .unwrap();
    }
    db.execute(
        "UPDATE work_items SET status='completed' WHERE status='locked'",
        [],
    )
    .unwrap();
    let count: i64 = db
        .query_row(
            "SELECT COUNT(*) FROM work_items WHERE status='completed'",
            [],
            |r| r.get(0),
        )
        .unwrap();
    assert_eq!(count, 10);
});

provider_test!(provider_message_properties, |db: &TestDb| {
    db.execute(
        "CREATE TABLE messages (id TEXT PRIMARY KEY, body TEXT, properties TEXT)",
        [],
    )
    .unwrap();
    db.execute(
        r#"INSERT INTO messages VALUES ('msg-1', 'payload', '{"type":"order","priority":1}')"#,
        [],
    )
    .unwrap();
    let props: String = db
        .query_row(
            "SELECT properties FROM messages WHERE id='msg-1'",
            [],
            |r| r.get(0),
        )
        .unwrap();
    assert!(props.contains("order"));
});

provider_test!(provider_dequeue_count, |db: &TestDb| {
    db.execute("CREATE TABLE messages (id TEXT, dequeue_count INTEGER)", [])
        .unwrap();
    db.execute("INSERT INTO messages VALUES ('msg-1', 0)", [])
        .unwrap();
    db.execute(
        "UPDATE messages SET dequeue_count=dequeue_count+1 WHERE id='msg-1'",
        [],
    )
    .unwrap();
    db.execute(
        "UPDATE messages SET dequeue_count=dequeue_count+1 WHERE id='msg-1'",
        [],
    )
    .unwrap();
    let count: i64 = db
        .query_row(
            "SELECT dequeue_count FROM messages WHERE id='msg-1'",
            [],
            |r| r.get(0),
        )
        .unwrap();
    assert_eq!(count, 2);
});

provider_test!(provider_visibility_timeout, |db: &TestDb| {
    db.execute(
        "CREATE TABLE messages (id TEXT, status TEXT, visible_at TEXT)",
        [],
    )
    .unwrap();
    db.execute(
        "INSERT INTO messages VALUES ('msg-1', 'invisible', DATETIME('now', '+5 minutes'))",
        [],
    )
    .unwrap();
    db.execute(
        "INSERT INTO messages VALUES ('msg-2', 'invisible', '2020-01-01')",
        [],
    )
    .unwrap();
    let count: i64 = db
        .query_row(
            "SELECT COUNT(*) FROM messages WHERE visible_at <= DATETIME('now')",
            [],
            |r| r.get(0),
        )
        .unwrap();
    assert_eq!(count, 1); // msg-2 is visible
});
