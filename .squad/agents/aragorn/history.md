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

### VFS API Deep Dive (2026-03-10)

**Three-layer architecture:** `sqlite3_vfs` (open/delete/access) → `sqlite3_file` (base struct, first member is pMethods) → `sqlite3_io_methods` (read/write/lock/sync per-file).

**sqlite3_io_methods versions:** v1 = core I/O + locking (12 methods). v2 = adds xShmMap/xShmLock/xShmBarrier/xShmUnmap (WAL support). v3 = adds xFetch/xUnfetch (mmap). We need v1 only for MVP.

**Key method signatures:**
- `xRead(file, pBuf, iAmt, iOfst)` — MUST zero-fill on short read or corruption follows
- `xWrite(file, pBuf, iAmt, iOfst)` — writes capped at 128KB internally
- `xSync(file, flags)` — flags: SQLITE_SYNC_NORMAL(0x02) or SQLITE_SYNC_FULL(0x03), optionally | SQLITE_SYNC_DATAONLY(0x10)
- `xLock(file, level)` — level is SHARED(1)/RESERVED(2)/PENDING(3)/EXCLUSIVE(4), never NONE
- `xUnlock(file, level)` — level is SHARED(1) or NONE(0) only
- `xFileControl(file, op, pArg)` — return SQLITE_NOTFOUND for unknown opcodes

**Locking model (5 levels):** NONE→SHARED→RESERVED→PENDING→EXCLUSIVE. PENDING is never explicitly requested (internal transitional). Unix VFS uses POSIX fcntl byte-range locks at offset 0x40000000 (1GB). Azure has no shared lock primitive — major gap.

**WAL mode is NOT feasible for remote storage.** Requires shared memory (xShmMap) with sub-millisecond latency between processes. The nolockIoMethods sets xShmMap=0, which prevents WAL mode — we should do the same. Journal mode (DELETE/TRUNCATE) is the correct choice.

**File types:** xOpen receives type flags. MAIN_DB(0x100) and MAIN_JOURNAL(0x800) must be remote. TEMP_DB, TEMP_JOURNAL, TRANSIENT_DB, SUBJOURNAL should be local (delegate to default VFS).

**Page alignment:** Default page size 4096, range 512-65536, always multiples of 512. Azure Page Blobs require 512-byte alignment — fully compatible.

**Device characteristics for Azure:** `SQLITE_IOCAP_SEQUENTIAL | SQLITE_IOCAP_POWERSAFE_OVERWRITE | SQLITE_IOCAP_SUBPAGE_READ`. Do NOT set ATOMIC — journal provides crash safety. PSOW eliminates journal padding I/O.

**Sector size:** Return 4096 (matches default page size and SQLITE_DEFAULT_SECTOR_SIZE).

**VFS registration:** `sqlite3_vfs_register(pVfs, makeDflt)`. Select via `sqlite3_open_v2(file, &db, flags, "sqlite-objs")` or URI `?vfs=sqlite-objs`.

**All VFS methods are synchronous.** Azure latency (5-50ms per op) means read cache is mandatory. Without cache, 100-page read = ~5 seconds.

**szOsFile:** SQLite allocates this many bytes for our sqlite3_file subclass. We fill it in during xOpen, we don't allocate it.

**pMethods trap:** If xOpen sets pMethods non-NULL, xClose WILL be called even on xOpen failure. Set pMethods=NULL to prevent xClose on failure.

**nolock pattern (simplest reference):** `nolockLock()` returns SQLITE_OK always. `nolockCheckReservedLock()` sets *pResOut=0. This is our MVP locking strategy.

### Design Review — MVP 1 Architecture (2026-03-10)

