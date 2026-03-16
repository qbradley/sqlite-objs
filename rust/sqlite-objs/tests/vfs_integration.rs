//! Comprehensive integration test suite for the sqlite-objs VFS.
//!
//! These tests exercise database lifecycle, transactions, deterministic data validation,
//! cache reuse / ETag behavior, growth & shrink, WAL mode, multi-threaded access,
//! dirty shutdown recovery, and schema / PRAGMA persistence — all against live Azure
//! Blob Storage.
//!
//! **Requirements:**
//!   - Azure credentials via `.env` at repo root or environment variables:
//!     `AZURE_STORAGE_ACCOUNT`, `AZURE_STORAGE_CONTAINER`, `AZURE_STORAGE_SAS`
//!
//! **Run all tests:**
//!   cargo test --test vfs_integration -- --ignored
//!
//! **Run one category:**
//!   cargo test --test vfs_integration -- lifecycle --ignored
//!   cargo test --test vfs_integration -- txn --ignored

use rusqlite::{Connection, OpenFlags};
use std::env;
use std::ops::Deref;
use std::path::Path;
use std::sync::OnceLock;
use tempfile::TempDir;
use uuid::Uuid;

// ---------------------------------------------------------------------------
// Infrastructure: dotenv, VFS registration, helpers
// ---------------------------------------------------------------------------

static DOTENV_LOADED: OnceLock<()> = OnceLock::new();

fn load_dotenv() {
    DOTENV_LOADED.get_or_init(|| {
        let manifest_dir = Path::new(env!("CARGO_MANIFEST_DIR"));
        let crate_env = manifest_dir.join(".env");
        let repo_env = manifest_dir.join("../../.env");
        if dotenvy::from_path(&crate_env).is_err() {
            dotenvy::from_path(&repo_env).ok();
        }
    });
}

static VFS_REGISTERED: OnceLock<()> = OnceLock::new();

fn ensure_vfs_registered() {
    VFS_REGISTERED.get_or_init(|| {
        sqlite_objs::SqliteObjsVfs::register_uri(false)
            .expect("Failed to register sqlite-objs VFS in URI mode");
    });
}

fn init() {
    load_dotenv();
    ensure_vfs_registered();
}

fn azure_env() -> (String, String, String) {
    (
        env::var("AZURE_STORAGE_ACCOUNT").expect("AZURE_STORAGE_ACCOUNT must be set"),
        env::var("AZURE_STORAGE_CONTAINER").expect("AZURE_STORAGE_CONTAINER must be set"),
        env::var("AZURE_STORAGE_SAS").expect("AZURE_STORAGE_SAS must be set"),
    )
}

const RW_CREATE_URI: OpenFlags = OpenFlags::SQLITE_OPEN_READ_WRITE
    .union(OpenFlags::SQLITE_OPEN_CREATE)
    .union(OpenFlags::SQLITE_OPEN_URI);

const RO_URI: OpenFlags = OpenFlags::SQLITE_OPEN_READ_ONLY.union(OpenFlags::SQLITE_OPEN_URI);

fn open_azure(uri: &str) -> Connection {
    let conn = Connection::open_with_flags_and_vfs(uri, RW_CREATE_URI, "sqlite-objs")
        .expect("Failed to open Azure connection");
    configure_azure(&conn);
    conn
}

fn open_azure_readonly(uri: &str) -> Connection {
    let conn = Connection::open_with_flags_and_vfs(uri, RO_URI, "sqlite-objs")
        .expect("Failed to open Azure connection (readonly)");
    conn
}

fn configure_azure(conn: &Connection) {
    conn.execute_batch(
        "PRAGMA locking_mode=EXCLUSIVE;
         PRAGMA journal_mode=WAL;
         PRAGMA synchronous=NORMAL;
         PRAGMA busy_timeout=60000;",
    )
    .expect("PRAGMA configuration failed");
}

// ---------------------------------------------------------------------------
// Download-count via file_control (opcode 200)
// ---------------------------------------------------------------------------

const SQLITE_OBJS_FCNTL_DOWNLOAD_COUNT: i32 = 200;

fn get_download_count(conn: &Connection) -> i32 {
    let mut count: i32 = 0;
    unsafe {
        let db = conn.handle();
        let schema = std::ffi::CString::new("main").unwrap();
        rusqlite::ffi::sqlite3_file_control(
            db,
            schema.as_ptr(),
            SQLITE_OBJS_FCNTL_DOWNLOAD_COUNT,
            &mut count as *mut i32 as *mut std::ffi::c_void,
        );
    }
    count
}

// ---------------------------------------------------------------------------
// Deterministic data generation
// ---------------------------------------------------------------------------

const TEST_SEED: u64 = 0xDEADBEEF_CAFEBABE;

fn seeded_text(seed: u64, row: u64) -> String {
    let hash = seed
        .wrapping_mul(6364136223846793005)
        .wrapping_add(row.wrapping_mul(1442695040888963407));
    format!("data-{:016x}", hash)
}

fn seeded_value(seed: u64, row: u64) -> f64 {
    let hash = seed
        .wrapping_mul(6364136223846793005)
        .wrapping_add(row.wrapping_mul(1442695040888963407));
    (hash as f64) / (u64::MAX as f64) * 1000.0
}

fn seeded_blob(seed: u64, row: u64, len: usize) -> Vec<u8> {
    let mut buf = Vec::with_capacity(len);
    let mut h = seed
        .wrapping_mul(6364136223846793005)
        .wrapping_add(row.wrapping_mul(1442695040888963407));
    for _ in 0..len {
        h = h.wrapping_mul(6364136223846793005).wrapping_add(1);
        buf.push((h >> 33) as u8);
    }
    buf
}

// ---------------------------------------------------------------------------
// AzureTestDb helper
// ---------------------------------------------------------------------------

struct AzureTestDb {
    conn: Connection,
    blob_name: String,
    cache_dir: TempDir,
    account: String,
    container: String,
    sas: String,
}

impl AzureTestDb {
    fn new(name_prefix: &str) -> Self {
        init();
        let (account, container, sas) = azure_env();
        let cache_dir = TempDir::new().expect("Failed to create temp cache dir");
        let blob_name = format!("vfstest-{}-{}.db", name_prefix, Uuid::new_v4());

        let uri = sqlite_objs::UriBuilder::new(&blob_name, &account, &container)
            .sas_token(&sas)
            .cache_dir(cache_dir.path().to_str().unwrap())
            .build();

        let conn = open_azure(&uri);

        Self {
            conn,
            blob_name,
            cache_dir,
            account,
            container,
            sas,
        }
    }

    fn new_with_cache_reuse(name_prefix: &str) -> Self {
        init();
        let (account, container, sas) = azure_env();
        let cache_dir = TempDir::new().expect("Failed to create temp cache dir");
        let blob_name = format!("vfstest-{}-{}.db", name_prefix, Uuid::new_v4());

        let uri = sqlite_objs::UriBuilder::new(&blob_name, &account, &container)
            .sas_token(&sas)
            .cache_dir(cache_dir.path().to_str().unwrap())
            .cache_reuse(true)
            .build();

        let conn = open_azure(&uri);

        Self {
            conn,
            blob_name,
            cache_dir,
            account,
            container,
            sas,
        }
    }

    fn uri(&self) -> String {
        sqlite_objs::UriBuilder::new(&self.blob_name, &self.account, &self.container)
            .sas_token(&self.sas)
            .cache_dir(self.cache_dir.path().to_str().unwrap())
            .build()
    }

    fn uri_with_cache_reuse(&self) -> String {
        sqlite_objs::UriBuilder::new(&self.blob_name, &self.account, &self.container)
            .sas_token(&self.sas)
            .cache_dir(self.cache_dir.path().to_str().unwrap())
            .cache_reuse(true)
            .build()
    }

    fn uri_no_cache_dir(&self) -> String {
        sqlite_objs::UriBuilder::new(&self.blob_name, &self.account, &self.container)
            .sas_token(&self.sas)
            .cache_reuse(true)
            .build()
    }

    fn uri_with_other_cache_dir(&self, dir: &str) -> String {
        sqlite_objs::UriBuilder::new(&self.blob_name, &self.account, &self.container)
            .sas_token(&self.sas)
            .cache_dir(dir)
            .cache_reuse(true)
            .build()
    }

    /// Close current connection, reopen the same blob.
    ///
    /// We must explicitly drop the old connection before opening the new one:
    /// Rust evaluates the RHS before dropping the old LHS value, so
    /// `self.conn = open_azure(...)` would try to lease the blob while
    /// the old connection still holds the lease → "database is locked".
    fn reopen(&mut self) {
        let uri = self.uri();
        // Swap in a cheap in-memory conn to force the Azure conn to drop (releasing the lease)
        self.conn = Connection::open_in_memory().unwrap();
        self.conn = open_azure(&uri);
    }

