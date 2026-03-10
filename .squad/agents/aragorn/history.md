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
- xDeviceCharacteristics: ATOMIC512 | SAFE_APPEND
- xSectorSize: 512 (Azure page blob alignment)
- Shell uses #define main shell_main trick to include shell.c as a translation unit

**Build system:**
- `-w` flag on sqlite3.c and shell.c compilation (suppress upstream warnings)
- Platform detection: Darwin omits -ldl, Linux includes it
- Static library: build/libazqlite.a containing sqlite3.o + azqlite_vfs.o + azure_client_stub.o

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


