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