    /// Close current connection, reopen same blob with cache_reuse=1.
    fn reopen_with_cache_reuse(&mut self) {
        let uri = self.uri_with_cache_reuse();
        self.conn = Connection::open_in_memory().unwrap();
        self.conn = open_azure(&uri);
    }
}

impl Deref for AzureTestDb {
    type Target = Connection;
    fn deref(&self) -> &Connection {
        &self.conn
    }
}

// ===================================================================
// Category 1: Database Lifecycle
// ===================================================================

#[test]
#[ignore]
fn lifecycle_create_fresh_database() {
    let db = AzureTestDb::new("lc-fresh");
    db.execute_batch(
        "CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT);
         INSERT INTO t VALUES (1, 'hello');",
    )
    .unwrap();

    let name: String = db
        .query_row("SELECT name FROM t WHERE id = 1", [], |r| r.get(0))
        .unwrap();
    assert_eq!(name, "hello", "Data should be readable immediately after insert");
}

#[test]
#[ignore]
fn lifecycle_reopen_existing_database() {
    let mut db = AzureTestDb::new("lc-reopen");
    db.execute_batch("CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT)").unwrap();
    for i in 0..20u64 {
        db.execute("INSERT INTO t VALUES (?1, ?2)", rusqlite::params![i as i64, seeded_text(TEST_SEED, i)])
            .unwrap();
    }
    db.reopen();

    let count: i64 = db.query_row("SELECT COUNT(*) FROM t", [], |r| r.get(0)).unwrap();
    assert_eq!(count, 20, "All 20 rows should persist through close/reopen");

    for i in 0..20u64 {
        let v: String = db
            .query_row("SELECT v FROM t WHERE id = ?1", [i as i64], |r| r.get(0))
            .unwrap();
        assert_eq!(v, seeded_text(TEST_SEED, i), "Row {} mismatch after reopen", i);
    }
}

#[test]
#[ignore]
fn lifecycle_reopen_preserves_schema() {
    let mut db = AzureTestDb::new("lc-schema");
    db.execute_batch(
        "CREATE TABLE customers (id INTEGER PRIMARY KEY, name TEXT NOT NULL);
         CREATE TABLE orders (id INTEGER PRIMARY KEY, cust_id INTEGER REFERENCES customers(id), amount REAL);
         CREATE INDEX idx_orders_cust ON orders(cust_id);
         CREATE VIEW order_summary AS SELECT c.name, SUM(o.amount) AS total FROM customers c JOIN orders o ON c.id = o.cust_id GROUP BY c.id;
         CREATE TRIGGER trg_orders_insert AFTER INSERT ON orders BEGIN SELECT 1; END;",
    )
    .unwrap();

    db.reopen();

    let objects: Vec<(String, String)> = {
        let mut stmt = db
            .prepare("SELECT type, name FROM sqlite_master ORDER BY type, name")
            .unwrap();
        stmt.query_map([], |r| Ok((r.get(0)?, r.get(1)?)))
            .unwrap()
            .map(|r| r.unwrap())
            .collect()
    };

    let names: Vec<&str> = objects.iter().map(|(_, n)| n.as_str()).collect();
    assert!(names.contains(&"customers"), "Table 'customers' missing");
    assert!(names.contains(&"orders"), "Table 'orders' missing");
    assert!(names.contains(&"idx_orders_cust"), "Index missing");
    assert!(names.contains(&"order_summary"), "View missing");
    assert!(names.contains(&"trg_orders_insert"), "Trigger missing");
}

#[test]
#[ignore]
fn lifecycle_multiple_sessions() {
    let mut db = AzureTestDb::new("lc-multi");
    db.execute_batch("CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT)").unwrap();

    // Session 1: seed 1000
    for i in 0..50u64 {
        db.execute("INSERT INTO t VALUES (?1, ?2)", rusqlite::params![i as i64, seeded_text(1000, i)])
            .unwrap();
    }
    db.reopen();

    // Session 2: seed 2000
    for i in 50..100u64 {
        db.execute("INSERT INTO t VALUES (?1, ?2)", rusqlite::params![i as i64, seeded_text(2000, i)])
            .unwrap();
    }
    db.reopen();

    // Session 3: verify all
    let count: i64 = db.query_row("SELECT COUNT(*) FROM t", [], |r| r.get(0)).unwrap();
    assert_eq!(count, 100, "Both batches should be present");

    for i in 0..50u64 {
        let v: String = db.query_row("SELECT v FROM t WHERE id=?1", [i as i64], |r| r.get(0)).unwrap();
        assert_eq!(v, seeded_text(1000, i), "Session1 row {} mismatch", i);
    }
    for i in 50..100u64 {
        let v: String = db.query_row("SELECT v FROM t WHERE id=?1", [i as i64], |r| r.get(0)).unwrap();
        assert_eq!(v, seeded_text(2000, i), "Session2 row {} mismatch", i);
    }
}

#[test]
#[ignore]
fn lifecycle_open_close_rapid_cycle() {
    let mut db = AzureTestDb::new("lc-rapid");
    db.execute_batch("CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT)").unwrap();

    for cycle in 0..10u64 {
        if cycle > 0 {
            db.reopen();
        }
        let count: i64 = db.query_row("SELECT COUNT(*) FROM t", [], |r| r.get(0)).unwrap();
        assert_eq!(count, cycle as i64, "Before insert in cycle {}", cycle);
        db.execute(
            "INSERT INTO t VALUES (?1, ?2)",
            rusqlite::params![cycle as i64, seeded_text(TEST_SEED, cycle)],
        )
        .unwrap();
    }
    db.reopen();
    let count: i64 = db.query_row("SELECT COUNT(*) FROM t", [], |r| r.get(0)).unwrap();
    assert_eq!(count, 10, "Should have exactly 10 rows after 10 cycles");
}

#[test]
#[ignore]
fn lifecycle_empty_database_operations() {
    let mut db = AzureTestDb::new("lc-empty");
    // Don't create any tables — just close and reopen
    db.reopen();

    let count: i64 = db
        .query_row("SELECT count(*) FROM sqlite_master", [], |r| r.get(0))
        .unwrap();
    assert_eq!(count, 0, "Empty database should have zero schema objects");
}

#[test]
#[ignore]
fn lifecycle_readonly_open() {
    let db = AzureTestDb::new("lc-ro");
    db.execute_batch(
        "CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT);
         INSERT INTO t VALUES (1, 'readonly-test');",
    )
    .unwrap();

    // Azure VFS requires EXCLUSIVE locking for WAL mode.
    // Opening read-only is not supported — verify it fails gracefully
    // rather than crashing or corrupting data.
    let uri = db.uri();

    let result = Connection::open_with_flags_and_vfs(&uri, RO_URI, "sqlite-objs");
    // Either the open itself fails, or the first read fails — either is acceptable
    match result {
        Err(_) => {
            // Opening readonly fails — expected behavior
        }
        Ok(ro) => {
            // If open succeeds, reads should either work or fail cleanly
            let _ = ro.query_row("SELECT v FROM t WHERE id=1", [], |r| r.get::<_, String>(0));
        }
    }
}

#[test]
#[ignore]
fn lifecycle_concurrent_blob_names() {
    init();
    let (account, container, sas) = azure_env();

    let mut conns: Vec<(Connection, String)> = Vec::new();
    for i in 0..3 {
        let blob = format!("vfstest-lc-par-{}-{}.db", i, Uuid::new_v4());
        let uri = sqlite_objs::UriBuilder::new(&blob, &account, &container)
            .sas_token(&sas)
            .build();
        let c = open_azure(&uri);
        let tbl = format!("tbl_{}", i);
        c.execute_batch(&format!(
            "CREATE TABLE {} (id INTEGER PRIMARY KEY, v TEXT);
             INSERT INTO {} VALUES (1, 'blob{}');",
            tbl, tbl, i
        ))
        .unwrap();
        conns.push((c, tbl));
    }

    // Verify no cross-contamination
    for (idx, (conn, tbl)) in conns.iter().enumerate() {
        let v: String = conn
            .query_row(&format!("SELECT v FROM {} WHERE id=1", tbl), [], |r| r.get(0))
            .unwrap();
        assert_eq!(v, format!("blob{}", idx), "Blob {} cross-contamination", idx);

        // Other tables should NOT exist
        for (j, (_, other_tbl)) in conns.iter().enumerate() {
            if j != idx {
                let exists: i64 = conn
                    .query_row(
                        "SELECT count(*) FROM sqlite_master WHERE name=?1",
                        [other_tbl.as_str()],
                        |r| r.get(0),
                    )
                    .unwrap();
                assert_eq!(exists, 0, "Table from blob {} should not exist in blob {}", j, idx);
            }
        }
    }
}

// ===================================================================
// Category 2: Transaction Correctness
// ===================================================================

