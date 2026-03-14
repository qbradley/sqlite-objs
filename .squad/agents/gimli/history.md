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

### Cargo Publish Fix — Bundled C Sources (2025-03-13)

**Problem:** `cargo publish --dry-run` failed because `build.rs` navigated to `../../src/` relative
to `CARGO_MANIFEST_DIR` to find C sources. When cargo packages for publish, it extracts to a temp
directory (`rust/target/package/sqlite-objs-sys-0.1.0-alpha/`) where `../../src/` doesn't exist.

**Solution:** Created `rust/sqlite-objs-sys/csrc/` directory and copied all needed C source and
header files there (7 files total: 4 `.c`, 3 `.h`). Updated `build.rs` to use
`CARGO_MANIFEST_DIR/csrc/` instead of navigating to repo root.

**Files bundled in csrc/:**
- `sqlite_objs_vfs.c`, `azure_client.c`, `azure_auth.c`, `azure_error.c`
- `sqlite_objs.h`, `azure_client.h`, `azure_client_impl.h`

**Pattern:** Any `-sys` crate that compiles C code must bundle its sources inside the crate
directory — relative paths outside the crate break during `cargo publish` verification. The `cc`
crate's `file()` and `include()` calls should only reference paths within `CARGO_MANIFEST_DIR`.

**Verification:** `cargo publish --dry-run -p sqlite-objs-sys --allow-dirty` succeeds. All 8 Rust
tests + 3 doc-tests pass. All 242 C unit tests pass. Makefile build unaffected.

### Linux Cross-Platform Portability Fix (2025-07-25)

**Problem:** `sqlite-objs-sys` crate failed to compile on Linux (x86_64-unknown-linux-gnu). Two root
causes: (1) `build.rs` defined `_DARWIN_C_SOURCE` unconditionally — a macOS-only macro that doesn't
expose POSIX functions on Linux (`strncasecmp`, `strcasecmp`, `gmtime_r`, `strtok_r`, `usleep`,
`useconds_t`). (2) OpenSSL detection fell back to `brew` on Linux where Homebrew doesn't exist.

**Fix — build.rs (3 changes):**
1. Platform-conditional feature macros via `CARGO_CFG_TARGET_OS`: define `_DARWIN_C_SOURCE` on macOS,
   `_GNU_SOURCE` on everything else (covers all POSIX + GNU extensions including deprecated `usleep`).
