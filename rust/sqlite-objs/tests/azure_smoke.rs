//! Azure Blob Storage smoke tests for the sqlite-objs Rust crate.
//!
//! These tests require live Azure credentials via environment variables:
//!   AZURE_STORAGE_ACCOUNT, AZURE_STORAGE_CONTAINER, AZURE_STORAGE_SAS
//!
//! Run with: cargo test --test azure_smoke
//! Skip with: cargo test (these are ignored by default)

use sqlite_objs::SqliteObjsVfs;
use rusqlite::{Connection, OpenFlags};
use std::sync::Once;

static REGISTER_VFS: Once = Once::new();

fn register_vfs() {
    REGISTER_VFS.call_once(|| {
        SqliteObjsVfs::register_uri(false).expect("VFS registration should succeed");
    });
}

fn azure_uri(blob_name: &str) -> String {
    let account =
        std::env::var("AZURE_STORAGE_ACCOUNT").expect("AZURE_STORAGE_ACCOUNT must be set");
    let container =
        std::env::var("AZURE_STORAGE_CONTAINER").expect("AZURE_STORAGE_CONTAINER must be set");
    let sas = std::env::var("AZURE_STORAGE_SAS").expect("AZURE_STORAGE_SAS must be set");

    // Percent-encode the SAS token for URI safety
    let encoded_sas = sas.replace('%', "%25").replace('&', "%26").replace('=', "%3D");

    format!(
        "file:{blob_name}?azure_account={account}&azure_container={container}&azure_sas={encoded_sas}"
    )
}

fn has_azure_creds() -> bool {
    std::env::var("AZURE_STORAGE_ACCOUNT").is_ok()
        && std::env::var("AZURE_STORAGE_CONTAINER").is_ok()
        && std::env::var("AZURE_STORAGE_SAS").is_ok()
}

#[test]
#[ignore] // Run explicitly: cargo test --test azure_smoke -- --ignored
fn smoke_create_table_insert_query() {
    if !has_azure_creds() {
        eprintln!("Skipping: Azure credentials not set");
        return;
    }

    register_vfs();

    let uri = azure_uri("rust_smoke_test.db");
    let flags = OpenFlags::SQLITE_OPEN_READ_WRITE
        | OpenFlags::SQLITE_OPEN_CREATE
        | OpenFlags::SQLITE_OPEN_URI;

    let conn =
        Connection::open_with_flags_and_vfs(&uri, flags, "sqlite-objs").expect("Failed to open DB");

    // Create table
    conn.execute_batch("DROP TABLE IF EXISTS smoke_test")
        .expect("DROP TABLE failed");
    conn.execute_batch(
        "CREATE TABLE smoke_test (
            id INTEGER PRIMARY KEY,
            name TEXT NOT NULL,
            value REAL
        )",
    )
    .expect("CREATE TABLE failed");

    // Insert rows
    conn.execute(
        "INSERT INTO smoke_test (id, name, value) VALUES (?1, ?2, ?3)",
        rusqlite::params![1, "alpha", 3.14],
    )
    .expect("INSERT 1 failed");

    conn.execute(
        "INSERT INTO smoke_test (id, name, value) VALUES (?1, ?2, ?3)",
        rusqlite::params![2, "beta", 2.718],
    )
    .expect("INSERT 2 failed");

    // Query back
    let mut stmt = conn
        .prepare("SELECT id, name, value FROM smoke_test ORDER BY id")
        .expect("SELECT prepare failed");

    let rows: Vec<(i32, String, f64)> = stmt
        .query_map([], |row| Ok((row.get(0)?, row.get(1)?, row.get(2)?)))
        .expect("query_map failed")
        .map(|r| r.expect("row error"))
        .collect();

    assert_eq!(rows.len(), 2);
    assert_eq!(rows[0], (1, "alpha".to_string(), 3.14));
    assert_eq!(rows[1], (2, "beta".to_string(), 2.718));

    // Clean up
    conn.execute_batch("DROP TABLE smoke_test")
        .expect("DROP TABLE cleanup failed");

    println!("✓ smoke_create_table_insert_query passed");
}

#[test]
#[ignore]
fn smoke_multiple_transactions() {
    if !has_azure_creds() {
        eprintln!("Skipping: Azure credentials not set");
        return;
    }

    register_vfs();

    let uri = azure_uri("rust_smoke_txn.db");
    let flags = OpenFlags::SQLITE_OPEN_READ_WRITE
        | OpenFlags::SQLITE_OPEN_CREATE
        | OpenFlags::SQLITE_OPEN_URI;

    let mut conn =
        Connection::open_with_flags_and_vfs(&uri, flags, "sqlite-objs").expect("Failed to open DB");

    conn.execute_batch("DROP TABLE IF EXISTS txn_test")
        .expect("DROP TABLE failed");
    conn.execute_batch("CREATE TABLE txn_test (k TEXT PRIMARY KEY, v INTEGER)")
        .expect("CREATE TABLE failed");

    // Transaction 1: insert
    {
        let tx = conn.transaction().expect("BEGIN failed");
        tx.execute("INSERT INTO txn_test VALUES ('a', 1)", [])
            .expect("INSERT a failed");
        tx.execute("INSERT INTO txn_test VALUES ('b', 2)", [])
            .expect("INSERT b failed");
        tx.commit().expect("COMMIT 1 failed");
    }

    // Transaction 2: update
    {
        let tx = conn.transaction().expect("BEGIN failed");
        tx.execute("UPDATE txn_test SET v = 10 WHERE k = 'a'", [])
            .expect("UPDATE failed");
        tx.commit().expect("COMMIT 2 failed");
    }

    // Verify
    let val: i32 = conn
        .query_row("SELECT v FROM txn_test WHERE k = 'a'", [], |row| row.get(0))
        .expect("SELECT failed");
    assert_eq!(val, 10);

    let count: i32 = conn
        .query_row("SELECT COUNT(*) FROM txn_test", [], |row| row.get(0))
        .expect("COUNT failed");
    assert_eq!(count, 2);

    conn.execute_batch("DROP TABLE txn_test")
        .expect("cleanup failed");

    println!("✓ smoke_multiple_transactions passed");
}

#[test]
#[ignore]
fn smoke_reopen_persists() {
    if !has_azure_creds() {
        eprintln!("Skipping: Azure credentials not set");
        return;
    }

    register_vfs();

    let uri = azure_uri("rust_smoke_persist.db");
    let flags = OpenFlags::SQLITE_OPEN_READ_WRITE
        | OpenFlags::SQLITE_OPEN_CREATE
        | OpenFlags::SQLITE_OPEN_URI;

    // Open, create, insert, close
    {
        let conn = Connection::open_with_flags_and_vfs(&uri, flags, "sqlite-objs")
            .expect("Failed to open DB (write)");

        conn.execute_batch("DROP TABLE IF EXISTS persist_test")
            .expect("DROP TABLE failed");
        conn.execute_batch("CREATE TABLE persist_test (x INTEGER)")
            .expect("CREATE TABLE failed");
        conn.execute("INSERT INTO persist_test VALUES (42)", [])
            .expect("INSERT failed");
    }

    // Reopen and verify data persisted
    {
        let conn = Connection::open_with_flags_and_vfs(&uri, flags, "sqlite-objs")
            .expect("Failed to open DB (read)");

        let val: i32 = conn
            .query_row("SELECT x FROM persist_test", [], |row| row.get(0))
            .expect("SELECT failed");
        assert_eq!(val, 42);

        conn.execute_batch("DROP TABLE persist_test")
            .expect("cleanup failed");
    }

    println!("✓ smoke_reopen_persists passed");
}