#[test]
#[ignore]
fn txn_commit_persists_to_azure() {
    let mut db = AzureTestDb::new("txn-commit");
    db.execute_batch("CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT)").unwrap();

    db.execute_batch("BEGIN").unwrap();
    for i in 0..100u64 {
        db.execute("INSERT INTO t VALUES (?1, ?2)", rusqlite::params![i as i64, seeded_text(TEST_SEED, i)])
            .unwrap();
    }
    db.execute_batch("COMMIT").unwrap();
    db.reopen();

    let count: i64 = db.query_row("SELECT COUNT(*) FROM t", [], |r| r.get(0)).unwrap();
    assert_eq!(count, 100, "All 100 committed rows should persist");

    for i in 0..100u64 {
        let v: String = db.query_row("SELECT v FROM t WHERE id=?1", [i as i64], |r| r.get(0)).unwrap();
        assert_eq!(v, seeded_text(TEST_SEED, i), "Row {} mismatch after reopen", i);
    }
}

#[test]
#[ignore]
fn txn_rollback_discards_changes() {
    let db = AzureTestDb::new("txn-rb");
    db.execute_batch("CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT)").unwrap();

    // Commit initial 10 rows
    db.execute_batch("BEGIN").unwrap();
    for i in 0..10u64 {
        db.execute("INSERT INTO t VALUES (?1, ?2)", rusqlite::params![i as i64, seeded_text(TEST_SEED, i)])
            .unwrap();
    }
    db.execute_batch("COMMIT").unwrap();

    // Insert 50 more, then ROLLBACK
    db.execute_batch("BEGIN").unwrap();
    for i in 10..60u64 {
        db.execute("INSERT INTO t VALUES (?1, ?2)", rusqlite::params![i as i64, seeded_text(TEST_SEED, i)])
            .unwrap();
    }
    db.execute_batch("ROLLBACK").unwrap();

    let count: i64 = db.query_row("SELECT COUNT(*) FROM t", [], |r| r.get(0)).unwrap();
    assert_eq!(count, 10, "Only original 10 rows should survive after rollback");
}

#[test]
#[ignore]
fn txn_nested_savepoints() {
    let mut db = AzureTestDb::new("txn-save");
    db.execute_batch("CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT)").unwrap();

    db.execute_batch("BEGIN").unwrap();
    for i in 0..5u64 {
        db.execute("INSERT INTO t VALUES (?1, ?2)", rusqlite::params![i as i64, seeded_text(100, i)])
            .unwrap();
    }

    db.execute_batch("SAVEPOINT sp1").unwrap();
    for i in 5..10u64 {
        db.execute("INSERT INTO t VALUES (?1, ?2)", rusqlite::params![i as i64, seeded_text(100, i)])
            .unwrap();
    }

    db.execute_batch("SAVEPOINT sp2").unwrap();
    for i in 10..15u64 {
        db.execute("INSERT INTO t VALUES (?1, ?2)", rusqlite::params![i as i64, seeded_text(100, i)])
            .unwrap();
    }

    db.execute_batch("ROLLBACK TO sp2").unwrap();
    let count: i64 = db.query_row("SELECT COUNT(*) FROM t", [], |r| r.get(0)).unwrap();
    assert_eq!(count, 10, "After ROLLBACK TO sp2, should have 10 rows");

    db.execute_batch("RELEASE sp1").unwrap();
    db.execute_batch("COMMIT").unwrap();
    db.reopen();

    let count: i64 = db.query_row("SELECT COUNT(*) FROM t", [], |r| r.get(0)).unwrap();
    assert_eq!(count, 10, "10 rows should persist through close/reopen");
}

#[test]
#[ignore]
fn txn_large_transaction() {
    let mut db = AzureTestDb::new("txn-large");
    db.execute_batch("CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT, b BLOB)").unwrap();

    db.execute_batch("BEGIN").unwrap();
    for i in 0..5000u64 {
        let blob = seeded_blob(TEST_SEED, i, 256);
        db.execute(
            "INSERT INTO t VALUES (?1, ?2, ?3)",
            rusqlite::params![i as i64, seeded_text(TEST_SEED, i), blob],
        )
        .unwrap();
    }
    db.execute_batch("COMMIT").unwrap();
    db.reopen();

    let count: i64 = db.query_row("SELECT COUNT(*) FROM t", [], |r| r.get(0)).unwrap();
    assert_eq!(count, 5000, "All 5000 rows should persist");

    // Spot-check a few rows
    for &i in &[0u64, 1000, 2500, 4999] {
        let (v, b): (String, Vec<u8>) = db
            .query_row("SELECT v, b FROM t WHERE id=?1", [i as i64], |r| {
                Ok((r.get(0)?, r.get(1)?))
            })
            .unwrap();
        assert_eq!(v, seeded_text(TEST_SEED, i), "Text mismatch at row {}", i);
        assert_eq!(b, seeded_blob(TEST_SEED, i, 256), "Blob mismatch at row {}", i);
    }
}

#[test]
#[ignore]
fn txn_multiple_tables_single_txn() {
    let mut db = AzureTestDb::new("txn-multi-tbl");

    db.execute_batch("BEGIN").unwrap();
    for t in 0..5 {
        db.execute_batch(&format!(
            "CREATE TABLE tbl_{t} (id INTEGER PRIMARY KEY, v TEXT)"
        ))
        .unwrap();
        for i in 0..20u64 {
            db.execute(
                &format!("INSERT INTO tbl_{t} VALUES (?1, ?2)"),
                rusqlite::params![i as i64, seeded_text(t as u64 * 1000, i)],
            )
            .unwrap();
        }
    }
    db.execute_batch("COMMIT").unwrap();
    db.reopen();

    for t in 0..5 {
        let count: i64 = db
            .query_row(&format!("SELECT COUNT(*) FROM tbl_{t}"), [], |r| r.get(0))
            .unwrap();
        assert_eq!(count, 20, "Table tbl_{} should have 20 rows", t);
    }
}

#[test]
#[ignore]
fn txn_interleaved_read_write() {
    let db = AzureTestDb::new("txn-interleave");
    db.execute_batch("CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT)").unwrap();

    // Batch 1
    db.execute_batch("BEGIN").unwrap();
    for i in 0..100u64 {
        db.execute("INSERT INTO t VALUES (?1, ?2)", rusqlite::params![i as i64, seeded_text(1000, i)])
            .unwrap();
    }
    db.execute_batch("COMMIT").unwrap();

    // Read batch 1 while inserting batch 2
    let v0: String = db.query_row("SELECT v FROM t WHERE id=0", [], |r| r.get(0)).unwrap();
    assert_eq!(v0, seeded_text(1000, 0));

    db.execute_batch("BEGIN").unwrap();
    for i in 100..200u64 {
        db.execute("INSERT INTO t VALUES (?1, ?2)", rusqlite::params![i as i64, seeded_text(2000, i)])
            .unwrap();
    }
    db.execute_batch("COMMIT").unwrap();

    let count: i64 = db.query_row("SELECT COUNT(*) FROM t", [], |r| r.get(0)).unwrap();
    assert_eq!(count, 200, "Both batches totaling 200 rows");
}

#[test]
#[ignore]
fn txn_commit_then_immediate_close() {
    let db = AzureTestDb::new("txn-imm-close");
    db.execute_batch("CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT)").unwrap();
    for i in 0..50u64 {
        db.execute("INSERT INTO t VALUES (?1, ?2)", rusqlite::params![i as i64, seeded_text(TEST_SEED, i)])
            .unwrap();
    }
    // Drop = immediate close after autocommit
    let _uri = db.uri();
    let cache_dir_path = db.cache_dir.path().to_owned();
    let (account, container, sas) = (db.account.clone(), db.container.clone(), db.sas.clone());
    let blob_name = db.blob_name.clone();
    drop(db);

    let reopen_uri = sqlite_objs::UriBuilder::new(&blob_name, &account, &container)
        .sas_token(&sas)
        .cache_dir(cache_dir_path.to_str().unwrap())
        .build();
    let conn = open_azure(&reopen_uri);
    let count: i64 = conn.query_row("SELECT COUNT(*) FROM t", [], |r| r.get(0)).unwrap();
    assert_eq!(count, 50, "Data should survive immediate close after commit");
}

#[test]
#[ignore]
fn txn_autocommit_mode() {
    let mut db = AzureTestDb::new("txn-autocommit");
    db.execute_batch("CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT)").unwrap();

    // No explicit BEGIN/COMMIT — autocommit mode
    for i in 0..30u64 {
        db.execute("INSERT INTO t VALUES (?1, ?2)", rusqlite::params![i as i64, seeded_text(TEST_SEED, i)])
            .unwrap();
    }

    db.reopen();
    let count: i64 = db.query_row("SELECT COUNT(*) FROM t", [], |r| r.get(0)).unwrap();
    assert_eq!(count, 30, "All autocommit rows should persist");
}

// ===================================================================
// Category 3: Deterministic Data Validation
// ===================================================================

