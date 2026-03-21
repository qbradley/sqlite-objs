# Project Context

- **Owner:** Quetzal Bradley
- **Project:** Azure Blob-backed SQLite (sqlite-objs) — a drop-in replacement for SQLite where all storage is backed by Azure Blob Storage, implemented as a custom VFS layer. MIT licensed.
- **Stack:** C, SQLite VFS API, Azure Blob Storage REST API, libcurl, OpenSSL
- **SQLite source:** `sqlite-autoconf-3520000/` (do not modify unless absolutely necessary)
- **Created:** 2026-03-10

## Key Context

- VFS implementation via sqlite3_vfs — xOpen, xRead, xWrite, xSync, xLock, etc.
- Goal: no SQLite source modifications — use VFS extension API exclusively
- Open question: WAL mode vs Journal mode vs both?
- SQLite locking model must map correctly to blob operations
- License: MIT

## Learnings

<!-- Append new learnings below. Each entry is something lasting about the project. -->

### Lazy Cache Filling Implementation (2026-03-22)

- **Implemented lazy cache filling** in `src/sqlite_objs_vfs.c` — `prefetch=all` (default, full download at xOpen) vs `prefetch=none` (lazy, on-demand page fetches).
- **New struct fields:** `aValid` bitmap (parallel to `aDirty`), `nValidPages`, `nValidAlloc`, `prefetchMode`.
- **Valid bitmap helpers:** `validMarkPage`, `validIsPageValid`, `validClearPage`, `validClearAll`, `validMarkAll`, `validEnsureCapacity`, `validMarkRange` — mirrors dirty bitmap API.
- **State file I/O:** `.state` sidecar with format: SQOS magic + version + pageSize + fileSize + bitmap + CRC32. Atomic rename write (`writeStateFile`), validated read (`readStateFile`), cleanup (`unlinkStateFile`).
- **CRC32 implementation:** Lookup table-based ISO 3309 CRC32, plus LE32/LE64 read/write helpers for cross-platform binary format.
- **`fetchPagesFromAzure()`:** 16-page readahead window on cache miss. Single `page_blob_read` call for the window, clamp to file size.
- **`prefetchInvalidPages()`:** Scans valid bitmap, coalesces contiguous invalid ranges, downloads all at once. Exposed via `PRAGMA sqlite_objs_prefetch`.
- **xRead:** Before pread, checks validity bitmap for all pages in read range. Fetches missing pages via readahead window.
- **xWrite:** Marks pages both dirty AND valid simultaneously.
- **xTruncate:** Clears valid bits for pages beyond new size.
- **xClose:** Write order: cache fsync → .state file (atomic rename) → .etag file. Frees `aValid`.
- **xSync:** Persists .state sidecar after successful upload (best-effort).
- **revalidateAfterLease (lazy mode):** Incremental diff used to MARK pages invalid (not download). If >50% pages changed, falls back to full download. After full re-download, marks all valid.
- **applyIncrementalDiff:** Now marks downloaded diff ranges as valid in the bitmap.
- **Constants:** `SQLITE_OBJS_PREFETCH_ALL=0`, `SQLITE_OBJS_PREFETCH_NONE=1`, `SQLITE_OBJS_READAHEAD_PAGES=16`, `SQLITE_OBJS_PAGE1_BOOTSTRAP=65536`, `SQLITE_OBJS_DIFF_THRESHOLD_PCT=50`.
- **Net new code:** ~500 lines. Zero regression — all 247 unit tests + 17 integration tests pass.
- **Key design insight:** `bitmapsEnsureCapacity()` wrapper grows both dirty and valid bitmaps together. `cacheEnsureSize()` calls it to keep both in sync when the cache file grows.
- **Test cleanup:** Updated `cleanup_cache_files()` in test_integration.c to also remove `.state` and `.snapshot` sidecar files.


## Core Context Summary

**MVP 1 Implementation (2026-03-10 through 2026-07):** 
Aragorn led VFS implementation and performance optimization. Three major phases: (1) Core VFS with lease-based locking and dirty bitmap coalescing (10K lines), (2) Phase 1 page coalescing reducing write amplification 5–10×, (3) Phase 3 ETag-based cache reuse with bitmap persistence. Code review APPROVED. Disk-backed cache reduces memory footprint to ~8MB. 242 tests passing. Key findings: (a) WAL mode impossible for remote storage; journal mode is architecturally correct. (b) Device characteristics MUST NOT set ATOMIC512 — journal provides crash safety. (c) Lease renewal requires inline checking (no background threads safe with synchronous xSync).

