//! Chaos tests for sqlite-objs Rust layer.
//!
//! These tests exercise edge cases, error conditions, and forward-compatibility
//! scenarios in the Rust safe wrapper layer:
//!
//! 1. **Registration & Config**: Null bytes, empty strings, invalid data
//! 2. **URI Builder**: Special character encoding, empty values, extreme lengths
//! 3. **Metrics Parsing**: Malformed input, unknown keys (forward compat), extreme values
//! 4. **Threading/Contention**: Local-only multi-threaded stress tests
//!
//! Most tests are deterministic and CI-friendly (no real Azure required).
//! Azurite-backed tests are marked with `#[ignore]`.

use rusqlite::{Connection, OpenFlags};
use sqlite_objs::metrics::VfsMetrics;
use sqlite_objs::{PrefetchMode, SqliteObjsConfig, SqliteObjsError, SqliteObjsVfs, UriBuilder};
use std::sync::{Arc, Barrier};
use std::thread;
use tempfile::TempDir;

// =============================================================================
// Registration & Config Error Handling
// =============================================================================

#[test]
fn config_null_byte_in_account() {
    let config = SqliteObjsConfig {
        account: "my\0account".to_string(),
        container: "container".to_string(),
        sas_token: Some("token".to_string()),
        account_key: None,
        endpoint: None,
    };

    let result = SqliteObjsVfs::register_with_config(&config, false);
    assert!(
        matches!(result, Err(SqliteObjsError::InvalidConfig(msg)) if msg.contains("account")),
        "Should reject account with null byte"
    );
}

#[test]
fn config_null_byte_in_container() {
    let config = SqliteObjsConfig {
        account: "account".to_string(),
        container: "cont\0ainer".to_string(),
        sas_token: Some("token".to_string()),
        account_key: None,
        endpoint: None,
    };

    let result = SqliteObjsVfs::register_with_config(&config, false);
    assert!(
        matches!(result, Err(SqliteObjsError::InvalidConfig(msg)) if msg.contains("container")),
        "Should reject container with null byte"
    );
}

#[test]
fn config_null_byte_in_sas_token() {
    let config = SqliteObjsConfig {
        account: "account".to_string(),
        container: "container".to_string(),
        sas_token: Some("sv=2024\0&sig=test".to_string()),
        account_key: None,
        endpoint: None,
    };

    let result = SqliteObjsVfs::register_with_config(&config, false);
    assert!(
        matches!(result, Err(SqliteObjsError::InvalidConfig(msg)) if msg.contains("sas_token")),
        "Should reject SAS token with null byte"
    );
}

#[test]
fn config_null_byte_in_account_key() {
    let config = SqliteObjsConfig {
        account: "account".to_string(),
        container: "container".to_string(),
        sas_token: None,
        account_key: Some("key\0value".to_string()),
        endpoint: None,
    };

    let result = SqliteObjsVfs::register_with_config(&config, false);
    assert!(
        matches!(result, Err(SqliteObjsError::InvalidConfig(msg)) if msg.contains("account_key")),
        "Should reject account key with null byte"
    );
}

#[test]
fn config_null_byte_in_endpoint() {
    let config = SqliteObjsConfig {
        account: "account".to_string(),
        container: "container".to_string(),
        sas_token: Some("token".to_string()),
        account_key: None,
        endpoint: Some("http://127.0.0.1\0:10000".to_string()),
    };

    let result = SqliteObjsVfs::register_with_config(&config, false);
    assert!(
        matches!(result, Err(SqliteObjsError::InvalidConfig(msg)) if msg.contains("endpoint")),
        "Should reject endpoint with null byte"
    );
}

#[test]
fn config_empty_account() {
    let config = SqliteObjsConfig {
        account: "".to_string(),
        container: "container".to_string(),
        sas_token: Some("token".to_string()),
        account_key: None,
        endpoint: None,
    };

    // Empty account is valid at Rust layer (C layer will reject)
    let _ = SqliteObjsVfs::register_with_config(&config, false);
}

#[test]
fn config_empty_container() {
    let config = SqliteObjsConfig {
        account: "account".to_string(),
        container: "".to_string(),
        sas_token: Some("token".to_string()),
        account_key: None,
        endpoint: None,
    };

    // Empty container is valid at Rust layer (C layer will reject)
    let _ = SqliteObjsVfs::register_with_config(&config, false);
}