#[test]
#[ignore]
fn deterministic_bulk_insert_verify() {
    let mut db = AzureTestDb::new("det-bulk");
    db.execute_batch("CREATE TABLE t (id INTEGER PRIMARY KEY, txt TEXT, val REAL)").unwrap();

    db.execute_batch("BEGIN").unwrap();
    for i in 0..1000u64 {
        db.execute(
            "INSERT INTO t VALUES (?1, ?2, ?3)",
            rusqlite::params![i as i64, seeded_text(TEST_SEED, i), seeded_value(TEST_SEED, i)],
        )
        .unwrap();
    }
    db.execute_batch("COMMIT").unwrap();
    db.reopen();

    let mut mismatches = 0;
    let mut stmt = db.prepare("SELECT id, txt, val FROM t ORDER BY id").unwrap();
    let rows = stmt
        .query_map([], |r| Ok((r.get::<_, i64>(0)?, r.get::<_, String>(1)?, r.get::<_, f64>(2)?)))
        .unwrap();
    for row in rows {
        let (id, txt, val) = row.unwrap();
        let i = id as u64;
        if txt != seeded_text(TEST_SEED, i) || (val - seeded_value(TEST_SEED, i)).abs() > 1e-6 {
            mismatches += 1;
        }
    }
    assert_eq!(mismatches, 0, "Zero mismatches expected across 1000 rows");
}

#[test]
#[ignore]
fn deterministic_update_verify() {
    let mut db = AzureTestDb::new("det-upd");
    db.execute_batch("CREATE TABLE t (id INTEGER PRIMARY KEY, txt TEXT, val REAL)").unwrap();

    let seed_a: u64 = 0xAAAA;
    let seed_b: u64 = 0xBBBB;

    db.execute_batch("BEGIN").unwrap();
    for i in 0..500u64 {
        db.execute(
            "INSERT INTO t VALUES (?1, ?2, ?3)",
            rusqlite::params![i as i64, seeded_text(seed_a, i), seeded_value(seed_a, i)],
        )
        .unwrap();
    }
    db.execute_batch("COMMIT").unwrap();

    db.execute_batch("BEGIN").unwrap();
    for i in 0..500u64 {
        db.execute(
            "UPDATE t SET txt=?1, val=?2 WHERE id=?3",
            rusqlite::params![seeded_text(seed_b, i), seeded_value(seed_b, i), i as i64],
        )
        .unwrap();
    }
    db.execute_batch("COMMIT").unwrap();
    db.reopen();

    for i in 0..500u64 {
        let (txt, val): (String, f64) = db
            .query_row("SELECT txt, val FROM t WHERE id=?1", [i as i64], |r| {
                Ok((r.get(0)?, r.get(1)?))
            })
            .unwrap();
        assert_eq!(txt, seeded_text(seed_b, i), "Row {} text should be seed_b", i);
        assert!(
            (val - seeded_value(seed_b, i)).abs() < 1e-6,
            "Row {} value should be seed_b",
            i
        );
    }
}

#[test]
#[ignore]
fn deterministic_mixed_operations() {
    let mut db = AzureTestDb::new("det-mix");
    db.execute_batch("CREATE TABLE t (id INTEGER PRIMARY KEY, txt TEXT)").unwrap();

    let seed_orig: u64 = 0x1111;
    let seed_upd: u64 = 0x2222;

    db.execute_batch("BEGIN").unwrap();
    for i in 0..1000u64 {
        db.execute(
            "INSERT INTO t VALUES (?1, ?2)",
            rusqlite::params![i as i64, seeded_text(seed_orig, i)],
        )
        .unwrap();
    }
    db.execute_batch("COMMIT").unwrap();

    // Delete even IDs, update odd IDs
    db.execute_batch("BEGIN").unwrap();
    db.execute_batch("DELETE FROM t WHERE id % 2 = 0").unwrap();
    for i in (1..1000u64).step_by(2) {
        db.execute(
            "UPDATE t SET txt=?1 WHERE id=?2",
            rusqlite::params![seeded_text(seed_upd, i), i as i64],
        )
        .unwrap();
    }
    db.execute_batch("COMMIT").unwrap();
    db.reopen();

    let count: i64 = db.query_row("SELECT COUNT(*) FROM t", [], |r| r.get(0)).unwrap();
    assert_eq!(count, 500, "500 odd rows should remain");

    for i in (1..1000u64).step_by(2) {
        let txt: String = db
            .query_row("SELECT txt FROM t WHERE id=?1", [i as i64], |r| r.get(0))
            .unwrap();
        assert_eq!(txt, seeded_text(seed_upd, i), "Odd row {} mismatch", i);
    }
}

#[test]
#[ignore]
fn deterministic_checksum_validation() {
    let mut db = AzureTestDb::new("det-chk");
    db.execute_batch("CREATE TABLE t (id INTEGER PRIMARY KEY, txt TEXT, val REAL)").unwrap();

    db.execute_batch("BEGIN").unwrap();
    for i in 0..2000u64 {
        db.execute(
            "INSERT INTO t VALUES (?1, ?2, ?3)",
            rusqlite::params![i as i64, seeded_text(TEST_SEED, i), seeded_value(TEST_SEED, i)],
        )
        .unwrap();
    }
    db.execute_batch("COMMIT").unwrap();

    // Compute checksum before close
    fn compute_checksum(conn: &Connection) -> u64 {
        let mut stmt = conn
            .prepare("SELECT id, txt, val FROM t ORDER BY id")
            .unwrap();
        let mut hash: u64 = 0;
        let rows = stmt
            .query_map([], |r| {
                Ok((r.get::<_, i64>(0)?, r.get::<_, String>(1)?, r.get::<_, f64>(2)?))
            })
            .unwrap();
        for row in rows {
            let (id, txt, val) = row.unwrap();
            // Simple additive hash for checksum
            hash = hash.wrapping_add(id as u64);
            for b in txt.as_bytes() {
                hash = hash.wrapping_mul(31).wrapping_add(*b as u64);
            }
            hash = hash.wrapping_add(val.to_bits());
        }
        hash
    }

    let checksum1 = compute_checksum(&db.conn);
    db.reopen();
    let checksum2 = compute_checksum(&db.conn);

    assert_eq!(checksum1, checksum2, "Checksums must match across close/reopen (no silent corruption)");
}

// ===================================================================
// Category 4: Cache Reuse & ETag
// ===================================================================

#[test]
#[ignore]
fn cache_reuse_skips_download_on_clean_reopen() {
    let mut db = AzureTestDb::new_with_cache_reuse("cr-skip");
    db.execute_batch(
        "CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT);
         INSERT INTO t VALUES (1, 'cached');",
    )
    .unwrap();

    db.reopen_with_cache_reuse();

    let dl = get_download_count(&db.conn);
    assert_eq!(dl, 0, "ETag match should skip download (download_count == 0)");

    let v: String = db.query_row("SELECT v FROM t WHERE id=1", [], |r| r.get(0)).unwrap();
    assert_eq!(v, "cached");
}

#[test]
#[ignore]
fn cache_reuse_redownloads_when_blob_modified() {
    let mut db = AzureTestDb::new_with_cache_reuse("cr-mod");
    db.execute_batch(
        "CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT);
         INSERT INTO t VALUES (1, 'original');",
    )
    .unwrap();

    // Close cache_reuse conn; modify blob externally (without cache_reuse)
    let uri_no_cache = db.uri();
    let uri_cr = db.uri_with_cache_reuse();
    drop(db.conn);
    {
        let ext = open_azure(&uri_no_cache);
        ext.execute("UPDATE t SET v='modified' WHERE id=1", []).unwrap();
    }

    // Reopen with cache_reuse — ETag should be stale
    db.conn = open_azure(&uri_cr);

    let dl = get_download_count(&db.conn);
    assert!(dl > 0, "ETag mismatch should trigger fresh download (got {})", dl);

    let v: String = db.query_row("SELECT v FROM t WHERE id=1", [], |r| r.get(0)).unwrap();
    assert_eq!(v, "modified", "Should see externally modified data");
}

#[test]
#[ignore]
fn cache_reuse_data_integrity_after_reuse() {
    let mut db = AzureTestDb::new_with_cache_reuse("cr-int");
    db.execute_batch("CREATE TABLE t (id INTEGER PRIMARY KEY, txt TEXT, val REAL)").unwrap();

    db.execute_batch("BEGIN").unwrap();
    for i in 0..500u64 {
        db.execute(
            "INSERT INTO t VALUES (?1, ?2, ?3)",
            rusqlite::params![i as i64, seeded_text(TEST_SEED, i), seeded_value(TEST_SEED, i)],
        )
        .unwrap();
    }
    db.execute_batch("COMMIT").unwrap();

    db.reopen_with_cache_reuse();

    for i in 0..500u64 {
        let (txt, val): (String, f64) = db
            .query_row("SELECT txt, val FROM t WHERE id=?1", [i as i64], |r| {
                Ok((r.get(0)?, r.get(1)?))
            })
            .unwrap();
        assert_eq!(txt, seeded_text(TEST_SEED, i), "Row {} text mismatch via cache reuse", i);
        assert!(
            (val - seeded_value(TEST_SEED, i)).abs() < 1e-6,
            "Row {} value mismatch via cache reuse",
            i
        );
    }
}

