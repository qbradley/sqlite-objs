# sqlite-objs

[![Crates.io](https://img.shields.io/crates/v/sqlite-objs.svg)](https://crates.io/crates/sqlite-objs)
[![Docs.rs](https://docs.rs/sqlite-objs/badge.svg)](https://docs.rs/sqlite-objs)

Safe Rust bindings for [sqlite-objs](https://github.com/qbradley/sqlite-objs) — a SQLite VFS that stores database files in **Azure Blob Storage**.

## Features

- 🌐 **Cloud-native SQLite** — Store databases in Azure Blob Storage (page blobs for DB, block blobs for journal/WAL)
- 🔒 **Safe concurrency** — Blob lease-based locking for multi-process/multi-node write access
- ⚡ **Performance** — Full-blob caching with ETag revalidation, lazy page loading, and configurable prefetch
- 🔐 **Flexible authentication** — SAS tokens, Shared Keys, or per-database URI credentials
- 📊 **Observability** — Detailed VFS activity metrics (cache hits, blob I/O, lease operations)

## Installation

Add this to your `Cargo.toml`:

```toml
[dependencies]
sqlite-objs = "0.1.6-alpha.1"
rusqlite = "0.38"
```

### Build Requirements

You'll need OpenSSL and libcurl headers installed. See [sqlite-objs-sys](https://crates.io/crates/sqlite-objs-sys) for details.

## Quick Start

### 1. Basic Usage (Environment Variables)

```rust
use sqlite_objs::SqliteObjsVfs;
use rusqlite::Connection;

// Register VFS from environment variables:
// AZURE_STORAGE_ACCOUNT, AZURE_STORAGE_CONTAINER, AZURE_STORAGE_SAS
SqliteObjsVfs::register(false)?;

let conn = Connection::open_with_flags_and_vfs(
    "mydb.db",
    rusqlite::OpenFlags::SQLITE_OPEN_READ_WRITE | rusqlite::OpenFlags::SQLITE_OPEN_CREATE,
    "sqlite-objs"
)?;

conn.execute("CREATE TABLE IF NOT EXISTS users (id INTEGER PRIMARY KEY, name TEXT)", [])?;
# Ok::<(), Box<dyn std::error::Error>>(())
```

### 2. URI Mode (Per-Database Credentials)

```rust
use sqlite_objs::{SqliteObjsVfs, UriBuilder};
use rusqlite::Connection;

// Register VFS in URI mode (no global config required)
SqliteObjsVfs::register_uri(false)?;

// Build URI with automatic URL encoding
let uri = UriBuilder::new("mydb.db", "myaccount", "databases")
    .sas_token("sv=2024-08-04&ss=b&srt=sco&sp=rwdlacyx&se=2026-01-01T00:00:00Z&sig=abc123")
    .cache_dir("/var/cache/myapp")
    .cache_reuse(true)  // Persist cache across connections (ETag revalidation)
    .try_build()?;

let conn = Connection::open_with_flags_and_vfs(
    &uri,
    rusqlite::OpenFlags::SQLITE_OPEN_READ_WRITE 
        | rusqlite::OpenFlags::SQLITE_OPEN_CREATE 
        | rusqlite::OpenFlags::SQLITE_OPEN_URI,
    "sqlite-objs"
)?;
# Ok::<(), Box<dyn std::error::Error>>(())
```

### 3. Explicit Configuration

```rust
use sqlite_objs::{SqliteObjsVfs, SqliteObjsConfig};

let config = SqliteObjsConfig {
    account: "myaccount".to_string(),
    container: "databases".to_string(),
    sas_token: Some("sv=2024-08-04&...".to_string()),
    account_key: None,
    endpoint: None,  // Or Some("http://127.0.0.1:10000".to_string()) for Azurite
};

SqliteObjsVfs::register_with_config(&config, false)?;
# Ok::<(), Box<dyn std::error::Error>>(())
```

## Features

### `rusqlite`

Enables integration with `rusqlite::Connection` for metrics and pragmas:

```rust
use sqlite_objs::pragmas;

let metrics = pragmas::get_stats(&conn)?;
println!("Cache hits: {}, Blob reads: {}", metrics.cache_hits, metrics.blob_reads);

pragmas::reset_stats(&conn)?;
let downloads = pragmas::get_download_count(&conn)?;
```

Enable it when you want the `pragmas` helpers:
```toml
[dependencies]
sqlite-objs = { version = "0.1.6-alpha.1", features = ["rusqlite"] }
```

## Advanced Features

### Prefetch Modes

```rust
use sqlite_objs::{UriBuilder, PrefetchMode};

// Lazy loading — only fetch pages on demand (good for large DBs with sparse access)
let uri = UriBuilder::new("big.db", "acct", "cont")
    .sas_token("tok")
    .prefetch(PrefetchMode::None)
    .try_build()?;
```

### Cache Reuse

Enable persistent cache files across connections to avoid re-downloads:

```rust
let uri = UriBuilder::new("mydb.db", "acct", "cont")
    .sas_token("tok")
    .cache_dir("/var/cache/myapp")
    .cache_reuse(true)  // Keep cache file, revalidate via ETag
    .try_build()?;
```

## Metrics

When the `rusqlite` feature is enabled, you can query VFS activity metrics:

```rust
use sqlite_objs::{pragmas, metrics::VfsMetrics};

let m: VfsMetrics = pragmas::get_stats(&conn)?;

println!("Disk I/O: {} reads ({} bytes), {} writes ({} bytes)",
    m.disk_reads, m.disk_bytes_read, m.disk_writes, m.disk_bytes_written);
println!("Blob I/O: {} reads ({} bytes), {} writes ({} bytes)",
    m.blob_reads, m.blob_bytes_read, m.blob_writes, m.blob_bytes_written);
println!("Cache: {} hits, {} misses ({} pages fetched)",
    m.cache_hits, m.cache_misses, m.cache_miss_pages);
```

All counters are documented in [`VfsMetrics`](https://docs.rs/sqlite-objs/latest/sqlite_objs/metrics/struct.VfsMetrics.html).

## Error Handling

```rust
use sqlite_objs::{SqliteObjsVfs, SqliteObjsError};

match SqliteObjsVfs::register(false) {
    Ok(()) => println!("VFS registered"),
    Err(SqliteObjsError::RegistrationFailed(msg)) => eprintln!("Failed: {}", msg),
    Err(e) => eprintln!("Error: {}", e),
}
```

## Repository

Full documentation, examples, and C source code: [github.com/qbradley/sqlite-objs](https://github.com/qbradley/sqlite-objs)

## License

MIT
