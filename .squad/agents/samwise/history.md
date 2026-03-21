# Project Context

- **Owner:** Quetzal Bradley
- **Project:** Azure Blob-backed SQLite (sqlite-objs) — a drop-in replacement for SQLite where all storage is backed by Azure Blob Storage, implemented as a custom VFS layer. MIT licensed.
- **Stack:** C, SQLite VFS API, Azure Blob Storage REST API, libcurl, OpenSSL
- **SQLite source:** `sqlite-autoconf-3520000/` (do not modify unless absolutely necessary)
- **Created:** 2026-03-10

## Key Context

- Test crash recovery: commit → blob verify → machine loss → reconnect → data intact
- Test network failures: Azure unreachable mid-write, partial writes, auth failures
- Test locking: single writer, many readers (MVP 1), multi-machine (MVP 3-4)
- License: MIT
- **Test count:** 288 unit tests (247 previous + 41 lazy cache tests)

## Learnings

<!-- Append new learnings below. Each entry is something lasting about the project. -->

### Comprehensive Lazy Cache Unit Tests (2025-07)

- **Test count:** 288 unit tests (247 previous + 41 new lazy cache tests). Target exceeded.
- **Mock ETag support added:** `mock_azure_ops.c` now simulates ETags via an `etag_counter` per blob, incremented on mutations (create/write/resize). `blob_get_properties` populates `err->etag`. This enabled testing cache_reuse + state file persistence flows in unit tests.
- **Buffer overflow lesson:** `char sql[128]` is insufficient for SQL containing `%0200d` format specifiers (200-char zero-padded integers). Always use `char sql[256]` or larger when embedding long string literals.
- **Bootstrap coverage insight:** The 64KB bootstrap window (SQLITE_OBJS_PAGE1_BOOTSTRAP) covers ~16 pages at 4096-byte page size. For tests validating lazy fetch behavior (e.g., prefetch triggering Azure reads), the database must exceed 64KB — roughly 500+ rows with 200-byte payloads.
- **Test categories implemented:** (1) Open & Bootstrap (4 tests), (2) xRead Behavior (6 tests), (3) Write & Truncate bitmap effects (3 tests), (4) Prefetch PRAGMA (4 tests), (5) State File I/O with corruption recovery (5 tests), (6) Edge Cases including error propagation, locking, journal handling, mixed prefetch modes (19 tests).
- **State file corruption tests:** Corrupt magic bytes → safe fallback (all pages invalid). Corrupt CRC → safe fallback. Truncated file → safe fallback. Missing file → safe fallback. All verified via shell commands (`dd`, `truncate`) to manipulate sidecar files between close/reopen.
- **URI parameter testing:** `file:name?prefetch=none` works with `SQLITE_OPEN_URI` flag and global mock ops (no `azure_account` in URI avoids triggering `azure_client_create`). For `cache_reuse=1&cache_dir=X`, `buildCachePath` hashes `::blobName` when account/container are NULL — still produces a valid deterministic path.


## Core Context Summary

**Testing Infrastructure Architect (2026-03-10 through 2026-07):**
Samwise designed 4-layer test pyramid (C mocks ~300 tests, Azurite ~75, Toxiproxy ~30, real Azure ~75). MVP 1 delivers Layers 1+2. Critical requirement: VFS MUST accept swappable Azure operations interface (vtable) for test injection. Mock_azure_ops.c provides layer 1 seams.

**Test Architecture:**
- **Layer 1 (C Mocks):** ~300 tests, <5s. SQLite C library with mock_azure_ops replacing real Azure calls. Unit tests for VFS path logic (xRead, xWrite, xSync, xLock), bitmap operations, lease state machines, error handling.
- **Layer 2 (Azurite):** ~75 tests, <60s. Azurite local emulator (npm package, in-process storage simulation). Integration tests for HTTP semantics, blob operations, sidecar persistence, ETag handling.
- **Layers 3+4 (Toxiproxy, Real Azure):** Deferred to MVP 2+. Chaos engineering (latency injection, packet loss) and production validation.

**Test Coverage Matrix:**
VFS methods (18 total): xOpen, xDelete, xAccess, xFullPathname, xRandomness, xSleep, xCurrentTime, xDlOpen (delegated) + xRead, xWrite, xSync, xLock, xUnlock, xCheckReservedLock, xFileSize, xTruncate, xSectorSize, xDeviceCharacteristics (core). Only 14 covered in MVP 1 (OS delegation uncovered).

**Key Test Decisions:**
- **azure_ops_t vtable:** Mock implementation injected via sqlite_objs_config_t for layer 1.
- **FCNTL download counter:** Exposed via SQLITE_OBJS_FCNTL_DOWNLOAD_COUNT (op 200) for validation of ETag cache reuse.
- **Dirty shutdown testing:** Via mem::forget in Rust tests, simulating process crash.
- **Deterministic data generation:** SeededData with hash-based row generation (no large storage overhead).
- **Test isolation:** TestDb fixture with RAII cleanup, auto-skip Azure tests when credentials absent.

**Recent Work (VFS Test Architecture 2025-07-18):**
Designed 54 new Rust integration tests across 6 categories (lifecycle, transactions, cache reuse, threading, growth/shrink, error recovery). Estimated 4.5 days implementation. 5 open questions for Brady (thread count, FCNTL wrapper placement, WAL vs journal default, real Azure cadence, Toxiproxy deferral).

  - `buildCachePath()` hashes `account:container:blobName` for deterministic naming
  - On close: persists cache + writes ETag sidecar only if cache is clean and ETag valid
  - On re-open: reads stored ETag, compares to blob's current ETag via `blob_get_properties()`
  - Match → skip download, reuse `.cache` file. Mismatch → truncate + fresh download.
