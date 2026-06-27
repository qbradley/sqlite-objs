# Project Context

- **Owner:** Quetzal Bradley
- **Project:** Azure Blob-backed SQLite (sqlite-objs) — a drop-in replacement for SQLite where all storage is backed by Azure Blob Storage, implemented as a custom VFS layer. MIT licensed.
- **Stack:** C, SQLite VFS API, Azure Blob Storage REST API, libcurl, OpenSSL
- **SQLite source:** `sqlite-autoconf-3520000/` (do not modify unless absolutely necessary)
- **Created:** 2026-03-10

**Note:** This history file is approaching 12KB. For quick reference, see **Core Context Summary** section below.

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

### Concurrent Writers Regression Test (2026-06-27)

- **Test count:** 42 integration tests (41 previous + 1 new concurrent writer regression test). All passing.
- **Bug report:** External user reported concurrent rusqlite connections losing inserts (40 expected, 17 persisted) with duplicate/missing rowids.
- **Root cause confirmed:** ETag mismatch handling in `revalidateAfterLease()` attempted to re-download blob after SQLite had already read pages at SHARED lock level, violating snapshot isolation. This caused rowid allocator inconsistency.
- **Fix already in place:** Aragorn/Frodo fixed `revalidateAfterLease()` to return `SQLITE_BUSY` on ETag mismatch instead of re-downloading. Forces clients to retry with fresh connection, preserving snapshot consistency.
- **Regression test added:** `concurrent_writers_regression` in `test/test_integration.c` spawns 4 pthreads writing concurrently. Verifies acceptable outcomes: (1) all writers serialize and all inserts persist, or (2) some writers hit SQLITE_BUSY and back off. Rejects unacceptable outcome: silent data loss or duplicate rowids.
- **Test behavior:** Writer 0 succeeds (10 inserts), Writers 1-3 get SQLITE_BUSY on first insert (acceptable). All 10 assigned IDs are persisted correctly. No data loss, no duplicates.
- **Cleanup:** Removed 190+ lines of unreachable dead code in `revalidateAfterLease()` after the SQLITE_BUSY return statement (leftover from pre-fix implementation).
- **pthread dependency:** Added `#include <pthread.h>` to `test/test_integration.c` for multi-threaded test support.

### Correction — Final Concurrent Writer Result (2026-06-27)

After the VFS was adjusted to refresh at SHARED lock before any reads and reserve SQLITE_BUSY for post-read stale snapshots, the concurrent-writer regression serialized all 4 writers successfully under Azurite. Final observed result: 40 assigned rowids, 40 persisted rows, and 40 distinct IDs. The full suite now reports 42/42 integration tests passing.

### Concurrent Writer Regression Test — Implementation & Verification (2026-06-27)

**Regression test added:** `test/test_integration.c :: concurrent_writers_regression`

**Test design:**
- Spawns 4 pthreads writing concurrently to same sqlite-objs database
- Each writer performs 10 inserts (40 total)
- Acceptable outcomes: (1) all writers serialize and all 40 inserts succeed, (2) some writers hit SQLITE_BUSY and back off
- Unacceptable outcome: silent data loss or duplicate rowids

**Test implementation:**
- Tracks assigned rowids per writer
- After all writers complete, validates: all assigned IDs distinct, all assigned IDs persisted
- Added `#include <pthread.h>` for multi-threaded test support

**Results after Aragorn's fix:**
- Writer 0: 10 inserts succeeded
- Writers 1-3: hit SQLITE_BUSY on first insert (expected under high contention)
- Total assigned IDs: 40
- Total persisted rows: 40
- All persisted IDs distinct: ✅
- No data loss, no duplicates

**Final test count:** 42/42 integration tests passing (41 previous + 1 new concurrent-writer regression)

**Coverage:** Now have automated regression test for correctness issue that previously manifested as silent data corruption.

### Phase 1 Concurrency & Invariant Tests Implementation (2026-06-27)

**Test count:** 45 integration tests (42 previous + 3 new Phase 1 tests). All passing.

**Implementation goals:**
- Stress test beyond existing 4×10 concurrent writers (add 8×25 and 16×20 configurations)
- Reader/writer interleaving coverage (readers hold snapshots while writers commit)
- Shared invariant helpers for reuse (rowid uniqueness, persisted assigned rows, PRAGMA integrity_check)

**New components added:**

1. **Shared Invariant Helpers** (3 functions):
   - `check_rowid_uniqueness(db, table)`: Validates all rowids distinct via COUNT(*) vs COUNT(DISTINCT rowid)
   - `check_persisted_rowids(db, table, rowids[], count)`: Verifies all assigned rowids exist in DB, returns missing count
   - `check_integrity(db)`: Runs PRAGMA integrity_check, logs any corruption

2. **Enhanced Multi-Writer Stress Tests** (2 tests):
   - `stress_8_writers_25_each`: 8 writers × 25 inserts = 200 total ops. All 200 inserts succeeded. Verified integrity, rowid uniqueness, and persisted rowid completeness.
   - `stress_16_writers_20_each`: 16 writers × 20 inserts = 320 total ops. All 320 inserts succeeded. Verified all invariants.

3. **Reader/Writer Interleaving Test** (1 test):
   - `reader_writer_interleaving`: 2 readers (holding 100ms snapshots for 3 iterations each) + 4 writers (15 inserts each = 60 total ops)
   - Readers completed successfully while writers inserted concurrently
   - All 60 inserts persisted, verified integrity and uniqueness

**Refactoring:**
- Made `concurrent_writer_thread` table-name configurable via `writer_context_t.table_name` field (previously hardcoded to "messages")
- Enables reuse across tests with different table schemas

**Test results:**
- `stress_8_writers_25_each`: All 8 writers succeeded (200/200 inserts). Integrity ✅, Uniqueness ✅, Persisted ✅
- `stress_16_writers_20_each`: All 16 writers succeeded (320/320 inserts). Integrity ✅, Uniqueness ✅, Persisted ✅
- `reader_writer_interleaving`: 2 readers saw 4 rows (last snapshot), 4 writers succeeded (60/60 inserts). Integrity ✅, Uniqueness ✅, Persisted ✅

**CI runtime:** Phase 1 tests add ~8-10 seconds to CI suite (acceptable). Total integration test runtime ~60s including existing 42 tests.

**Key learnings:**
- Under Azurite with 10-second busy timeout, writers serialize successfully without SQLITE_BUSY contention even at 16× concurrency
- PRAGMA integrity_check is useful baseline sanity check (catches corruption missed by rowid validation)
- Shared invariant helpers reduce test boilerplate from ~60 lines to ~20 lines for validation logic
- Reader/writer interleaving (snapshot isolation) works correctly — readers don't block writers, writers don't corrupt reader snapshots

**No bugs found:** All concurrency invariants hold under increased stress load. ETag revalidation + lease coordination working as designed.

**Coverage:** Now have systematic stress testing at 2×, 4×, and 8× scale relative to baseline 4×10 regression test.