#[test]
fn config_both_sas_and_key() {
    let config = SqliteObjsConfig {
        account: "account".to_string(),
        container: "container".to_string(),
        sas_token: Some("sas_token".to_string()),
        account_key: Some("account_key".to_string()),
        endpoint: None,
    };

    // Both set is valid — SAS takes precedence
    let _ = SqliteObjsVfs::register_with_config(&config, false);
}

#[test]
fn config_neither_sas_nor_key() {
    let config = SqliteObjsConfig {
        account: "account".to_string(),
        container: "container".to_string(),
        sas_token: None,
        account_key: None,
        endpoint: None,
    };

    // No auth is valid at Rust layer (C layer will use env or fail)
    let _ = SqliteObjsVfs::register_with_config(&config, false);
}

// =============================================================================
// URI Builder Edge Cases
// =============================================================================

#[test]
fn uri_builder_special_characters_in_sas() {
    let sas = "sv=2024-08-04&ss=b&srt=sco&sp=rwdlacyx&se=2026-01-01T00:00:00Z&sig=abc123+/=";
    let uri = UriBuilder::new("test.db", "account", "container")
        .sas_token(sas)
        .build();

    // Should be percent-encoded
    assert!(
        uri.contains("azure_sas=sv%3D2024"),
        "Should encode '=' in SAS token"
    );
    assert!(uri.contains("%26ss%3Db"), "Should encode '&' in SAS token");
    assert!(uri.contains("%2B"), "Should encode '+' in SAS token");
    assert!(uri.contains("%2F"), "Should encode '/' in SAS token");
}

#[test]
fn uri_builder_unicode_in_database_name() {
    let uri = UriBuilder::new("数据库.db", "account", "container")
        .sas_token("token")
        .build();

    // Unicode should be percent-encoded in the database filename.
    assert!(
        uri.starts_with("file:%E6%95%B0%E6%8D%AE%E5%BA%93.db?"),
        "Should encode Unicode database name, got: {uri}"
    );
    assert!(uri.contains("azure_account=account"), "Should have account");
}

#[test]
fn uri_builder_spaces_in_values() {
    let uri = UriBuilder::new("my db.db", "my account", "my container")
        .sas_token("my token")
        .cache_dir("/path/with spaces")
        .build();

    // Spaces should be percent-encoded as %20 in path and query values.
    assert!(uri.contains("%20"), "Should encode spaces, got: {}", uri);
    assert!(
        uri.starts_with("file:my%20db.db?"),
        "Should encode spaces in database name, got: {uri}"
    );
}

#[test]
fn uri_builder_reserved_chars_in_database_name() {
    let uri = UriBuilder::new("my database?.db&v=1", "account", "container")
        .sas_token("token")
        .build();

    assert!(
        uri.starts_with("file:my%20database%3F.db%26v%3D1?"),
        "Should encode reserved characters in database name, got: {uri}"
    );
    assert!(
        !uri.starts_with("file:my database?.db&v=1?"),
        "Database name must not be emitted raw"
    );
}

#[test]
fn uri_builder_empty_database() {
    let uri = UriBuilder::new("", "account", "container")
        .sas_token("token")
        .build();

    assert!(uri.starts_with("file:?"), "Empty database should work");
}

#[test]
fn uri_builder_empty_account() {
    let uri = UriBuilder::new("test.db", "", "container")
        .sas_token("token")
        .build();

    assert!(
        uri.contains("azure_account=&"),
        "Empty account should produce empty parameter value"
    );
}

#[test]
fn uri_builder_empty_container() {
    let uri = UriBuilder::new("test.db", "account", "")
        .sas_token("token")
        .build();

    assert!(
        uri.contains("azure_container=&"),
        "Empty container should produce empty parameter value"
    );
}

#[test]
fn uri_builder_empty_sas_token() {
    let uri = UriBuilder::new("test.db", "account", "container")
        .sas_token("")
        .build();

    assert!(
        uri.contains("azure_sas="),
        "Empty SAS token should be included"
    );
}

#[test]
fn uri_builder_all_prefetch_modes() {
    let uri_all = UriBuilder::new("test.db", "account", "container")
        .sas_token("token")
        .prefetch(PrefetchMode::All)
        .build();

    let uri_none = UriBuilder::new("test.db", "account", "container")
        .sas_token("token")
        .prefetch(PrefetchMode::None)
        .build();

    assert!(
        uri_all.contains("&prefetch=all"),
        "Should include prefetch=all"
    );
    assert!(
        uri_none.contains("&prefetch=none"),
        "Should include prefetch=none"
    );
}