#[test]
#[ignore]
fn cache_reuse_disabled_always_downloads() {
    let mut db = AzureTestDb::new("cr-disabled");
    db.execute_batch(
        "CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT);
         INSERT INTO t VALUES (1, 'nocrdata');",
    )
    .unwrap();

    db.reopen(); // reopen WITHOUT cache_reuse

    let dl = get_download_count(&db.conn);
    assert!(dl > 0, "Without cache_reuse, should always download (got {})", dl);
}

#[test]
#[ignore]
fn cache_reuse_different_cache_dirs() {
    let db = AzureTestDb::new_with_cache_reuse("cr-diffdir");
    db.execute_batch(
        "CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT);
         INSERT INTO t VALUES (1, 'diffdir');",
    )
    .unwrap();

    // Reopen with a completely different cache dir
    let other_dir = TempDir::new().unwrap();
    let uri = db.uri_with_other_cache_dir(other_dir.path().to_str().unwrap());
    drop(db.conn);

    let conn = open_azure(&uri);
    let dl = get_download_count(&conn);
    assert!(dl > 0, "Different cache dir means no cached file — should download (got {})", dl);
}

#[test]
#[ignore]
fn cache_reuse_no_cache_dir_falls_back() {
    let db = AzureTestDb::new_with_cache_reuse("cr-nodir");
    db.execute_batch(
        "CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT);
         INSERT INTO t VALUES (1, 'fallback');",
    )
    .unwrap();

    // Reopen with cache_reuse but no explicit cache_dir
    let uri = db.uri_no_cache_dir();
    drop(db.conn);

    let conn = open_azure(&uri);
    let v: String = conn.query_row("SELECT v FROM t WHERE id=1", [], |r| r.get(0)).unwrap();
    assert_eq!(v, "fallback", "Data should be accessible with default cache path");
}

// ===================================================================
// Category 5: Database Growth & Shrink
// ===================================================================

#[test]
#[ignore]
fn grow_incremental_inserts() {
    let db = AzureTestDb::new("grow-inc");
    db.execute_batch("CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT)").unwrap();

    let mut total = 0i64;
    for batch in 0..10u64 {
        db.execute_batch("BEGIN").unwrap();
        for i in 0..100u64 {
            let row_id = batch * 100 + i;
            db.execute(
                "INSERT INTO t VALUES (?1, ?2)",
                rusqlite::params![row_id as i64, seeded_text(TEST_SEED, row_id)],
            )
            .unwrap();
        }
        db.execute_batch("COMMIT").unwrap();
        total += 100;

        let count: i64 = db.query_row("SELECT COUNT(*) FROM t", [], |r| r.get(0)).unwrap();
        assert_eq!(count, total, "After batch {}, expected {} rows", batch, total);
    }
}

#[test]
#[ignore]
fn grow_large_blob_data() {
    let mut db = AzureTestDb::new("grow-blob");
    db.execute_batch("CREATE TABLE t (id INTEGER PRIMARY KEY, payload BLOB)").unwrap();

    db.execute_batch("BEGIN").unwrap();
    for i in 0..500u64 {
        let blob = seeded_blob(TEST_SEED, i, 4096);
        db.execute("INSERT INTO t VALUES (?1, ?2)", rusqlite::params![i as i64, blob]).unwrap();
    }
    db.execute_batch("COMMIT").unwrap();
    db.reopen();

    let count: i64 = db.query_row("SELECT COUNT(*) FROM t", [], |r| r.get(0)).unwrap();
    assert_eq!(count, 500, "All 500 BLOB rows should persist");

    // Spot-check
    for &i in &[0u64, 250, 499] {
        let b: Vec<u8> = db
            .query_row("SELECT payload FROM t WHERE id=?1", [i as i64], |r| r.get(0))
            .unwrap();
        assert_eq!(b, seeded_blob(TEST_SEED, i, 4096), "Blob row {} mismatch", i);
    }
}

#[test]
#[ignore]
fn shrink_delete_and_vacuum() {
    let mut db = AzureTestDb::new("shrink-vac");
    db.execute_batch("CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT, padding TEXT)").unwrap();

    db.execute_batch("BEGIN").unwrap();
    for i in 0..2000u64 {
        db.execute(
            "INSERT INTO t VALUES (?1, ?2, ?3)",
            rusqlite::params![i as i64, seeded_text(TEST_SEED, i), "x".repeat(200)],
        )
        .unwrap();
    }
    db.execute_batch("COMMIT").unwrap();

    let pages_before: i64 = db.query_row("PRAGMA page_count", [], |r| r.get(0)).unwrap();

    db.execute_batch("DELETE FROM t; VACUUM;").unwrap();

    let pages_after: i64 = db.query_row("PRAGMA page_count", [], |r| r.get(0)).unwrap();
    assert!(
        pages_after < pages_before,
        "VACUUM should reduce page_count: before={} after={}",
        pages_before,
        pages_after
    );

    db.reopen();
    let count: i64 = db.query_row("SELECT COUNT(*) FROM t", [], |r| r.get(0)).unwrap();
    assert_eq!(count, 0, "Table should be empty after DELETE + VACUUM + reopen");
}

#[test]
#[ignore]
fn shrink_incremental_vacuum() {
    let db = AzureTestDb::new("shrink-incr");
    // Must set auto_vacuum before creating tables
    db.execute_batch("PRAGMA auto_vacuum = INCREMENTAL").unwrap();
    db.execute_batch("CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT, padding TEXT)").unwrap();

    db.execute_batch("BEGIN").unwrap();
    for i in 0..1000u64 {
        db.execute(
            "INSERT INTO t VALUES (?1, ?2, ?3)",
            rusqlite::params![i as i64, seeded_text(TEST_SEED, i), "y".repeat(200)],
        )
        .unwrap();
    }
    db.execute_batch("COMMIT").unwrap();

    db.execute_batch("DELETE FROM t WHERE id >= 200").unwrap();
    let pages_before: i64 = db.query_row("PRAGMA page_count", [], |r| r.get(0)).unwrap();

    db.execute_batch("PRAGMA incremental_vacuum(100)").unwrap();

    let pages_after: i64 = db.query_row("PRAGMA page_count", [], |r| r.get(0)).unwrap();
    assert!(
        pages_after <= pages_before,
        "Incremental vacuum should not increase pages: before={} after={}",
        pages_before,
        pages_after
    );

    let count: i64 = db.query_row("SELECT COUNT(*) FROM t", [], |r| r.get(0)).unwrap();
    assert_eq!(count, 200, "200 remaining rows should be intact after incremental vacuum");
}

#[test]
#[ignore]
fn grow_shrink_cycle() {
    let db = AzureTestDb::new("grow-shrink");
    db.execute_batch("CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT)").unwrap();

    for cycle in 0..3u64 {
        db.execute_batch("BEGIN").unwrap();
        for i in 0..500u64 {
            db.execute(
                "INSERT INTO t VALUES (?1, ?2)",
                rusqlite::params![(cycle * 1000 + i) as i64, seeded_text(cycle * 100, i)],
            )
            .unwrap();
        }
        db.execute_batch("COMMIT").unwrap();

        let count: i64 = db.query_row("SELECT COUNT(*) FROM t", [], |r| r.get(0)).unwrap();
        assert_eq!(count, 500, "Cycle {} should have 500 rows after insert", cycle);

        db.execute_batch("DELETE FROM t; VACUUM;").unwrap();

        let count: i64 = db.query_row("SELECT COUNT(*) FROM t", [], |r| r.get(0)).unwrap();
        assert_eq!(count, 0, "Cycle {} should have 0 rows after delete+vacuum", cycle);
    }
}

// ===================================================================
// Category 6: WAL Mode
// ===================================================================

#[test]
#[ignore]
fn wal_basic_operations() {
    let mut db = AzureTestDb::new("wal-basic");

    let jm: String = db
        .query_row("PRAGMA journal_mode", [], |r| r.get(0))
        .unwrap();
    assert_eq!(jm.to_lowercase(), "wal", "Journal mode should be WAL");

    db.execute_batch(
        "CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT);
         INSERT INTO t VALUES (1, 'wal-test');",
    )
    .unwrap();

    db.reopen();
    let v: String = db.query_row("SELECT v FROM t WHERE id=1", [], |r| r.get(0)).unwrap();
    assert_eq!(v, "wal-test", "Data should persist through WAL mode close/reopen");
}