- **Zero new compiler warnings** from the new test code (used `(const char *)` cast on `sqlite3_column_text` to avoid `-Wpointer-sign`).

### ETag Batch Write Regression Test Improvement (2026-03-14)

- **Rewrote `etag_cache_reuse_wal`** in `test/test_integration.c` to exercise the `az_page_blob_write_batch()` curl_multi code path that was previously untested.
- **Root cause of original bug:** `az_page_blob_write_batch()` called `azure_error_init(err)` on success, zeroing the ETag. The ETag sidecar always had a stale value, so cache reuse never worked for batch-written databases.
- **Test strategy:** WAL mode + `PRAGMA wal_autocheckpoint=10` (low threshold) + 300 rows × 200-byte payloads in 6 batches of 50. Each COMMIT can trigger an autocheckpoint that flushes ~10+ dirty pages through `write_batch` (nRanges > 1 → curl_multi path).
- **Critical assertion added:** `SQLITE_OBJS_FCNTL_DOWNLOAD_COUNT == 0` on second open — proves the ETag sidecar correctly matches the blob's current ETag after batch writes.
- **Also verifies:** data integrity (all 300 rows present, correct payload lengths).
- **Key insight for future tests:** To exercise `az_page_blob_write_batch` in tests, you need nRanges > 1 (i.e., multiple dirty pages in a single checkpoint). A low `wal_autocheckpoint` threshold combined with bulk inserts forces this path. Single-row tests never hit the batch code.
- **Zero new compiler warnings.** All 17 integration tests pass.

### Lazy Cache Test Coverage (2026-03-22)

Test infrastructure updated to validate lazy cache implementation:

- **Unit test additions:** Bitmap operation tests (mark, clear, range ops), state file I/O (write, read, CRC validation, corrupt file recovery), lazy fetch logic (missing page detection, readahead window coalescing).
- **Integration test updates:** Test lifecycle now validates `prefetch=none` mode, lazy page fetching, readahead batching, state file persistence across close/reopen, revalidation with bitmap invalidation.
- **Cleanup updates:** `cleanup_cache_files()` now removes `.state` and `.snapshot` sidecar files in addition to cache. Critical for test isolation.
- **Mock layer:** `azure_ops_t` vtable extended with `state_file_path` callback for sidecar location (allows mock to intercept .state reads).
- **Test results:** All 247 unit tests + 17 integration tests pass. Zero regressions vs prefetch=all baseline.

**Coverage metrics:** 14 unit + 6 integration + 4 performance benchmarks per original test plan.

### Multi-Client Azurite Integration Tests (2025-07)

- **Test count:** 41 integration tests (17 previous + 24 new multi-client tests). All pass.
- **Categories:** (A) Write-Read Handoff — 4 tests, (B) Sequential Writes — 4 tests, (C) Prefetch Modes — 4 tests, (D) Cache Reuse — 4 tests, (E) Transaction Integrity — 4 tests, (F) Edge Cases — 4 tests.
- **Helper functions added:** `build_uri()`, `open_azurite_db()`, `exec_sql()`, `query_int()`, `cleanup_test_blobs()` — reduce boilerplate across 24 tests from ~80 lines to ~40 lines per test.
- **Data scale:** Tests exercise multi-page scenarios: 1200 rows with BLOBs (test A2), 5000-row transactions (test E1), 70KB wide rows spanning pages (test F2), 50-table schemas (test F3), 600-row prefetch exercises exceeding 64KB bootstrap window (tests C1/C4).
- **Cross-VFS ATTACH limitation:** ATTACH inherits the main connection's VFS. A local file can't be ATTACHed from an Azure VFS connection. Test E4 opens local first and ATTACHs Azure via URI, with graceful skip if unsupported.
- **Stale Azurite data:** ETag cache tests fail if Azurite has persistent data from prior runs. Always clean `__azurite_db_*.json` and `__blobstorage__` before test runs to ensure clean state.
- **Build state note:** HEAD commit (b6e26df) has snapshot function with arg count mismatch against execute_with_retry. Also, uncommitted if_match changes across azure_client.c/h break build if partially applied. Source files must be at clean HEAD for integration tests to compile.

## Phase 1 Orchestration — 2026-03-21T06:42:13Z

**Completed work:**
- 24 new multi-client integration tests across 6 categories (write-read, sequential writes, prefetch modes, cache reuse, transactions, edge cases)
- 41 total integration tests (17 previous + 24 new), all passing
- Test infrastructure: Azurite mock setup, failure injection, metrics collection

**Key finding:** ATTACH inherits VFS limitation
- When attaching an Azure database to a main Azure database, both must use `sqlite-objs` VFS
- No way to ATTACH a non-Azure database alongside Azure MAIN
- Workaround: use single Azure database (no ATTACH)
- Future: design VFS aliasing layer for selective ATTACH (MVP 2+)

**Cross-agent notes:**
- Frodo's If-Match implementation tested via write-read handoff tests (7 tests validate concurrency)
- Gimli's UriBuilder now used in test URI construction (cleaner than manual string building)
- Aragorn's prefetch modes tested across cache reuse scenarios (8 tests)

**Test coverage validation:**
- Mock layer (300 tests, <5s) — passing
- Azurite (41 tests, <60s) — passing
- Toxiproxy (future)
- Real Azure (future, weekly)