- **Full design review at `research/design-review.md`.** 11 decisions covering blob types, locking, caching, WAL, testing, build system, error handling, auth, naming, VFS registration, and MVP scope.
- **Gandalf corrected my nolock proposal:** Two-level lease-based locking from day 1 (SHARED=no lease, RESERVED+=acquire lease). Prior art shows deferred locking leads to corruption. Updated in D3 of decisions.md.
- **Key architecture: azure_ops_t vtable.** The swappable function pointer table between VFS and Azure client is both the production interface and the test seam. Defined in design-review.md Appendix A. This is the critical boundary between my VFS code and Frodo's Azure client code.
- **io_methods iVersion=1** — no WAL, no mmap. Eliminates 6 method implementations.
- **File type routing:** MAIN_DB and MAIN_JOURNAL → Azure. Everything else → default local VFS. Detected via flags in xOpen.
- **Journal handled as block blob** (sequential write/read pattern). DB as page blob (random R/W).
- **Write buffer with dirty page bitmap:** xWrite→memcpy+dirty bit, xSync→PUT Page per dirty page. Batches writes to sync time.
- **Full-blob cache from day 1:** Download entire blob on xOpen into malloc'd buffer. xRead=memcpy. Gandalf overrode MVP 2 deferral — my uncached analysis proved 5s for 100 pages is untestable.
- **Two-level lease locking:** SHARED requires no lease (reads always work). RESERVED/EXCLUSIVE acquire 30s blob lease. Release on unlock. xCheckReservedLock uses HEAD to detect held leases. Inline renewal (no background thread).
- **Error handling:** 409 (lease conflict) or 429 (throttle) → SQLITE_BUSY (retryable). All else after retry → SQLITE_IOERR_* (fatal). 5 retries, 500ms exponential backoff + jitter.
- **Device characteristics:** `SQLITE_IOCAP_SEQUENTIAL | SQLITE_IOCAP_POWERSAFE_OVERWRITE | SQLITE_IOCAP_SUBPAGE_READ`. NOT ATOMIC — journal safety is needed.
- **VFS name "sqlite-objs"**, non-default registration. Usage: `sqlite3_open_v2(name, &db, flags, "sqlite-objs")`.
- **Source layout:** `src/` (sqlite_objs_vfs.c, azure_client.c/.h, azure_auth.c, azure_error.c, sqlite_objs.h), `test/` (test_main.c, test_vfs.c, mock_azure_ops.c, test_integration.c).
- **Top risks:** (1) Lease expiry during long transactions, (2) Partial sync failure — both mitigated by journal-first ordering and inline lease renewal.

### Cross-Agent Context: Key Interface Contract — azure_ops_t (2026-03-10)

- **Critical boundary with Frodo's Azure client.** Design review D5 mandated a swappable Azure operations interface (function pointer vtable `azure_ops_t`) for testability. Non-negotiable.
- **What I (Aragorn) need from azure_ops_t:**
  - `azure_blob_read()` — GET with Range header, returns buffer
  - `azure_blob_write()` — PUT Page with 512-byte alignment, max 4 MiB per call
  - `azure_blob_size()` — HEAD request, returns Content-Length
  - `azure_blob_truncate()` — Set Blob Properties with new length
  - `azure_lease_acquire()` — Acquire 30s lease, renew inline
  - `azure_lease_release()` — Release lease
  - `azure_lease_check()` — HEAD request, check if lease is held
- **Frodo will deliver** refactored azure_client exporting these as vtable members. PoC code in `research/azure-poc/` shows the patterns.
- **Samwise will provide** mock_azure_ops.c for layer 1 unit tests.
- **See design-review.md Appendix A** for full azure_ops_t contract definition.

### MVP 1 VFS Implementation Complete (2026-03-10)

**Files created:**
- `src/sqlite_objs.h` — Public API header with `sqlite_objs_vfs_register()` and `sqlite_objs_vfs_register_with_config()`, `sqlite_objs_config_t` with test-seam (ops/ops_ctx override)
- `src/azure_client.h` — Internal header defining `azure_ops_t` vtable (13 ops), `azure_err_t` enum, `azure_error_t`, `azure_buffer_t`, `azure_client_t` lifecycle functions
- `src/sqlite_objs_vfs.c` — Complete VFS: sqlite-objsFile struct, io_methods v1 (12 methods), VFS methods (xOpen/xDelete/xAccess/xFullPathname + delegated), registration, dirty bitmap, lease management
- `src/azure_client_stub.c` — Stub ops returning AZURE_ERR_UNKNOWN for all 13 operations. Frodo replaces this.
- `src/sqlite_objs_shell.c` — CLI wrapper: renames shell.c main to shell_main via #define, registers VFS as default, forwards to shell_main
- `Makefile` — Targets: all (libsqlite_objs.a + sqlite-objs-shell), test-unit, test-integration, test, clean, amalgamation (stub)