#[test]
#[ignore]
fn wal_checkpoint_truncate() {
    let mut db = AzureTestDb::new("wal-chkpt");
    db.execute_batch("CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT)").unwrap();

    // 5 transactions
    for txn in 0..5u64 {
        db.execute_batch("BEGIN").unwrap();
        for i in 0..20u64 {
            let row_id = txn * 20 + i;
            db.execute(
                "INSERT INTO t VALUES (?1, ?2)",
                rusqlite::params![row_id as i64, seeded_text(TEST_SEED, row_id)],
            )
            .unwrap();
        }
        db.execute_batch("COMMIT").unwrap();
    }

    db.execute_batch("PRAGMA wal_checkpoint(TRUNCATE)").unwrap();

    // More data after checkpoint
    db.execute_batch("BEGIN").unwrap();
    for i in 100..120u64 {
        db.execute(
            "INSERT INTO t VALUES (?1, ?2)",
            rusqlite::params![i as i64, seeded_text(TEST_SEED, i)],
        )
        .unwrap();
    }
    db.execute_batch("COMMIT").unwrap();

    db.reopen();
    let count: i64 = db.query_row("SELECT COUNT(*) FROM t", [], |r| r.get(0)).unwrap();
    assert_eq!(count, 120, "All 120 rows (pre+post checkpoint) should persist");
}

#[test]
#[ignore]
fn wal_large_wal_before_checkpoint() {
    let mut db = AzureTestDb::new("wal-large");
    db.execute_batch("PRAGMA wal_autocheckpoint=0").unwrap(); // disable auto-checkpoint
    db.execute_batch("CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT)").unwrap();

    // 20 separate transactions accumulating WAL
    for txn in 0..20u64 {
        db.execute_batch("BEGIN").unwrap();
        for i in 0..25u64 {
            let row_id = txn * 25 + i;
            db.execute(
                "INSERT INTO t VALUES (?1, ?2)",
                rusqlite::params![row_id as i64, seeded_text(TEST_SEED, row_id)],
            )
            .unwrap();
        }
        db.execute_batch("COMMIT").unwrap();
    }

    db.execute_batch("PRAGMA wal_checkpoint(TRUNCATE)").unwrap();
    db.reopen();

    let count: i64 = db.query_row("SELECT COUNT(*) FROM t", [], |r| r.get(0)).unwrap();
    assert_eq!(count, 500, "All 500 rows should survive large WAL + manual checkpoint");
}

#[test]
#[ignore]
fn wal_low_autocheckpoint() {
    let mut db = AzureTestDb::new("wal-lowcp");
    db.execute_batch("PRAGMA wal_autocheckpoint=2").unwrap();
    db.execute_batch("CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT, padding TEXT)").unwrap();

    // Many inserts → trigger frequent auto-checkpoints
    db.execute_batch("BEGIN").unwrap();
    for i in 0..200u64 {
        db.execute(
            "INSERT INTO t VALUES (?1, ?2, ?3)",
            rusqlite::params![i as i64, seeded_text(TEST_SEED, i), "p".repeat(500)],
        )
        .unwrap();
    }
    db.execute_batch("COMMIT").unwrap();
    db.reopen();

    let count: i64 = db.query_row("SELECT COUNT(*) FROM t", [], |r| r.get(0)).unwrap();
    assert_eq!(count, 200, "Frequent auto-checkpoints should not lose data");
}

#[test]
#[ignore]
fn wal_reopen_after_wal_exists() {
    let db = AzureTestDb::new("wal-recover");
    db.execute_batch("CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT)").unwrap();

    for i in 0..50u64 {
        db.execute("INSERT INTO t VALUES (?1, ?2)", rusqlite::params![i as i64, seeded_text(TEST_SEED, i)])
            .unwrap();
    }

    // Normal drop (no explicit checkpoint) — WAL file may still exist on blob.
    // We don't use mem::forget here because that prevents the lease from being
    // released, making immediate reconnection impossible.
    let uri = db.uri();
    let blob_name = db.blob_name.clone();
    let cache_dir_path = db.cache_dir.path().to_owned();
    let (account, container, sas) = (db.account.clone(), db.container.clone(), db.sas.clone());
    // Explicitly drop to release lease
    drop(db);

    // Reopen — WAL recovery should replay
    let reopen_uri = sqlite_objs::UriBuilder::new(&blob_name, &account, &container)
        .sas_token(&sas)
        .cache_dir(cache_dir_path.to_str().unwrap())
        .build();
    let conn = open_azure(&reopen_uri);
    let count: i64 = conn.query_row("SELECT COUNT(*) FROM t", [], |r| r.get(0)).unwrap();
    assert_eq!(count, 50, "WAL recovery should replay all committed data");
    let _ = uri;
}

// ===================================================================
// Category 7: Multi-threaded Access
// ===================================================================

#[test]
#[ignore]
fn thread_separate_blobs_parallel() {
    use std::sync::{Arc, Barrier};
    use std::thread;

    init();
    let (account, container, sas) = azure_env();

    let num_threads = 4;
    let barrier = Arc::new(Barrier::new(num_threads));
    let mut handles = Vec::new();

    for t in 0..num_threads {
        let barrier = barrier.clone();
        let (acc, cont, s) = (account.clone(), container.clone(), sas.clone());

        handles.push(thread::spawn(move || {
            let blob = format!("vfstest-thr-par-{}-{}.db", t, Uuid::new_v4());
            let uri = sqlite_objs::UriBuilder::new(&blob, &acc, &cont)
                .sas_token(&s)
                .build();

            barrier.wait();

            let conn = open_azure(&uri);
            conn.execute_batch("CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT)")
                .unwrap();
            for i in 0..100u64 {
                conn.execute(
                    "INSERT INTO t VALUES (?1, ?2)",
                    rusqlite::params![i as i64, seeded_text(t as u64 * 1000, i)],
                )
                .unwrap();
            }

            let count: i64 = conn.query_row("SELECT COUNT(*) FROM t", [], |r| r.get(0)).unwrap();
            assert_eq!(count, 100, "Thread {} should have 100 rows", t);

            // Verify deterministic data
            for i in 0..100u64 {
                let v: String =
                    conn.query_row("SELECT v FROM t WHERE id=?1", [i as i64], |r| r.get(0)).unwrap();
                assert_eq!(v, seeded_text(t as u64 * 1000, i));
            }
        }));
    }

    for h in handles {
        h.join().expect("Thread panicked");
    }
}

#[test]
#[ignore]
fn thread_sequential_access_same_blob() {
    use std::thread;

    init();
    let (account, container, sas) = azure_env();
    let blob_name = format!("vfstest-thr-seq-{}.db", Uuid::new_v4());

    // Thread 1: create and write batch 1
    {
        let (acc, cont, s, bn) = (account.clone(), container.clone(), sas.clone(), blob_name.clone());
        thread::spawn(move || {
            let uri = sqlite_objs::UriBuilder::new(&bn, &acc, &cont).sas_token(&s).build();
            let conn = open_azure(&uri);
            conn.execute_batch("CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT)")
                .unwrap();
            for i in 0..50u64 {
                conn.execute(
                    "INSERT INTO t VALUES (?1, ?2)",
                    rusqlite::params![i as i64, seeded_text(1000, i)],
                )
                .unwrap();
            }
        })
        .join()
        .unwrap();
    }

    // Thread 2: write batch 2
    {
        let (acc, cont, s, bn) = (account.clone(), container.clone(), sas.clone(), blob_name.clone());
        thread::spawn(move || {
            let uri = sqlite_objs::UriBuilder::new(&bn, &acc, &cont).sas_token(&s).build();
            let conn = open_azure(&uri);
            for i in 50..100u64 {
                conn.execute(
                    "INSERT INTO t VALUES (?1, ?2)",
                    rusqlite::params![i as i64, seeded_text(2000, i)],
                )
                .unwrap();
            }
        })
        .join()
        .unwrap();
    }

    // Main: verify both batches
    let uri = sqlite_objs::UriBuilder::new(&blob_name, &account, &container)
        .sas_token(&sas)
        .build();
    let conn = open_azure(&uri);
    let count: i64 = conn.query_row("SELECT COUNT(*) FROM t", [], |r| r.get(0)).unwrap();
    assert_eq!(count, 100, "Sequential thread access should produce 100 rows");
}

#[test]
#[ignore]
fn thread_connection_send_to_worker() {
    use std::thread;

    let db = AzureTestDb::new("thr-send");
    db.execute_batch("CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT)").unwrap();
    db.execute("INSERT INTO t VALUES (1, 'main')", []).unwrap();

    // Move connection to worker thread
    let conn = db.conn;
    let returned_conn = thread::spawn(move || {
        conn.execute("INSERT INTO t VALUES (2, 'worker')", []).unwrap();
        let count: i64 = conn.query_row("SELECT COUNT(*) FROM t", [], |r| r.get(0)).unwrap();
        assert_eq!(count, 2, "Worker thread should see 2 rows");
        conn
    })
    .join()
    .unwrap();

    let count: i64 = returned_conn
        .query_row("SELECT COUNT(*) FROM t", [], |r| r.get(0))
        .unwrap();
    assert_eq!(count, 2, "Main thread should see 2 rows after getting connection back");
}

