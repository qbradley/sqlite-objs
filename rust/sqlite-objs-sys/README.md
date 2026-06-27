# sqlite-objs-sys

[![Crates.io](https://img.shields.io/crates/v/sqlite-objs-sys.svg)](https://crates.io/crates/sqlite-objs-sys)
[![Docs.rs](https://docs.rs/sqlite-objs-sys/badge.svg)](https://docs.rs/sqlite-objs-sys)

Raw FFI bindings to the [sqlite-objs](https://github.com/qbradley/sqlite-objs) C library.

**sqlite-objs** is a SQLite VFS (Virtual File System) that stores database files in Azure Blob Storage, enabling cloud-native SQLite deployments with blob lease-based locking.

## ⚠️ Safety Notice

This crate provides **unsafe**, low-level FFI bindings to C functions. Most users should use the **[`sqlite-objs`](https://crates.io/crates/sqlite-objs)** crate instead, which wraps these bindings in a safe, idiomatic Rust API.

Use this crate only if you need direct access to the C API or are implementing custom wrappers.

## Installation

Add this to your `Cargo.toml`:

```toml
[dependencies]
sqlite-objs-sys = "0.1.6-alpha.1"
```

## Build Requirements

The crate bundles the sqlite-objs C sources and compiles them during the build process. You'll need:

- **C11 compiler** (GCC, Clang, or MSVC)
- **OpenSSL** headers and libraries (libssl-dev, openssl-devel, or Homebrew openssl on macOS)
- **libcurl** headers and libraries

On Debian/Ubuntu:
```bash
sudo apt-get install build-essential libssl-dev libcurl4-openssl-dev
```

On macOS (Homebrew):
```bash
brew install openssl curl
```

The build script (`build.rs`) automatically:
- Locates OpenSSL using `pkg-config` or Homebrew (macOS)
- Links against the bundled SQLite from `libsqlite3-sys`
- Compiles `sqlite_objs_vfs.c`, `azure_client.c`, `azure_auth.c`, and `azure_error.c`

## FFI API

All C functions and types are declared in `lib.rs`. Key exports:

### VFS Registration

```rust
use sqlite_objs_sys::*;
use std::ffi::CString;
use std::ptr;

let account = CString::new("myaccount").unwrap();
let container = CString::new("databases").unwrap();
let sas = CString::new("sv=2024-08-04&...").unwrap();

unsafe {
    // Register VFS from environment variables
    let rc = sqlite_objs_vfs_register(0);  // 0 = don't make default

    // Register with explicit config
    let config = sqlite_objs_config_t {
        account: account.as_ptr(),
        container: container.as_ptr(),
        sas_token: sas.as_ptr(),
        account_key: ptr::null(),
        endpoint: ptr::null(),
        ops: ptr::null(),
        ops_ctx: ptr::null_mut(),
    };
    let rc = sqlite_objs_vfs_register_with_config(&config, 0);

    // Register in URI mode (per-database credentials)
    let rc = sqlite_objs_vfs_register_uri(0);
}
```

### Configuration Struct

```rust
#[repr(C)]
pub struct sqlite_objs_config_t {
    pub account: *const c_char,
    pub container: *const c_char,
    pub sas_token: *const c_char,
    pub account_key: *const c_char,
    pub endpoint: *const c_char,
    pub ops: *const azure_ops_t,
    pub ops_ctx: *mut c_void,
}
```

### File Control Opcodes

```rust
pub const SQLITE_OBJS_FCNTL_DOWNLOAD_COUNT: c_int = 200;  // Query download count (int*)
pub const SQLITE_OBJS_FCNTL_STATS: c_int = 201;           // Retrieve metrics (char**)
pub const SQLITE_OBJS_FCNTL_STATS_RESET: c_int = 202;     // Reset metrics
```

## Memory Safety

- The `sqlite_objs_config_t` struct is **copied** by the C library during registration. You may safely free Rust-allocated strings after the call returns.
- `SQLITE_OBJS_FCNTL_STATS` returns a `char*` allocated with `sqlite3_malloc`. **You must free it** with `sqlite3_free` (available via `rusqlite::ffi::sqlite3_free`).

## Repository

Full documentation and C source code: [github.com/qbradley/sqlite-objs](https://github.com/qbradley/sqlite-objs)

## License

MIT