#[test]
fn uri_builder_cache_reuse_true() {
    let uri = UriBuilder::new("test.db", "account", "container")
        .sas_token("token")
        .cache_reuse(true)
        .build();

    assert!(
        uri.contains("&cache_reuse=1"),
        "Should include cache_reuse=1"
    );
}

#[test]
fn uri_builder_cache_reuse_false() {
    let uri = UriBuilder::new("test.db", "account", "container")
        .sas_token("token")
        .cache_reuse(false)
        .build();

    assert!(
        !uri.contains("cache_reuse"),
        "Should not include cache_reuse when false"
    );
}

#[test]
fn uri_builder_endpoint_azurite() {
    let uri = UriBuilder::new("test.db", "devstoreaccount1", "testcontainer")
        .sas_token("token")
        .endpoint("http://127.0.0.1:10000/devstoreaccount1")
        .build();

    assert!(
        uri.contains("azure_endpoint=http%3A%2F%2F127.0.0.1%3A10000%2Fdevstoreaccount1"),
        "Should percent-encode endpoint URL, got: {}",
        uri
    );
}

#[test]
fn uri_builder_very_long_values() {
    let long_account = "a".repeat(1000);
    let long_container = "c".repeat(1000);
    let long_sas = "t".repeat(2000);

    let uri = UriBuilder::new("test.db", &long_account, &long_container)
        .sas_token(&long_sas)
        .build();

    assert!(uri.len() > 3000, "Should handle very long values");
    assert!(uri.contains("azure_account="), "Should still be valid URI");
}

#[test]
fn uri_builder_special_chars_in_endpoint() {
    let uri = UriBuilder::new("test.db", "account", "container")
        .sas_token("token")
        .endpoint("https://account.blob.core.windows.net/path?query=value&other=1")
        .build();

    // All special chars in endpoint should be encoded
    assert!(
        uri.contains("%3F") && uri.contains("%3D") && uri.contains("%26"),
        "Should encode ?, =, & in endpoint, got: {}",
        uri
    );
}

#[test]
fn uri_builder_percent_sign_in_value() {
    let uri = UriBuilder::new("test.db", "account", "container")
        .sas_token("sig=ABC%DEF")
        .build();

    // Percent sign should itself be encoded as %25
    assert!(
        uri.contains("%25"),
        "Should encode % sign itself, got: {}",
        uri
    );
}

#[test]
fn uri_builder_hash_in_cache_dir() {
    let uri = UriBuilder::new("test.db", "account", "container")
        .sas_token("token")
        .cache_dir("/tmp/cache#123")
        .build();

    // Hash should be encoded
    assert!(
        uri.contains("%23"),
        "Should encode # in cache_dir, got: {}",
        uri
    );
}

// =============================================================================
// Metrics Parsing Chaos
// =============================================================================

#[test]
fn metrics_parse_malformed_no_equals() {
    let text = "disk_reads 10";
    let result = VfsMetrics::parse(text);
    assert!(
        matches!(result, Err(ref e) if e.message.contains("key=value")),
        "Should reject lines without '='"
    );
}

#[test]
fn metrics_parse_malformed_non_integer() {
    let text = "disk_reads=not_a_number";
    let result = VfsMetrics::parse(text);
    assert!(
        matches!(result, Err(ref e) if e.message.contains("disk_reads")),
        "Should reject non-integer values"
    );
}

#[test]
fn metrics_parse_malformed_float() {
    let text = "cache_hits=100.5";
    let result = VfsMetrics::parse(text);
    assert!(result.is_err(), "Should reject float values (only i64)");
}

#[test]
fn metrics_parse_malformed_overflow() {
    // i64::MAX + 1 should overflow
    let text = format!("disk_reads={}", (i64::MAX as u128) + 1);
    let result = VfsMetrics::parse(&text);
    assert!(result.is_err(), "Should reject values that overflow i64");
}

#[test]
fn metrics_parse_empty_key() {
    let text = "=123";
    let result = VfsMetrics::parse(text);
    // Empty key is unknown, so should be ignored (forward compat)
    assert!(result.is_ok(), "Should ignore empty key");
}

#[test]
fn metrics_parse_empty_value() {
    let text = "disk_reads=";
    let result = VfsMetrics::parse(text);
    assert!(
        result.is_err(),
        "Should reject empty value (not a valid integer)"
    );
}