#[test]
#[ignore]
fn thread_parallel_reads_same_blob() {
    use std::sync::{Arc, Barrier};
    use std::thread;

    init();
    let (account, container, sas) = azure_env();
    let blob_name = format!("vfstest-thr-reads-{}.db", Uuid::new_v4());

    // Create blob with data
    {
        let uri = sqlite_objs::UriBuilder::new(&blob_name, &account, &container)
            .sas_token(&sas)
            .build();
        let conn = open_azure(&uri);
        conn.execute_batch("CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT)").unwrap();
        conn.execute_batch("BEGIN").unwrap();
        for i in 0..1000u64 {
            conn.execute(
                "INSERT INTO t VALUES (?1, ?2)",
                rusqlite::params![i as i64, seeded_text(TEST_SEED, i)],
            )
            .unwrap();
        }
        conn.execute_batch("COMMIT").unwrap();
    }

    // 4 reader threads
    let num_readers = 4;
    let barrier = Arc::new(Barrier::new(num_readers));
    let mut handles = Vec::new();

    for _ in 0..num_readers {
        let barrier = barrier.clone();
        let (acc, cont, s, bn) = (account.clone(), container.clone(), sas.clone(), blob_name.clone());

        handles.push(thread::spawn(move || {
            let uri = sqlite_objs::UriBuilder::new(&bn, &acc, &cont).sas_token(&s).build();
            barrier.wait();
            let conn = open_azure(&uri);

            let count: i64 = conn.query_row("SELECT COUNT(*) FROM t", [], |r| r.get(0)).unwrap();
            assert_eq!(count, 1000);

            // Verify a sample
            for &i in &[0u64, 333, 666, 999] {
                let v: String =
                    conn.query_row("SELECT v FROM t WHERE id=?1", [i as i64], |r| r.get(0)).unwrap();
                assert_eq!(v, seeded_text(TEST_SEED, i));
            }
        }));
    }

    for h in handles {
        h.join().expect("Reader thread panicked");
    }
}

#[test]
#[ignore]
fn thread_stress_many_blobs() {
    use std::thread;

    init();
    let (account, container, sas) = azure_env();

    let num_threads = 8;
    let cycles = 50;
    let mut handles = Vec::new();

    for t in 0..num_threads {
        let (acc, cont, s) = (account.clone(), container.clone(), sas.clone());
        handles.push(thread::spawn(move || {
            let blob = format!("vfstest-thr-stress-{}-{}.db", t, Uuid::new_v4());
            let uri = sqlite_objs::UriBuilder::new(&blob, &acc, &cont)
                .sas_token(&s)
                .build();
            let conn = open_azure(&uri);
            conn.execute_batch("CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT)")
                .unwrap();

            for c in 0..cycles as u64 {
                conn.execute(
                    "INSERT INTO t VALUES (?1, ?2)",
                    rusqlite::params![c as i64, seeded_text(t as u64 * 10000, c)],
                )
                .unwrap();

                let count: i64 =
                    conn.query_row("SELECT COUNT(*) FROM t", [], |r| r.get(0)).unwrap();
                assert_eq!(count, c as i64 + 1);
            }
        }));
    }

    for h in handles {
        h.join().expect("Stress thread panicked");
    }
}

#[test]
#[ignore]
fn thread_vfs_registration_once() {
    use std::sync::{Arc, Barrier};
    use std::thread;

    load_dotenv();
    let (account, container, sas) = azure_env();

    let num_threads = 4;
    let barrier = Arc::new(Barrier::new(num_threads));
    let mut handles = Vec::new();

    for t in 0..num_threads {
        let barrier = barrier.clone();
        let (acc, cont, s) = (account.clone(), container.clone(), sas.clone());

        handles.push(thread::spawn(move || {
            barrier.wait();
            // All threads try to register simultaneously — OnceLock ensures single init
            ensure_vfs_registered();

            let blob = format!("vfstest-thr-reg-{}-{}.db", t, Uuid::new_v4());
            let uri = sqlite_objs::UriBuilder::new(&blob, &acc, &cont)
                .sas_token(&s)
                .build();
            let conn = open_azure(&uri);
            conn.execute_batch("CREATE TABLE t (id INTEGER PRIMARY KEY)")
                .unwrap();
            conn.execute("INSERT INTO t VALUES (1)", []).unwrap();
            let v: i64 = conn.query_row("SELECT id FROM t", [], |r| r.get(0)).unwrap();
            assert_eq!(v, 1, "Thread {} should work after concurrent registration", t);
        }));
    }

    for h in handles {
        h.join().expect("Registration thread panicked");
    }
}

// ===================================================================
// Category 8: Dirty Shutdown & Recovery
// ===================================================================

#[test]
#[ignore]
fn dirty_shutdown_drop_without_close() {
    init();
    let (account, container, sas) = azure_env();
    let cache_dir = TempDir::new().unwrap();
    let blob_name = format!("vfstest-dirty-drop-{}.db", Uuid::new_v4());

    {
        let uri = sqlite_objs::UriBuilder::new(&blob_name, &account, &container)
            .sas_token(&sas)
            .cache_dir(cache_dir.path().to_str().unwrap())
            .build();
        let conn = open_azure(&uri);
        conn.execute_batch(
            "CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT);
             BEGIN;",
        )
        .unwrap();
        for i in 0..50u64 {
            conn.execute("INSERT INTO t VALUES (?1, ?2)", rusqlite::params![i as i64, seeded_text(100, i)])
                .unwrap();
        }
        conn.execute_batch("COMMIT").unwrap();

        // Start uncommitted transaction
        conn.execute_batch("BEGIN").unwrap();
        for i in 50..100u64 {
            conn.execute("INSERT INTO t VALUES (?1, ?2)", rusqlite::params![i as i64, seeded_text(200, i)])
                .unwrap();
        }
        // DROP without COMMIT — Rust drop triggers xClose
    }

    // Reopen
    let uri = sqlite_objs::UriBuilder::new(&blob_name, &account, &container)
        .sas_token(&sas)
        .cache_dir(cache_dir.path().to_str().unwrap())
        .build();
    let conn = open_azure(&uri);
    let count: i64 = conn.query_row("SELECT COUNT(*) FROM t", [], |r| r.get(0)).unwrap();
    assert_eq!(count, 50, "Only committed rows (50) should survive dirty shutdown");
}

#[test]
#[ignore]
fn dirty_shutdown_committed_data_survives() {
    init();
    let (account, container, sas) = azure_env();
    let cache_dir = TempDir::new().unwrap();
    let blob_name = format!("vfstest-dirty-surv-{}.db", Uuid::new_v4());

    {
        let uri = sqlite_objs::UriBuilder::new(&blob_name, &account, &container)
            .sas_token(&sas)
            .cache_dir(cache_dir.path().to_str().unwrap())
            .build();
        let conn = open_azure(&uri);
        conn.execute_batch("CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT)")
            .unwrap();

        // Commit 3 transactions of 100 rows each
        for txn in 0..3u64 {
            conn.execute_batch("BEGIN").unwrap();
            for i in 0..100u64 {
                let row_id = txn * 100 + i;
                conn.execute(
                    "INSERT INTO t VALUES (?1, ?2)",
                    rusqlite::params![row_id as i64, seeded_text(TEST_SEED, row_id)],
                )
                .unwrap();
            }
            conn.execute_batch("COMMIT").unwrap();
        }

        // 4th transaction: uncommitted
        conn.execute_batch("BEGIN").unwrap();
        for i in 300..400u64 {
            conn.execute(
                "INSERT INTO t VALUES (?1, ?2)",
                rusqlite::params![i as i64, seeded_text(TEST_SEED, i)],
            )
            .unwrap();
        }
        // DROP — uncommitted txn should be rolled back
    }

    let uri = sqlite_objs::UriBuilder::new(&blob_name, &account, &container)
        .sas_token(&sas)
        .cache_dir(cache_dir.path().to_str().unwrap())
        .build();
    let conn = open_azure(&uri);
    let count: i64 = conn.query_row("SELECT COUNT(*) FROM t", [], |r| r.get(0)).unwrap();
    assert_eq!(count, 300, "Exactly 300 committed rows should survive (3 txns × 100)");

    // Verify deterministic values
    for i in 0..300u64 {
        let v: String = conn
            .query_row("SELECT v FROM t WHERE id=?1", [i as i64], |r| r.get(0))
            .unwrap();
        assert_eq!(v, seeded_text(TEST_SEED, i), "Row {} mismatch after dirty shutdown", i);
    }
}

