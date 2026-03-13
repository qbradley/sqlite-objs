# Gimli — History

## Project Context

**Project:** Azure Blob-backed SQLite (sqlite-objs)
**Owner:** Quetzal Bradley
**Stack:** C, SQLite VFS API, Azure Blob Storage REST API, libcurl, OpenSSL — with Rust crate wrappers
**License:** MIT

### What This Project Does

A drop-in replacement for SQLite where all storage is backed by Azure Blob Storage. Implemented as a custom VFS layer in C. My role is to package this as Rust crates so Rust developers can use it via rusqlite.

### Key Architecture

- C source in `src/` — sqlite_objs_vfs.c (VFS impl), azure_client.c/.h (Azure REST), sqlite_objs.h (public API)
- SQLite source in `sqlite-autoconf-3520000/`
- Public C API: `sqlite_objs_vfs_register()`, `sqlite_objs_vfs_register_uri()`, `sqlite_objs_vfs_register_with_config()`
- Dependencies: libcurl, OpenSSL (system libraries)
- URI-based per-file config: `file:db?azure_account=...&azure_container=...&azure_sas=...`

## Learnings

### Rust Crate Structure (2024-03-11)

**Decision:** Two-crate workspace pattern — `sqlite-objs-sys` (raw FFI) + `sqlite-objs` (safe wrapper).

- Workspace root: `rust/` subdirectory (parallel to `src/` C code)
- `sqlite-objs-sys`: Raw FFI bindings with `build.rs` that compiles C sources using `cc` crate
- `sqlite-objs`: Safe wrapper exposing `SqliteObjsVfs::register()`, `register_uri()`, `register_with_config()`
- All C compilation happens in build.rs — no external build system required beyond cargo

**Key Files:**
- `rust/Cargo.toml` — workspace manifest
- `rust/sqlite-objs-sys/build.rs` — C compilation logic (SQLite amalgamation + sqlite-objs sources)
- `rust/sqlite-objs-sys/src/lib.rs` — FFI bindings to `sqlite_objs_config_t` and registration functions
- `rust/sqlite-objs/src/lib.rs` — Safe wrapper with owned `SqliteObjsConfig` struct
- `rust/sqlite-objs/examples/basic.rs` — Usage demonstration

### Build System Integration

**C Compilation via cc crate:**
- Compiles `sqlite3.c` amalgamation as separate static lib (warnings disabled)
- Compiles sqlite-objs sources: `sqlite_objs_vfs.c`, `azure_client.c`, `azure_auth.c`, `azure_error.c`
- Include paths: `src/`, `sqlite-autoconf-3520000/`
- Defines: `SQLITE_THREADSAFE=1`, `SQLITE_ENABLE_FTS5`, `SQLITE_ENABLE_JSON1`, `_DARWIN_C_SOURCE`
- Links: `curl`, `ssl`, `crypto`, `pthread`, `m`

**macOS OpenSSL Discovery:**
- Use `pkg-config --cflags-only-I openssl` for include paths
- Use `pkg-config --libs-only-L openssl` for library paths
- Fallback to `brew --prefix openssl` if pkg-config not available
- Both include and lib paths needed for successful compilation and linking

### API Design Choices

**Safe Rust API:**
- Zero-sized `SqliteObjsVfs` type with static methods (VFS is global, process-lifetime)
- Owned `SqliteObjsConfig` struct (all String fields, not raw pointers)
- Null-byte validation on config fields before FFI call
- Custom error type: `SqliteObjsError` with `InvalidConfig`, `RegistrationFailed`, `Sqlite` variants
- Result type for all public APIs

**Integration with rusqlite:**
- Users call `SqliteObjsVfs::register*()` once at startup
- Then use standard `rusqlite::Connection::open_with_flags_and_vfs()` with `vfs="sqlite-objs"`
- URI mode works with `SQLITE_OPEN_URI` flag and query parameters in filename

### Testing Strategy

**Unit tests:**
- FFI linkage test: `test_register_uri()` calls C function, verifies SQLITE_OK
- Config validation: `test_invalid_config()` verifies null-byte rejection
- No Azure credentials needed for basic FFI tests

**Example:**
- Demonstrates VFS registration (no Azure connection required)
- Documents URI format and env var usage in output
- Shows rusqlite integration pattern

### Build Artifacts

All tests pass (5 total):
- `sqlite-objs-sys`: 2 unit tests
- `sqlite-objs`: 3 unit tests + 3 doc tests
- Example builds and runs successfully

`cargo build` compiles ~50 crates in ~4s (release: TBD)
`cargo test` all passing in <1s (FFI only, no I/O)