2. Homebrew fallback gated behind `target_os == "macos"` — Linux uses pkg-config only.
3. Link `libdl` on Linux (matches Makefile's `-ldl` flag for dynamic loading).

**Fix — C source files (both src/ and csrc/):**
- Added `#include <strings.h>` to `azure_client.c` and `azure_auth.c`. This is the proper POSIX
  header for `strcasecmp`/`strncasecmp` and works on both macOS and Linux regardless of feature macros.

**Key insight:** The Makefile already had correct platform detection (`_DARWIN_C_SOURCE` on Darwin,
`_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE` on Linux). The build.rs just needed to mirror this.
Used `_GNU_SOURCE` instead of the Makefile's more conservative pair because it's a strict superset
and simpler for a single define.

**Verification:** macOS cargo build ✓, cargo test (5 unit + 3 doc-tests) ✓, Makefile build ✓,
242 C unit tests ✓, cargo publish --dry-run ✓, csrc/ files match src/ ✓.

### Docker-Based Linux Build Verification (2025-07)

**Created:** `rust/docker-test/` — containerized smoke tests for Linux compilation.

**Architecture:**
- `Dockerfile.ubuntu24` — Ubuntu 24.04 with apt-get (primary Linux target)
- `Dockerfile.azurelinux3` — Azure Linux 3 with tdnf (Microsoft's distro)
- `sample-project/` — minimal Cargo project that depends on sqlite-objs via path
- `test.sh` — orchestrator that builds images & runs containers with `rust/` mounted read-only

**Key design decisions:**
1. Build + test runs at `docker run` time (not `docker build`) because the rust/ crate source is
   a read-only bind mount at `/rust-crates/`. This means no caching of Cargo builds across runs,
   but guarantees a clean-room test every time.
2. `[patch.crates-io]` in sample Cargo.toml overrides sqlite-objs-sys so Cargo resolves the
   workspace path dependency chain correctly when building from outside the workspace.
3. macOS containers are infeasible (Apple licensing) — noted in test.sh as a skip.

**Learnings:**
- When referencing a workspace crate via path from an external project, Cargo follows the workspace
  root to resolve transitive workspace dependencies. The `[patch.crates-io]` belt-and-suspenders
  approach ensures it works even if Cargo's workspace cross-boundary resolution has edge cases.
- Azure Linux 3 uses `tdnf` (not dnf/yum) and package names like `curl-devel`/`openssl-devel`
  (Fedora-style, not Debian-style).
- Docker not available on the current dev machine — files verified via rustfmt + bash syntax check.
  Actual container tests need Docker Desktop or a CI runner.

### URI Builder Helper (2025-03-13)

**Added:** `UriBuilder` struct to `rust/sqlite-objs/src/lib.rs` for constructing SQLite URIs with proper URL encoding.

**Problem:** Users manually building URIs like `file:db?azure_account=x&azure_container=y&azure_sas=token` hit encoding issues. SAS tokens contain `&`, `=`, `%` characters that break URI query strings if not percent-encoded.

**Solution:**
- Builder pattern: `UriBuilder::new(db, account, container).sas_token(token).build()`
- Inline percent-encoding implementation (no external deps) — encodes all non-unreserved characters per RFC 3986
- Prefers SAS token over account key when both set (auth precedence logic)
- Optional endpoint parameter for Azurite/custom endpoints
- Returns a `String` (no rusqlite dependency in main crate)

**API Design:**
```rust
let uri = UriBuilder::new("mydb.db", "myaccount", "databases")
    .sas_token("sv=2024-08-04&ss=b&...&sig=abc")
    .endpoint("http://127.0.0.1:10000/devstoreaccount1")  // optional
    .build();
// → "file:mydb.db?azure_account=myaccount&azure_container=databases&azure_sas=sv%3D2024-08-04%26ss%3Db%26...%26sig%3Dabc"
```

**Key decisions:**
1. No `percent-encoding` crate dependency — keep it lightweight with inline implementation
2. Encode all non-unreserved chars (`A-Za-z0-9-_.~`) to avoid edge cases with special chars in Azure tokens
3. Builder pattern for clarity and optional params (sas_token, account_key, endpoint)
4. Mutual exclusion: sas_token takes precedence over account_key if both set

**Testing:**
- 8 new unit tests covering basic, SAS, account_key, endpoint, precedence, encoding edge cases
- Updated example (basic.rs) to demonstrate UriBuilder usage with real-looking URIs
- All 11 unit tests + 4 doc-tests pass
- Clippy clean (fixed trim().split_whitespace() warning in build.rs)

**Files:**
- `rust/sqlite-objs/src/lib.rs` — UriBuilder struct, percent_encode() function, 8 tests
- `rust/sqlite-objs/examples/basic.rs` — demo with Azurite endpoint example
- `rust/sqlite-objs-sys/build.rs` — clippy fix (remove redundant trim())

**Impact:** Eliminates common URI encoding errors for Rust users. Builder API is more ergonomic than manual string concatenation. Zero external dependencies added.

### UriBuilder: cache_dir Parameter (2025-07)

**Added:** `cache_dir` field and builder method to `UriBuilder` in `rust/sqlite-objs/src/lib.rs`.

**Context:** The C VFS is getting a `cache_dir` URI parameter to control where local cache files are stored (defaults to `/tmp`). The Rust builder needed to match.

**Changes:**
- Added `cache_dir: Option<String>` field to `UriBuilder` struct
- Added `.cache_dir(dir)` builder method with doc comment
- Appends `&cache_dir={encoded}` to URI in `build()` if set
- Updated module-level doc example to show `cache_dir` usage
- 2 new tests: `test_uri_builder_with_cache_dir`, `test_uri_builder_cache_dir_without_auth`

**Verification:** All 13 unit tests + 4 doc-tests pass. No new dependencies.

### UriBuilder: cache_reuse Parameter (2025-07)

**Added:** `cache_reuse` field and builder method to `UriBuilder` in `rust/sqlite-objs/src/lib.rs`.

**Context:** The C VFS is getting ETag-based cache reuse. When enabled via `cache_reuse=true` in
the URI, the VFS keeps cache files after close and reuses them on reconnect if the blob's ETag
matches. The Rust builder needed to expose this parameter.

**Changes:**
- Added `cache_reuse: bool` field to `UriBuilder` struct (default `false`)
- Added `.cache_reuse(enabled)` builder method with doc comment explaining ETag behavior
- `build()` appends `&cache_reuse=1` only when `cache_reuse` is `true`; omitted when `false`
  (C VFS defaults to off, so no need to send `cache_reuse=0`)
- Updated module-level doc example to show `cache_reuse(true)` in the builder chain
- 3 new tests: `test_uri_builder_cache_reuse_enabled`, `test_uri_builder_cache_reuse_default_omitted`,
  `test_uri_builder_cache_reuse_with_cache_dir`

**Verification:** All 16 unit tests + 4 doc-tests pass. No new dependencies.


### Multi-Threading Test Suite (2025-07)

**Added:** Comprehensive multi-threading tests in `rust/sqlite-objs/tests/threading.rs` to verify
thread safety after discovering a C-level thread-safety bug (shared curl handle across connections).

**Key Finding — rusqlite::Connection Thread Safety:**
- `Connection` is **Send** (can be moved between threads) ✓
- `Connection` is **NOT Sync** (cannot be shared via Arc) ✗
- Root cause: `Connection` contains `RefCell` (interior mutability, not thread-safe)
- Best practice: Each thread creates its own `Connection` to the same database file

**Test Coverage (5 tests, all passing):**

1. **test_two_threads_separate_databases** — Two threads, separate DB files, parallel work with
   barrier synchronization. Verifies Connection is Send and VFS registration is thread-safe.

2. **test_multiple_threads_mutex_sequential** — Arc<Mutex<Connection>> pattern (not recommended
   but technically valid). 4 threads sequentially increment a shared counter. Verifies mutex
   serialization works correctly.

3. **test_multiple_threads_separate_connections** — **Recommended pattern.** 5 threads, each with
   its own Connection to the same DB file. Each creates a unique table and writes to a shared
   table. SQLite's file locking handles concurrent access. Barrier ensures parallel stress.

4. **test_stress_many_threads** — 10 threads × 5 iterations = 50 concurrent inserts. Verifies no
   crashes or data corruption under load. Each thread verifies its own row count incrementally.

5. **test_connection_send_trait** — Explicit Send trait test. Creates Connection in main thread,
   moves to worker thread, performs work, moves back to main. Verifies ownership transfer semantics.

**Design Notes:**
- All tests use local file paths (tempfile crate), not Azure URIs — VFS independence testing
- Tests do NOT require Azure credentials or Azurite — pure SQLite threading semantics
- Barrier synchronization ensures true parallelism (not just concurrent, but simultaneous)
- No new dependencies (rusqlite and tempfile already in dev-dependencies)

**Verification:** 27 total tests pass (16 unit + 3 ignored Azure + 5 threading + 2 sys + 4 doc).
Threading tests complete in ~0.1s. No test failures, no flaky behavior across multiple runs.

**Impact:** Provides regression coverage for the C-level mutex fix to `azure_client_t`. If the C
code regresses to a shared curl handle without locking, these tests won't directly catch it (they
don't use Azure), but they establish the threading contract expected by Rust users. Future Azure
integration tests will combine this threading pattern with real Azure operations.