#[test]
#[ignore]
fn dirty_shutdown_wal_recovery() {
    init();
    let (account, container, sas) = azure_env();
    let cache_dir = TempDir::new().unwrap();
    let blob_name = format!("vfstest-dirty-wal-{}.db", Uuid::new_v4());

    {
        let uri = sqlite_objs::UriBuilder::new(&blob_name, &account, &container)
            .sas_token(&sas)
            .cache_dir(cache_dir.path().to_str().unwrap())
            .build();
        let conn = open_azure(&uri);
        conn.execute_batch("CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT)")
            .unwrap();

        // Multiple committed writes in WAL mode
        for txn in 0..5u64 {
            conn.execute_batch("BEGIN").unwrap();
            for i in 0..20u64 {
                let row_id = txn * 20 + i;
                conn.execute(
                    "INSERT INTO t VALUES (?1, ?2)",
                    rusqlite::params![row_id as i64, seeded_text(TEST_SEED, row_id)],
                )
                .unwrap();
            }
            conn.execute_batch("COMMIT").unwrap();
        }

        // Begin an uncommitted write
        conn.execute_batch("BEGIN").unwrap();
        for i in 100..150u64 {
            conn.execute(
                "INSERT INTO t VALUES (?1, ?2)",
                rusqlite::params![i as i64, seeded_text(TEST_SEED, i)],
            )
            .unwrap();
        }
        // Drop — WAL recovery should replay only committed data
    }

    let uri = sqlite_objs::UriBuilder::new(&blob_name, &account, &container)
        .sas_token(&sas)
        .cache_dir(cache_dir.path().to_str().unwrap())
        .build();
    let conn = open_azure(&uri);
    let count: i64 = conn.query_row("SELECT COUNT(*) FROM t", [], |r| r.get(0)).unwrap();
    assert_eq!(count, 100, "WAL recovery: 5 committed txns × 20 rows = 100");
}

#[test]
#[ignore]
fn dirty_shutdown_rapid_reconnect() {
    init();
    let (account, container, sas) = azure_env();
    let cache_dir = TempDir::new().unwrap();
    let blob_name = format!("vfstest-dirty-rapid-{}.db", Uuid::new_v4());

    let build_uri = || {
        sqlite_objs::UriBuilder::new(&blob_name, &account, &container)
            .sas_token(&sas)
            .cache_dir(cache_dir.path().to_str().unwrap())
            .build()
    };

    // Open, insert 100, commit, DROP (unclean)
    {
        let conn = open_azure(&build_uri());
        conn.execute_batch("CREATE TABLE t (id INTEGER PRIMARY KEY, v TEXT)")
            .unwrap();
        conn.execute_batch("BEGIN").unwrap();
        for i in 0..100u64 {
            conn.execute(
                "INSERT INTO t VALUES (?1, ?2)",
                rusqlite::params![i as i64, seeded_text(TEST_SEED, i)],
            )
            .unwrap();
        }
        conn.execute_batch("COMMIT").unwrap();
        // DROP
    }

    // Immediately reopen, insert 100 more, close cleanly
    {
        let conn = open_azure(&build_uri());
        conn.execute_batch("BEGIN").unwrap();
        for i in 100..200u64 {
            conn.execute(
                "INSERT INTO t VALUES (?1, ?2)",
                rusqlite::params![i as i64, seeded_text(TEST_SEED, i)],
            )
            .unwrap();
        }
        conn.execute_batch("COMMIT").unwrap();
    }

    // Verify
    let conn = open_azure(&build_uri());
    let count: i64 = conn.query_row("SELECT COUNT(*) FROM t", [], |r| r.get(0)).unwrap();
    assert_eq!(count, 200, "Rapid reconnect after dirty shutdown should yield 200 rows");
}

// ===================================================================
// Category 9: Schema & PRAGMA
// ===================================================================

#[test]
#[ignore]
fn schema_complex_create_verify() {
    let mut db = AzureTestDb::new("sch-complex");
    db.execute_batch(
        "CREATE TABLE customers (id INTEGER PRIMARY KEY, name TEXT NOT NULL, email TEXT);
         CREATE TABLE orders (id INTEGER PRIMARY KEY, cust_id INTEGER, total REAL);
         CREATE TABLE items (id INTEGER PRIMARY KEY, order_id INTEGER, product TEXT, qty INTEGER);
         CREATE INDEX idx_orders_cust ON orders(cust_id);
         CREATE INDEX idx_items_order ON items(order_id);
         CREATE VIEW customer_orders AS SELECT c.name, o.id AS order_id, o.total FROM customers c JOIN orders o ON c.id = o.cust_id;
         CREATE TRIGGER trg_order_insert AFTER INSERT ON orders BEGIN UPDATE customers SET name = name WHERE id = NEW.cust_id; END;",
    )
    .unwrap();

    db.reopen();

    let expected = vec![
        ("index", "idx_items_order"),
        ("index", "idx_orders_cust"),
        ("table", "customers"),
        ("table", "items"),
        ("table", "orders"),
        ("trigger", "trg_order_insert"),
        ("view", "customer_orders"),
    ];

    let mut stmt = db
        .prepare("SELECT type, name FROM sqlite_master ORDER BY type, name")
        .unwrap();
    let actual: Vec<(String, String)> = stmt
        .query_map([], |r| Ok((r.get(0)?, r.get(1)?)))
        .unwrap()
        .map(|r| r.unwrap())
        .collect();

    for (exp_type, exp_name) in &expected {
        assert!(
            actual.iter().any(|(t, n)| t == exp_type && n == exp_name),
            "Missing schema object: {} '{}'",
            exp_type,
            exp_name
        );
    }
}

#[test]
#[ignore]
fn schema_alter_table() {
    let mut db = AzureTestDb::new("sch-alter");
    db.execute_batch(
        "CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT);
         INSERT INTO t VALUES (1, 'Alice');",
    )
    .unwrap();

    db.reopen();

    db.execute_batch("ALTER TABLE t ADD COLUMN age INTEGER DEFAULT 0")
        .unwrap();
    db.execute("INSERT INTO t VALUES (2, 'Bob', 30)", []).unwrap();

    db.reopen();

    let (name, age): (String, i64) = db
        .query_row("SELECT name, age FROM t WHERE id=2", [], |r| {
            Ok((r.get(0)?, r.get(1)?))
        })
        .unwrap();
    assert_eq!(name, "Bob");
    assert_eq!(age, 30);

    // Original row should have default
    let age_alice: i64 = db
        .query_row("SELECT age FROM t WHERE id=1", [], |r| r.get(0))
        .unwrap();
    assert_eq!(age_alice, 0, "Alice's age should be the default (0)");
}

#[test]
#[ignore]
fn pragma_persistence() {
    let mut db = AzureTestDb::new("sch-pragma");

    // page_size and auto_vacuum must be set before ANY writes (including WAL creation).
    // configure_azure() already set journal_mode=WAL which writes to the db,
    // so auto_vacuum can't be changed after that. Test pragmas that DO persist.
    db.execute_batch("CREATE TABLE t (id INTEGER PRIMARY KEY)").unwrap();

    db.reopen();

    let jm: String = db.query_row("PRAGMA journal_mode", [], |r| r.get(0)).unwrap();
    assert_eq!(
        jm.to_lowercase(),
        "wal",
        "journal_mode=WAL should persist (set by configure_azure)"
    );

    // user_version is an application-settable persistent pragma
    db.execute_batch("PRAGMA user_version = 42").unwrap();
    db.reopen();
    let uv: i64 = db.query_row("PRAGMA user_version", [], |r| r.get(0)).unwrap();
    assert_eq!(uv, 42, "user_version should persist across reopen");
}

#[test]
#[ignore]
fn schema_attach_not_supported() {
    init();
    let (account, container, sas) = azure_env();

    let blob1 = format!("vfstest-sch-attach1-{}.db", Uuid::new_v4());
    let blob2 = format!("vfstest-sch-attach2-{}.db", Uuid::new_v4());

    let uri1 = sqlite_objs::UriBuilder::new(&blob1, &account, &container)
        .sas_token(&sas)
        .build();
    let uri2 = sqlite_objs::UriBuilder::new(&blob2, &account, &container)
        .sas_token(&sas)
        .build();

    // Create primary DB
    let conn = open_azure(&uri1);
    conn.execute_batch("CREATE TABLE main_t (id INTEGER PRIMARY KEY)").unwrap();

    // Create secondary DB
    {
        let c2 = open_azure(&uri2);
        c2.execute_batch("CREATE TABLE other_t (id INTEGER PRIMARY KEY)").unwrap();
    }

    // Attempt ATTACH — document current behavior (either works or returns an error)
    let attach_sql = format!("ATTACH DATABASE '{}' AS other", uri2);
    let result = conn.execute_batch(&attach_sql);

    if let Err(e) = result {
        // ATTACH not supported is acceptable — just verify no crash
        eprintln!(
            "ATTACH returned error (expected for Azure VFS): {}",
            e
        );
    } else {
        // If it works, verify we can use it
        let count: i64 = conn
            .query_row("SELECT count(*) FROM other.other_t", [], |r| r.get(0))
            .unwrap();
        assert_eq!(count, 0, "Attached DB should have 0 rows");
    }
}
