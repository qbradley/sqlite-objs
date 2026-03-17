//! Reconnect benchmark: measures cold download and warm+stale reconnect times
//! with two clients (A and B) sharing an Azure blob database.
//!
//! Each client has its own cache directory with `cache_reuse=true`, so reconnects
//! use ETag-based revalidation rather than always re-downloading.
//!
//! Usage:
//!   source ../../.env && cargo run --example reconnect_bench
//!
//! Requires: AZURE_STORAGE_ACCOUNT, AZURE_STORAGE_CONTAINER, AZURE_STORAGE_SAS

use rusqlite::{Connection, OpenFlags};
use sqlite_objs::{SqliteObjsVfs, UriBuilder};
use std::path::Path;
use std::sync::OnceLock;
use std::time::Instant;
use tempfile::TempDir;
use uuid::Uuid;

const RW_CREATE_URI: OpenFlags = OpenFlags::SQLITE_OPEN_READ_WRITE
    .union(OpenFlags::SQLITE_OPEN_CREATE)
    .union(OpenFlags::SQLITE_OPEN_URI);

static INIT: OnceLock<()> = OnceLock::new();

fn init() {
    INIT.get_or_init(|| {
        let manifest_dir = Path::new(env!("CARGO_MANIFEST_DIR"));
        let crate_env = manifest_dir.join(".env");
        let repo_env = manifest_dir.join("../../.env");
        if dotenvy::from_path(&crate_env).is_err() {
            dotenvy::from_path(&repo_env).ok();
        }
        SqliteObjsVfs::register_uri(false).expect("VFS registration failed");
    });
}

fn azure_env() -> (String, String, String) {
    (
        std::env::var("AZURE_STORAGE_ACCOUNT").expect("AZURE_STORAGE_ACCOUNT not set"),
        std::env::var("AZURE_STORAGE_CONTAINER").expect("AZURE_STORAGE_CONTAINER not set"),
        std::env::var("AZURE_STORAGE_SAS").expect("AZURE_STORAGE_SAS not set"),
    )
}

fn build_uri(blob: &str, account: &str, container: &str, sas: &str, cache_dir: &str) -> String {
    UriBuilder::new(blob, account, container)
        .sas_token(sas)
        .cache_dir(cache_dir)
        .cache_reuse(true)
        .build()
}

fn open(uri: &str) -> Connection {
    let conn = Connection::open_with_flags_and_vfs(uri, RW_CREATE_URI, "sqlite-objs")
        .expect("Failed to open Azure connection");
    conn.execute_batch(
        "PRAGMA locking_mode=EXCLUSIVE;
         PRAGMA journal_mode=DELETE;
         PRAGMA synchronous=NORMAL;
         PRAGMA busy_timeout=60000;",
    )
    .expect("PRAGMA configuration failed");
    conn
}

fn main() {
    init();
    let (account, container, sas) = azure_env();

    let blob_name = format!("reconnect-bench-{}.db", Uuid::new_v4());
    let cache_a = TempDir::new().expect("Failed to create cache dir A");
    let cache_b = TempDir::new().expect("Failed to create cache dir B");

    let cache_a_path = cache_a.path().to_str().unwrap();
    let cache_b_path = cache_b.path().to_str().unwrap();

    println!("=== Reconnect Benchmark ===");
    println!("Blob: {}", blob_name);
    println!("Data size: ~100MB");
    println!();

    // ── Step 1: Client A creates and populates the database ──────────────
    let t = Instant::now();
    let uri_a = build_uri(&blob_name, &account, &container, &sas, cache_a_path);
    let conn_a = open(&uri_a);

    conn_a
        .execute_batch("CREATE TABLE bench (id INTEGER PRIMARY KEY, data BLOB)")
        .unwrap();

    // ~100MB: 10,000 rows × 10KB BLOB each
    conn_a.execute_batch("BEGIN").unwrap();
    {
        let mut stmt = conn_a
            .prepare("INSERT INTO bench (id, data) VALUES (?1, randomblob(10000))")
            .unwrap();
        for i in 0..10_000i64 {
            stmt.execute([i]).unwrap();
        }
    }
    // Marker row for correctness verification
    conn_a
        .execute(
            "INSERT INTO bench (id, data) VALUES (99999, zeroblob(1))",
            [],
        )
        .unwrap();
    conn_a.execute_batch("COMMIT").unwrap();

    let populate_time = t.elapsed();
    println!(
        "[1] Client A: Create + populate    ... {:.2}s",
        populate_time.as_secs_f64()
    );

    // ── Step 2: Client A disconnects ─────────────────────────────────────
    drop(conn_a);

    // ── Step 3: Client B connects cold (empty cache, full download) ──────
    let t = Instant::now();
    let uri_b = build_uri(&blob_name, &account, &container, &sas, cache_b_path);
    let conn_b = open(&uri_b);
    let count: i64 = conn_b
        .query_row("SELECT COUNT(*) FROM bench", [], |r| r.get(0))
        .unwrap();
    let cold_time = t.elapsed();
    println!(
        "[2] Client B: First connect (cold)  ... {:.2}s  ({} rows)",
        cold_time.as_secs_f64(),
        count
    );

    // ── Step 4: Client B does a small write ──────────────────────────────
    let t = Instant::now();
    conn_b.execute_batch("BEGIN").unwrap();
    conn_b
        .execute("UPDATE bench SET data = X'DEADBEEF' WHERE id = 99999", [])
        .unwrap();
    conn_b.execute_batch("COMMIT").unwrap();
    let write_time = t.elapsed();
    println!(
        "[3] Client B: Small write           ... {:.2}s",
        write_time.as_secs_f64()
    );

    // ── Step 5: Client B disconnects ─────────────────────────────────────
    drop(conn_b);

    // ── Step 6: Client A reconnects (warm cache, stale ETag) ─────────────
    let t = Instant::now();
    let conn_a2 = open(&uri_a);
    let marker: Vec<u8> = conn_a2
        .query_row("SELECT data FROM bench WHERE id = 99999", [], |r| r.get(0))
        .unwrap();
    let reconnect_time = t.elapsed();
    println!(
        "[4] Client A: Reconnect (warm+stale)... {:.2}s",
        reconnect_time.as_secs_f64()
    );

    // ── Correctness check ────────────────────────────────────────────────
    let correct = marker == vec![0xDE, 0xAD, 0xBE, 0xEF];
    if correct {
        println!("[✓] Correctness: A can read B's write");
    } else {
        eprintln!(
            "[✗] Correctness FAILED: expected DEADBEEF, got {:02X?}",
            marker
        );
    }

    drop(conn_a2);

    // ── Summary ──────────────────────────────────────────────────────────
    println!();
    println!("Summary:");
    println!(
        "  Populate (100MB):                     {:.2}s",
        populate_time.as_secs_f64()
    );
    println!(
        "  Cold download (100MB):                {:.2}s",
        cold_time.as_secs_f64()
    );
    println!(
        "  Small write:                          {:.2}s",
        write_time.as_secs_f64()
    );
    println!(
        "  Stale reconnect (100MB, re-download): {:.2}s",
        reconnect_time.as_secs_f64()
    );

    if !correct {
        std::process::exit(1);
    }
}