**Performance Results:**
- TPC-C OLTP: 1.2× write amplification with coalescing; 10K txn/s threshold where lease renewal overhead becomes measurable; projected 50–100K txns/s for 1GB cache.
- Phase 1 coalescing: 5–10× improvement for sequential writes by batching dirty pages into 4 MiB chunks (handles edge cases: boundary splits, scattered pages, 512-alignment padding).
- ETag cache reuse: Reconnect latency drops from 2s (full download) to 100ms for unchanged databases.

**Key Architecture Decisions:**
- Disk-backed cache file (not malloc buffer) via pread/pwrite, reducing memory footprint.
- Dirty bitmap (1 bit per page) + valid bitmap (future lazy cache).
- Lease locking: SHARED=no lease, RESERVED+=30s blob lease with HEAD check + inline renewal.
- Error mapping: 409/429→SQLITE_BUSY (retryable), else→SQLITE_IOERR_* (fatal).
- azure_ops_t vtable: 13 operations for swappable Azure client (critical testability boundary).

**Code Review Outcomes:**
- MVP 1 gate: APPROVE WITH CONDITIONS. Fixed C1 (device flags), C2 (URL buffer overflow).
- Phase 1+2: APPROVED. `coalesceDirtyRanges()` handles all edge cases; zero-copy batch design safe; recommend jitter on batch retry.

**Recent Work (Lazy Cache Analysis 2026-03-21):**
Analyzed Quetzal's lazy cache filling proposal. Verdict: Architecturally sound but requires three critical additions: (1) Page-1 bootstrap at xOpen, (2) Readahead-on-fault for cold-cache sequential reads, (3) Bitmap persistence atomicity contract. Current disk-backed cache + dirty bitmap + ETag sidecars provide 80% of scaffolding. Implementation estimate 4 days.

- **Deleted code:** ~110 lines (full download in xOpen)
- **Net:** +400 lines

**Risks identified:**
- **High:** xRead performance regression on first access (mitigated by prefetch), bitmap persistence bugs (mitigated by atomic rename)
- **Medium:** Page size detection edge cases (mitigated by eager page 0 fetch), ETag/state desync (mitigated by deleting all sidecars together)
- **Low:** Prefetch complexity, incremental diff interaction

**Testing requirements:** 14 unit tests (bitmap ops, state I/O, lazy fetch, truncate), 6 integration tests (open without download, lazy fetch, write-read, revalidate, prefetch), 4 performance benchmarks (cold open, first query, prefetch, diff efficiency).

**Deliverable:** Documented as `.squad/decisions/inbox/aragorn-lazy-cache-code-analysis.md` — comprehensive implementation roadmap with line numbers, function signatures, and specific code changes needed.

### Lazy Cache Code Analysis (2026-03-21)

- **Completed code-level VFS analysis:** `.squad/decisions/inbox/aragorn-lazy-cache-code-analysis.md` (merged to D23).
- **Major finding:** Current architecture already has 80% of scaffolding needed (disk-backed cache, dirty bitmap, ETag sidecars, incremental diff support). Lazy cache is primarily a resequencing (defer xOpen download) rather than new machinery.
- **Scope:** ~400-500 lines of new code + ~200 lines of refactoring in `src/sqlite_objs_vfs.c`.
- **Critical modifications mapped:**
  - **xOpen** (2071-2570): Skip full blob download, load valid bitmap from sidecar, page-1 bootstrap for size detection.
  - **xRead** (877-964): Add lazy-fetch logic, readahead window on cache miss.
  - **xWrite** (971-1041): Mark page as valid+dirty simultaneously.
  - **xSync** (1182-1523): Atomicity guarantee — sync bitmap BEFORE blob writes.
  - **Sidecar persistence:** New `.state` file with bitmap, ETag, CRC32 checksum.
- **Hotspots identified:**
  - `dirtyBitmapSize()` (269) must accommodate parallel `aValid` bitmap.
  - Incremental diff logic (521-635) gains importance for reconnect scenarios.
  - Lease renewal (`revalidateAfterLease`, 1524-1665) must validate against bitmap.
- **Implementation estimate:** 4 days (2 for xOpen/xRead/xWrite/xSync, 1 for sidecar, 1 for tests).
- **Vtable contract:** No changes required; all modifications internal to sqlite_objs_vfs.c.

### VFS Bitmap & Code Duplication Refactor (2026-07)