#[test]
fn metrics_parse_negative_values() {
    let text = "disk_reads=-42\ncache_hits=-100";
    let m = VfsMetrics::parse(text).expect("Should parse negative values");
    assert_eq!(m.disk_reads, -42);
    assert_eq!(m.cache_hits, -100);
}

#[test]
fn metrics_parse_extreme_values() {
    let text = format!("disk_reads={}\ncache_hits={}", i64::MAX, i64::MIN);
    let m = VfsMetrics::parse(&text).expect("Should parse extreme i64 values");
    assert_eq!(m.disk_reads, i64::MAX);
    assert_eq!(m.cache_hits, i64::MIN);
}

#[test]
fn metrics_parse_leading_zeros() {
    let text = "disk_reads=00042\ncache_hits=000";
    let m = VfsMetrics::parse(text).expect("Should parse leading zeros");
    assert_eq!(m.disk_reads, 42);
    assert_eq!(m.cache_hits, 0);
}

#[test]
fn metrics_parse_leading_plus_sign() {
    let text = "disk_reads=+42";
    let m = VfsMetrics::parse(text).expect("Should parse leading + sign");
    assert_eq!(m.disk_reads, 42);
}

#[test]
fn metrics_parse_unknown_keys_ignored_forward_compat() {
    let text = "\
disk_reads=10
future_counter_v2=999
cache_hits=20
new_metric_2027=123456
blob_reads=5";

    let m = VfsMetrics::parse(text).expect("Should ignore unknown keys");
    assert_eq!(m.disk_reads, 10);
    assert_eq!(m.cache_hits, 20);
    assert_eq!(m.blob_reads, 5);
    // Unknown keys should be silently ignored, not cause errors
}

#[test]
fn metrics_parse_revalidation_counters() {
    let text = "\
revalidations=10
revalidation_downloads=2
revalidation_diffs=7
revalidation_busy=1
pages_invalidated=42";

    let m = VfsMetrics::parse(text).expect("Should parse revalidation counters");
    assert_eq!(m.revalidations, 10);
    assert_eq!(m.revalidation_downloads, 2);
    assert_eq!(m.revalidation_diffs, 7);
    assert_eq!(m.revalidation_busy, 1);
    assert_eq!(m.pages_invalidated, 42);
}

#[test]
fn metrics_parse_chaos_counters() {
    let text = "\
azure_errors=5
revalidations=100
revalidation_busy=10";

    let m = VfsMetrics::parse(text).expect("Should parse chaos/error counters");
    assert_eq!(m.azure_errors, 5, "Should track Azure errors");
    assert_eq!(m.revalidations, 100, "Should track revalidations");
    assert_eq!(
        m.revalidation_busy, 10,
        "Should track SQLITE_BUSY from revalidation"
    );
}

#[test]
fn metrics_parse_missing_all_keys() {
    let text = "";
    let m = VfsMetrics::parse(text).expect("Should parse empty string");
    assert_eq!(m, VfsMetrics::default(), "All fields should be zero");
}

#[test]
fn metrics_parse_only_blank_lines() {
    let text = "\n\n\n\n";
    let m = VfsMetrics::parse(text).expect("Should parse blank lines");
    assert_eq!(m, VfsMetrics::default());
}

#[test]
fn metrics_parse_mixed_whitespace() {
    // After fix: Parser should trim key/value after split
    let text = "  disk_reads  =  10  \n\t\tcache_hits\t=\t20\t\n";
    let m = VfsMetrics::parse(text).expect("Should handle whitespace around '='");
    assert_eq!(m.disk_reads, 10);
    assert_eq!(m.cache_hits, 20);
}

#[test]
fn metrics_parse_whitespace_edge_cases() {
    // Verify various whitespace combinations work
    let text = "disk_reads=10\n  cache_hits  =  20  \n\tblob_reads\t=\t5\t";
    let m = VfsMetrics::parse(text).expect("Should handle mixed whitespace");
    assert_eq!(m.disk_reads, 10);
    assert_eq!(m.cache_hits, 20);
    assert_eq!(m.blob_reads, 5);
}

#[test]
fn metrics_parse_duplicate_keys_last_wins() {
    let text = "disk_reads=10\ndisk_reads=20\ndisk_reads=30";
    let m = VfsMetrics::parse(text).expect("Should parse duplicate keys");
    assert_eq!(m.disk_reads, 30, "Last value should win");
}