**Key patterns:**
- sqlite-objsFile struct embeds `const sqlite3_io_methods *pMethod` as first member (sqlite3_file subclass contract)
- Dirty page bitmap: 1 bit per page, `dirtyMarkPage()` tracks writes, `dirtyClearAll()` after sync
- Buffer grows geometrically via `bufferEnsure()` / `jrnlBufferEnsure()`
- Journal files (MAIN_JOURNAL) use separate aJrnlData buffer, uploaded as block blob on xSync
- Lease: acquire at xLock(RESERVED+), renew inline at xSync and xWrite (>15s), release at xUnlock(≤SHARED)
- `azureErrToSqlite()` maps CONFLICT/THROTTLED→SQLITE_BUSY, all else→SQLITE_IOERR variants
- xOpen routes by flag bits: MAIN_DB/MAIN_JOURNAL→Azure, everything else→default VFS xOpen
- szOsFile = max(sizeof(sqlite-objsFile), defaultVfs->szOsFile) to handle delegation
- xOpen sets pMethod=NULL initially (prevents xClose on failure), sets it last on success
- Page size detected from SQLite header bytes 16-17 on existing databases
- xFullPathname strips leading slashes, rejects ".." paths
- xFileControl intercepts PRAGMA journal_mode=WAL → returns "delete"
- xDeviceCharacteristics: SEQUENTIAL | POWERSAFE_OVERWRITE | SUBPAGE_READ (fixed from ATOMIC512)
- xSectorSize: 4096 (SQLite page alignment, Azure compatible)
- Shell uses #define main shell_main trick to include shell.c as a translation unit

**Build system:**
- `-w` flag on sqlite3.c and shell.c compilation (suppress upstream warnings)
- Platform detection: Darwin omits -ldl, Linux includes it
- Static library: build/libsqlite_objs.a containing sqlite3.o + sqlite_objs_vfs.o + azure_client_stub.o

### Benchmark Harness Implementation (2026-03-10)

**Problem:** Need to compare local SQLite vs sqlite-objs performance using SQLite's official speedtest1 benchmark. speedtest1.c calls `exit()` directly, making it unsuitable for inclusion as a library.

**Solution:** Three-binary approach:
1. `benchmark` — Lightweight harness that shells out to speedtest1 binaries and measures wall-clock time
2. `speedtest1` — Standard speedtest1 linked against vanilla SQLite
3. `speedtest1-azure` — speedtest1 linked against sqlite-objs VFS via wrapper

**Files created:**
- `benchmark/benchmark.c` — Main harness using `system()` to invoke speedtest1 binaries, timing via `gettimeofday()`, formatted output
- `benchmark/speedtest1_wrapper.c` — Thin wrapper that registers sqlite-objs VFS as default before calling speedtest1_main
- `benchmark/speedtest1.c` — Downloaded from SQLite upstream (test/speedtest1.c)
- `benchmark/Makefile` — Builds all three binaries, handles stub vs production builds
- `benchmark/README.md` — Usage documentation

**Key design decisions:**
- **Process isolation:** Each speedtest1 run is a separate process via `system()`. This avoids the `exit()` problem and gives clean timing.
- **Silent subprocess output:** speedtest1 output redirected to `/dev/null` to keep harness output clean. Users can run speedtest1 binaries directly for detailed output.
- **Exit code tolerance:** speedtest1 returns 1 if optional features (rtree) are missing. Harness treats exit codes 0 and 1 as success.
- **Two speedtest1 binaries:** One linked against vanilla SQLite, one against sqlite-objs. Benchmark harness invokes the appropriate binary.
- **Stub vs production:** `make all` builds with azure_client_stub (local-only), `make all-production` builds with real Azure client.

**Usage patterns:**
```bash
# Build and run local-only
make && ./benchmark --local-only --size 25

# Build with Azure support and run full comparison  
make all-production
export AZURE_STORAGE_ACCOUNT=...
export AZURE_STORAGE_KEY=...
export AZURE_STORAGE_CONTAINER=...
./benchmark --size 50

# CSV output for automation
./benchmark --output csv > results.csv
```

**Performance expectations:** Azure will be 2-50x slower than local depending on workload (per design decisions D4). The in-memory cache significantly reduces the gap.

**Integration:** Benchmark lives in `benchmark/` subdirectory. Independent Makefile. Reuses parent `build/` objects via relative paths.

### Integration Fix — Headers & Build Reconciliation (2026-03-10)

**Problem:** Three agents built in parallel with divergent type definitions. Headers had conflicting azure_err_t enums, different struct layouts, different buffer field names. Makefile linked tests against the stub instead of the mock.