- **Unified Bitmap API:** Introduced `Bitmap` struct (`data/nSet/nAlloc`) replacing 6 loose fields per bitmap. Generic functions (`bitmapSetBit`, `bitmapTestBit`, `bitmapClearBit`, `bitmapClearAll`, `bitmapSetAll`, `bitmapSetRange`, `bitmapHasAny`, `bitmapEnsureCapacity`, `bitmapFree`) replace 12 duplicated dirty/valid functions.
- **Sidecar path builder:** `buildSidecarPath(cachePath, ext)` replaces 3 identical `buildEtagPath`/`buildSnapshotPath`/`buildStatePath` functions. Similarly `unlinkSidecarFile(cachePath, ext)` replaces 3 unlink wrappers.
- **Buffer helpers:** `bufferEnsure(ppBuf, pAlloc, newSize)` replaces duplicated `jrnlBufferEnsure`/`walBufferEnsure`. `bufferRead(src, srcLen, pBuf, iAmt, iOfst)` replaces duplicated WAL/journal read logic in xRead.
- **Result:** -158 net lines, zero regressions (247/247 tests pass). Pure refactor.
- **Key insight:** The dirty and valid bitmaps were structurally identical — 8 of 12 functions had exactly the same bit-manipulation logic, differing only in which struct fields they operated on. The `Bitmap` type makes this sharing explicit and prevents future drift.

### VFS Activity Metrics with PRAGMA Exposure (2026-07)

- **Added `sqlite_objs_metrics` struct** to `sqlite_objs.h` with 27 `sqlite3_int64` counters: disk I/O (reads/writes/bytes), Azure blob I/O (reads/writes/bytes), cache behavior (hits/misses/miss_pages/prefetch_pages), lease lifecycle (acquires/renewals/releases), sync (count/dirty_pages/resizes), revalidation (count/downloads/diffs/pages_invalidated), journal/WAL uploads (count/bytes), and azure_errors.
- **Per-connection tracking:** Metrics struct lives in `sqliteObjsFile`, zeroed by `memset` in xOpen. No globals, no allocations, just integer increments.
- **Instrumented 14 code paths:** `fetchPagesFromAzure`, `prefetchInvalidPages`, `applyIncrementalDiff`, `leaseRenewIfNeeded`, `sqliteObjsRead` (cache hit/miss tracking), `sqliteObjsWrite`, `sqliteObjsSync` (WAL/journal/MAIN_DB paths), `sqliteObjsLock`, `sqliteObjsUnlock`, `revalidateAfterLease` (all 3 sub-paths), and `sqliteObjsOpen` (bootstrap + full download).
- **Exposed via PRAGMA:** `PRAGMA sqlite_objs_stats` returns all metrics as key=value text (one per line); `PRAGMA sqlite_objs_stats_reset` zeroes all counters. Also available programmatically via `SQLITE_OBJS_FCNTL_STATS` (201) and `SQLITE_OBJS_FCNTL_STATS_RESET` (202).
- **`formatMetrics()` helper:** Uses `sqlite3_mprintf` to format all 27 counters. Returns caller-freed string. Clean pattern — adding a counter only requires struct field + one `sqlite3_mprintf` format line + one instrumentation increment.
- **7 new tests:** `metrics_pragma_returns_stats`, `metrics_disk_io_counters`, `metrics_blob_io_counters`, `metrics_sync_counters`, `metrics_reset_clears_counters`, `metrics_pragma_reset`, `metrics_all_fields_present`. All 295 tests pass.
- **Existing globals preserved:** `g_xread_count` and `g_xread_journal_count` kept for backwards compatibility (used in timing output). New per-connection metrics are the preferred API.
- **Key design decision:** Single text result via PRAGMA (key=value format) rather than virtual table result set. Simpler implementation, easily parseable, avoids complex sqlite3_stmt machinery. FCNTL provides programmatic access for C callers.

### If-Match / Undelete Phase 1 — Compilation Fixes (2026-07)

- **Fixed 3 compilation errors** in `src/azure_client.c` from Frodo's If-Match / Undelete changes:
  1. `az_blob_undelete`: 13 args to `execute_with_retry` (expects 12). Removed extra NULL.
  2. `az_page_blob_write`: Added missing `const char *if_match` parameter to match vtable. Implemented proper `If-Match:` header construction (4 header array variants: both/lease-only/match-only/none).
  3. `az_page_blob_write_batch`: Added `const char *if_match` parameter. Threaded through to `batch_init_easy` which now emits `If-Match:` header on curl handles.
- **Also fixed:** `batch_init_easy` signature + call site, and the single-range fallback call within `az_page_blob_write_batch`.
- **Build:** Zero errors, zero warnings with `-Wall -Wextra -Wpedantic`.
- **Tests:** All 295 unit tests pass.
- **Synced** to `rust/sqlite-objs-sys/csrc/azure_client.c`.
- **Key insight:** When vtable function signatures change in the header, the production client, mock client, and any batch helper functions that call through to those functions must ALL be updated in lockstep. The compiler won't always catch mismatches when function pointers are involved — only when the static function is assigned directly to the vtable struct.

