# sqlite-objs Rust Quick Start

## Installation

```toml
[dependencies]
sqlite-objs = { path = "../path/to/sqlite-objs/rust/sqlite-objs" }
rusqlite = "0.32"
```

Or when published to crates.io:

```toml
[dependencies]
sqlite-objs = "0.1"
rusqlite = "0.32"
```

## Requirements

- Rust 1.70+
- System libraries: libcurl, OpenSSL
- macOS: `brew install curl openssl` (usually already present)
- Linux: `apt-get install libcurl4-openssl-dev libssl-dev`

## Usage

### URI Mode (Recommended)

```rust
use sqlite_objs::SqliteObjsVfs;
use rusqlite::{Connection, OpenFlags};

// Register VFS once at startup
SqliteObjsVfs::register_uri(false)?;

// Open database with Azure credentials in URI
let conn = Connection::open_with_flags_and_vfs(
    "file:mydb.db?azure_account=myaccount&azure_container=databases&azure_sas=sv=2024...",
    OpenFlags::SQLITE_OPEN_READ_WRITE 
        | OpenFlags::SQLITE_OPEN_CREATE 
        | OpenFlags::SQLITE_OPEN_URI,
    "sqlite-objs"
)?;

// Use standard rusqlite API
conn.execute("CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT)", [])?;
```

### Environment Variable Mode

```rust
use sqlite_objs::SqliteObjsVfs;
use rusqlite::{Connection, OpenFlags};

// Set environment variables first:
// export AZURE_STORAGE_ACCOUNT=myaccount
// export AZURE_STORAGE_CONTAINER=databases
// export AZURE_STORAGE_SAS='sv=2024-08-04&...'

SqliteObjsVfs::register(false)?;

let conn = Connection::open_with_flags_and_vfs(
    "mydb.db",
    OpenFlags::SQLITE_OPEN_READ_WRITE | OpenFlags::SQLITE_OPEN_CREATE,
    "sqlite-objs"
)?;
```

### Explicit Configuration

```rust
use sqlite_objs::{SqliteObjsVfs, SqliteObjsConfig};

let config = SqliteObjsConfig {
    account: "myaccount".to_string(),
    container: "databases".to_string(),
    sas_token: Some("sv=2024-08-04&...".to_string()),
    account_key: None,
    endpoint: None, // or Some("http://127.0.0.1:10000/devstoreaccount1".to_string()) for Azurite
};

SqliteObjsVfs::register_with_config(&config, false)?;
```

## Testing with Azurite

```bash
# Start Azurite (Azure emulator)
azurite-blob --silent --location /tmp/azurite &

# Use Azurite endpoint in your config
let config = SqliteObjsConfig {
    account: "devstoreaccount1".to_string(),
    container: "databases".to_string(),
    sas_token: None,
    account_key: Some("Eby8vdM02xNOcqFlqUwJPLlmEtlCDXJ1OUzFT50uSRZ6IFsuFq2UVErCz4I6tq/K1SZFPTOtr/KBHBeksoGMGw==".to_string()),
    endpoint: Some("http://127.0.0.1:10000/devstoreaccount1".to_string()),
};
```

## Error Handling

```rust
use sqlite_objs::{SqliteObjsVfs, SqliteObjsError};

match SqliteObjsVfs::register_uri(false) {
    Ok(_) => println!("VFS registered"),
    Err(SqliteObjsError::InvalidConfig(msg)) => eprintln!("Config error: {}", msg),
    Err(SqliteObjsError::RegistrationFailed(msg)) => eprintln!("Registration failed: {}", msg),
    Err(e) => eprintln!("Error: {}", e),
}
```

## Build from Source

```bash
cd rust
cargo build --release
cargo test
cargo run --example basic
```
