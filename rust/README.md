# sqlite-objs Rust Bindings

Safe Rust bindings for sqlite-objs - a SQLite VFS backed by Azure Blob Storage.

## Structure

This workspace contains two crates:

- **`sqlite-objs-sys`** - Raw FFI bindings to the sqlite-objs C library
- **`sqlite-objs`** - Safe, idiomatic Rust API

## Building

Requirements:
- Rust 1.82 or later
- libcurl, OpenSSL (linked dynamically)
- C compiler (for building the C sources)

```sh
cd rust
cargo build
```

## Testing

### Unit Tests (FFI Layer)

```sh
cargo test -p sqlite-objs-sys
```

Tests FFI bindings:
- `test_config_size` — Verify C struct layouts match Rust FFI
- `test_register_uri` — Verify URI registration works
- `test_fcntl_constants_match_header` — Check FCNTL constants

### Integration Tests (High-Level API)

```sh
cargo test -p sqlite-objs
```

Tests safe Rust API:
- URI builder URL encoding
- Error handling and conversions
- Configuration validation

### Performance Benchmarks (Requires Azurite)

```sh
# Start Azurite first
npm install -g azurite
azurite-blob --silent --location /tmp/azurite &

# Run performance matrix tests
cargo test --test perf_matrix -- --nocapture

# What it benchmarks:
#  - Local SQLite (baseline)
#  - sqlite-objs with Azurite (network latency)
#  - Various workload patterns (OLTP, OLAP, etc.)
```

### Full Rust Validation

```sh
# Local tests only (no dependencies)
cargo test --workspace

# With Azurite (performance benchmarks)
cd rust && cargo test --tests
```

## Contributing

See main project [TEST_DOCS_INDEX.md](../TEST_DOCS_INDEX.md) for:
- Fast gate commands
- Sanitizer validation (`make sanitize`)
- Stress testing modes
- TCL test suite
- Known testing gaps

## Example

See `sqlite-objs/examples/basic.rs`:

```sh
cargo run --example basic
```

## Usage

Add to your `Cargo.toml`:

```toml
[dependencies]
sqlite-objs = { path = "path/to/rust/sqlite-objs" }
rusqlite = "0.32"
```

Then in your code:

```rust
use sqlite_objs::SqliteObjsVfs;
use rusqlite::Connection;

// Register VFS in URI mode
SqliteObjsVfs::register_uri(false)?;

// Open database with Azure credentials in URI
let conn = Connection::open_with_flags_and_vfs(
    "file:mydb.db?azure_account=myaccount&azure_container=databases&azure_sas=sv=2024...",
    OpenFlags::SQLITE_OPEN_READ_WRITE | OpenFlags::SQLITE_OPEN_CREATE | OpenFlags::SQLITE_OPEN_URI,
    "sqlite-objs"
)?;
```

## License

MIT