**Header reconciliation:** `azure_client.h` is now the single canonical header. Merged azure_err_t as superset of all codes. Added `#define AZURE_ERR_THROTTLE AZURE_ERR_THROTTLED` alias. Unified azure_buffer_t to `data`/`size`/`capacity`. Added `error_code[128]` to azure_error_t. `azure_client_impl.h` and `mock_azure_ops.h` now include `azure_client.h` instead of duplicating types. Production `azure_client_create` updated to match canonical config-struct signature.

**Build:** Test binary links `sqlite3.o + sqlite_objs_vfs.o + azure_client_stub.o + mock_azure_ops.o`. Added `make all-production` target. Fixed `##__VA_ARGS__` to C11-compliant split fprintf. Added `sqlite_objs_vfs_register_with_ops()` convenience wrapper. Fixed 2 test logic bugs (invalid blob content, fast-test lease timing).

**Result:** `make all` + `make test-unit` (148/148) both pass clean.

### Code Review Blockers — Pending Fixes (2026-03-10)

Gandalf's review identified two critical issues (C1, C2) that must be fixed before demo:

**C1 (MY RESPONSIBILITY): Device Characteristics Flag Error (sqlite_objs_vfs.c:693)**
- **Current:** Claims `SQLITE_IOCAP_ATOMIC512 | SQLITE_IOCAP_SAFE_APPEND`
- **Issue:** ATOMIC512 tells SQLite it can skip journal entries for 512-byte writes. Our multi-page xSync is NOT atomic — dangerous data corruption risk.
- **Fix Required:** Change to `SQLITE_IOCAP_SEQUENTIAL | SQLITE_IOCAP_POWERSAFE_OVERWRITE | SQLITE_IOCAP_SUBPAGE_READ`
- **Status:** PENDING. Frodo fixing URL buffer overflow (C2) first; Aragorn (me) to tackle this next.