#[test]
fn metrics_parse_multiple_equals_signs() {
    let text = "disk_reads=10=20";
    let result = VfsMetrics::parse(text);
    // split_once will split on first '=', so value is "10=20"
    assert!(
        result.is_err(),
        "Should reject value '10=20' as non-integer"
    );
}

#[test]
fn metrics_parse_windows_line_endings() {
    let text = "disk_reads=10\r\ncache_hits=20\r\n";
    let m = VfsMetrics::parse(text).expect("Should handle CRLF");
    assert_eq!(m.disk_reads, 10);
    assert_eq!(m.cache_hits, 20);
}

#[test]
fn metrics_parse_mixed_line_endings() {
    let text = "disk_reads=10\ncache_hits=20\r\nblob_reads=5\r";
    let m = VfsMetrics::parse(text).expect("Should handle mixed line endings");
    assert_eq!(m.disk_reads, 10);
    assert_eq!(m.cache_hits, 20);
    assert_eq!(m.blob_reads, 5);
}

#[test]
fn metrics_field_count_constant() {
    // Verify FIELD_COUNT matches actual struct
    assert_eq!(
        VfsMetrics::FIELD_COUNT,
        28,
        "FIELD_COUNT should match actual number of fields"
    );
}

// =============================================================================
// Local Threading/Contention Tests (No Azure)
// =============================================================================

#[test]
fn threading_concurrent_uri_builder() {
    let thread_count = 10;
    let mut handles = vec![];
    let barrier = Arc::new(Barrier::new(thread_count));

    for i in 0..thread_count {
        let b = barrier.clone();
        let handle = thread::spawn(move || {
            b.wait();

            // Each thread builds URIs independently
            let uri = UriBuilder::new(&format!("db{}.db", i), "account", "container")
                .sas_token(&format!("token_{}", i))
                .cache_dir(&format!("/cache/{}", i))
                .cache_reuse(i % 2 == 0)
                .prefetch(if i % 2 == 0 {
                    PrefetchMode::All
                } else {
                    PrefetchMode::None
                })
                .build();

            assert!(uri.contains(&format!("db{}.db", i)));
            assert!(uri.contains(&format!("token_{}", i)));
        });
        handles.push(handle);
    }

    for h in handles {
        h.join().expect("Thread should not panic");
    }
}

#[test]
fn threading_concurrent_metrics_parsing() {
    let thread_count = 10;
    let mut handles = vec![];
    let barrier = Arc::new(Barrier::new(thread_count));

    for i in 0..thread_count {
        let b = barrier.clone();
        let handle = thread::spawn(move || {
            b.wait();

            // Each thread parses metrics independently
            let text = format!(
                "disk_reads={}\ncache_hits={}\nblob_reads={}",
                i * 10,
                i * 100,
                i * 5
            );

            let m = VfsMetrics::parse(&text).expect("Parse should succeed");
            assert_eq!(m.disk_reads, (i * 10) as i64);
            assert_eq!(m.cache_hits, (i * 100) as i64);
            assert_eq!(m.blob_reads, (i * 5) as i64);
        });
        handles.push(handle);
    }

    for h in handles {
        h.join().expect("Thread should not panic");
    }
}

