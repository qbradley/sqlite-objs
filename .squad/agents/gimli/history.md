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

## Core Context Summary

**Performance & Rust Integration Lead (2026-03-10 through 2026-07):**
Gimli designed performance test harness (TPC-C OLTP benchmark), Rust bindings (rusqlite), and URI builder API. Measured 50–100K txns/s projected throughput on 1GB cache with Phase 1 coalescing.

**Performance Benchmarking:**
- **TPC-C OLTP:** Real-world transactional workload reproducing contention patterns. Measurements at 1K, 5K, 10K, 50K, 100K transactions. 10K txn/s threshold: lease renewal overhead becomes measurable. Phase 1 coalescing reduces write amplification to 1.2× (dirty pages → 4 MiB batch). Projected 50–100K txns/s throughput for sqlite-objs vs baseline SQLite.
- **Speedtest1:** SQLite official benchmark. Three binaries: vanilla SQLite, sqlite-objs with stub Azure, sqlite-objs with real Azure. Process isolation (via system()) prevents exit() conflicts. Measurements: local-only, full comparison, detailed profile.
- **Metrics tracked:** Throughput (txns/sec), latency (p50/p95/p99), lease renewal cost, batch hit rate, cache footprint.

**Rust Integration:**
- **UriBuilder API:** Fluent builder for safe SQLite URI construction with Azure credentials. Inline RFC 3986 percent-encoding (no external deps) to handle special characters in SAS tokens (&, =, %, :). 11 unit tests + 4 doc tests.
- **rusqlite Bindings:** Thin wrapper exposing sqlite-objs VFS selection via URI parameter (?vfs=sqlite-objs).
- **Cargo integration:** Rust crate sqlite-objs-sys for C bindings, sqlite-objs for high-level API.

**Key Performance Insights:**
- **Write batching:** Coalescing dirty pages into 4 MiB chunks reduces HTTP requests by 64× (128 pages at 32 KB each vs 1 batch). Effect multiplicative with curl_multi parallelism.
- **Lease renewal cost:** Inline renewal during xSync + xWrite adds <5% overhead at steady state. No background thread required (sync model safe with btree mutex).
- **Cache misses:** ETag persistence enables reconnect without full download (100ms vs 2s for 100 MB database).

**Recent Work (VFS Integration Test Architecture, 2025-07-18):**
Co-designed 54 Rust integration tests. 6 categories: lifecycle, transactions, cache reuse, threading, growth/shrink, error recovery. TestDb fixture abstracts Azurite/Azure backends. FCNTL download counter validates cache reuse. Dirty shutdown via mem::forget tests crash safety.


**Key decisions:**
- `PRAGMA journal_mode=DELETE` (not WAL) per task spec — simpler for benchmarks
- Used `dev-dependencies` (rusqlite, tempfile, uuid, dotenvy) which are available to examples
- UUID-based blob names eliminate need for pre-run blob deletion
- Marker row (id=99999, data=DEADBEEF) as correctness canary

**Observed timings (dev build, macOS):**
- Populate 100MB: ~7s
- Cold download: ~4s
- Small write: ~0.2s
- Stale reconnect: ~4.6s (full re-download since ETag changed)

**Files:** `rust/sqlite-objs/examples/reconnect_bench.rs`

## Learnings

**Ergonomic Rust API Additions (2025-07-22):**
- Added `PrefetchMode` enum and `UriBuilder::prefetch()` for the `prefetch` URI parameter (`"all"` or `"none"`). Only emitted when explicitly set, keeping default URIs short.
- Task spec had wrong FCNTL numbers (200=WAL_PARALLEL, etc.). Actual C header: 200=DOWNLOAD_COUNT, 201=STATS, 202=STATS_RESET. Always verify against the C header, not spec documents.
- `VfsMetrics` struct (27 counters) with forward-compatible parser — unknown keys are ignored so older Rust code works with newer C builds. Field names match the C `formatMetrics()` output exactly (e.g. `prefetch_pages` not `cache_prefetch_pages`).
- C metrics struct has `journal_bytes_uploaded` and `wal_bytes_uploaded` which the task spec omitted, and lacks `azure_retries` which the spec invented. Always match the C reality.
- `rusqlite` 0.38 does NOT expose a public `file_control()` method. Must use unsafe FFI: `rusqlite::ffi::sqlite3_file_control(conn.handle(), ...)`. This is the standard pattern for custom FCNTLs.
- STATS FCNTL returns a `sqlite3_malloc`'d string; caller must `sqlite3_free()` it. The pragmas module handles this lifecycle automatically.
- Gated the `pragmas` module behind a `rusqlite` Cargo feature to keep the core crate dependency-free for non-rusqlite users.
- C string literal syntax `c"main"` (stabilised in Rust 1.77) is cleaner than `CString::new("main").unwrap()` for known-good constants.


## Phase 1 Orchestration — 2026-03-21T06:42:13Z

**Completed work:**
- PrefetchMode enum: Off, On, Adaptive
- UriBuilder::prefetch() fluent API for ergonomic URI construction
- VfsMetrics struct: 27 fields covering lease ops, downloads, cache, ETags, WAL, snapshots, timing
- Pragmas module: `get_stats()`, `reset_stats()`, `get_download_count()` with `rusqlite` feature gating
- 664 lines added (300 pragmas, 200 UriBuilder, 164 docs/tests)
- 34 lib tests + 8 doc tests passing, clippy clean

**Integration points:**
- Pragmas module exposes Aragorn's custom FCNTL convention (op codes 200+)
- UriBuilder simplifies Samwise's test URI construction
- VfsMetrics enables Frodo's ETag cache hit verification (via `get_download_count()`)

**Cross-agent notes:**
- Samwise now uses UriBuilder instead of string concatenation in integration tests
- Frodo can use `get_download_count()` PRAGMA to assert on cache hits
- Aragorn's metrics (lease count, download latency) exposed via pragmas module
- Future: Gandalf's monitoring dashboards can consume VfsMetrics snapshots

**Tech debt noted:**
- Feature flag required for rusqlite: `--features bin-deps` when building binaries
- Unsafe FFI required for file_control() (no public rusqlite method)
- Metrics parser is forward-compatible (unknown keys ignored)