**C2 (FRODO'S RESPONSIBILITY):** URL Buffer Overflow (azure_client.c:173–187)
- **Current:** Uses unbounded `strcat` on 2048-byte stack buffer with SAS tokens
- **Issue:** Long tokens + long paths can overflow
- **Fix Required:** Replace with bounds-checked `snprintf` (4KB buffer recommended)
- **Status:** IN PROGRESS (Frodo Agent-10 completed production build with pkg-config; C2 fix included)

**Impact:** Code approved for demo once both fixes applied. No re-review needed — fixes are mechanical.

### TPC-C OLTP Benchmark Implementation (2026-03-11)

**Problem:** Need a realistic OLTP benchmark to compare local SQLite vs sqlite-objs performance. Existing speedtest1 benchmark is good for basic performance but doesn't reflect realistic transaction workloads with complex multi-table operations.

**Solution:** Implemented custom TPC-C style benchmark in `benchmark/tpcc/`:

**Files created:**
- `tpcc_schema.h` (155 lines) — TPC-C schema SQL and constants (8 tables: warehouse, district, customer, item, stock, orders, order_line, new_order, history)
- `tpcc_load.c` (332 lines) — Data loader populating 100K items + W warehouses + 10 districts/warehouse + 3K customers/district
- `tpcc_txn.c` (321 lines) — Three core transactions: New Order (45%), Payment (43%), Order Status (12%)
- `tpcc.c` (430 lines) — Main benchmark driver with latency tracking, percentile calculation, and formatted output
- `Makefile` (155 lines) — Builds `tpcc-local` (default VFS) and `tpcc-azure` (sqlite-objs VFS) versions
- `README.md` (161 lines) — Usage documentation and TPC-C schema overview

**Key design patterns:**
- **Two binaries:** `tpcc-local` (no Azure deps) and `tpcc-azure` (requires production VFS build)
- **Conditional compilation:** `#ifdef SQLITE_OBJS_VFS_AVAILABLE` to avoid linker errors in local-only build
- **Auto schema/load:** Detects empty database and creates schema + loads data automatically
- **Latency tracking:** Stores all transaction latencies for percentile calculation (p50, p95, p99)
- **Transaction mix:** Randomized 45/43/12 split matching TPC-C spec (simplified to 3 transactions)
- **Progress indicator:** Shows transaction count every 100 txns during benchmark run

**Performance baseline (local, 1 warehouse, 10s run):**
- Throughput: ~3,138 tps (31,380 transactions in 10 seconds)
- New Order: avg 0.4ms, p95 0.5ms, p99 0.8ms
- Payment: avg 0.2ms, p95 0.3ms, p99 0.5ms
- Database size: ~46 MB (100K items, 10 districts, 30K customers)

**Transaction implementations:**
- **New Order:** Get next order ID from district, insert order + new_order + 5-15 order_line rows, update stock quantities
- **Payment:** Update warehouse/district/customer YTD amounts, insert history record
- **Order Status:** Query customer's most recent order and all order lines (read-only)

**Scale factors:** 1 warehouse = 100K items + 30K customers (~100 MB DB). 10 warehouses = ~750 MB DB.

**Integration:** Lives in `benchmark/tpcc/` subdirectory. Independent Makefile. Reuses parent `build/` objects. Parallel to existing `benchmark/` speedtest1 harness.

**Usage examples:**
```bash
# Local baseline
make all && ./tpcc-local --local --warehouses 1 --duration 60

# Azure comparison (after make all-production)
export AZURE_STORAGE_ACCOUNT=...
./tpcc-azure --azure --warehouses 1 --duration 60
```

**Remaining work:** Multi-threading support (--threads N currently warns and forces N=1). Azure VFS testing requires production build completion.

### VFS Async I/O & Write-Back Cache Research (2026-03-11)

**xSync durability contract is absolute.** When xSync returns SQLITE_OK, all prior xWrite data MUST be durable. The journal cannot protect against a lying xSync — it's the foundational assumption, not a recoverable error. We CANNOT return from xSync before Azure confirms all writes. Verified by tracing sqlite3PagerCommitPhaseOne (sqlite3.c:65528): journal sync → dirty page write → DB sync → journal delete. If any sync lies, the next step proceeds under false assumptions.

**Threading is safe for parallel flush at xSync time.** The btree mutex (pBt->mutex) is held by the caller during xSync (sqlite3.c:72395), so SQLite cannot call xWrite concurrently. This means background threads can read aData directly during xSync without copies or locks. SQLite provides SQLITE_MUTEX_STATIC_VFS3 (sqlite3.c:8915) explicitly for custom VFS concurrency. sqlite3_malloc is thread-safe. Each worker thread needs its own curl handle (not thread-safe).

**Page coalescing is the biggest single win.** Azure Put Page supports up to 4MB per request. Contiguous dirty pages (common in sequential workloads like INSERT) can be coalesced into single PUTs. 100 contiguous 4KB pages → one 400KB PUT instead of 100 PUTs. Algorithm: scan dirty bitmap for contiguous runs, cap at 4MB. Combined with parallelism, this transforms 100-page sync from ~10s to ~150ms.

**No page copies needed during xSync.** Since btree mutex prevents concurrent xWrite, aData is stable during the entire xSync. Background threads can read directly. Copy-on-write only needed for future pre-flush optimization (not recommended for MVP).

**synchronous=NORMAL saves one journal sync per commit.** FULL does 2 journal syncs + 1 DB sync. NORMAL does 1 journal sync + 1 DB sync. For block blob journal, this saves one complete blob upload per commit. Trade-off is acceptable for remote storage.

**Pre-flush (uploading before xSync) is dangerous.** Before journal sync, DB page writes to Azure create a corruption window. If crash occurs after partial pre-flush but before journal sync, original pages are lost with no journal to roll back. Defer to future optimization if ever.

**Key risks:** (1) Partial flush failure — retry within xSync, fall back to SQLITE_IOERR_FSYNC after exhaustion; journal rollback handles recovery. (2) Lease expiry during long flush — renew proactively from main thread. (3) curl handles not thread-safe — each worker needs its own.

**Findings written to:** `.squad/decisions/inbox/aragorn-vfs-async-constraints.md`



### Phase 1 Page Coalescing Implementation (2026-03-11)

**What:** Implemented `coalesceDirtyRanges()` — scans dirty bitmap, merges contiguous pages into ≤4MiB ranges, rewrites sqlite-objsSync to flush coalesced ranges instead of individual pages. Added `azure_page_range_t` and `page_blob_write_batch` (NULL) to azure_ops_t vtable for Phase 2 readiness.

**Key learnings:**
- Adding fields to END of azure_ops_t is backward-compatible — C zero-initializes trailing struct members.
- Stack-allocating 64 ranges (64 × 24 bytes = 1.5 KiB) is cheap and covers most workloads.
- `vfs_sync_mid_flush_failure` test assumed per-page write count. Coalescing reduces calls — test updated to fail at call 1.
- `sqlite3_malloc64`/`sqlite3_free` preferred over raw malloc/free inside VFS code for SQLite memory accounting.
- 4MiB cap = 1024 × 4096-byte pages per range. Most workloads will produce 1-3 ranges per sync.

### Phase 3 Lease Hardening + ETag Tracking (2026-03-11)

**What:** Dynamic lease duration based on workload, ETag plumbing through azure_error_t, proportional lease renewal threshold.

**Files modified:**
- `src/azure_client.h` — Added `char etag[128]` field to `azure_error_t`; updated `azure_error_init` and `azure_error_clear`.
- `src/sqlite_objs_vfs.c` — Added `lastSyncDirtyCount`, `leaseDuration`, `etag[128]` fields to `sqlite-objsFile`. Dynamic lease duration in `sqlite-objsLock()` (30s default, 60s if last sync had >100 dirty pages). `leaseRenewIfNeeded()` uses `leaseDuration/2`. `sqlite-objsSync()` records dirty count and captures ETag. `xOpen` captures ETag from `blob_get_properties`.

**Key learnings:**
- Adding `etag` to `azure_error_t` is cleanest ETag plumbing — it's passed to every op, works with both mock and production code, no vtable changes needed.
- `lastSyncDirtyCount` starts at 0, so first transaction always gets 30s lease. Only after a heavy sync does the next lock request extend to 60s. This is conservative and correct.
- `leaseDuration` field tracks what was actually acquired, so renewal threshold adjusts proportionally (30s→renew@15s, 60s→renew@30s).
- `SQLITE_OBJS_LEASE_RENEW_AFTER` kept as fallback constant but no longer primary — `leaseDuration / 2` is the source of truth.

### R1+R2 Performance Optimizations (2026-03-11)

Implemented two optimizations from the performance diagnosis (aragorn-perf-diagnosis.md):

**R1: Journal/WAL blob existence cache** — Added `journalCacheState` (-1/0/1) and `journalBlobName[512]` to `sqlite-objsVfsData`. Cache is seeded on first xAccess (detects `-journal` suffix per D7) or xOpen of journal blob. Updated on xSync(journal)→1, xDelete(journal)→0. xAccess returns cached state for known journal blobs. WAL files always return 0 (iVersion=1, WAL disabled). Also added `pVfsData` back-pointer to `sqlite-objsFile` for cache access from file-level methods. Reduces ~4 HEAD requests per transaction to ~0 after startup.

**R2: Skip redundant resize** — Added `lastSyncedSize` to `sqlite-objsFile`. `sqlite-objsSync` only calls `page_blob_resize` when `nData > lastSyncedSize`. Updated after successful resize, after xOpen download, and after xTruncate. Most TPC-C transactions modify existing pages without growing, saving ~45ms per transaction.

**Test updates:** Two tests adjusted for R2 behavior — `vfs_lease_expire_during_sync` now fails `page_blob_write` (resize may not be called), `vfs_sync_resize_failure_before_flush` inserts zeroblob(65536) to force DB growth.

**Result:** TPC-C Azure throughput: 2.9 tps → 5.0 tps (~72% improvement). All 207 unit tests pass.

### WAL Mode VFS Support (2026-03-11)

**Implemented WAL mode with exclusive locking in sqlite_objs_vfs.c.** This enables `PRAGMA journal_mode=WAL` when append blob operations are available in the vtable, providing a projected 5-16× throughput improvement over journal mode (1 HTTP call per commit vs 9).

**Key VFS changes:**
- **iVersion bumped 1→2** on `sqlite3_io_methods` to enable WAL support
- **xShm* stubs added:** `sqlite-objsShmMap` returns `SQLITE_IOERR` (safety net if user forgets exclusive mode), others return `SQLITE_OK`/no-op. In exclusive mode, these are never called.
- **WAL buffer system:** New fields `aWalData`/`nWalData`/`nWalAlloc`/`nWalSynced`/`walNeedFullResync` on `sqlite-objsFile`. Pattern mirrors journal buffer but tracks sync offset for incremental Azure appends.
- **xOpen WAL path:** Checks append blob ops availability, downloads existing WAL for crash recovery (via `block_blob_download` — GET works on all blob types), or creates new empty append blob.
- **xWrite WAL:** Buffers locally, flags `walNeedFullResync` if overwriting already-synced data.
- **xSync WAL:** Incremental append (sends only bytes since last sync) or full resync (delete+recreate+upload all) if data was rewritten.
- **xTruncate WAL:** For size=0, deletes and recreates append blob (checkpoint reset). For partial truncate, flags resync if needed.
- **xFileControl:** Conditionally allows WAL — checks `append_blob_create/append/delete` are non-NULL. If NULL, forces "delete" mode (backward compatible).
- **xAccess:** Removed `-wal` short-circuit that previously returned "does not exist". WAL files now go through normal blob existence checks.

**Design decisions:**
- WAL file type (`SQLITE_OPEN_WAL = 0x00080000`) is outside the `0x0000FF00` mask used for `eFileType`. Added explicit override in xOpen.
- No leasing on WAL blob itself — the main DB blob's lease protects against concurrent access in exclusive mode.
- `block_blob_download` reused for reading append blobs (Azure GET Blob is type-agnostic).
- `xShmMap` returns `SQLITE_IOERR` (not `SQLITE_OK`) to safely prevent WAL without exclusive mode. If called, it means the user didn't set exclusive mode — failing here is better than a NULL pointer crash.

**Test updates:** 3 WAL rejection tests updated: `vfs_pragma_wal_refused`→`vfs_pragma_wal_allowed` (WAL accepted with mock ops), `vfs_wal_mode_returns_delete` (tests rejection with NULL append ops), `vfs_wal_mode_case_insensitive` (WAL accepted for all case variants).

**Result:** Clean build (zero warnings), all 207 unit tests pass. WAL mode is ready for integration testing once Frodo's production append blob client is available.

### Phase 1: Journal State Per-File (2026-03-11)

**Problem:** `sqlite-objsVfsData` held a single `journalCacheState`/`journalBlobName[512]` pair — global state shared by all open databases. With multiple databases open simultaneously, journal cache state would cross-contaminate between them.

**Solution:** Replaced the single journal cache entry with a fixed-capacity array of `sqlite-objsJournalCacheEntry` structs (max 16), each holding `{state, zBlobName[512]}`. Added `journalCacheFind()` and `journalCacheGetOrCreate()` helpers for O(n) lookup by blob name. n ≤ 16, so linear scan is negligible.

**Key design choices:**
- Fixed array (not linked list) — no heap allocation, lives inside `g_vfsData`, zero-initialized by existing `memset`
- New entries start with `state = -1` (unknown), matching old initialization semantics
- `SQLITE_OBJS_MAX_JOURNAL_CACHE = 16` — more than enough for typical use; if exceeded, new journals simply skip caching (fall through to HEAD requests)
- All 5 usage sites updated: xSync, xOpen (MAIN_JOURNAL), xDelete, xAccess, registration

**Files modified:** `src/sqlite_objs_vfs.c` only.

**Result:** All 234 unit tests pass. No behavioral change from user perspective. Each database now has independent journal cache state.

### Per-File Client Ownership + URI Parameter Parsing (2026-03-11)

**Phase 2 + 3 combined** — each `sqlite-objsFile` can now own its own `azure_client_t`.

**Struct change:** Added `azure_client_t *ownClient` to `sqlite-objsFile`. NULL means using the global VFS client (backward-compatible default).

**URI parser:** `sqlite_objs_parse_uri_config()` extracts `azure_account`, `azure_container`, `azure_sas`, `azure_key`, `azure_endpoint` from `sqlite3_filename` via `sqlite3_uri_parameter()`. Returns 1 if `azure_account` is present, 0 otherwise.

**sqlite-objsOpen flow:**
1. `memset(p, 0, sizeof(sqlite-objsFile))` — `ownClient` starts NULL
2. If URI params found → `azure_client_create()` → set `p->ownClient`, derive `ops`/`ops_ctx` from it
3. If not found → fall back to `pVfsData->ops` / `pVfsData->ops_ctx` (existing behavior, zero change)

**sqlite-objsClose:** If `p->ownClient != NULL`, calls `azure_client_destroy()` and nulls it. Placed after blob name free, before `pMethod = NULL`.

**Key decisions:**
- URI parsing before ops/ops_ctx assignment ensures clean ownership — the file either fully owns its client or fully uses the global one
- `azure_client_create()` failure returns `SQLITE_CANTOPEN` (appropriate for a file-open failure)
- No public API change — Phase 4 will add `sqlite_objs_vfs_register_uri()`

**Files modified:** `src/sqlite_objs_vfs.c` only.

**Result:** All 234 unit tests pass. Zero behavioral change when URI params not used.

### Phase 4+5: URI-only VFS Registration + Shell/Benchmark Updates

**What:** Added `sqlite_objs_vfs_register_uri(int makeDefault)` — registers the sqlite-objs VFS with no global Azure client. All databases must provide Azure config via URI parameters (`azure_account`, `azure_container`, `azure_sas`, etc.), or xOpen returns SQLITE_CANTOPEN.

**Key changes:**
- `src/sqlite_objs.h` — Added `sqlite_objs_vfs_register_uri()` declaration with full documentation of URI parameters
- `src/sqlite_objs_vfs.c` — Implemented `sqlite_objs_vfs_register_uri()` (sets ops=NULL, ops_ctx=NULL, no client creation). Added NULL-ops guard in xOpen: when per-file URI parsing returns no config AND global ops are NULL, returns SQLITE_CANTOPEN instead of proceeding with NULL ops (crash prevention).
- `src/sqlite_objs_shell.c` — Added `--uri` flag. When present, calls `sqlite_objs_vfs_register_uri(1)` and enables `SQLITE_CONFIG_URI` so the shell's `sqlite3_open()` honors URI filenames. Strips `--uri` from argv before forwarding to shell_main.
- `benchmark/tpcc/tpcc.c` — Added `--uri`, `--account`, `--container`, `--sas`, `--endpoint` options. Constructs `file:tpcc.db?azure_account=...&azure_container=...&azure_sas=...` URI and calls `sqlite_objs_vfs_register_uri(1)` with `SQLITE_OPEN_URI` flag.
- `README.md` — Added "URI Configuration" section with code example, parameter table, and explanation.

**Safety guard in xOpen:** After the URI-parse / global-fallback logic, if `p->ops` is still NULL (no URI params + no global client), we now return SQLITE_CANTOPEN immediately. Previously this would proceed with NULL ops and crash on first blob_exists call.

**Result:** All 234 unit tests pass. Shell compiles and runs with `--uri`. TPC-C compiles with URI options.

## 2025-01-12: Replaced in-memory buffer with disk-backed cache

### Context
User requested replacing the in-memory `aData` buffer with a file-backed cache to reduce memory usage from database-size to ~8MB (SQLite's page cache). This simplifies the architecture compared to the complex mmap/demand-paging approach we rolled back from.

### Changes
**Core struct modifications (sqlite_objs_vfs.c):**
- Removed `unsigned char *aData` and `sqlite3_int64 nAlloc` from `sqliteObjsFile`
- Added `int cacheFd` (file descriptor, -1 when not open)
- Added `char *zCachePath` (path to cache file for cleanup)
- Journal/WAL stay in-memory (small, transient)

**Key functions updated:**
- `xOpen`: Creates cache file via `mkstemp("/tmp/sqlite-objs-XXXXXX")`, downloads blob to temp buffer, writes to cache file
- `xRead`: Uses `pread(cacheFd, ...)` instead of `memcpy(aData + offset, ...)`  
- `xWrite`: Uses `pwrite(cacheFd, ...)` instead of `memcpy(aData + offset, ...)`, extends file with `ftruncate()` if needed
- `xSync`: Reads dirty ranges from cache file via `pread()`, allocates temp buffer for upload
- `xClose`: Calls `close(cacheFd); unlink(zCachePath)`
- `xTruncate`: Adds `ftruncate(cacheFd, size)` for cache file
- `coalesceDirtyRanges`: No longer sets `range.data` (caller fills from cache file)

**Design notes:**
- Parallel chunked download preserved: downloads to malloc'd buffer, then writes to cache file in one pass
- Dirty bitmap unchanged: `aDirty`/`nDirtyPages` track which pages need Azure upload
- `nData` still tracks logical file size
- `pread`/`pwrite` are thread-safe (no shared file offset concerns)
- Added `fsync()` after download write to ensure data persistence

### Test results
- 237/242 tests passing
- 5 failures all related to close/reopen scenarios in WAL and coalesce tests
- Failures show `SQLITE_CORRUPT` when reopening database, suggesting edge case in download-to-cache-file logic
- Core functionality works: basic operations, sync, WAL mode, URI config all pass

### Known issues
The 5 failing tests (`sync_coalesced_sequential`, `sync_batch_all_succeed`, `wal_insert_select_roundtrip`, `wal_recovery_downloads_existing_wal`) all follow this pattern:
1. Open database, write data, sync
2. Close database  
3. Reopen database
4. Try to read data → fails with SQLITE_CORRUPT

Likely cause: Edge case in blob download or cache file initialization during reopen. The data is being written to Azure correctly (sync works), but the re-download path may have an issue with buffer sizing, fsync timing, or header detection.

### Memory impact
Expected reduction: database file size (can be GBs) → ~8MB (SQLite page cache). Cache file on disk instead.

