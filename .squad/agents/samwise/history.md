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
- **Test count:** 242 unit tests (234 previous + 8 URI config tests)

## Learnings

<!-- Append new learnings below. Each entry is something lasting about the project. -->


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
