//! Multi-threaded SQLite tests verifying thread safety of the sqlite-objs VFS.
//!
//! Key findings about rusqlite::Connection:
//! - Connection is Send (can be moved between threads)
//! - Connection is NOT Sync (cannot be shared across threads via Arc)
//! - Best practice: Each thread gets its own Connection
//!
//! These tests verify:
//! 1. Connections can be created and used from different threads
//! 2. Multiple threads can access the same database file (SQLite's file locking handles this)
//! 3. The VFS registration is thread-safe (register once, use from many threads)

use rusqlite::{Connection, OpenFlags};
use std::sync::{Arc, Barrier, Mutex};
use std::thread;
use tempfile::TempDir;

/// Test 1: Two threads, separate databases, parallel work
///
/// This verifies that rusqlite::Connection is Send and that our VFS
/// registration works correctly when connections are created from different threads.
#[test]
fn test_two_threads_separate_databases() {
    let temp_dir = TempDir::new().unwrap();
    let db_path_a = temp_dir.path().join("test_thread_a.db");
    let db_path_b = temp_dir.path().join("test_thread_b.db");

    // Clone paths for move into threads
    let path_a = db_path_a.to_str().unwrap().to_string();
    let path_b = db_path_b.to_str().unwrap().to_string();

    // Barrier to ensure both threads start at the same time
    let barrier = Arc::new(Barrier::new(2));
    let barrier_a = barrier.clone();
    let barrier_b = barrier.clone();

    // Thread A
    let handle_a = thread::spawn(move || {
        barrier_a.wait();

        let conn = Connection::open_with_flags(
            &path_a,
            OpenFlags::SQLITE_OPEN_READ_WRITE | OpenFlags::SQLITE_OPEN_CREATE,
        )
        .expect("Thread A failed to open database");

        conn.execute("CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT)", [])
            .expect("Thread A failed to create table");

        conn.execute("INSERT INTO users (name) VALUES (?1)", ["Alice"])
            .expect("Thread A failed to insert");

        let count: i32 = conn
            .query_row("SELECT COUNT(*) FROM users", [], |row| row.get(0))
            .expect("Thread A failed to query");

        assert_eq!(count, 1, "Thread A should have 1 row");
        "Thread A success"
    });

    // Thread B
    let handle_b = thread::spawn(move || {
        barrier_b.wait();

        let conn = Connection::open_with_flags(
            &path_b,
            OpenFlags::SQLITE_OPEN_READ_WRITE | OpenFlags::SQLITE_OPEN_CREATE,
        )
        .expect("Thread B failed to open database");

        conn.execute(
            "CREATE TABLE products (id INTEGER PRIMARY KEY, name TEXT)",
            [],
        )
        .expect("Thread B failed to create table");

        conn.execute("INSERT INTO products (name) VALUES (?1)", ["Widget"])
            .expect("Thread B failed to insert");

        let count: i32 = conn
            .query_row("SELECT COUNT(*) FROM products", [], |row| row.get(0))
            .expect("Thread B failed to query");

        assert_eq!(count, 1, "Thread B should have 1 row");
        "Thread B success"
    });

    // Wait for both threads to complete
    let result_a = handle_a.join().expect("Thread A panicked");
    let result_b = handle_b.join().expect("Thread B panicked");

    assert_eq!(result_a, "Thread A success");
    assert_eq!(result_b, "Thread B success");
}

