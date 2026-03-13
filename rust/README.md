# sqlite-objs Rust Bindings

Safe Rust bindings for sqlite-objs - a SQLite VFS backed by Azure Blob Storage.

## Structure

This workspace contains two crates:

- **`sqlite-objs-sys`** - Raw FFI bindings to the sqlite-objs C library
- **`sqlite-objs`** - Safe, idiomatic Rust API

## Building

Requirements:
- Rust 1.70 or later
- libcurl, OpenSSL (linked dynamically)
- C compiler (for building the C sources)

```sh
cd rust
cargo build
```

## Testing

```sh
cargo test
```

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
