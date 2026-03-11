# Project Context

- **Owner:** Quetzal Bradley
- **Project:** Azure Blob-backed SQLite (azqlite) — a drop-in replacement for SQLite where all storage is backed by Azure Blob Storage, implemented as a custom VFS layer. MIT licensed.
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

**VFS registration:** `sqlite3_vfs_register(pVfs, makeDflt)`. Select via `sqlite3_open_v2(file, &db, flags, "azqlite")` or URI `?vfs=azqlite`.

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
- **VFS name "azqlite"**, non-default registration. Usage: `sqlite3_open_v2(name, &db, flags, "azqlite")`.
- **Source layout:** `src/` (azqlite_vfs.c, azure_client.c/.h, azure_auth.c, azure_error.c, azqlite.h), `test/` (test_main.c, test_vfs.c, mock_azure_ops.c, test_integration.c).
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
- `src/azqlite.h` — Public API header with `azqlite_vfs_register()` and `azqlite_vfs_register_with_config()`, `azqlite_config_t` with test-seam (ops/ops_ctx override)
- `src/azure_client.h` — Internal header defining `azure_ops_t` vtable (13 ops), `azure_err_t` enum, `azure_error_t`, `azure_buffer_t`, `azure_client_t` lifecycle functions
- `src/azqlite_vfs.c` — Complete VFS: azqliteFile struct, io_methods v1 (12 methods), VFS methods (xOpen/xDelete/xAccess/xFullPathname + delegated), registration, dirty bitmap, lease management
- `src/azure_client_stub.c` — Stub ops returning AZURE_ERR_UNKNOWN for all 13 operations. Frodo replaces this.
- `src/azqlite_shell.c` — CLI wrapper: renames shell.c main to shell_main via #define, registers VFS as default, forwards to shell_main
- `Makefile` — Targets: all (libazqlite.a + azqlite-shell), test-unit, test-integration, test, clean, amalgamation (stub)

**Key patterns:**
- azqliteFile struct embeds `const sqlite3_io_methods *pMethod` as first member (sqlite3_file subclass contract)
- Dirty page bitmap: 1 bit per page, `dirtyMarkPage()` tracks writes, `dirtyClearAll()` after sync
- Buffer grows geometrically via `bufferEnsure()` / `jrnlBufferEnsure()`
- Journal files (MAIN_JOURNAL) use separate aJrnlData buffer, uploaded as block blob on xSync
- Lease: acquire at xLock(RESERVED+), renew inline at xSync and xWrite (>15s), release at xUnlock(≤SHARED)
- `azureErrToSqlite()` maps CONFLICT/THROTTLED→SQLITE_BUSY, all else→SQLITE_IOERR variants
- xOpen routes by flag bits: MAIN_DB/MAIN_JOURNAL→Azure, everything else→default VFS xOpen
- szOsFile = max(sizeof(azqliteFile), defaultVfs->szOsFile) to handle delegation
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
- Static library: build/libazqlite.a containing sqlite3.o + azqlite_vfs.o + azure_client_stub.o

### Benchmark Harness Implementation (2026-03-10)

**Problem:** Need to compare local SQLite vs azqlite performance using SQLite's official speedtest1 benchmark. speedtest1.c calls `exit()` directly, making it unsuitable for inclusion as a library.

**Solution:** Three-binary approach:
1. `benchmark` — Lightweight harness that shells out to speedtest1 binaries and measures wall-clock time
2. `speedtest1` — Standard speedtest1 linked against vanilla SQLite
3. `speedtest1-azure` — speedtest1 linked against azqlite VFS via wrapper

**Files created:**
- `benchmark/benchmark.c` — Main harness using `system()` to invoke speedtest1 binaries, timing via `gettimeofday()`, formatted output
- `benchmark/speedtest1_wrapper.c` — Thin wrapper that registers azqlite VFS as default before calling speedtest1_main
- `benchmark/speedtest1.c` — Downloaded from SQLite upstream (test/speedtest1.c)
- `benchmark/Makefile` — Builds all three binaries, handles stub vs production builds
- `benchmark/README.md` — Usage documentation

**Key design decisions:**
- **Process isolation:** Each speedtest1 run is a separate process via `system()`. This avoids the `exit()` problem and gives clean timing.
- **Silent subprocess output:** speedtest1 output redirected to `/dev/null` to keep harness output clean. Users can run speedtest1 binaries directly for detailed output.
- **Exit code tolerance:** speedtest1 returns 1 if optional features (rtree) are missing. Harness treats exit codes 0 and 1 as success.
- **Two speedtest1 binaries:** One linked against vanilla SQLite, one against azqlite. Benchmark harness invokes the appropriate binary.
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

**Build:** Test binary links `sqlite3.o + azqlite_vfs.o + azure_client_stub.o + mock_azure_ops.o`. Added `make all-production` target. Fixed `##__VA_ARGS__` to C11-compliant split fprintf. Added `azqlite_vfs_register_with_ops()` convenience wrapper. Fixed 2 test logic bugs (invalid blob content, fast-test lease timing).

**Result:** `make all` + `make test-unit` (148/148) both pass clean.

### Code Review Blockers — Pending Fixes (2026-03-10)

Gandalf's review identified two critical issues (C1, C2) that must be fixed before demo:

**C1 (MY RESPONSIBILITY): Device Characteristics Flag Error (azqlite_vfs.c:693)**
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

**Problem:** Need a realistic OLTP benchmark to compare local SQLite vs azqlite performance. Existing speedtest1 benchmark is good for basic performance but doesn't reflect realistic transaction workloads with complex multi-table operations.

**Solution:** Implemented custom TPC-C style benchmark in `benchmark/tpcc/`:

**Files created:**
- `tpcc_schema.h` (155 lines) — TPC-C schema SQL and constants (8 tables: warehouse, district, customer, item, stock, orders, order_line, new_order, history)
- `tpcc_load.c` (332 lines) — Data loader populating 100K items + W warehouses + 10 districts/warehouse + 3K customers/district
- `tpcc_txn.c` (321 lines) — Three core transactions: New Order (45%), Payment (43%), Order Status (12%)
- `tpcc.c` (430 lines) — Main benchmark driver with latency tracking, percentile calculation, and formatted output
- `Makefile` (155 lines) — Builds `tpcc-local` (default VFS) and `tpcc-azure` (azqlite VFS) versions
- `README.md` (161 lines) — Usage documentation and TPC-C schema overview

**Key design patterns:**
- **Two binaries:** `tpcc-local` (no Azure deps) and `tpcc-azure` (requires production VFS build)
- **Conditional compilation:** `#ifdef AZQLITE_VFS_AVAILABLE` to avoid linker errors in local-only build
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

**What:** Implemented `coalesceDirtyRanges()` — scans dirty bitmap, merges contiguous pages into ≤4MiB ranges, rewrites azqliteSync to flush coalesced ranges instead of individual pages. Added `azure_page_range_t` and `page_blob_write_batch` (NULL) to azure_ops_t vtable for Phase 2 readiness.

**Key learnings:**
- Adding fields to END of azure_ops_t is backward-compatible — C zero-initializes trailing struct members.
- Stack-allocating 64 ranges (64 × 24 bytes = 1.5 KiB) is cheap and covers most workloads.
- `vfs_sync_mid_flush_failure` test assumed per-page write count. Coalescing reduces calls — test updated to fail at call 1.
- `sqlite3_malloc64`/`sqlite3_free` preferred over raw malloc/free inside VFS code for SQLite memory accounting.
- 4MiB cap = 1024 × 4096-byte pages per range. Most workloads will produce 1-3 ranges per sync.