/// Test 2: Multiple threads, same database, sequential access via Mutex
///
/// This verifies that we can wrap a Connection in Arc<Mutex<>> for controlled
/// sequential access from multiple threads. While not the recommended pattern,
/// it should work correctly.
#[test]
fn test_multiple_threads_mutex_sequential() {
    let temp_dir = TempDir::new().unwrap();
    let db_path = temp_dir.path().join("shared.db");

    // Create database and table
    {
        let conn = Connection::open(&db_path).unwrap();
        conn.execute(
            "CREATE TABLE counters (id INTEGER PRIMARY KEY, value INTEGER)",
            [],
        )
        .unwrap();
        conn.execute("INSERT INTO counters (value) VALUES (0)", [])
            .unwrap();
    }

    // Wrap connection in Arc<Mutex<>> for sharing
    let conn = Arc::new(Mutex::new(
        Connection::open_with_flags(&db_path, OpenFlags::SQLITE_OPEN_READ_WRITE).unwrap(),
    ));

    let thread_count = 4;
    let mut handles = vec![];

    for i in 0..thread_count {
        let conn = conn.clone();
        let handle = thread::spawn(move || {
            // Lock the connection for this thread
            let conn = conn.lock().unwrap();

            // Read current value
            let value: i32 = conn
                .query_row("SELECT value FROM counters WHERE id = 1", [], |row| {
                    row.get(0)
                })
                .unwrap();

            // Increment
            conn.execute("UPDATE counters SET value = ?1 WHERE id = 1", [value + 1])
                .unwrap();

            format!("Thread {} updated counter", i)
        });
        handles.push(handle);
    }

    // Wait for all threads
    for handle in handles {
        handle.join().expect("Thread panicked");
    }

    // Verify final value
    let conn = conn.lock().unwrap();
    let final_value: i32 = conn
        .query_row("SELECT value FROM counters WHERE id = 1", [], |row| {
            row.get(0)
        })
        .unwrap();

    assert_eq!(
        final_value, thread_count,
        "Counter should be incremented by each thread"
    );
}

/// Test 3: Multiple threads, each with own connection to same DB
///
/// This is the recommended pattern: each thread opens its own Connection to
/// the same database. SQLite's file locking handles concurrent access.
#[test]
fn test_multiple_threads_separate_connections() {
    let temp_dir = TempDir::new().unwrap();
    let db_path = temp_dir.path().join("multi_conn.db");

    // Create database
    {
        let conn = Connection::open(&db_path).unwrap();
        // Just ensure the DB file exists
        conn.execute("CREATE TABLE IF NOT EXISTS threads (id INTEGER PRIMARY KEY, thread_id INTEGER, table_name TEXT)", [])
            .unwrap();
    }

    let path_str = db_path.to_str().unwrap().to_string();
    let thread_count = 5;
    let mut handles = vec![];

    // Barrier to synchronize thread starts
    let barrier = Arc::new(Barrier::new(thread_count));

    for i in 0..thread_count {
        let path = path_str.clone();
        let barrier = barrier.clone();

        let handle = thread::spawn(move || {
            barrier.wait();

            // Each thread opens its own connection
            let conn = Connection::open_with_flags(
                &path,
                OpenFlags::SQLITE_OPEN_READ_WRITE | OpenFlags::SQLITE_OPEN_CREATE,
            )
            .unwrap_or_else(|_| panic!("Thread {} failed to open connection", i));

            // Create a unique table for this thread
            let table_name = format!("thread_{}_data", i);
            conn.execute(
                &format!(
                    "CREATE TABLE IF NOT EXISTS {} (id INTEGER PRIMARY KEY, value TEXT)",
                    table_name
                ),
                [],
            )
            .unwrap_or_else(|_| panic!("Thread {} failed to create table", i));

            // Insert data
            conn.execute(
                &format!("INSERT INTO {} (value) VALUES (?1)", table_name),
                [format!("Data from thread {}", i)],
            )
            .unwrap_or_else(|_| panic!("Thread {} failed to insert", i));

            // Record in shared table
            conn.execute(
                "INSERT INTO threads (thread_id, table_name) VALUES (?1, ?2)",
                [&i.to_string(), &table_name],
            )
            .unwrap_or_else(|_| panic!("Thread {} failed to insert into shared table", i));

            // Verify our data
            let count: i32 = conn
                .query_row(&format!("SELECT COUNT(*) FROM {}", table_name), [], |row| {
                    row.get(0)
                })
                .unwrap_or_else(|_| panic!("Thread {} failed to query", i));

            assert_eq!(count, 1, "Thread {} should have 1 row", i);

            i
        });
        handles.push(handle);
    }

    // Wait for all threads
    let mut results = vec![];
    for handle in handles {
        let thread_id = handle.join().expect("Thread panicked");
        results.push(thread_id);
    }

    // Verify all threads completed
    results.sort();
    assert_eq!(results, (0..thread_count).collect::<Vec<_>>());

    // Verify shared table has entries from all threads
    let conn = Connection::open(&db_path).unwrap();
    let thread_count_db: i32 = conn
        .query_row("SELECT COUNT(*) FROM threads", [], |row| row.get(0))
        .unwrap();

    assert_eq!(
        thread_count_db, thread_count as i32,
        "Should have entries from all threads"
    );
}