#[test]
fn threading_local_db_separate_connections_chaos() {
    // Stress test with local SQLite file, no VFS, many threads
    let temp_dir = TempDir::new().unwrap();
    let db_path = temp_dir.path().join("chaos.db");

    // Initialize DB
    {
        let conn = Connection::open(&db_path).unwrap();
        conn.execute(
            "CREATE TABLE chaos (id INTEGER PRIMARY KEY, thread_id INTEGER, value TEXT)",
            [],
        )
        .unwrap();
    }

    let path_str = db_path.to_str().unwrap().to_string();
    let thread_count = 20;
    let ops_per_thread = 10;
    let mut handles = vec![];

    for thread_id in 0..thread_count {
        let path = path_str.clone();
        let handle = thread::spawn(move || {
            let conn =
                Connection::open_with_flags(&path, OpenFlags::SQLITE_OPEN_READ_WRITE).unwrap();

            for op in 0..ops_per_thread {
                // Chaos: random mix of inserts, reads, updates
                match op % 3 {
                    0 => {
                        // Insert
                        conn.execute(
                            "INSERT INTO chaos (thread_id, value) VALUES (?1, ?2)",
                            rusqlite::params![thread_id, format!("t{}v{}", thread_id, op)],
                        )
                        .expect("Insert failed");
                    }
                    1 => {
                        // Read
                        let count: i32 = conn
                            .query_row(
                                "SELECT COUNT(*) FROM chaos WHERE thread_id = ?1",
                                [thread_id],
                                |row| row.get(0),
                            )
                            .expect("Read failed");
                        assert!(count >= 0, "Count should be non-negative");
                    }
                    2 => {
                        // Update (may affect 0 rows if inserts haven't run yet)
                        conn.execute(
                            "UPDATE chaos SET value = ?1 WHERE thread_id = ?2",
                            rusqlite::params![format!("updated_{}", op), thread_id],
                        )
                        .expect("Update failed");
                    }
                    _ => unreachable!(),
                }
            }

            thread_id
        });
        handles.push(handle);
    }

    for h in handles {
        h.join().expect("Thread should not panic");
    }

    // Verify final state
    let conn = Connection::open(&db_path).unwrap();
    let total: i32 = conn
        .query_row("SELECT COUNT(*) FROM chaos", [], |row| row.get(0))
        .unwrap();

    // Each thread does ops_per_thread / 3 inserts (roughly)
    let expected_min = thread_count * (ops_per_thread / 3) - thread_count;
    assert!(
        total >= expected_min,
        "Should have at least {} rows, got {}",
        expected_min,
        total
    );
}

// =============================================================================
// Azurite-backed Chaos Tests (Ignored by Default)
// =============================================================================

/// Test URI mode with Azurite under concurrent load.
///
/// Run with: cargo test --test chaos -- --ignored azurite_uri_chaos
#[test]
#[ignore]
fn azurite_uri_chaos() {
    // Register VFS in URI mode
    SqliteObjsVfs::register_uri(false).expect("VFS registration should succeed");

    let thread_count = 5;
    let ops_per_thread = 3;
    let mut handles = vec![];
    let barrier = Arc::new(Barrier::new(thread_count));

    for thread_id in 0..thread_count {
        let b = barrier.clone();
        let handle = thread::spawn(move || {
            b.wait();

            // Each thread uses its own database and connection
            let db_name = format!("chaos_{}.db", thread_id);
            let uri = UriBuilder::new(&db_name, "devstoreaccount1", "testcontainer")
                .account_key(
                    "Eby8vdM02xNOcqFlqUwJPLlmEtlCDXJ1OUzFT50uSRZ6IFsuFq2UVErCz4I6tq/K1SZFPTOtr/KBHBeksoGMGw==",
                )
                .endpoint("http://127.0.0.1:10000/devstoreaccount1")
                .build();

            let conn = Connection::open_with_flags_and_vfs(
                &uri,
                OpenFlags::SQLITE_OPEN_READ_WRITE
                    | OpenFlags::SQLITE_OPEN_CREATE
                    | OpenFlags::SQLITE_OPEN_URI,
                "sqlite-objs",
            )
            .expect("Failed to open connection");

            conn.execute(
                "CREATE TABLE IF NOT EXISTS test (id INTEGER, value TEXT)",
                [],
            )
            .expect("CREATE failed");

            for op in 0..ops_per_thread {
                conn.execute(
                    "INSERT INTO test (id, value) VALUES (?1, ?2)",
                    rusqlite::params![op, format!("thread_{}_op_{}", thread_id, op)],
                )
                .expect("INSERT failed");
            }

            let count: i32 = conn
                .query_row("SELECT COUNT(*) FROM test", [], |row| row.get(0))
                .expect("SELECT failed");

            assert_eq!(count, ops_per_thread, "Should have inserted all rows");

            thread_id
        });
        handles.push(handle);
    }

    for h in handles {
        h.join().expect("Thread should not panic");
    }
}

/// Test config registration errors with Azurite (missing credentials).
///
/// Run with: cargo test --test chaos -- --ignored azurite_config_errors
#[test]
#[ignore]
fn azurite_config_errors() {
    // Config with missing credentials should fail at C layer
    let config = SqliteObjsConfig {
        account: "devstoreaccount1".to_string(),
        container: "testcontainer".to_string(),
        sas_token: None,
        account_key: None,
        endpoint: Some("http://127.0.0.1:10000/devstoreaccount1".to_string()),
    };

    let result = SqliteObjsVfs::register_with_config(&config, false);
    // Without credentials, C layer should reject (unless env vars are set)
    // This test documents the behavior; exact error depends on C implementation
    let _ = result;
}