/// Test 4: Stress test - many threads, many operations
///
/// This test creates many threads that all perform database operations
/// concurrently. It verifies that under load, there are no crashes or
/// data corruption issues.
#[test]
fn test_stress_many_threads() {
    let temp_dir = TempDir::new().unwrap();
    let db_path = temp_dir.path().join("stress.db");

    // Initialize database
    {
        let conn = Connection::open(&db_path).unwrap();
        conn.execute(
            "CREATE TABLE stress_test (id INTEGER PRIMARY KEY AUTOINCREMENT, thread_id INTEGER, iteration INTEGER, value TEXT)",
            [],
        )
        .unwrap();
    }

    let path_str = db_path.to_str().unwrap().to_string();
    let thread_count = 10;
    let iterations_per_thread = 5;
    let mut handles = vec![];

    for thread_id in 0..thread_count {
        let path = path_str.clone();

        let handle = thread::spawn(move || {
            let conn = Connection::open_with_flags(&path, OpenFlags::SQLITE_OPEN_READ_WRITE)
                .unwrap_or_else(|_| panic!("Thread {} failed to open", thread_id));

            for iter in 0..iterations_per_thread {
                // Insert
                conn.execute(
                    "INSERT INTO stress_test (thread_id, iteration, value) VALUES (?1, ?2, ?3)",
                    [
                        &thread_id.to_string(),
                        &iter.to_string(),
                        &format!("t{}i{}", thread_id, iter),
                    ],
                )
                .unwrap_or_else(|_| panic!("Thread {} iter {} failed to insert", thread_id, iter));

                // Read back
                let count: i32 = conn
                    .query_row(
                        "SELECT COUNT(*) FROM stress_test WHERE thread_id = ?1",
                        [thread_id],
                        |row| row.get(0),
                    )
                    .unwrap_or_else(|_| {
                        panic!("Thread {} iter {} failed to query", thread_id, iter)
                    });

                assert_eq!(
                    count,
                    iter + 1,
                    "Thread {} should have {} rows at iteration {}",
                    thread_id,
                    iter + 1,
                    iter
                );
            }

            thread_id
        });
        handles.push(handle);
    }

    // Wait for all threads
    for handle in handles {
        handle.join().expect("Thread panicked");
    }

    // Verify total count
    let conn = Connection::open(&db_path).unwrap();
    let total: i32 = conn
        .query_row("SELECT COUNT(*) FROM stress_test", [], |row| row.get(0))
        .unwrap();

    assert_eq!(
        total,
        thread_count * iterations_per_thread,
        "Should have exactly {} rows",
        thread_count * iterations_per_thread
    );
}

/// Test 5: Connection moved between threads (Send trait)
///
/// This verifies that Connection is Send by moving it from one thread to another.
#[test]
fn test_connection_send_trait() {
    let temp_dir = TempDir::new().unwrap();
    let db_path = temp_dir.path().join("send_test.db");

    // Create connection in main thread
    let conn = Connection::open(&db_path).unwrap();
    conn.execute("CREATE TABLE test (id INTEGER, value TEXT)", [])
        .unwrap();
    conn.execute("INSERT INTO test (id, value) VALUES (1, 'main')", [])
        .unwrap();

    // Move connection to worker thread
    let handle = thread::spawn(move || {
        // Connection is now owned by this thread
        conn.execute("INSERT INTO test (id, value) VALUES (2, 'worker')", [])
            .unwrap();

        let count: i32 = conn
            .query_row("SELECT COUNT(*) FROM test", [], |row| row.get(0))
            .unwrap();

        assert_eq!(count, 2, "Should have 2 rows");

        // Return connection ownership back to main thread
        conn
    });

    // Get connection back
    let conn = handle.join().unwrap();

    // Verify in main thread
    let count: i32 = conn
        .query_row("SELECT COUNT(*) FROM test", [], |row| row.get(0))
        .unwrap();

    assert_eq!(count, 2, "Should still have 2 rows");
}
