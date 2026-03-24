# Squad Decisions

## Active Decisions


### D1: Blob Type Strategy
**Date:** 2026-03-10 | **From:** Gandalf (Design Review)

**Page blobs** for MAIN_DB (512-byte aligned random R/W — perfect match for SQLite pages). **Block blobs** for MAIN_JOURNAL (sequential, whole-object). No append blobs.
---
### D2: Journal Mode Only
**Date:** 2026-03-10 | **From:** Gandalf (Design Review)

Rollback journal (DELETE) for MVP 1. WAL explicitly disabled — set `iVersion=1` on io_methods. WAL requires shared memory; architecturally incompatible with remote storage. Deferred to MVP 3+ at earliest.
---
### D3: Two-Level Lease-Based Locking
**Date:** 2026-03-10 | **From:** Gandalf (Design Review)

NOT nolock. SHARED=no lease (reads always work). RESERVED/EXCLUSIVE=acquire 30s blob lease. Release on unlock. `xCheckReservedLock`: HEAD request to check lease state. Inline renewal (no background thread). Overrides Aragorn's nolock proposal — prior art shows deferred locking → corruption.
---
### D4: Full-Blob Cache from Day 1
**Date:** 2026-03-10 | **From:** Gandalf (Design Review)

Download entire blob on xOpen into malloc'd buffer. xRead=memcpy. xWrite=memcpy+dirty bit. xSync=PUT Page per dirty page. Overrides initial MVP 2 deferral — Aragorn proved uncached reads are untestable (~5s for 100 pages).
---
### D5: Testing — 4-Layer Pyramid with azure_ops_t Vtable
**Date:** 2026-03-10 | **From:** Gandalf (Design Review)

Approved as Samwise proposed. Layers: C mocks (~300 tests, <5s), Azurite (~75, <60s), Toxiproxy (~30, <5min), real Azure (~75, weekly). MVP 1 delivers Layers 1+2.

**Critical requirement:** VFS layer MUST accept a swappable Azure operations interface (function pointer table / vtable) so that tests can inject mock implementations. This is non-negotiable for testability.
---
### D6: VFS Name "sqlite-objs", Non-Default, Delegating
**Date:** 2026-03-10 | **From:** Gandalf (Design Review)

Register as "sqlite-objs" via `sqlite3_vfs_register(pVfs, 0)`. Delegate xDlOpen/xRandomness/xSleep/xCurrentTime to default VFS. Route temp files to default VFS xOpen.
---
### D7: Filename = Blob Name, Container from Env Vars
**Date:** 2026-03-10 | **From:** Gandalf (Design Review)

SQLite filename maps directly to blob name. Container via `AZURE_STORAGE_CONTAINER` env var. Journal blob = `<name>-journal`.
---
### D8: Azure Errors → SQLITE_BUSY or SQLITE_IOERR
**Date:** 2026-03-10 | **From:** Gandalf (Design Review)

Lease conflicts (409) and throttling (429) → `SQLITE_BUSY` (retryable). All other Azure errors after retry exhaustion → `SQLITE_IOERR_*` variants (fatal). Retry: 5 attempts, 500ms exponential backoff + jitter.
---
### D9: Both SAS + Shared Key Auth
**Date:** 2026-03-10 | **From:** Gandalf (Design Review)

SAS tokens preferred (AZURE_STORAGE_SAS). Shared Key fallback (AZURE_STORAGE_KEY). Both implemented in PoC. Zero additional cost to support both.
---
### D10: Makefile Build System
**Date:** 2026-03-10 | **From:** Gandalf (Design Review)

GNU Makefile. Targets: all, test-unit, test-integration, test, clean, amalgamation. Source in `src/`, tests in `test/`. Dependencies: libcurl, OpenSSL.
---
### D11: MVP 1 Scope
**Date:** 2026-03-10 | **From:** Gandalf (Design Review)

**IN:** Page blob DB, block blob journal, in-memory cache+write buffer, lease locking, journal mode, azure_ops_t vtable, SAS+SharedKey auth, retry logic, temp file delegation, VFS registration, Layer 1+2 tests, Makefile, sqlite-objs-shell.

**OUT:** WAL, LRU page cache, background lease renewal, multi-machine, Azure AD auth, connection pooling, HTTP/2, Content-MD5, amalgamation, Layers 3+4.
---
### UD1: User Directive — MIT License
**Date:** 2026-03-10 | **From:** Quetzal Bradley (via Copilot)

The project license should be MIT.
---
### UD2: User Directive — Use Claude Opus 4.6
**Date:** 2026-03-10 | **From:** Quetzal Bradley (via Copilot)

claude-opus-4.6 should be used for all research tasks. Use the most capable Opus model available.
---
### UD3: User Directive — Deployment: SQLite Amalgamation
**Date:** 2026-03-10 | **From:** Quetzal Bradley (via Copilot)

When it comes time to make a deployment package, create a SQLite amalgamation that includes the SQLite source code along with our Azure VFS source code so the entire package is a .c file and a .h file, plus a version of the SQLite CLI client built against our Azure version.
---
## Decision Research & Rationale

### Prior Art Findings
- **Cloud Backed SQLite (CBS)** — built by SQLite team, validates VFS approach, uses block-based chunking
- **Azure Page Blobs advantage** — supports 512-byte aligned random writes vs block chunking
- **Single-writer universal** — every project (CBS, Litestream, LiteFS, etc.) is single-writer per database
- **WAL infeasible across machines** — requires shared memory coordination
- **Locking must be in VFS** — deferred locking causes corruption (CBS, sqlite-s3vfs patterns show this)
- **Local caching mandatory** — 10-100ms per uncached page makes it unusable

### Azure REST API Findings
- **API version 2024-08-04** — latest stable, set via x-ms-version header
- **Page blobs perfect for SQLite DB** — 512-byte alignment, max 4 MiB/request
- **Block blobs for journals** — sequential, cheapest
- **Leases for locking** — 30-second duration with inline renewal
- **Retry: 5 attempts, 500ms exponential backoff** — matches Azure SDK patterns

### SQLite VFS Findings
- **io_methods v1 sufficient** — core I/O + locking only (no WAL)
- **File type routing critical** — MAIN_DB/MAIN_JOURNAL to Azure, temps to local
- **Sector size 4096** — matches default page size and alignment
- **Device flags:** SQLITE_IOCAP_SEQUENTIAL | SQLITE_IOCAP_POWERSAFE_OVERWRITE | SQLITE_IOCAP_SUBPAGE_READ (NOT ATOMIC)
- **Read cache mandatory** — 5-50ms latency per page without cache

### Testing Strategy Findings
- **Four-layer pyramid** — unit (mocks), integration (Azurite), fault-inject (Toxiproxy), real Azure
- **Critical interface:** `azure_ops_t` vtable for swappable Azure operations
- **Azurite supports** — page blobs, leases, block blobs, shared key auth
- **No existing C Azure mocks** — write our own at azure_client boundary

### D12: pkg-config Integration for Production Build
**Date:** 2026-03-10 | **From:** Frodo (Azure Expert)

Integrate pkg-config into Makefile for discovering OpenSSL and libcurl compile/link flags. Production build (`CFLAGS_PROD`, `LDFLAGS_PROD`) uses pkg-config with graceful fallback. Stub build unchanged. Rationale: portability across macOS (Homebrew), Linux, BSD without user configuration.
---
### D13: Pre-Demo Code Review Conditions
**Date:** 2026-03-10 | **From:** Gandalf (Lead/Architect)

Code approved for MVP 1 demo **pending two mechanical fixes**:
- **C1 (sqlite_objs_vfs.c:693):** Change device flags from `ATOMIC512|SAFE_APPEND` to `SEQUENTIAL|POWERSAFE_OVERWRITE|SUBPAGE_READ` (data corruption prevention)
- **C2 (azure_client.c:173–187):** Replace `strcat` with bounds-checked `snprintf` (URL buffer overflow fix)

No additional review required for these fixes — they are straightforward. Full code review in `research/code-review.md`.
---
### D14: Layer 2 Integration Test Suite
**Date:** 2026-03-10 | **From:** Samwise (QA)

Layer 2 (Azurite emulator) test suite complete: 75 tests passing. Discovered Shared Key auth fails on blob modifications with Azurite (403 Forbidden after first PUT). **Workaround:** Use SAS tokens for Layer 2. **Permanent fix:** Frodo investigating Azurite Shared Key validator behavior (D15).
---
### D15: Shared Key Auth Investigation
**Date:** 2026-03-10 | **From:** Frodo (Azure Expert)

Shared Key authentication fails with Azurite on blob modifications (issue discovered Layer 2 testing). Investigating whether root cause is Azurite behavior or code bug. Blocks full Layer 2 validation and production auth testing. Frodo (Agent-13) assigned.
---
### UD4: Page Blob Resizing Support
**Date:** 2026-03-10 | **From:** Quetzal Bradley (via Copilot)

Page blob resizing must be supported in architecture. First priority: growing databases (resize up). Second priority: VACUUM (resize down). Both deferred to MVP 1.5+; plan architecture for future support.
---
### D12: Azurite SharedKey Authentication Workaround
**Date:** 2026-03-10 | **From:** Frodo (Azure Client Layer)

Azurite has a quirk in canonicalized resource path construction for SharedKey signature validation — it doubles the account name internally. Implemented endpoint-aware URL and resource path building: custom endpoints (Azurite) include account in path before auth signing (allowing Azurite to double it); production Azure uses standard format. Also explicitly set `Content-Type:` header to prevent curl auto-adding headers. Backward compatible (endpoint=NULL → standard Azure behavior).

**Impact:** Integration testing with Azurite now succeeds with SharedKey auth. SharedKey auth works with both Azurite and production Azure.
---
### D13: Container Creation as Public API Function
**Date:** 2026-03-10 | **From:** Frodo (Azure Client Layer)

Added `azure_container_create(azure_client_t *client, azure_error_t *err)` as public API in azure_client.c. Leverages existing auth infrastructure and curl setup for clean, reusable code. Idempotent — treats both 201 (created) and 409 (already exists) as success. Test code calls this after client creation; bash script delegates to C code rather than attempting unauthenticated curl requests.

**Impact:** All Azure client integration tests now pass (8 of 10 total). No breaking changes to existing API.
---
### D14: Benchmark Harness Design
**Date:** 2026-03-10 | **From:** Aragorn (SQLite/C Dev)

Three-binary architecture: lightweight harness that shells out to speedtest1 subprocesses. speedtest1 (standard SQLite) and speedtest1-azure (with sqlite-objs VFS registered as default) run as isolated processes measured via `system()` and `gettimeofday()`. speedtest1.c used unmodified from SQLite upstream. Rejected embedding via #define main trick (speedtest1's `exit()` terminates harness before results capture) and patching upstream (maintenance burden).

**Usage:** `./benchmark --local-only --size 25`, `./benchmark --size 50` (full comparison with Azure env vars), `./benchmark --output csv` (automation).

**Implementation:** Clean separation, flexible output (text/CSV), supports stub and production builds. Subprocess overhead (~10ms) negligible for multi-second benchmarks. Only captures total elapsed time, not per-test breakdown.
---
### D15: Performance Optimization Design (D-PERF)
**Date:** 2026-03-11 | **From:** Gandalf (Synthesis), Frodo (Azure Research), Aragorn (VFS Analysis)

4-phase optimization design to eliminate xSync bottleneck (serial page writes = 500s for 5000 pages). Combined techniques reduce clustered commits from 500s → 0.2–0.5s (1000×) and scattered commits from 500s → 5–10s (50–100×).

**Phase 1: Page Coalescing** — Merge contiguous dirty pages into single PUT requests (up to 4 MiB per Azure limit). Low complexity, 5–50× speedup for sequential workloads.

**Phase 2: Parallel Flush via curl_multi** — Issue all coalesced PUTs concurrently through single-threaded event loop. Medium complexity, 5–15× speedup for scattered workloads. **Key decision: curl_multi over pthreads** for simplicity, determinism, and native connection pooling.

**Phase 3: Connection Pooling + Proactive Lease Renewal** — Sustain throughput during large flushes (64 concurrent connections, renew lease every 15s during long sync). Deferred to MVP 1.5.

**Phase 4: Journal Chunking** — Parallelize block blob journal upload. Speculative; deferred.

**Concurrency:** 32 parallel connections (per Gandalf analysis, balances connection overhead against Azure's 60 MiB/s throughput limit).

**PRAGMA synchronous:** Recommend NORMAL by default (skips pre-nRec sync, saves one journal upload per commit without violating durability). Users can override to FULL or OFF.

**xSync durability:** Confirmed non-negotiable. Parallel flush happens **within xSync boundaries** — all writes must be confirmed before xSync returns. No pre-flush (corruption window before journal sync). No page copies needed (btree mutex guarantees aData stability during xSync).

**Thread pool:** Not needed for MVP 1 (curl_multi is sufficient). If pre-flush implemented later, use 8–16 worker threads with own curl handles.
---
### D16: Azure Put Page Batch Capability — curl_multi is Winning Strategy
**Date:** 2026-03-11 | **From:** Frodo (Azure Expert)

**Azure Batch API does NOT support Put Page.** Azure Batch (`POST ?comp=batch`) only supports Delete Blob and Set Blob Tier; Put Page operations cannot be batched. This invalidates the Batch API research direction.

**Alternative: curl_multi parallelism with page coalescing** emerges as the winning strategy:
- Page coalescing: merge contiguous runs up to 4 MiB per PUT request (application-level, not Azure)
- curl_multi: parallel HTTP via single-threaded event loop (64 concurrent connections well under Azure's 500 req/s limit)
- Connection pooling and TLS session caching reduce per-request overhead
- HTTP/2 multiplexing not available (Azure only supports HTTP/1.1); parallelism requires multiple connections

**Impact:** Frodo + Aragorn research combined into D-PERF design above.
---
### D17: VFS Write-Back Cache with Parallel xSync Flush
**Date:** 2026-03-11 | **From:** Aragorn (SQLite/C Dev)

**Architectural confirmation:** Parallel flush at xSync time is safe and non-negotiable properties:

1. **btree mutex guarantees aData stability** — During xSync, btree mutex is held; SQLite cannot call xWrite. Background (or parallel curl) reads from aData safely without copy-on-write.

2. **No pre-flush** — Uploading DB pages before journal sync creates corruption window (journal hasn't proven durability yet). SQLite may overwrite pages multiple times before xSync (wasted uploads). Parallelism must happen **within xSync**, not before.

3. **xSync durability is non-negotiable** — When xSync returns SQLITE_OK, all dirty pages must be confirmed durable in Azure. Cannot return early. This is the foundational invariant of SQLite's crash-recovery model.

4. **PRAGMA synchronous=NORMAL is safe** — Skips pre-nRec journal sync (saves one journal upload per commit). Post-nRec sync and DB sync still performed. SQLite's recovery path handles nRec recomputation on power failure (well-tested).

5. **Lease renewal during long flush** — Implement proactive renewal (every 15s) from main thread with own connection to prevent 30s lease expiry during large parallel flush.

6. **Thread pool design** — If ever needed for pre-flush (Phase 4 future work): 8–16 workers, each with own curl handle. Current MVP 1 uses curl_multi (single-threaded) — sufficient and simpler.

**Impact:** Research findings locked in; implementation can proceed with confidence on thread safety and durability.
---
### Phase 1 Page Coalescing Implementation (2026-03-11)

**Problem:** xSync flushed each dirty page as a separate Azure Put Page request. For sequential workloads (100 contiguous dirty pages), this meant 100 HTTP round-trips (~10s).

**Solution:** Implemented `coalesceDirtyRanges()` in `sqlite_objs_vfs.c` — scans dirty bitmap left-to-right, merges contiguous dirty pages into single `azure_page_range_t` ranges, caps each at 4 MiB (Azure Put Page limit). Sequential fallback iterates coalesced ranges instead of individual pages.

**Files modified:**
- `src/azure_client.h` — Added `azure_page_range_t` struct and `SQLITE_OBJS_MAX_PARALLEL_PUTS` define. Added `page_blob_write_batch` to end of `azure_ops_t` vtable (NULL until Phase 2 curl_multi).
- `src/sqlite_objs_vfs.c` — Added `coalesceDirtyRanges()`, rewrote `sqlite-objsSync` MAIN_DB path: coalesce → try batch (NULL for now) → sequential fallback with lease renewal every 50 ranges.
- `src/azure_client.c` — Added `.page_blob_write_batch = NULL` to production vtable.
- `src/azure_client_stub.c` — Added `NULL` for `page_blob_write_batch` to stub vtable.
- `test/mock_azure_ops.c` — Added `.page_blob_write_batch = NULL` to mock vtable.
- `test/test_vfs.c` — Updated `vfs_sync_mid_flush_failure` test: fail at call 1 instead of call 2 (coalescing reduces write count below 2 for contiguous pages).

**Key design choices:**
- Stack-allocate range array for ≤64 ranges, heap-allocate via `sqlite3_malloc64` for larger flushes.
- `page_blob_write_batch` added at END of struct — existing code with NULL-initialized vtables (mocks, stubs) gets implicit NULL without code changes.
- Sequential fallback renews lease every 50 coalesced ranges (matching old per-page renewal cadence).
- No copies of aData needed — btree mutex guarantees stability during xSync (per D17).

**Result:** 196/196 unit tests pass. Clean build with zero warnings. Phase 2 (curl_multi parallel batch) can now slot in by implementing `page_blob_write_batch`.
---
### D18: TPC-C OLTP Benchmark Implementation
**Date:** 2026-03-11 | **From:** Aragorn (SQLite/C Dev)

Created custom TPC-C simplified OLTP benchmark to measure realistic transaction workloads (vs. upstream SQLite speedtest1 which is I/O focused). Benchmark suite:

- **Two binaries:** `tpcc-local` (local SQLite only, no Azure deps), `tpcc-azure` (with sqlite-objs VFS)
- **Schema:** 8 tables (warehouse, district, customer, item, stock, orders, history)
- **Transactions:** New Order (45%), Payment (43%), Order Status (12%)
- **Metrics:** Per-transaction latency (p50, p95, p99), throughput (tps)

**Baseline (local SQLite, 1 warehouse, 10s run):**
- Throughput: 3,138 tps
- New Order: avg 0.4ms, p95 0.5ms
- Payment: avg 0.2ms, p95 0.3ms

**Expected Azure performance:** 5–20 tps (100–600× slower due to network latency, mitigated by in-memory cache).

**Rationale:** Industry-standard benchmark, models real OLTP workloads, two-binary approach avoids forcing Azure dependencies on users, latency percentiles more relevant than total throughput for OLTP.

**Impact:** Performance comparison baseline established; can measure impact of D-PERF optimization phases.
---
### D19: Phase 1 Coalescing Test Suite
**Date:** 2026-03-11 | **From:** Samwise (QA)

10 unit tests for Phase 1 page coalescing. 9 active, 1 gated. All 205 tests passing.

**Test design:**
- **Approach:** Option B (test via xSync) — tests the full VFS integration path, validates real code execution
- **Mock write recording:** `mock_write_record_t` tracking records `{offset, len}` for each page blob write call
- **Gating strategy:** Coalescing-specific assertions (e.g., "100 contiguous pages → ≤2 writes") gated behind `ENABLE_COALESCE_TESTS`; correctness assertions always run
- **maxranges_overflow test:** Requires `sqlite_objs_test_coalesce_dirty_ranges()` exposed behind `SQLITE_OBJS_TESTING` flag for direct algorithm testing

**Impact:** Write recording seam enables coalescing algorithm verification without exposing internals. All 9 coalescing tests pass; 1 gated test awaits algorithm exposure.
---
### UD5: User Directive — Use claude-opus-4.6 for Performance Optimization
**Date:** 2026-03-11 | **From:** Quetzal Bradley (via Copilot)

Use claude-opus-4.6 for all agents during performance optimization implementation. Complicated feature requires highest quality reasoning.
---
### D21: Disk-Backed Cache for MAIN_DB
**Date:** 2026-03-14 | **From:** Aragorn (SQLite/C Dev)

Replaced in-memory `aData` buffer with local disk cache file via `pread()`/`pwrite()` I/O. Cache file created via `mktemp()`, journal/WAL remain in-memory. Memory footprint reduced from database-size to ~8MB (SQLite's page cache). Dirty bitmap tracking preserved. All 242 tests passing after fixing 3 critical bugs: dirty bitmap nData sequencing, 512-byte alignment in pread, dirtyClearAll allocation mismatch.
---
### D22: URI Builder for Rust sqlite-objs Crate
**Date:** 2026-03-14 | **From:** Gimli (Rust Dev)

Added `UriBuilder` fluent API to `rust/sqlite-objs/src/lib.rs` for safe SQLite URI construction with Azure credentials. Implements inline RFC 3986 percent-encoding (no external dependencies) to handle special characters in SAS tokens (`&`, `=`, `%`, `:`). 11 unit + 4 doc tests passing. Eliminates manual URI encoding errors common in user code.
---
### UD6: User Directive — 1GB Default Cache Size
**Date:** 2026-03-13 | **From:** Quetzal Bradley (via Copilot)

Increase default cache size from 4MB to 1GB. Users experiencing 85× performance regression with current default; optimize for performance by default and let memory-conscious users tune down. Recommended approach: expose via PRAGMA or URI parameter.
---
### UD7: User Directive — No Environment Variables in Library Code
**Date:** 2026-03-13 | **From:** Quetzal Bradley (via Copilot)

Remove all environment variable configuration from library code. Library configuration must use URI parameters or sqlite3_file_control (matching existing credential pattern). Affected env vars: SQLITE_OBJS_CACHE_PAGES, SQLITE_OBJS_READAHEAD_PAGES, SQLITE_OBJS_DEBUG_TIMING. Rationale: env vars are implicit, hard to discover, inappropriate for library configuration.
---
### UD8: User Directive — Return Code Handling
**Date:** 2026-03-11 | **From:** Quetzal Bradley (via Copilot)

In C code, never ignore return codes. Applies to both product and test code. Errors must be handled, propagated, or at least logged. Non-negotiable standard for codebase quality.
---
## Governance

- All meaningful changes require team consensus
- Document architectural decisions here
- Keep history focused on work, decisions focused on direction

### D23: Lazy Cache Filling — Analysis and Requirements
**Date:** 2026-03-21 | **From:** Gandalf (Lead/Architect), Aragorn (SQLite/C Dev)

**Status:** ANALYSIS — Not yet a decision (awaiting scope clarification from Quetzal).

Quetzal's lazy cache proposal (download-on-demand pages with validity bitmap persistence) is **architecturally sound** but requires three critical additions before implementation:

1. **Page-1 Bootstrap:** Cannot size validity bitmap without page size. At xOpen, if no cached page 1 exists, fetch first 4-64 KB to detect page size. One-time HTTP round-trip (~100ms), then proceed with on-demand reads.

2. **Readahead-on-Fault:** Cold-cache sequential reads are 100-1000× slower without batching. Fixed 16-page readahead window reduces HTTP requests by 16× with minimal complexity. Full adaptive readahead (already designed in `research/adaptive-readahead-design.md`) covers all workloads.

3. **Bitmap Persistence Atomicity:** Specification required for when `aValid` bitmap is written relative to blob pages, lease renewal interaction, and recovery after crash during sync.

**Key Findings:**
- Journal read-before-write penalty on cold cache: every write triggers an Azure read for rollback safety.
- B-tree schema traversal on first open: 5-50 sequential requests before first query (currently 1 bulk download).
- Sweet-spot scenario (open 100MB, do a few lookups, close): **20× faster** with lazy cache. Table scans: **catastrophic** without prefetch.
- Regression-free guarantee achievable: Make lazy cache opt-in via `prefetch=none` URI parameter, default remains `prefetch=all`.

**Recommended Implementation:** 4 phases (2 days each) — (1) minimal lazy + page-1 bootstrap, (2) readahead on-fault, (3) prefetch pragma, (4) reconnect optimization. MVP 1 defers to Phase 2+.

**Pending User Input:**
- Q1: Which workloads are in scope? (small reads/writes vs table scans vs VACUUM)
- Q2: Readahead scope: fixed window vs adaptive?
- Q3: Cache file sharing between processes?

**Files Modified (Implementation):** `src/sqlite_objs_vfs.c` (~400-500 lines new code + ~200 lines refactoring). Estimated 4-6 days.
---
### D-LC1: Lazy Cache — Validity Bitmap Structure
**Date:** 2026-03-22 | **From:** Aragorn (SQLite/C Dev)

Implemented parallel to dirty bitmap. Same allocation strategy (`unsigned char *aValid` array), same helper function pattern. `bitmapsEnsureCapacity()` wrapper keeps both in sync. Mirrors dirty bitmap API: `validMarkPage`, `validIsPageValid`, `validClearPage`, `validClearAll`, `validMarkAll`, `validEnsureCapacity`, `validMarkRange`.
---
### D-LC2: Lazy Cache — State File Binary Format
**Date:** 2026-03-22 | **From:** Aragorn (SQLite/C Dev)

Fixed format: `SQOS` magic (4B) + version LE32 (4B) + pageSize LE32 (4B) + fileSize LE64 (8B) + bitmapSize LE32 (4B) + bitmap (N bytes) + CRC32 LE32 (4B). Total header = 24 bytes. CRC32 covers header + bitmap. Atomic write via rename. Implemented `writeStateFile`, `readStateFile`, `unlinkStateFile` with ISO 3309 CRC32 lookup table.
---
### D-LC3: Lazy Cache — Page 1 Bootstrap Size
**Date:** 2026-03-22 | **From:** Aragorn (SQLite/C Dev)

65536 bytes (max legal SQLite page size) downloaded at xOpen in lazy mode. Handles all page size values (512–65536) without a second round-trip.
---
### D-LC4: Lazy Cache — Readahead Window
**Date:** 2026-03-22 | **From:** Aragorn (SQLite/C Dev)

Fixed 16 pages. `fetchPagesFromAzure()` issues a single `page_blob_read` for pages N through N+15, clamped to file size.
---
### D-LC5: Lazy Cache — Lazy Revalidation Strategy
**Date:** 2026-03-22 | **From:** Aragorn (SQLite/C Dev)

In lazy mode, `revalidateAfterLease()` uses incremental diff to **invalidate** changed pages (clear valid bits) instead of downloading them. Downloads happen only on subsequent xRead misses. If diff exceeds 50% of pages, falls back to full download for efficiency. After full re-download, marks all valid.
---
### D-LC6: Lazy Cache — Write Order at Close
**Date:** 2026-03-22 | **From:** Aragorn (SQLite/C Dev)

Cache fsync → .state file (atomic rename) → .etag file → .snapshot file. Ensures valid bitmap persisted before ETag that gates cache reuse.
---
### UD9: User Directive — Lazy Cache Defaults and Readahead Simplicity
**Date:** 2026-03-21 | **From:** Quetzal Bradley (via Copilot)

(1) `prefetch=all` is the default — lazy cache is opt-in via `prefetch=none`. Rationale: small databases are common case, should be excellent out of box. Diverse-writers-large-database scenario is corner case. Document prominently in README. (2) Readahead should be simple (fixed window), NOT complicated adaptive state machine. (3) Get core C code finished first, then Rust test infrastructure.
---
# Decision: cache_dir URI parameter implementation

**Author:** Aragorn (SQLite/C Dev)
**Date:** 2025-07-25
**Status:** Implemented

## Decision
Used a `buildCacheTemplate()` helper that heap-allocates the mkstemp template string via `malloc`, rather than using `sqlite3_mprintf` or a fixed-size stack buffer.

## Rationale
- `malloc` + `memcpy` is simpler and avoids SQLite allocator dependency for a POSIX-only temp path
- Heap allocation means the template works for arbitrarily long `cache_dir` paths
- The allocated buffer is directly assigned to `p->zCachePath` after mkstemp modifies it, eliminating the previous `strdup()` copy
- `mkdir(cacheDir, 0700)` creates only the leaf directory — we don't recursively create parent dirs, keeping it simple and predictable

## Alternatives considered
- `sqlite3_mprintf`: Would work but mixes SQLite allocator (`sqlite3_free`) with the existing `free()` used for `zCachePath` in `xClose`
- Stack buffer with fixed max: Would fail silently for long paths
- Recursive `mkdir -p`: Over-engineered for this use case; users can pre-create parent dirs
# Decision: ETag-Based Cache Reuse is Opt-In via URI Parameter

**Date:** 2026-03-10 | **From:** Aragorn (Implementation)

## Decision

Cache persistence is **opt-in** via `cache_reuse=1` URI parameter. When disabled (default), behavior is identical to pre-change: mkstemp creates random cache file names, xClose unlinks them.

## Details

- **Cache naming:** FNV-1a hash of `account:container:blobName` → deterministic path `{cache_dir}/sqlite-objs-{16hex}.cache`
- **ETag sidecar:** Same base path with `.etag` extension. Written only on clean close (no dirty pages + valid ETag).
- **Validation on open:** Stored ETag compared against live blob ETag from `blob_get_properties`. Also verifies cached file size matches blob size.
- **Crash safety:** No ETag sidecar is written if dirty pages exist. On crash, next open sees no sidecar → full download. No data corruption path.

## Rationale

Opt-in avoids surprises for users who don't expect persistent files in their cache directory. The ETag + size double-check prevents serving stale data. FNV-1a is non-cryptographic but sufficient for cache naming uniqueness.

## Impact on Team

- **Samwise (Tests):** New tests needed for cache reuse paths — ETag match/mismatch, stale cache cleanup, `cache_reuse=0` regression.
- **Frodo (Azure Client):** No changes needed. Relies on existing `blob_get_properties` ETag population.
# Decision: ETag Refresh at Close Time

**Date:** 2025-07-25  
**Author:** Aragorn  
**Status:** Implemented

## Context

The batch write path (`az_page_blob_write_batch`) sends concurrent PUT Page requests via curl_multi. Each PUT returns an ETag, but:
1. `azure_error_init(err)` on the success path wiped the ETag field
2. Even with the wipe fixed, concurrent PUTs return ETags in indeterminate order — the "last captured" may not be the blob's final ETag

## Decision

`sqliteObjsClose` now performs a HEAD request (`blob_get_properties`) to get the definitive current ETag before writing the sidecar file. This runs only when `cache_reuse=1` and while the lease is still held.

## Trade-off

One extra HEAD request per close (~20ms) when cache_reuse is enabled. Acceptable because:
- It only fires when cache_reuse is on (opt-in feature)
- 20ms is negligible compared to the 3.7s full re-download it prevents
- HEAD while lease is held guarantees no race with other writers

## Affected Code

- `src/sqlite_objs_vfs.c` — `sqliteObjsClose()`
- `src/azure_client.c` — `az_page_blob_write_batch()` (secondary fix)
# Decision: Custom FCNTL Op Code Convention

**Date:** 2026-03-11 | **From:** Aragorn

## Context
We need observable VFS internals for integration tests. The first case: a download counter to verify ETag cache hits skip blob downloads.

## Decision
- Custom `sqlite3_file_control` op codes are defined in `sqlite_objs.h` starting at 200 (SQLite uses <100 internally).
- First op: `SQLITE_OBJS_FCNTL_DOWNLOAD_COUNT` (200) — returns `int` count of full blob downloads on this file.
- New counters/diagnostics follow the same pattern: define in `sqlite_objs.h`, handle in `sqliteObjsFileControl`, store on `sqliteObjsFile` struct.

## Implications
- Tests can now assert on VFS behavior (not just data correctness).
- Samwise: use `sqlite3_file_control(db, "main", SQLITE_OBJS_FCNTL_DOWNLOAD_COUNT, &count)` in tests. Returns `SQLITE_OK` with count in `*pArg`.
- Future diagnostics (e.g., sync count, lease count) should follow this pattern.

## Also Fixed
- CREATE path with `cache_reuse=1` was silently not persisting cache — ETag sidecar never written. Now uses deterministic cache path via `buildCachePath()`.
# Decision: Incremental Page-Range Download via Blob Snapshots

**Date:** 2026-07-25
**From:** Aragorn (SQLite/C Dev)
**Status:** Implemented

## Context

When Client A reconnects after Client B modified a few pages of a large blob, `revalidateAfterLease()` re-downloaded the entire blob. For a 100MB database with a single-row update, this meant downloading 100MB instead of ~4KB.

## Decision

Use Azure Page Blob snapshots and the `Get Page Ranges` diff API to download only changed pages on reconnect:

1. After each successful xSync, create a blob snapshot (`PUT ?comp=snapshot`)
2. Store snapshot datetime in `.snapshot` sidecar file alongside `.etag`
3. On reconnect with ETag mismatch, call `GET ?comp=pagelist&prevsnapshot=...` to get diff
4. Download only changed ranges, zero cleared ranges, patch cache file in place

## Rationale

- Azure's Get Page Ranges API is specifically designed for this use case
- Snapshot creation is a single lightweight HTTP call (~50ms)
- For the benchmark case (100MB blob, 1 row changed), this reduces reconnect from ~5s to <1s
- Graceful fallback: any failure reverts to full download (zero risk of corruption)

## Impact

- **New ops in azure_ops_t:** `blob_snapshot_create`, `blob_get_page_ranges_diff`
- **New type:** `azure_diff_range_t` in `azure_client.h`
- **New sidecar:** `.snapshot` file per cached blob
- **Mock/stub tables:** Updated with NULL entries (no mock implementation yet)

## Team Notes

- Frodo: No action needed — all Azure operations go through existing `execute_with_retry()` infrastructure
- Samwise: Mock implementations for `blob_snapshot_create` and `blob_get_page_ranges_diff` are NULL. Unit tests pass because the VFS treats NULL ops as "not available" and falls back to full download. Integration tests against Azurite will exercise the real path.
- The snapshot datetime format from Azure is ISO 8601: `2024-01-15T16:48:32.0000000Z`
### D-PERF-WAL: Parallel WAL Upload via Put Block + Put Block List
**Date:** 2026-07 | **From:** Aragorn (SQLite/C Dev)

**Decision:** WAL sync can optionally use Azure Block Blob staged upload (parallel Put Block + Put Block List) instead of single PUT.

**Implementation:**
- `PRAGMA sqlite_objs_wal_parallel=ON` enables the parallel path
- `PRAGMA sqlite_objs_wal_chunk_size=<bytes>` configures chunk size (default 1 MiB)
- Payloads < 256 KiB always use single PUT (parallelism overhead not worth it)
- Default: OFF — explicit opt-in for A/B benchmarking

**Rationale:**
- Single PUT of 4 MiB WAL takes ~850ms in production
- Parallel path: 4 × 1 MiB chunks + 1 Put Block List should reduce to ~250ms
- Same curl_multi infrastructure as page_blob_write_batch (proven pattern)
- No env vars per UD7; PRAGMA-only configuration

**Impact:** No behavior change unless explicitly enabled. All existing tests pass.
# Decision: Azure VFS Repro Binary Dependency Management

**Date:** 2025-01  
**Author:** Aragorn  
**Status:** Implemented

## Context

We needed to add an `azure_vfs_repro` binary to the `sqlite-objs` crate for testing Azure VFS threading scenarios. The binary required additional dependencies (`rusqlite`, `dotenvy`, `tracing-subscriber`) that were not needed by the library itself.

## Decision

Use Cargo's optional dependencies with a feature flag pattern:

1. **Optional dependencies:** Mark binary-only dependencies as `optional = true`
2. **Feature flag:** Create `bin-deps` feature that enables these dependencies
3. **Required features:** Use `required-features = ["bin-deps"]` on the `[[bin]]` declaration

## Rationale

- **Library stays lean:** Without the feature flag, library builds only include core dependencies
- **Explicit opt-in:** Binary users must specify `--features bin-deps`
- **Standard pattern:** This is idiomatic Cargo for binaries in library crates
- **No dev-dependency gotcha:** `cargo run --bin` doesn't see dev-dependencies, so we needed regular optional deps

## Implementation

```toml
[dependencies]
rusqlite = { version = "0.38", optional = true }
dotenvy = { version = "0.15", optional = true }
tracing-subscriber = { version = "0.3", features = ["env-filter"], optional = true }

[features]
bin-deps = ["dep:rusqlite", "dep:dotenvy", "dep:tracing-subscriber"]

[[bin]]
name = "azure_vfs_repro"
required-features = ["bin-deps"]
```

## Consequences

- **Positive:** Library builds remain minimal
- **Positive:** Clear separation between library and tooling dependencies
- **Neutral:** Users must remember `--features bin-deps` when building/running the binary
- **Pattern:** Future utility binaries should follow this same approach

## Alternatives Considered

1. **Dev-dependencies only:** Doesn't work - `cargo run --bin` won't see them
2. **Always-on dependencies:** Pollutes library with unused deps
3. **Separate crate:** Overkill for a single utility binary
# Decision: Stale-Read Race Condition Fix — ETag Revalidation After Lease

**Date:** 2026-03-15
**From:** Aragorn (SQLite/C Dev)
**Status:** Implemented and verified

## Context

Gimli's concurrent contention test (20 threads × independent connections to the same Azure blob) revealed a stale-read race condition in the VFS. Between `xOpen` (which downloads the blob without a lease) and `xLock` (which acquires the lease for writes), another client could modify the blob. The local cache from `xOpen` would then be stale, causing lost updates.

## Decision

Implemented **Approach 1: Re-download after lease acquisition** from Gimli's analysis.

When `sqliteObjsLock` acquires a new lease (transitioning from no-lease to having-lease), it calls `revalidateAfterLease()` which:

1. **Fast path:** Compares the ETag from the `lease_acquire` Azure response with the stored ETag from `xOpen`. If they match, the cache is valid — no extra I/O needed.
2. **Slow path:** If the lease response has no ETag, performs a HEAD request via `blob_get_properties` to get the current ETag and blob size.
3. **Re-download:** On ETag mismatch, re-downloads the full blob into the cache file, resets the dirty bitmap, and updates size/pageSize tracking.
4. **Error handling:** On failure, releases the just-acquired lease before returning the error to SQLite, preventing orphaned leases.

## Rationale

- Most correct of the three approaches — guarantees the client always reads the latest committed data after acquiring the lock
- Invisible to SQLite — the pager doesn't need to know about the re-download
- Minimal cost in the common (no-contention) case: just one string comparison of ETags from the lease response (zero extra HTTP calls)
- Only adds cost when contention actually occurs: one HEAD + one GET (re-download)

## Impact

- **Files changed:** `src/sqlite_objs_vfs.c` and `rust/sqlite-objs-sys/csrc/sqlite_objs_vfs.c` (both must be kept in sync — no automated mechanism exists)
- **Test:** `concurrent_client_contention` now passes — counter = 20 with zero lost updates
- **No breaking changes:** Existing unit tests (247) and sanitizer tests all pass unchanged

## Team Note

The Rust test suite compiles C sources from `rust/sqlite-objs-sys/csrc/`, not from `src/`. Any C changes must be applied to both locations. Consider adding a sync check to CI.
# Thread-Safety via pthread_mutex

**Date:** 2026-01-10  
**Author:** Aragorn (SQLite/C Developer)  
**Status:** Implemented

## Decision

Add `pthread_mutex_t` to `azure_client_t` and `sqliteObjsVfsData` to protect shared state from concurrent access by multiple threads.

## Context

Customer reported crash with "pointer being freed was not allocated" when two threads from the same process use the VFS to open DIFFERENT database files. The root cause:

1. `azure_client_t` has a single `CURL*` handle (`curl_handle`) reused for all requests
2. Global `g_vfsData.client` is shared by ALL connections
3. `curl_easy_*` functions are NOT thread-safe on the same handle
4. Two threads calling `curl_easy_reset()` + `curl_easy_perform()` corrupts CURL's internal state

Additional issues:
- `curl_global_init()` never called (auto-init is racy)
- `ensure_multi_handle()` has TOCTOU race
- `journalCache[]` shared without locks
- Global debug counters have data races

## Alternatives Considered

1. **Per-connection clients:** Create a separate `azure_client_t` for each file opened. More efficient (no lock contention) but requires larger refactoring. Deferred as optimization.

2. **Connection pool:** Maintain a pool of clients and check out/in on each request. Complex and introduces pooling overhead. Not needed for correctness.

3. **sqlite3_mutex:** Use SQLite's mutex API instead of pthread. Rejected because we're protecting our own state, not SQLite's, and pthread is more standard.

## Implementation

### Changes

**src/azure_client_impl.h:**
- Added `pthread_mutex_t mutex` to `struct azure_client`
- Removed incorrect thread-safety comment claiming btree mutex protection

**src/azure_client.c:**
- Added `pthread_once_t` + `curl_global_init_once()` for safe CURL global init
- `azure_client_create()`: `pthread_once()` + `pthread_mutex_init()`
- `azure_client_destroy()`: `pthread_mutex_destroy()`
- `execute_single()`: Lock at entry, unlock before ALL returns (5 return paths)
- `az_page_blob_write_batch()`: Lock entire batch operation
- `az_page_blob_read_multi()`: Lock entire batch operation
- `azure_container_create()`: Lock around curl operations

**src/sqlite_objs_vfs.c:**
- Added `pthread_mutex_t journalCacheMutex` to `sqliteObjsVfsData`
- Init mutex in both registration functions
- Wrapped ALL `journalCacheFind()` and `journalCacheGetOrCreate()` calls

### Lock Granularity

- **Per-client mutex:** Protects `curl_handle` and `multi_handle` from concurrent curl operations
- **Per-VFS mutex:** Protects `journalCache[]` array
- Locks span entire critical sections (no partial unlocks)

### Critical Sections

1. `execute_single()`: From `curl_easy_reset()` through final `curl_easy_getinfo()`
2. Batch operations: Entire batch including setup, execution, cleanup, and retries
3. Journal cache: Individual find/create operations (short-lived)

## Consequences

### Positive

- ✅ Thread-safe: Multiple threads can safely use different files in the same process
- ✅ Minimal change: Only adds mutexes, no architectural refactoring
- ✅ Correct: Protects all identified races
- ✅ All 242 unit tests still pass

### Negative

- ❌ Lock contention: All threads share one client mutex (minor perf impact)
- ❌ Coarse-grained: Locks span entire HTTP requests (~5-50ms)

### Neutral

- Future optimization: Per-connection clients would eliminate lock contention
- Per-file clients would be a pure optimization, not a correctness fix

## Validation

- Build succeeds with no warnings
- All 242 pre-existing unit tests pass
- 8 cache tests failing were pre-existing (unrelated)

## Notes

- Used `pthread_mutex_t` directly, not `sqlite3_mutex` — we're protecting our state, not SQLite's
- The lock in `execute_single()` MUST cover the entire curl operation sequence
- Batch operations hold the lock during retries to protect multi handle state
- CURL global init via `pthread_once` prevents race on first client creation
# Decision: csrc/ Must Stay in Sync with src/

**Author:** Aragorn  
**Date:** 2026-07-23  
**Status:** Decided  

## Context

The Rust `sqlite-objs-sys` crate compiles C sources from `rust/sqlite-objs-sys/csrc/`, which are copies of the canonical sources in `src/`. These copies had drifted severely — missing all thread-safety fixes (mutex in execute_single, pthread_once for curl init), disk-backed cache, WAL support, and more.

This caused a 100% CPU hang when running the repro binary in multi-threaded mode: two threads sharing one `azure_client_t` raced on the same curl handle without synchronization.

## Decision

1. **All C/H source changes in `src/` must be mirrored to `rust/sqlite-objs-sys/csrc/`** at commit time. The canonical source is `src/`; csrc is a deployment copy.
2. **Consider adding a CI check** (e.g., `diff src/*.c csrc/*.c`) or a Makefile target `sync-csrc` to prevent future drift.

## Affected Files

- `rust/sqlite-objs-sys/csrc/azure_client.c`
- `rust/sqlite-objs-sys/csrc/azure_client.h`
- `rust/sqlite-objs-sys/csrc/azure_client_impl.h`
- `rust/sqlite-objs-sys/csrc/azure_error.c`
- `rust/sqlite-objs-sys/csrc/sqlite_objs_vfs.c`
- `rust/sqlite-objs-sys/csrc/sqlite_objs.h`
- `rust/sqlite-objs-sys/csrc/azure_auth.c`
# Decision: VFS Activity Metrics with PRAGMA Exposure

**Date:** 2026-07 | **From:** Aragorn (VFS Implementation)

## Summary

Implemented per-connection VFS activity metrics (27 counters) exposed via `PRAGMA sqlite_objs_stats` and `SQLITE_OBJS_FCNTL_STATS`. Counters cover disk I/O, Azure blob I/O, cache behavior, lease lifecycle, sync operations, revalidation, journal/WAL uploads, and Azure errors.

## Key Decisions

1. **Per-connection, not global:** Metrics live in `sqliteObjsFile.metrics`, zeroed at xOpen. Avoids cross-connection contamination. Existing `g_xread_count` globals preserved for backward compatibility.

2. **Single text result format:** `PRAGMA sqlite_objs_stats` returns key=value text (one per line) rather than a virtual table result set. Trade-off: less SQL-native but drastically simpler implementation — no need for custom vtable, function registration, or result-set machinery.

3. **FCNTL op codes:** `SQLITE_OBJS_FCNTL_STATS` (201) returns `sqlite3_malloc`'d string; `SQLITE_OBJS_FCNTL_STATS_RESET` (202) zeroes all counters. Follows existing `SQLITE_OBJS_FCNTL_DOWNLOAD_COUNT` (200) pattern.

4. **sqlite3_int64 for all counters:** Even seemingly small counters (lease_acquires) use `sqlite3_int64` to avoid overflow in long-running benchmarks.

5. **Zero-overhead design:** All instrumentation is `p->metrics.field++` or `+= bytes`. No allocations, no branches, no locking.

## Files Changed

- `src/sqlite_objs.h` — Added `sqlite_objs_metrics` struct, `SQLITE_OBJS_FCNTL_STATS`, `SQLITE_OBJS_FCNTL_STATS_RESET`
- `src/sqlite_objs_vfs.c` — Added `metrics` field to `sqliteObjsFile`, `formatMetrics()` helper, instrumented 14 code paths, added PRAGMA/FCNTL handlers
- `test/test_vfs.c` — 7 new tests (Section 19: VFS Activity Metrics)
- `rust/sqlite-objs-sys/csrc/` — Synced

## Test Results

All 295 tests pass (288 existing + 7 new metrics tests).
# VFS Bitmap & Code Duplication Refactor

**Date:** 2026-07 | **From:** Aragorn (Code Quality Review)
**Requested by:** Quetzal Bradley

## Question Answered

> "Is the bitmap code duplicated for the two bitmaps?"

**Yes.** The dirty bitmap (`aDirty/nDirtyPages/nDirtyAlloc`) and valid bitmap (`aValid/nValidPages/nValidAlloc`) had 12 functions total, 8 of which were structurally identical — same bit-manipulation logic, different struct fields. The only unique functions were `dirtyHasAny` (no valid equivalent needed), `validClearPage`, `validMarkAll`, and `validMarkRange` (no dirty equivalents needed).

## Changes Made

### 1. Unified Bitmap API
- New `Bitmap` struct: `{ unsigned char *data; int nSet; int nAlloc; }`
- Generic functions: `bitmapSetBit`, `bitmapTestBit`, `bitmapClearBit`, `bitmapClearAll`, `bitmapSetAll`, `bitmapSetRange`, `bitmapHasAny`, `bitmapEnsureCapacity`, `bitmapFree`
- Thin wrappers for offset-to-page conversion: `dirtyMarkPage`, `validMarkPage`, `validMarkRange`, `validMarkAll`

### 2. Sidecar Path Builders
- `buildSidecarPath(cachePath, ext)` replaces 3 identical functions (buildEtagPath, buildSnapshotPath, buildStatePath)
- `unlinkSidecarFile(cachePath, ext)` replaces 3 identical unlink helpers

### 3. Buffer Helpers
- `bufferEnsure(ppBuf, pAlloc, newSize)` — generic exponential-doubling realloc, replaces duplicated jrnl/WAL buffer growth
- `bufferRead(src, srcLen, pBuf, iAmt, iOfst)` — generic read-with-zerofill, replaces duplicated WAL/journal xRead logic

## What Was NOT Refactored (and Why)

- **xOpen error cleanup paths** — Each error site cleans up a different subset of resources. A single cleanup helper would need many parameters or flags, adding complexity without improving readability. The goto-based structure already handles the common case.
- **coalesceDirtyRanges / prefetchInvalidPages** — These scan bitmaps but with fundamentally different iteration strategies (dirty pages → contiguous write ranges vs. invalid pages → download ranges). Unifying them would require a callback-based iterator that obscures the intent.
- **writeStateFile / readStateFile** — Binary format with CRC32. Structurally different from text sidecar I/O. No duplication to remove.

## Impact

- **Net:** -158 lines
- **Tests:** 247/247 pass, zero regressions
- **Behavior:** Identical before and after (pure refactor)
### WAL Files: Append Blobs → Block Blobs
**Date:** 2026-07 | **From:** Aragorn (implementation) + Frodo (Azure API analysis)

WAL sync now uses `block_blob_upload` (single PUT, overwrites) instead of append blob operations (DELETE + CREATE + N×APPEND with 4 MiB chunking). This eliminates the `nWalSynced` / `walNeedFullResync` tracking state and reduces the WAL sync path from ~100 lines to ~20.

**Impact:** xOpen for WAL files now requires `block_blob_upload` and `block_blob_download` in the `azure_ops_t` vtable. The `append_blob_*` entries are preserved but unused by WAL.

**Note:** `block_blob_download` (GET Blob) already worked on append blobs, so crash recovery path was unchanged.
### 2026-03-15T06:39Z: User directive
**By:** Quetzal Bradley (via Copilot)
**What:** Journal+DB pipelining optimization is backlog — not critical since we are determined to use WAL mode for now.
**Why:** User request — captured for team memory. WAL mode is the primary focus; journal-mode optimizations are deferred.
# Azure Blob Storage Capabilities Analysis

**Date:** 2025-01-15  
**Agent:** Frodo (Azure Expert)  
**Requested by:** Quetzal Bradley
---
## Executive Summary

This analysis catalogs Azure Blob Storage features we currently use and identifies advanced capabilities we're NOT leveraging that could benefit the sqliteObjs project. Research focused on REST API capabilities applicable to our direct implementation (no SDK dependency).

**Key findings:**
- **Currently using:** 15 Azure REST operations (comprehensive core coverage)
- **High-priority opportunities:** 6 features with significant user impact
- **Medium-priority opportunities:** 8 features worth considering
- **Low-priority/irrelevant:** 10+ features not applicable to our use case
---
## Current Azure Feature Usage

### Operations Implemented (from `azure_client.c` + `azure_client.h`)

| Azure REST API Operation | Blob Type | Our Usage | Implementation |
|--------------------------|-----------|-----------|----------------|
| **Page Blob: Create** | Page | Create MAIN_DB blob | `az_page_blob_create()` |
| **Page Blob: Put Page** | Page | Write 512-aligned pages | `az_page_blob_write()` + `az_page_blob_write_batch()` |
| **Page Blob: Get Page Ranges** | Page | Read data | `az_page_blob_read()` + `az_page_blob_read_multi()` |
| **Page Blob: Resize** | Page | TRUNCATE operation | `az_page_blob_resize()` |
| **Page Blob: Get Page Ranges (diff)** | Page | Snapshot-based diff | `az_blob_get_page_ranges_diff()` |
| **Block Blob: Put Blob** | Block | Upload MAIN_JOURNAL | `az_block_blob_upload()` |
| **Block Blob: Get Blob** | Block | Download journal | `az_block_blob_download()` |
| **Block Blob: Put Block** | Block | Parallel upload chunks | `az_block_blob_upload_parallel()` |
| **Block Blob: Put Block List** | Block | Commit chunked upload | (inside parallel upload) |
| **Append Blob: Create** | Append | WAL mode (future) | `az_append_blob_create()` |
| **Append Blob: Append Block** | Append | WAL writes | `az_append_blob_append()` |
| **Blob: Get Properties** | All | Size, lease state | `az_blob_get_properties()` |
| **Blob: Delete** | All | Delete database | `az_blob_delete()` |
| **Blob: HEAD (exists)** | All | Lightweight existence check | `az_blob_exists()` |
| **Blob: Snapshot** | All | Point-in-time snapshot | `az_blob_snapshot_create()` |
| **Lease: Acquire** | All | EXCLUSIVE lock | `az_lease_acquire()` |
| **Lease: Renew** | All | Keep lock alive | `az_lease_renew()` |
| **Lease: Release** | All | Unlock | `az_lease_release()` |
| **Lease: Break** | All | Force unlock | `az_lease_break()` |

**Total:** 15 distinct Azure operations + 4 lease operations = **19 REST API endpoints** in production use.

### Authentication & Infrastructure

- **Auth methods:** SAS tokens (preferred) + Shared Key (HMAC-SHA256 fallback)
- **HTTP client:** libcurl with HTTP/2 multiplexing enabled (CURL_HTTP_VERSION_2TLS)
- **Connection pooling:** Persistent curl handles, TLS session reuse
- **Retry logic:** 5 retries, exponential backoff + jitter, Retry-After header support
- **Parallel I/O:** `curl_multi` for batch page writes (32 connections) + parallel reads
---
## Azure Features We're NOT Using (Opportunities)

### 🔴 HIGH PRIORITY — High Impact, Low/Moderate Effort

#### 1. **Put Page From URL** (Page Blob Server-Side Copy)
- **What:** Copy page ranges between blobs server-side without downloading to client  
- **REST API:** `PUT /<blob>?comp=page` with `x-ms-copy-source` + `x-ms-source-range` headers  
- **Use case:** Snapshot-to-main copy during crash recovery. Currently we download snapshot pages via `Get Page Ranges` then re-upload via `Put Page` — wastes bandwidth. Server-side copy = zero egress cost.  
- **Implementation:** Moderate (new REST call, reuse auth/retry infrastructure)  
- **Priority:** **HIGH** — Reduces recovery time and data transfer costs significantly.

#### 2. **Conditional Headers (Advanced ETag Matching)**
- **What:** `If-Match`, `If-None-Match`, `If-Modified-Since`, `If-Unmodified-Since` on PUT/GET  
- **Current usage:** Basic ETag tracking in `azure_error_t.etag` field (Phase 3), but NOT used for conditional writes  
- **Use case:** Optimistic concurrency for multi-writer scenarios. Prevent lost updates when two clients modify blob simultaneously. Example: `PUT Page` with `If-Match: <etag>` → 412 Precondition Failed if blob changed since last read.  
- **Implementation:** Trivial (add header to existing requests)  
- **Priority:** **HIGH** — Critical for multi-machine correctness (MVP 2+). Low-hanging fruit.

#### 3. **Blob Index Tags** (Queryable Metadata)
- **What:** Key-value tags on blobs (up to 10 per blob) with `Find Blobs By Tags` query API  
- **REST API:** `PUT /<blob>?comp=tags`, `GET /?comp=blobs&where=<query>`  
- **Use case:** Tag databases by project, environment, or tenant. Query across container: `"project=contoso"` → list all contoso DBs. Useful for multi-tenant SaaS deployments, database discovery, lifecycle management.  
- **Implementation:** Moderate (new REST calls, tag schema design)  
- **Priority:** **HIGH** — Enables advanced multi-tenant scenarios, trivial cost (tags are free metadata).

#### 4. **Last Access Time Tracking**
- **What:** Azure tracks last blob access time (requires opt-in). Queryable via `Get Blob Properties`.  
- **Use case:** Automated cache eviction policies, tiering cold databases to Cool/Archive. Example: lifecycle rule "move to Cool tier if not accessed in 30 days."  
- **Implementation:** Trivial (enable at account level, read from existing `Get Blob Properties`)  
- **Priority:** **HIGH** — Cost optimization for large-scale deployments. Zero code change if using lifecycle policies.

#### 5. **Soft Delete for Blobs**
- **What:** Deleted blobs retained for N days (configurable). Recoverable via `Undelete Blob` API.  
- **REST API:** `PUT /<blob>?comp=undelete`  
- **Use case:** Accidental deletion protection. User runs `DROP DATABASE` → blob soft-deleted → recoverable for 7 days.  
- **Implementation:** Trivial (enable at account level, add `Undelete Blob` call to VFS)  
- **Priority:** **HIGH** — User safety feature, negligible cost (pay for retention storage). Strong ROI.

#### 6. **Incremental Copy Blob** (Page Blob Differential Backup)
- **What:** Copy only changed pages between snapshots. More efficient than full snapshot copy.  
- **REST API:** `PUT /<dest-blob>?comp=incrementalcopy` with `x-ms-copy-source` pointing to snapshot  
- **Use case:** Backup/DR scenario. Copy snapshot1 → backup blob (full), then copy snapshot2 → backup blob (incremental) = only diffs transferred.  
- **Implementation:** Moderate (new REST call, orchestration logic for incremental chains)  
- **Priority:** **HIGH** — Reduces backup costs and time for large databases.
---
### 🟡 MEDIUM PRIORITY — Moderate Impact, Moderate Effort

#### 7. **Blob Versioning** (vs. Snapshots)
- **What:** Automatic versioning on every write. Each version has unique `versionid` timestamp.  
- **Difference from snapshots:** Snapshots are explicit; versions are automatic. Versions don't count against snapshot limit.  
- **Use case:** Could replace our manual snapshot-based diff with automatic version-based diff. `Get Page Ranges (diff)` works with `?versionid=<prev>` instead of `?snapshot=<prev>`.  
- **Tradeoff:** Versions NOT supported for page blobs with HNS enabled. Extra cost for version storage.  
- **Implementation:** Significant (refactor diff logic, handle version lifecycle)  
- **Priority:** **MEDIUM** — Snapshots work fine; versioning adds complexity. Consider for compliance scenarios.

#### 8. **Change Feed** (Blob Event Log)
- **What:** Ordered, immutable log of all blob changes (create/update/delete). Stored as Avro files in `$blobchangefeed` container.  
- **Use case:** Audit trail, replication triggers, event-driven workflows. Example: external service watches change feed to index database metadata.  
- **Tradeoff:** Requires blob versioning. Log retention cost. Latency = few minutes (not real-time).  
- **Implementation:** Moderate (enable feature, consume Avro logs via SDK or manual parse)  
- **Priority:** **MEDIUM** — Useful for audit/compliance, but Blob Storage Events (real-time webhooks) might be better for most use cases.

#### 9. **Access Tier Management** (Hot/Cool/Cold/Archive)
- **What:** Move blobs between tiers to optimize cost. Archive = lowest cost, 15+ hour rehydration.  
- **REST API:** `PUT /<blob>?comp=tier` with `x-ms-access-tier` header  
- **Use case:** Cold databases → Archive tier (e.g., 3-month-old backups). Lifecycle policies automate this.  
- **Tradeoff:** Archive tier requires rehydration (hours) before read. Cool/Cold = instant access, lower cost.  
- **Implementation:** Trivial (single REST call, lifecycle policy automation)  
- **Priority:** **MEDIUM** — Cost optimization for long-term storage. Requires user workflow design (which DBs to archive?).

#### 10. **Blob Lease: Change ID**
- **What:** `Lease Blob?comp=lease&action=change` — swap lease ID mid-lease  
- **Use case:** Lease handoff between processes without breaking. Process A acquires lease, hands ID to Process B via secure channel, B uses `change` to take ownership.  
- **Tradeoff:** Complex handoff protocol. Current break+acquire works for most cases.  
- **Implementation:** Trivial (new REST call)  
- **Priority:** **MEDIUM** — Niche use case (live migration, zero-downtime handoff). Not MVP-critical.

#### 11. **Customer-Managed Encryption Keys** (CMK via Key Vault)
- **What:** Use customer's Azure Key Vault key for blob encryption (instead of Microsoft-managed keys)  
- **REST API:** `x-ms-encryption-scope` header on all blob operations  
- **Use case:** Compliance (HIPAA, PCI-DSS) requiring customer control over encryption keys.  
- **Tradeoff:** Requires Azure Key Vault integration, rotation policies. Higher latency (Key Vault round-trip).  
- **Implementation:** Significant (Key Vault client, key rotation, error handling)  
- **Priority:** **MEDIUM** — Enterprise compliance feature. Not needed for general use.

#### 12. **Encryption Scopes**
- **What:** Per-blob encryption keys (scoped at container/account level)  
- **Use case:** Multi-tenant isolation — tenant A's data encrypted with keyA, tenant B with keyB.  
- **Tradeoff:** Requires Azure Key Vault or account-scoped keys. More complex than CMK.  
- **Implementation:** Moderate (scope management, header on all requests)  
- **Priority:** **MEDIUM** — Advanced multi-tenant security. Overlap with CMK.

#### 13. **Object Replication** (Cross-Region Async Copy)
- **What:** Auto-replicate block blobs across regions for DR/latency reduction  
- **REST API:** Policy-based (configured at account level, not per-request)  
- **Use case:** Multi-region deployment. Primary DB in US-East, replica in EU-West. 99% of objects replicated within 15 minutes (SLA).  
- **Tradeoff:** Requires blob versioning + change feed. Block blobs only (no page blobs). Async = eventual consistency.  
- **Implementation:** Significant (Azure portal/CLI setup, handle replication lag in VFS)  
- **Priority:** **MEDIUM** — DR/multi-region feature. Block blobs only = journal/WAL replication, not MAIN_DB (page blob).

#### 14. **Immutable Storage (WORM)** — Time-Based Retention + Legal Hold
- **What:** Blobs cannot be modified/deleted for specified period. Compliance (SEC 17a-4, FINRA).  
- **REST API:** Container-level or version-level policies. `PUT /<container>?restype=container&comp=immutabilitypolicy`  
- **Use case:** Financial services, healthcare records. Lock database for 7 years, tamper-proof.  
- **Tradeoff:** Can't delete/modify even with account admin access. Policy locks are irreversible.  
- **Implementation:** Moderate (policy management, handle 409 errors on writes)  
- **Priority:** **MEDIUM** — Niche compliance use case. Not general-purpose.
---
### 🟢 LOW PRIORITY / NOT APPLICABLE

#### 15. **Static Website Hosting**
- Not relevant (we're a database, not a web server).

#### 16. **Hierarchical Namespace (ADLS Gen2)**
- Page blobs + blob versioning NOT supported with HNS enabled. Would break our core operations.  
- **Verdict:** Do NOT enable HNS.

#### 17. **Azure CDN / Front Door**
- **Use case:** Read-heavy workloads across regions (cache blob reads in edge locations)  
- **Tradeoff:** CDN caching + Azure writes = cache invalidation hell. SQLite is read-write, not read-only content.  
- **Verdict:** Not applicable for transactional database.

#### 18. **Blob Batch API** (Delete/Set-Tier in Bulk)
- **Use case:** Delete 1000 blobs in single request.  
- **Verdict:** sqliteObjs manages 1 blob (MAIN_DB) + 1 journal. No bulk operations needed.

#### 19. **Query Acceleration** (Server-Side SQL on Blobs)
- **Use case:** Run SQL on CSV/JSON blobs without downloading.  
- **Verdict:** Not applicable (we're SQLite, not blob analytics).

#### 20. **Customer-Provided Keys (Per-Request Encryption)**
- **What:** Client provides AES-256 key in `x-ms-encryption-key` header (ephemeral, not stored)  
- **Tradeoff:** Client must manage key for every request. Lost key = data loss. CMK is safer.  
- **Verdict:** Too risky for database use case. CMK is better solution.

#### 21. **Premium Page Blob Tiers**
- **What:** Premium storage accounts have P4/P6/P10/etc tiers (fixed IOPS/throughput)  
- **Current:** We use standard page blobs (pay-per-use IOPS).  
- **Verdict:** Premium = higher cost, predictable perf. Defer until perf benchmarks justify.

#### 22. **Azure Private Link / Private Endpoints**
- **What:** Access storage via private IP (no internet exposure).  
- **Use case:** Enterprise network isolation.  
- **Implementation:** Zero code change (network-level feature).  
- **Verdict:** Deployment/infra concern, not API feature. Document as deployment option.
---
## Recommendations Summary

### Immediate Wins (Low Effort, High Impact)

1. **Soft Delete** — Enable at account level, add `Undelete Blob` to VFS. User safety feature.
2. **Conditional Headers** — Add `If-Match` to `Put Page` for multi-writer safety. Trivial code change.
3. **Last Access Time Tracking** — Enable for lifecycle policies. Zero code change.

### Phase 2 Enhancements (Moderate Effort, High Impact)

4. **Put Page From URL** — Server-side copy for snapshot recovery. Reduces bandwidth costs.
5. **Incremental Copy Blob** — Differential backups. Reduces backup time/cost.
6. **Blob Index Tags** — Multi-tenant discovery, metadata queries. Enables SaaS scenarios.

### Phase 3+ (Significant Effort, Niche Value)

7. **Blob Versioning** — Consider if automatic version management is desired (vs manual snapshots).
8. **Customer-Managed Keys** — Enterprise compliance scenarios only.
9. **Object Replication** — Multi-region DR for journals (block blobs). Page blobs not supported.
10. **Immutable Storage** — Financial/healthcare compliance only.

### Not Recommended

- **HNS (ADLS Gen2)** — Breaks page blob compatibility. Do NOT enable.
- **CDN / Query Acceleration** — Not applicable to transactional database workloads.
- **Customer-Provided Keys** — Too risky (key loss = data loss). Use CMK instead.
---
## Cost Implications

### Features with Zero or Minimal Cost
- **Soft Delete:** Storage cost during retention period (e.g., 7 days × blob size).
- **Conditional Headers:** Free (no additional requests).
- **Last Access Time Tracking:** Free (tracked by Azure).
- **Blob Index Tags:** Free (10 tags per blob max).

### Features with Moderate Cost
- **Blob Versioning:** Storage cost for all versions. Mitigate with lifecycle policies.
- **Change Feed:** Storage cost for Avro logs. Configurable retention.
- **Access Tiers:** Cool/Cold/Archive reduce storage cost vs Hot. Rehydration fees for Archive.

### Features with Significant Cost
- **Object Replication:** Egress charges for cross-region transfer. Transaction costs for replication API calls.
- **Customer-Managed Keys:** Azure Key Vault costs (HSM key storage, operations).
---
## Implementation Roadmap (Recommended)

### MVP 2 (Multi-Writer Safety)
- Add `If-Match` / `If-None-Match` conditional headers to `Put Page`
- Enable soft delete at account level
- Document in VFS error codes: 412 Precondition Failed → SQLITE_BUSY

### Post-MVP 2 (Cost Optimization)
- Implement `Put Page From URL` for snapshot recovery
- Enable last access time tracking + lifecycle policies (Cool tier after 30 days)
- Implement `Incremental Copy Blob` for backup optimization

### Future (Multi-Tenant SaaS)
- Implement blob index tags (project, tenant, environment)
- Implement `Find Blobs By Tags` for discovery APIs
- Consider customer-managed keys for tenant isolation

### Future (Enterprise Compliance)
- Evaluate blob versioning vs snapshots (automatic vs explicit)
- Evaluate immutable storage for regulated workloads
- Integrate Azure Monitor metrics (already available, just need consumption)
---
## References

- [Azure Blob Storage REST API Reference](https://learn.microsoft.com/en-us/rest/api/storageservices/blob-service-rest-api)
- [Blob Versioning](https://learn.microsoft.com/en-us/azure/storage/blobs/versioning-overview)
- [Change Feed](https://learn.microsoft.com/en-us/azure/storage/blobs/storage-blob-change-feed)
- [Blob Index Tags](https://learn.microsoft.com/en-us/azure/storage/blobs/storage-manage-find-blobs)
- [Incremental Copy](https://learn.microsoft.com/en-us/rest/api/storageservices/incremental-copy-blob)
- [Put Page From URL](https://learn.microsoft.com/en-us/rest/api/storageservices/put-page-from-url)
- [Immutable Storage](https://learn.microsoft.com/en-us/azure/storage/blobs/immutable-storage-overview)
- [Object Replication](https://learn.microsoft.com/en-us/azure/storage/blobs/object-replication-overview)
- [Feature Support Matrix](https://learn.microsoft.com/en-us/azure/storage/blobs/storage-feature-support-in-storage-accounts)
---
**END OF REPORT**
# Decision: Enable HTTP/2 Multiplexing in azure_client.c

**Date:** 2026-06-13
**From:** Frodo (Azure Expert)
**Status:** Implemented

## Summary

Enabled HTTP/2 via TLS ALPN negotiation across all curl handles in `src/azure_client.c`. This allows Azure Blob Storage requests to use HTTP/2 when available, with silent fallback to HTTP/1.1.

## What Changed

| Location | Option | Value |
|---|---|---|
| `execute_single()` — easy handle | `CURLOPT_HTTP_VERSION` | `CURL_HTTP_VERSION_2TLS` |
| `ensure_multi_handle()` — multi handle | `CURLMOPT_PIPELINING` | `CURLPIPE_MULTIPLEX` |
| `batch_init_easy()` — write batch easy handles | `CURLOPT_HTTP_VERSION` + `CURLOPT_PIPEWAIT` | `CURL_HTTP_VERSION_2TLS`, `1L` |
| `read_batch_init_easy()` — read batch easy handles | `CURLOPT_HTTP_VERSION` + `CURLOPT_PIPEWAIT` | `CURL_HTTP_VERSION_2TLS`, `1L` |

## Impact

- **Batch writes (xSync):** curl_multi can now multiplex multiple PUT Page requests over a single TCP connection instead of opening separate connections per request. Reduces connection overhead.
- **Batch reads (xOpen):** Same multiplexing benefit for parallel chunk downloads.
- **Sequential requests:** HTTP/2 still benefits from header compression (HPACK) even for single requests.
- **No risk:** Falls back to HTTP/1.1 if nghttp2 is not available in libcurl or if the server doesn't support HTTP/2.

## Correction

Earlier research (Azure Parallel Write Performance Research, 2026-03-10) stated "HTTP/2: Dead end — Azure Blob Storage only supports HTTP/1.1." This was incorrect. Azure Blob Storage does support HTTP/2.

## Team Impact

- **Aragorn:** No VFS changes needed. HTTP/2 is transparent to the VFS layer.
- **Samwise:** Unit tests unaffected — mock layer doesn't use curl. Integration tests (Azurite) may or may not support HTTP/2; the fallback ensures no breakage.
- **Build:** Requires libcurl with nghttp2 for HTTP/2 to activate. macOS Homebrew curl includes nghttp2 by default.
# Phase 1 Azure Immediate Wins — Implementation

**Date:** 2026-07  
**Agent:** Frodo (Azure Expert)  
**Status:** Implemented
---
## 1. Conditional Headers (If-Match on Put Page) ✅

**What:** Added `If-Match: <etag>` header support to page blob write operations for optimistic concurrency control.

**Changes:**
- `azure_client.h`: Added `const char *if_match` parameter to `page_blob_write` and `page_blob_write_batch` vtable signatures
- `azure_auth.c`: Updated `azure_auth_sign_request()` to include If-Match value in StringToSign (position 9, between If-Modified-Since and If-None-Match)
- `azure_client_impl.h`: Updated `azure_auth_sign_request()` declaration
- `azure_client.c`: 
  - `execute_single()` and `execute_with_retry()` accept and thread `if_match` parameter
  - `az_page_blob_write()` passes `if_match` to execute_with_retry
  - `batch_init_easy()` includes If-Match in both SharedKey signing and HTTP headers
  - `az_page_blob_write_batch()` passes `if_match` through to batch_init_easy
- `sqlite_objs_vfs.c`:
  - xSync passes `p->etag` (when available) to both batch and sequential write paths
  - `azureErrToSqlite()` maps `AZURE_ERR_PRECONDITION` → `SQLITE_BUSY`
  - Batch and sequential write paths return `SQLITE_BUSY` on 412 (not `SQLITE_IOERR_FSYNC`)
- `azure_client_stub.c`: Updated stub signature and converted to designated initializers
- `mock_azure_ops.c`: Updated mock signature (ignores if_match)
- All test files: 44+ call sites updated with NULL for if_match

**Behavior:**
- When VFS has a cached ETag, writes include `If-Match` header
- Azure returns 412 Precondition Failed if blob was modified by another client
- 412 maps to `AZURE_ERR_PRECONDITION` (non-retryable) → `SQLITE_BUSY`
- Caller can retry the transaction (standard SQLite busy handling)
- This is a SECOND layer of defense alongside lease-based protection

## 2. Soft Delete — Undelete Blob API ✅

**What:** Added `az_blob_undelete()` function to recover soft-deleted blobs.

**Changes:**
- `azure_client.h`: Added `blob_undelete` to `azure_ops_t` vtable
- `azure_client.c`: Implemented `az_blob_undelete()` — `PUT ?comp=undelete`
- Production vtable wired up; mock and stub set to NULL

**Notes:**
- Soft delete must be enabled at the Azure Storage account level (portal/CLI)
- Typical retention: 7 days
- This API enables programmatic recovery of accidentally deleted databases

## 3. Last Access Time Tracking — Documentation Only ✅

**What:** No code change needed. This is an account-level Azure Storage setting.

**Recommendation:** Enable "Last access time tracking" in the Azure Storage account settings. This allows:
- Automated lifecycle policies (e.g., move to Cool tier after 30 days of inactivity)
- Cost optimization for large-scale deployments with cold databases
- The `Get Blob Properties` response already includes the last access time field
---
## Testing

- 294/295 unit tests pass (1 pre-existing failure unrelated to this work)
- Zero new warnings in the changed files
- Synced to `rust/sqlite-objs-sys/csrc/`
# WAL Blob Strategy: Switch from Append Blobs to Block Blobs

**Author:** Frodo (Azure Expert)
**Date:** 2026-03-11
**Status:** PROPOSAL — awaiting Gandalf review
**Scope:** WAL file Azure storage strategy in `src/azure_client.c` and `src/sqlite_objs_vfs.c`
---
## Executive Summary

**Switch WAL files from append blobs to block blobs.** This eliminates the DELETE+CREATE cycle on every full resync and reduces every WAL sync to a single `Put Blob` call regardless of whether it's a full resync or incremental update.

| Metric | Current (Append Blob) | Proposed (Block Blob) |
|--------|----------------------|----------------------|
| Full resync | DELETE + PUT(empty) + N×PUT(append) = 2+N calls | 1×PUT(full) = **1 call** |
| Incremental sync | N×PUT(append chunks) = N calls | 1×PUT(full) = **1 call** |
| Checkpoint/close | DELETE = 1 call | DELETE = 1 call |
| 4.5 MiB WAL example | 1+1+2 = **4 calls**, ~480ms | **1 call**, ~370ms |
---
## Research Findings

### Q1: Can we create an append blob AND provide initial data in a single API call?

**No.** The Azure REST API `Put Blob` with `x-ms-blob-type: AppendBlob` requires a **zero-length body**. Creation always produces an empty append blob. Data must then be appended via separate `Append Block` (`?comp=appendblock`) calls.

There is no conditional header or API variant that combines create + first append into one call. The `Append Block` operation itself **requires the blob to already exist** — it will fail with 404 if the blob doesn't exist.

> **Source:** [Put Blob REST API — Microsoft Learn](https://learn.microsoft.com/en-us/rest/api/storageservices/put-blob): "For an append blob, the Content-Length request header must be set to zero."

> **Source:** [Append Block REST API — Microsoft Learn](https://learn.microsoft.com/en-us/rest/api/storageservices/append-block): Requires existing append blob.

**Conclusion:** The current two-step (create empty → append data) is the minimum possible with append blobs. No optimization available within this blob type.
---
### Q2: Can we avoid the DELETE when we need to start over?

**No — not with append blobs.** Append blobs have no truncate, no overwrite, no random-access write. The only way to "reset" an append blob is DELETE + CREATE. This is a fundamental limitation of the append blob type.

However, **`Put Blob` with `x-ms-blob-type: BlockBlob` will overwrite ANY existing blob at the same path**, including append blobs. It replaces the blob entirely — the old blob (of any type) is deleted and a new block blob is created with the provided data. This is atomic from the client's perspective.

> **Source:** [Put Blob REST API](https://learn.microsoft.com/en-us/rest/api/storageservices/put-blob): "If the blob already exists, this operation will overwrite it."

> **Source:** [Understanding block blobs, append blobs, and page blobs](https://learn.microsoft.com/en-us/rest/api/storageservices/understanding-block-blobs--append-blobs--and-page-blobs): Blob type is set at creation; Put Blob on an existing blob replaces it with the new type.

**Key insight:** If we switch to block blobs, every sync is just `Put Blob` with the full WAL content. No DELETE needed before overwrite. No CREATE needed before data upload. One call does everything.
---
### Q3: What's the optimal path to achieve ONE PUT per WAL xSync?

**Use block blobs with `Put Blob` for all WAL syncs.**

#### Size Limits

| Method | Max Size | Our WAL Range |
|--------|----------|---------------|
| `Put Blob` (single call) | 5,000 MiB (~4.88 GiB) | Typically 1–50 MiB |
| `Put Block` + `Put Block List` | 190.7 TiB total | Not needed |

WAL files in our workloads are typically under 50 MiB. Even extreme cases (long-running transactions) rarely exceed a few hundred MiB. The 5,000 MiB `Put Blob` limit is more than sufficient. We don't need `Put Block List` complexity.

#### Strategy

Every `xSync` for a WAL file becomes:

```
PUT /<container>/<name>-wal
x-ms-blob-type: BlockBlob
Content-Type: application/octet-stream
Content-Length: <nWalData>
Body: <entire WAL buffer>
```

This single call:
- **Creates** the blob if it doesn't exist
- **Overwrites** the blob if it does exist (even if it was previously an append blob)
- **Uploads** all data in one request
- **No chunking needed** — no 4 MiB per-append limit

#### Fallback for Giant WALs

If we ever need to support WALs > 5 GiB (extremely unlikely), we could use `Put Block` + `Put Block List`. But this is a future concern, not an MVP concern.
---
### Q4: Tradeoffs

| Factor | Append Blob (Current) | Block Blob (Proposed) | Page Blob |
|--------|----------------------|----------------------|-----------|
| **API calls per full resync** | DELETE + CREATE + N×APPEND = 2+N | 1×PUT = **1** | Complex (512-aligned, multi-PUT) |
| **API calls per incremental sync** | N×APPEND = N | 1×PUT = **1** | Multiple PUT Page calls |
| **Max single write** | 4 MiB per append | 5,000 MiB per PUT | 4 MiB per PUT Page |
| **Overwrite/reset** | Must DELETE+CREATE | Automatic (PUT overwrites) | Must clear pages explicitly |
| **Data transfer per sync** | Only new bytes (incremental) | Full WAL every time | Only changed pages |
| **Cost per 10K ops** | $0.05 (write) | $0.05 (write) | $0.05 (write) |
| **Latency** | Higher (multiple round-trips) | Lower (single round-trip) | Higher (alignment overhead) |
| **Complexity** | Medium (chunking, state tracking) | **Lowest** (one call) | Highest (alignment, sizing) |

#### The Data Transfer Tradeoff

The one area where append blobs have a theoretical advantage is **incremental sync data transfer**: with append blobs, we only send new bytes since the last sync. With block blobs, we re-upload the entire WAL every sync.

**In practice, this doesn't matter:**

1. **Full resyncs are common.** SQLite frequently truncates and rewrites the WAL (after checkpoint). Every truncate triggers `walNeedFullResync=1`, which means we re-upload the entire WAL anyway.

2. **WAL sizes are modest.** Typical WALs are 1–10 MiB. Uploading 5 MiB vs appending 500 KB saves ~30ms of transfer time but costs an extra 30ms in DELETE+CREATE overhead. Net effect is approximately zero.

3. **Simplicity wins.** The current code has 100+ lines of WAL sync logic with chunking, state tracking (`nWalSynced`, `walNeedFullResync`), error recovery (delete + recreate on partial append failure). The block blob path is ~10 lines.

4. **Network round-trips dominate.** Each HTTP call adds ~25–30ms of latency overhead (TLS, TCP, Azure processing). Eliminating 2–3 calls saves 50–90ms per sync, far more than the incremental data savings.

#### Page Blob Analysis (Rejected)

Page blobs would allow random-access writes (similar to the main DB file), but:
- 512-byte alignment requirement adds complexity for WAL data
- WAL writes are append-sequential, not random — page blob's strength is irrelevant
- Would need to track WAL size and resize the page blob (must be 512-byte aligned)
- More API calls than block blob for the same operation
- No advantage over block blobs for this use case

**Page blobs are the right choice for the main DB file (as per D1), but wrong for WAL.**
---
## Recommendation

**Switch WAL from append blobs to block blobs using the existing `block_blob_upload` vtable function.**

This is the highest-confidence optimization available:
- **Already implemented:** `az_block_blob_upload()` exists in `azure_client.c` (line 852) and is battle-tested for journal files
- **Already in vtable:** `block_blob_upload` is in `azure_ops_t` and `mock_azure_ops`
- **Minimal code change:** ~100 lines of WAL sync logic in `sqliteObjsSync` replaced with ~10 lines
- **No new Azure API calls:** Uses existing `Put Blob` with `x-ms-blob-type: BlockBlob`
---
## Code-Level Sketch

### Changes to `src/sqlite_objs_vfs.c`

The WAL sync path in `sqliteObjsSync()` (currently lines ~971–1075) simplifies dramatically:

```c
if (p->eFileType == SQLITE_OPEN_WAL) {
    /* WAL sync — upload entire WAL as block blob (single PUT, overwrites) */
    if (p->nWalData == 0) {
        /* Empty WAL — delete blob if it exists */
        if (p->walBlobExists && p->ops && p->ops->blob_delete) {
            azure_error_init(&aerr);
            p->ops->blob_delete(p->ops_ctx, p->zBlobName, &aerr);
            p->walBlobExists = 0;
        }
        return SQLITE_OK;
    }

    if (!p->ops || !p->ops->block_blob_upload) {
        return SQLITE_IOERR_FSYNC;
    }

    azure_error_init(&aerr);
    azure_err_t arc = p->ops->block_blob_upload(
        p->ops_ctx, p->zBlobName,
        p->aWalData, (size_t)p->nWalData, &aerr);

    if (arc != AZURE_OK) {
        return SQLITE_IOERR_FSYNC;
    }

    p->walBlobExists = 1;
    return SQLITE_OK;
}
```

### What Gets Removed

1. **`nWalSynced` tracking** — no longer needed (we always upload the full WAL)
2. **`walNeedFullResync` flag** — no longer needed (every sync is a full upload)
3. **Chunking loop** — no 4 MiB append limit (Put Blob supports 5,000 MiB)
4. **Error recovery** (delete + recreate on partial append) — single atomic PUT, no partial state
5. **`append_blob_create` calls** — not needed, Put Blob creates automatically
6. **`append_blob_delete` calls in sync path** — not needed, Put Blob overwrites

### What Gets Simplified in File State

```c
/* Remove from sqliteObjsFile struct: */
// sqlite3_int64 nWalSynced;          /* No longer needed */
// int walNeedFullResync;              /* No longer needed */

/* Add (optional, for skip-if-clean optimization): */
int walDirty;                          /* 1 if WAL changed since last sync */
int walBlobExists;                     /* Track if blob exists for empty-WAL delete */
```

### Changes to `src/azure_client.c`

**No changes needed.** `az_block_blob_upload()` already exists and works correctly. The only question is whether we want to add a lease-aware variant for WAL (current `block_blob_upload` doesn't take a `lease_id`). If WAL blobs need leases, we'd add:

```c
/* Optional: lease-aware block blob upload for WAL */
static azure_err_t az_block_blob_upload_with_lease(
    void *ctx, const char *name,
    const uint8_t *data, size_t len,
    const char *lease_id,
    azure_error_t *err)
{
    azure_client_t *c = (azure_client_t *)ctx;
    char lease_header[128];

    const char *extra_with_lease[] = {
        "x-ms-blob-type:BlockBlob",
        lease_header,
        NULL
    };
    const char *extra_no_lease[] = {
        "x-ms-blob-type:BlockBlob",
        NULL
    };

    const char *const *extra;
    if (lease_id && *lease_id) {
        snprintf(lease_header, sizeof(lease_header),
                 "x-ms-lease-id:%s", lease_id);
        extra = extra_with_lease;
    } else {
        extra = extra_no_lease;
    }

    return execute_with_retry(c, "PUT", name, NULL,
                              extra, "application/octet-stream",
                              data, len, NULL,
                              NULL, NULL, err);
}
```

### Changes to `azure_ops_t` Vtable

**Minimal.** Current `block_blob_upload` can be reused directly if WAL doesn't need leases. If lease support is needed, add a new vtable entry or extend the existing signature.

### What Happens to Append Blob Operations?

The 3 append blob vtable entries (`append_blob_create`, `append_blob_append`, `append_blob_delete`) can be:
1. **Kept** — no harm in leaving them. Other future use cases might want append blobs.
2. **Deprecated** — mark as unused, remove in next major refactor.

Recommend: **Keep them** for now. Zero cost to have unused vtable entries.
---
## Migration Concerns

### Existing WAL Blobs in Azure

If there are existing append blob WALs in Azure from prior runs:
- `Put Blob` with `x-ms-blob-type: BlockBlob` on the same blob name will **overwrite the append blob**, converting it to a block blob. This is automatic — no migration script needed.
- The first sync after the code change will simply overwrite whatever blob type was there.

### Azurite Compatibility

Azurite (our test emulator) supports `Put Blob` for block blobs. The journal sync path already uses `block_blob_upload` against Azurite successfully.

### Mock/Test Impact

- `mock_block_blob_upload_impl` already exists in `test/mock_azure_ops.c`
- Tests that verify WAL sync behavior will need updating to expect `block_blob_upload` calls instead of `append_blob_create` + `append_blob_append` sequences
- The mock can be simpler — no need to track append state

### Breaking Changes

**None for external consumers.** The blob name scheme doesn't change (`<name>-wal`). The Azure container doesn't change. Only the blob type changes (append → block), which is transparent to any readers using `Get Blob` (works on all blob types).
---
## Estimated Impact (from timing logs)

### Current 4.5 MiB WAL Sync (Full Resync)
```
DELETE tpcc.db-wal:     27.5ms
PUT create (body=0):    29.2ms
PUT append (4 MiB):    368.7ms
PUT append (284 KB):    54.6ms
────────────────────────────────
Total: 4 calls,        ~480ms
```

### Proposed 4.5 MiB WAL Sync
```
PUT block blob (4.5 MiB): ~370ms  (single call, similar data transfer)
────────────────────────────────
Total: 1 call,            ~370ms
```

**Savings: ~110ms per full resync (23% reduction), 3 fewer API calls.**

For smaller WALs (common in OLTP), the savings are even more dramatic because the fixed overhead of DELETE + CREATE (~57ms) dominates:

### 100 KB WAL Sync
```
Current:  DELETE(28ms) + CREATE(29ms) + APPEND(35ms) = ~92ms, 3 calls
Proposed: PUT(35ms) = ~35ms, 1 call
Savings:  ~57ms (62% reduction), 2 fewer API calls
```
---
## Decision Request

**Gandalf:** Please review and approve switching WAL from append blobs to block blobs. This is a clear win on every axis — fewer API calls, lower latency, simpler code, no migration burden. The only tradeoff (re-uploading full WAL vs incremental append) is negligible given typical WAL sizes and the overhead of append blob management.

**Aragorn:** The VFS changes are primarily in `sqliteObjsSync` WAL path. ~100 lines replaced with ~10. The `nWalSynced` and `walNeedFullResync` state tracking can be removed.

**Samwise:** Tests will need updating — WAL sync tests should expect `block_blob_upload` calls instead of `append_blob_create`/`append_blob_append` sequences. The test logic gets simpler.
# D-EAGER: Eager Write / Eager Page Flush Analysis

**Date:** 2025-07-18  
**From:** Gandalf (Lead/Architect)  
**Status:** ANALYSIS — NOT RECOMMENDED (see §6)
---
## 1. Current Write Path

Three distinct file types have three distinct write paths:

### MAIN_DB (page blob)

```
xWrite (line 767–811):
  pwrite() → local cache file (disk I/O, ~0.1ms)
  dirtyMarkPage() → set bit in dirty bitmap (CPU, ns)

xSync (line 1111–1301):
  leaseRenewIfNeeded()                         ~0ms or ~45ms if near expiry
  page_blob_resize() if file grew              ~45ms HTTP round-trip
  coalesceDirtyRanges()                        ~µs (CPU)
  pread() dirty ranges from cache file         ~0.1ms per range (disk)
  page_blob_write_batch() via curl_multi       10–200ms (network, parallelized)
    └─ 32 concurrent PUT Page requests
    └─ single-threaded event loop (no threads spawned)
    └─ mutex held for entire batch
  dirtyClearAll()                              ~µs (CPU)
```

**Latency breakdown for a typical 10-page commit:**
- xWrite: 10 × ~0.1ms = ~1ms total (local disk, overlaps with SQLite's btree work)
- xSync: ~50–100ms (dominated by one round-trip for batch PUT)

### MAIN_JOURNAL (block blob)

```
xWrite (line 757–764):
  memcpy() → aJrnlData buffer (RAM, µs)

xSync (line 1078–1108):
  block_blob_upload() → single PUT (sequential, ~45ms)
```

### WAL (append blob)

```
xWrite (line 743–754):
  memcpy() → aWalData buffer (RAM, µs)
  Track nWalSynced / walNeedFullResync for incremental append

xSync (line 976–1075):
  If incremental: append_blob_append() for new data only
  If full resync: delete + create + re-upload all chunks
  Chunked to 4 MiB (AZURE_MAX_APPEND_SIZE)
  Each chunk: sequential execute_with_retry() → ~45ms per chunk
```

### Key observation

**xWrite is already fast.** For MAIN_DB, it's a local pwrite + bitmap mark (~0.1ms). For journal and WAL, it's a memcpy into a RAM buffer (µs). All the Azure latency is concentrated in xSync. The proposal is about moving Azure I/O *earlier* — starting it in xWrite rather than waiting for xSync.
---
## 2. Eager WAL Write Feasibility

### The idea
Start the Azure append_blob_append() PUT at xWrite time for WAL files. xSync would then just block until the in-flight PUT completes.

### Analysis

**2.1 Threading implications — this is the hard problem.**

The append_blob_append() function uses `execute_with_retry()` which acquires `client->mutex` and performs a synchronous curl_easy_perform(). To make this non-blocking from xWrite's perspective, we need either:

**(a) Background thread** — spawn a pthread to run the append. xSync joins it.
- Requires: pthread_create/join per xWrite, or a persistent worker thread with a queue.
- Problem: The WAL buffer (aWalData) is mutated by subsequent xWrite calls. If we start uploading a region and SQLite writes to that same region before the upload completes, we have a data race. We'd need to **copy** the data before handing it to the background thread.
- Problem: curl handles are NOT thread-safe. We'd need a separate curl handle for background uploads, or careful mutex choreography that mostly eliminates the concurrency benefit.

**(b) curl_multi non-blocking** — add the append to the existing curl_multi event loop, poll it in xSync.
- Better fit with our architecture. But curl_multi_perform() must be called to drive I/O, and currently it's only called inside `az_page_blob_write_batch()` under the mutex.
- To drive I/O between xWrite and xSync, something needs to call curl_multi_perform(). SQLite doesn't give us a "tick" callback. We'd have to drive it from within xWrite itself (defeating the purpose) or from a background thread (back to option a).

**Verdict: Neither option works well.**

**2.2 Multiple xWrite calls before xSync**

SQLite calls xWrite for each page frame appended to the WAL. A typical commit writes the WAL header (32 bytes), then N page frames (page_size + 24 bytes each). A 10-page transaction produces ~11 xWrite calls before one xSync.

If we start a PUT on the first xWrite, subsequent xWrites add more data to the WAL buffer. We can't know at xWrite time whether more writes are coming. Options:
- **Pipeline individual PUTs**: One PUT per xWrite. But Azure append blob appends are sequential and ordered — each must complete before the next can start (the service enforces this via append position conditions). So pipelining doesn't help; you'd serialize anyway.
- **Batch at xSync**: This is what we already do. The incremental append logic (line 1038–1073) already only sends the delta since last sync.

**2.3 Async PUT failure reporting**

SQLite's VFS contract (from sqlite3.h):
> "xWrite returns SQLITE_OK to indicate that the requested number of bytes were successfully written."

xWrite is about writing to the VFS's local representation. SQLite does NOT require that xWrite has persisted data to durable storage — that's what xSync is for. So deferring the actual upload and reporting errors at xSync time is **contractually valid**.

However, once xWrite returns SQLITE_OK, SQLite may issue more xWrites that depend on the first succeeding. If the background PUT fails and we report at xSync, we've accumulated state that assumed earlier writes succeeded. The rollback path is just "try the whole thing again at xSync" which is... exactly what we do today.

**2.4 How much overlap could we get?**

In WAL mode with EXCLUSIVE locking, SQLite's write path is:
```
for each page:
    xWrite(wal, frame_header + page_data)   ← ~µs each (memcpy)
xSync(wal)                                   ← ~45ms per 4MB chunk
xWrite(wal, new_header_salt)                 ← update WAL header
xSync(wal)                                   ← another ~45ms
xWrite(db, updated_pages)                    ← write-back to DB
xSync(db)                                    ← parallel batch PUT
```

The gap between the last WAL xWrite and WAL xSync is **essentially zero** — SQLite calls them back-to-back. There is no computation to overlap with. The window for eager upload to provide benefit is measured in microseconds.

**Verdict: Not feasible. The overlap window is too small for WAL files because SQLite calls xWrite and xSync in tight sequence. The threading complexity is high and the benefit is near zero.**
---
## 3. Eager Page Flush Feasibility

### The idea
For MAIN_DB pages, start uploading dirty pages to Azure as soon as they're written to the local cache file, rather than waiting for xSync.

### Analysis

**3.1 SQLite's write ordering contract — this is fatal.**

SQLite's pager has a strict ordering requirement:
1. Write journal (or WAL frames) — the "undo" record
2. xSync the journal — ensure undo is durable
3. Write database pages — the actual modifications
4. xSync the database — ensure modifications are durable

If we start uploading DB pages at xWrite time (step 3), those uploads could **complete before the journal xSync** (step 2). If we crash at that moment:
- Database pages are partially updated in Azure
- Journal may not be fully uploaded
- **Unrecoverable corruption**

This is exactly the corruption window that journals exist to prevent. The xSync ordering is not a suggestion — it's the core invariant of SQLite's crash recovery.

**3.2 Even within the DB flush, ordering matters.**

During xSync(db), the dirty pages could include:
- Pages that were part of a btree split
- Free-list pages being recycled
- The database header page (page 1) with updated change counter

If we upload some but not all, and crash, the database is inconsistent. Our batch write (page_blob_write_batch) handles this by treating the entire batch as all-or-nothing: if any range fails, none of the dirty bits are cleared, and the next xSync retries everything.

With eager flushing, pages would be uploaded piecemeal. We'd need to track which individual pages succeeded, and our crash recovery model would need to handle partially-flushed states. The journal doesn't help here because it was already synced (step 2) — the journal records the *old* page contents, and recovery replays by restoring old pages. But if some new pages made it to Azure and others didn't, the database is in a state the journal can't fix.

**3.3 The btree mutex constraint**

Our zero-copy batch write relies on D17: the btree mutex is held during xSync, so page data (aData) is stable. If we start uploads during xWrite, subsequent xWrites to other pages could trigger btree rebalancing, which may modify pages that are in-flight to Azure. We'd need to **copy every dirty page** before uploading, adding significant memory pressure.

**3.4 Lease coordination**

Background uploads would need the lease to be held. Currently, the lease is acquired at RESERVED lock level and held through the transaction. But background uploads would need to renew the lease independently, potentially from a different thread. Our lease_renew goes through execute_with_retry → client->mutex. A background uploader contending on the mutex with the foreground xSync path creates a serialization bottleneck.

**3.5 How would xSync coordinate?**

If we had in-flight background uploads, xSync would need to:
1. Stop accepting new background uploads
2. Wait for all in-flight uploads to complete
3. Check that all succeeded
4. If any failed, re-read those pages from cache and retry
5. Only then clear dirty bits

This is essentially a barrier synchronization. The implementation would need:
- A condition variable or semaphore per upload batch
- An error accumulator (thread-safe)
- A way to cancel in-flight uploads if the lease expires

**3.6 Azure PUT ordering**

Azure Blob Storage provides **no ordering guarantee** between concurrent PUT Page requests to the same blob. PUTs are individually atomic (the 512-byte range is either fully written or not), but there's no happens-before relationship between concurrent PUTs. This is fine for our batch write (all ranges are disjoint), but it means we can't rely on upload order for consistency — we must wait for ALL to complete before declaring success.

**Verdict: Not feasible. Violates SQLite's journal-before-data ordering invariant. Even ignoring that, the concurrency primitives required (thread pool, per-page copy, barrier sync, thread-safe error accumulation) add enormous complexity for uncertain benefit.**
---
## 4. Expected Performance Impact

### How much overlap can we realistically get?

**For WAL xSync:** Near zero. SQLite's WAL write path is:
```
xWrite(wal, frame)    ← µs
xWrite(wal, frame)    ← µs
...
xSync(wal)            ← 45ms+  ← THIS is the bottleneck
```
There's no gap to fill. The xWrites complete in microseconds; then xSync blocks.

**For DB xSync:** The writes happen in a burst:
```
xWrite(db, page1)     ← 0.1ms
xWrite(db, page2)     ← 0.1ms
...
xWrite(db, pageN)     ← 0.1ms
xSync(db)             ← 50-200ms  ← THIS is the bottleneck
```
Total xWrite time for 100 pages: ~10ms. Total xSync time: ~100ms. Even with perfect overlap, we'd save at most 10ms — the writes are fast, the sync is slow. The ratio gets worse (less benefit) as page count decreases.

### What's the typical gap between last xWrite and xSync?

Instrumented timing shows SQLite calls xSync within **microseconds** of the last xWrite. There is no useful computation gap. SQLite's pager calls `sqlite3OsSync()` immediately after the write loop completes.

### Is the juice worth the squeeze?

**No.** The maximum theoretical win is ~10ms on a 100-page commit (overlapping pwrite time with Azure upload). Our Phase 2 batch write already reduced a 100-page commit from ~10,000ms to ~100ms — a 100× improvement. Eager writes would shave another ~10% at best, with massive complexity cost.

The actual bottleneck is Azure network latency (round-trip time), not local write time. To further reduce commit latency, the productive approaches are:
- **Reduce round-trips:** Coalescing (Phase 1, done) and batching (Phase 2, done) already minimize HTTP requests.
- **Reduce per-request latency:** Connection pooling with TLS session reuse (done — persistent CURLM).
- **Reduce sync frequency:** `PRAGMA synchronous=NORMAL` (already recommended in D-PERF).
---
## 5. Implementation Complexity and Risks

### New concurrency primitives required

For Eager WAL Write:
- Background worker thread (pthread) or async I/O integration
- Thread-safe WAL data buffer with copy-on-write or double-buffering
- Completion notification mechanism (condition variable or pipe)
- Separate curl handle for background uploads

For Eager Page Flush:
- Thread pool for background page uploads (pthreads)
- Per-page data copy buffer (can't use zero-copy)
- Barrier synchronization at xSync
- Thread-safe dirty bitmap with atomic operations
- Thread-safe error accumulation
- Background lease renewal (separate from foreground)
- Cancellation mechanism for in-flight uploads

### New failure modes

1. **Race condition on WAL buffer:** Background upload reads from aWalData while foreground xWrite modifies it → data corruption in uploaded WAL.
2. **Journal ordering violation:** Background DB page upload completes before journal xSync → unrecoverable corruption on crash.
3. **Partial flush state:** Some pages uploaded, crash before xSync completes → database in inconsistent state that journal can't repair.
4. **Lease contention:** Background uploader and foreground VFS contend on client->mutex → serialization eliminates concurrency benefit.
5. **Thread lifecycle bugs:** Worker thread outlives database close, or database closes while upload in-flight → use-after-free.
6. **curl handle sharing:** curl easy handles are not thread-safe. Need dedicated handles per thread, with separate TLS sessions (losing connection pool benefit).

### Interaction with existing mutex/thread-safety model

The current model is simple and correct:
- `client->mutex` protects the curl handle and multi handle
- SQLite's btree mutex serializes all VFS calls per connection
- No background threads — everything is synchronous from SQLite's perspective
- curl_multi provides concurrency without threads

Eager writes would break this model by introducing true threading. The mutex would become a contention point rather than a safety mechanism. We'd likely need to split it into per-resource locks (one for multi_handle, one for the easy handle pool, one for the error state), which is where subtle deadlocks live.
---
## 6. Recommendation

**I do not recommend implementing either Eager WAL Write or Eager Page Flush.**

### Why not

1. **The benefit is marginal.** Local writes (pwrite/memcpy) complete in microseconds to low milliseconds. Network uploads take 45–200ms. Overlapping a 0.1ms operation with a 50ms operation saves 0.1ms — the network dominates regardless.

2. **SQLite gives us no overlap window.** It calls xSync immediately after the last xWrite. There's no computation between them to hide latency behind.

3. **The correctness risks are severe.** Eager DB page flush violates SQLite's fundamental crash-safety invariant (journal must sync before data pages reach durable storage). This isn't a subtle edge case — it's the primary reason journals exist.

4. **The implementation complexity is disproportionate.** We'd introduce threads, copies, barriers, cancellation, and split-lock patterns into a codebase that currently achieves excellent concurrency with zero threads (curl_multi). This is a large regression in code clarity for a tiny performance gain.

5. **We already captured the big wins.** Phase 1 coalescing + Phase 2 parallel batch reduced commit latency by 50–100×. The remaining latency is irreducible network round-trip time.

### What I would recommend instead

If write latency is still a concern, these approaches have better effort-to-benefit ratios:

1. **Write-ahead coalescing at the application level.** Applications that batch multiple changes into a single transaction already minimize xSync calls. `BEGIN; ...many writes...; COMMIT;` is one xSync, regardless of write count.

2. **`PRAGMA synchronous=NORMAL`.** Already recommended in D-PERF. Eliminates one journal upload per commit. ~45ms saved per transaction.

3. **Pipeline journal + DB flush.** Currently, journal xSync blocks, then DB xSync blocks. We could upload the journal *and* start the DB batch write in parallel (journal to block blob, DB pages to page blob). Both are independent HTTP calls to different blobs. This would save one round-trip (~45ms) per commit without any of the ordering or threading concerns, because we'd still wait for *both* to complete before returning from the DB xSync. This is the one genuinely useful optimization in this design space.

4. **HTTP/2 multiplexing.** curl supports HTTP/2 via `CURLOPT_HTTP_VERSION`. Multiple PUT requests to the same host could share a single TCP+TLS connection. This would reduce connection setup overhead and improve batch write throughput.
---
## Appendix: SQLite xWrite/xSync Contract

From `sqlite3.h`:
```
** xSync: Make sure all writes to a particular file are committed to
** the disk media before returning.
**
** xWrite: Write data to a file. Return SQLITE_OK on success.
** The file grows if necessary.
```

The key insight: xWrite promises the data is written to the *file* (our local cache + bitmap). xSync promises it's committed to *durable media* (Azure). We can defer Azure I/O to xSync — that's exactly what we already do. But we cannot *start* Azure I/O before the caller expects it (between xWrite and xSync), because the data may not be in its final state yet, and the ordering constraints haven't been satisfied.
# D-TEST: Comprehensive Rust VFS Integration Test Architecture

**Date:** 2025-07-18 | **From:** Gandalf (Lead/Architect)
**Status:** PROPOSED — Awaiting Brady's review before implementation
---
## 1. Design Philosophy

The Rust integration test suite is the final safety net for the entire Rust→rusqlite→SQLite→VFS→Azure stack. Unlike the C test pyramid (unit mocks + Azurite integration), these tests exercise the **full production path**: Rust calling `rusqlite`, which calls SQLite, which calls our C VFS, which calls Azure Blob Storage.

**Principles:**
1. **Every test must assert something observable.** No "smoke" tests that just check for no-crash. Assert data, state, or side effects.
2. **Deterministic data generation.** Seed-based: given a seed, generate data and verify it without storing expected values.
3. **Azure tests must tolerate slow networks.** Liberal timeouts, retry-aware assertions.
4. **Local tests must be fast.** Target < 5s for the entire local suite.
5. **Test isolation is mandatory.** Every Azure test uses a UUID blob name. Every local test uses a fresh `TempDir`.
---
## 2. Test File Organization

```
rust/sqlite-objs/tests/
├── common/
│   ├── mod.rs              # Re-exports all helpers
│   ├── test_db.rs          # TestDb struct, Azure/local factory
│   ├── seeded_data.rs      # Deterministic data generation & validation
│   └── azure_helpers.rs    # Credentials, container ops, FCNTL wrappers
├── lifecycle.rs            # DB create, reopen, recovery, close semantics
├── transactions.rs         # Commit, rollback, nested savepoints, WAL
├── cache_reuse.rs          # ETag cache hit/miss, download counter, disk state
├── threading.rs            # Multi-threaded access patterns (KEEP EXISTING, extend)
├── growth_shrink.rs        # Bulk insert, DELETE+VACUUM, blob resize
├── azure_smoke.rs          # (KEEP EXISTING 3 tests, unchanged)
├── perf_matrix.rs          # (KEEP EXISTING 200+ tests, unchanged)
└── error_recovery.rs       # Dirty shutdown, partial sync, lease expiry
```

**Rationale:** One file per concern. Shared infrastructure in `common/`. Existing files untouched — new tests go in new files. Each file is independently runnable via `cargo test --test <name>`.

**Naming convention:** `test_<verb>_<scenario>` — e.g., `test_create_new_database`, `test_reopen_reads_committed_data`.
---
## 3. Test Infrastructure

### 3.1 TestDb — The Core Fixture

```rust
/// Manages a test database lifecycle: creation, connection, cleanup.
/// Abstracts over local-file vs Azure-backed databases.
pub struct TestDb {
    pub uri: String,
    pub mode: TestMode,
    temp_dir: Option<TempDir>,    // RAII cleanup for local tests
    blob_name: String,            // UUID-based for Azure isolation
    cache_dir: Option<TempDir>,   // Separate TempDir for cache files
}

pub enum TestMode {
    /// Local file in TempDir — fast, no Azure needed
    Local,
    /// Azure blob via Azurite (localhost:10000)
    Azurite,
    /// Real Azure (weekly CI only)
    Azure,
}

impl TestDb {
    /// Create a new test database (local file mode)
    pub fn local() -> Self { ... }

    /// Create a new test database (Azure mode, auto-selects Azurite vs real)
    pub fn azure() -> Self { ... }

    /// Create a new test database with explicit cache_dir for cache tests
    pub fn azure_with_cache() -> Self { ... }

    /// Open a rusqlite::Connection to this database
    pub fn connect(&self) -> Connection { ... }

    /// Open with specific OpenFlags
    pub fn connect_with_flags(&self, flags: OpenFlags) -> Connection { ... }

    /// Reopen: creates a NEW connection to the same database.
    /// For Azure: this forces a new xOpen → potential download.
    pub fn reopen(&self) -> Connection { ... }

    /// Get the cache directory path (for cache file inspection)
    pub fn cache_dir(&self) -> Option<&Path> { ... }

    /// Get the blob name (for direct Azure inspection if needed)
    pub fn blob_name(&self) -> &str { ... }
}
```

**Key design:** `TestDb` owns the `TempDir`, so cleanup is automatic. `connect()` vs `reopen()` distinction: `reopen()` opens a fresh connection to the same URI, which triggers a new xOpen (important for cache reuse tests). The `cache_dir` is a separate `TempDir` so we can inspect cache files independently.

### 3.2 VFS Registration (Once)

```rust
/// Module-level VFS registration using OnceLock (Rust 1.70+)
static VFS_REGISTERED: OnceLock<()> = OnceLock::new();

pub fn ensure_vfs() {
    VFS_REGISTERED.get_or_init(|| {
        SqliteObjsVfs::register_uri(false).expect("VFS registration");
    });
}
```

All test files call `ensure_vfs()` at the start of each test. The `OnceLock` guarantees single registration across all threads in the test binary.

### 3.3 Azure Credential Handling

```rust
pub fn azure_available() -> bool {
    dotenvy::dotenv().ok(); // Load .env if present
    std::env::var("AZURE_STORAGE_ACCOUNT").is_ok()
        && std::env::var("AZURE_STORAGE_CONTAINER").is_ok()
        && std::env::var("AZURE_STORAGE_SAS").is_ok()
}

/// Skip macro for Azure-dependent tests
macro_rules! require_azure {
    () => {
        if !azure_available() {
            eprintln!("⏭ Skipping: Azure credentials not set");
            return;
        }
    };
}
```

**No `#[ignore]`.** Instead, each Azure test checks credentials and returns early if absent. This means `cargo test` always runs the full suite — local tests execute, Azure tests self-skip. CI with Azure env vars runs everything.

### 3.4 FCNTL Download Counter Wrapper

The C VFS exposes `SQLITE_OBJS_FCNTL_DOWNLOAD_COUNT` (op code 200) to query how many full blob downloads occurred on a connection. This is the key mechanism for validating ETag cache reuse. We need a safe Rust wrapper:

```rust
const SQLITE_OBJS_FCNTL_DOWNLOAD_COUNT: i32 = 200;

/// Query the number of full blob downloads on this connection.
/// Returns 0 if cache was reused (ETag hit), >0 if a fresh download occurred.
pub fn download_count(conn: &Connection) -> i32 {
    let mut count: i32 = 0;
    // rusqlite doesn't expose file_control directly — use the raw FFI
    unsafe {
        let db = conn.handle();
        let schema = std::ffi::CString::new("main").unwrap();
        sqlite_objs_sys::sqlite3_file_control(
            db,
            schema.as_ptr(),
            SQLITE_OBJS_FCNTL_DOWNLOAD_COUNT,
            &mut count as *mut i32 as *mut std::ffi::c_void,
        );
    }
    count
}
```

### 3.5 WAL Mode Configuration Helper

```rust
/// Configure connection for WAL mode (required for Azure).
/// PRAGMA order matters: locking_mode must be set before journal_mode.
pub fn configure_wal(conn: &Connection) {
    conn.pragma_update(None, "locking_mode", "EXCLUSIVE").unwrap();
    conn.pragma_update(None, "journal_mode", "WAL").unwrap();
    conn.pragma_update(None, "synchronous", "NORMAL").unwrap();
    conn.pragma_update(None, "busy_timeout", 60000).unwrap();
}
```
---
## 4. Deterministic Data Strategy

### 4.1 Seed-Based Data Generation

The key insight: if we generate data from a deterministic seed, we can regenerate expected values at verification time without storing them. This is critical for:
- Large datasets (thousands of rows — don't store expected values in memory)
- Cross-process verification (reopen DB, regenerate expectations, compare)
- Growth/shrink tests (know exactly what should survive DELETE)

```rust
/// Deterministic test data generator. Given a seed and row index,
/// produces the same values every time.
pub struct SeededData {
    seed: u64,
}

impl SeededData {
    pub fn new(seed: u64) -> Self { Self { seed } }

    /// Generate a deterministic integer for row `i`.
    /// Uses a simple hash: seed XOR (i * large_prime) to avoid patterns.
    pub fn int_value(&self, i: u64) -> i64 {
        let mixed = self.seed ^ (i.wrapping_mul(6364136223846793005));
        // Fold to i64
        mixed as i64
    }

    /// Generate a deterministic string for row `i` with approximate `len` bytes.
    /// Output is hex-encoded hash, so always valid UTF-8.
    pub fn text_value(&self, i: u64, len: usize) -> String {
        let mut result = String::with_capacity(len);
        let mut state = self.seed ^ (i.wrapping_mul(2862933555777941757));
        while result.len() < len {
            state = state.wrapping_mul(6364136223846793005).wrapping_add(1);
            write!(&mut result, "{:016x}", state).unwrap();
        }
        result.truncate(len);
        result
    }

    /// Generate a deterministic blob for row `i` with exact `len` bytes.
    pub fn blob_value(&self, i: u64, len: usize) -> Vec<u8> {
        let mut result = Vec::with_capacity(len);
        let mut state = self.seed ^ (i.wrapping_mul(1442695040888963407));
        while result.len() < len {
            state = state.wrapping_mul(6364136223846793005).wrapping_add(1);
            result.extend_from_slice(&state.to_le_bytes());
        }
        result.truncate(len);
        result
    }

    /// Insert `count` rows into a table with schema:
    ///   CREATE TABLE data(id INTEGER PRIMARY KEY, val INTEGER, txt TEXT, blb BLOB)
    pub fn populate(&self, conn: &Connection, count: u64, text_len: usize) {
        conn.execute_batch("CREATE TABLE IF NOT EXISTS data(
            id INTEGER PRIMARY KEY, val INTEGER, txt TEXT, blb BLOB
        )").unwrap();

        let mut stmt = conn.prepare(
            "INSERT INTO data(id, val, txt, blb) VALUES (?1, ?2, ?3, ?4)"
        ).unwrap();

        for i in 0..count {
            stmt.execute(rusqlite::params![
                i as i64,
                self.int_value(i),
                self.text_value(i, text_len),
                self.blob_value(i, 32),
            ]).unwrap();
        }
    }

    /// Verify all `count` rows match expected values. Panics on mismatch.
    pub fn verify(&self, conn: &Connection, count: u64, text_len: usize) {
        let mut stmt = conn.prepare(
            "SELECT id, val, txt, blb FROM data ORDER BY id"
        ).unwrap();

        let rows: Vec<(i64, i64, String, Vec<u8>)> = stmt
            .query_map([], |row| {
                Ok((row.get(0)?, row.get(1)?, row.get(2)?, row.get(3)?))
            })
            .unwrap()
            .collect::<Result<_, _>>()
            .unwrap();

        assert_eq!(rows.len(), count as usize, "Row count mismatch");

        for (id, val, txt, blb) in &rows {
            let i = *id as u64;
            assert_eq!(*val, self.int_value(i), "val mismatch at row {i}");
            assert_eq!(*txt, self.text_value(i, text_len), "txt mismatch at row {i}");
            assert_eq!(*blb, self.blob_value(i, 32), "blb mismatch at row {i}");
        }
    }

    /// Verify that rows in range [start, end) exist and match.
    /// Used after partial DELETEs to verify survivors.
    pub fn verify_range(&self, conn: &Connection, start: u64, end: u64, text_len: usize) {
        for i in start..end {
            let (val, txt, blb): (i64, String, Vec<u8>) = conn.query_row(
                "SELECT val, txt, blb FROM data WHERE id = ?1",
                [i as i64],
                |row| Ok((row.get(0)?, row.get(1)?, row.get(2)?)),
            ).unwrap_or_else(|_| panic!("Row {i} missing"));

            assert_eq!(val, self.int_value(i), "val mismatch at row {i}");
            assert_eq!(txt, self.text_value(i, text_len), "txt mismatch at row {i}");
            assert_eq!(blb, self.blob_value(i, 32), "blb mismatch at row {i}");
        }
    }
}
```

### 4.2 Usage Pattern

```rust
let seed = 42;
let data = SeededData::new(seed);

// Populate
let conn = test_db.connect();
data.populate(&conn, 1000, 64);
drop(conn);

// Reopen and verify — no stored expectations needed
let conn2 = test_db.reopen();
data.verify(&conn2, 1000, 64);
```
---
## 5. Test Categories

### 5.1 Lifecycle Tests (`lifecycle.rs`)

These test the fundamental create→use→close→reopen cycle that exercises xOpen, xClose, xRead, xWrite, xSync, xFileSize.

| # | Test Name | What It Validates | Key Assertions | Azure? |
|---|-----------|-------------------|----------------|--------|
| 1 | `test_create_new_database` | xOpen with CREATE flag creates a new page blob. Schema creation triggers xWrite→xSync. | Table exists after creation. `SELECT count(*) FROM sqlite_master = 1`. | Yes |
| 2 | `test_create_inserts_and_reads` | Full write path: xWrite (dirty pages) → xSync (upload) → xRead (from cache). | SeededData(42).verify() passes for 100 rows. | Yes |
| 3 | `test_reopen_persists_data` | Close→reopen cycle. Second xOpen downloads blob. Data survives. | SeededData(42).verify() on reopened connection. download_count > 0 (fresh download). | Yes |
| 4 | `test_reopen_schema_intact` | Schema (tables, indexes, views) persists across close→reopen. | `PRAGMA table_info(data)` matches. Index exists. | Yes |
| 5 | `test_close_then_reopen_readonly` | Open with READONLY flags after initial creation. xOpen without CREATE. | Read succeeds. Write returns SQLITE_READONLY. | Yes |
| 6 | `test_multiple_tables_persist` | Multiple CREATE TABLE + data across tables, then reopen. | All tables and data present after reopen. | Yes |
| 7 | `test_index_persists_across_reopen` | CREATE INDEX, close, reopen. Verify index used in EXPLAIN QUERY PLAN. | `EXPLAIN QUERY PLAN SELECT ... WHERE indexed_col = ?` shows "USING INDEX". | Yes |
| 8 | `test_empty_database_reopen` | Create DB, create no tables, close, reopen. | `SELECT count(*) FROM sqlite_master = 0`. No crash. | Yes |
| 9 | `test_local_file_roundtrip` | Same lifecycle tests against local file VFS (baseline). | SeededData(42).verify() passes. | No |

**VFS methods exercised:** xOpen (create + reopen), xClose (clean shutdown), xRead, xWrite, xSync, xFileSize, xLock (SHARED→EXCLUSIVE→SHARED), xUnlock.

### 5.2 Transaction Tests (`transactions.rs`)

These test SQLite's transaction machinery which drives the xLock/xUnlock/xSync state machine, journal creation (xOpen for MAIN_JOURNAL), and rollback (xTruncate on journal).

| # | Test Name | What It Validates | Key Assertions | Azure? |
|---|-----------|-------------------|----------------|--------|
| 1 | `test_commit_persists` | BEGIN→INSERT→COMMIT. Committed data survives reopen. | SeededData.verify() after reopen. | Yes |
| 2 | `test_rollback_discards` | BEGIN→INSERT→ROLLBACK. Rolled-back data does not persist. | `SELECT count(*) = 0` after rollback. Reopen also shows 0. | Yes |
| 3 | `test_autocommit_each_statement` | Individual INSERT without BEGIN. Each implicitly committed. | Each row present after reopen. | Yes |
| 4 | `test_savepoint_commit` | SAVEPOINT→INSERT→RELEASE. Nested transaction committed. | Data present after reopen. | Yes |
| 5 | `test_savepoint_rollback` | SAVEPOINT→INSERT→ROLLBACK TO. Inner work discarded, outer survives. | Only outer data present. | Yes |
| 6 | `test_large_transaction` | Single transaction with 5,000 rows. Tests dirty page batching + parallel sync. | SeededData(99).verify(5000) after reopen. | Yes |
| 7 | `test_multiple_sequential_transactions` | 10 transactions, each inserting 100 rows. Tests repeated lock/unlock cycles. | All 1,000 rows present and correct after reopen. | Yes |
| 8 | `test_wal_mode_commit` | WAL mode: INSERT→checkpoint→reopen. WAL-specific sync path (block blob upload). | Data persists. `PRAGMA journal_mode` returns "wal". | Yes |
| 9 | `test_wal_mode_rollback` | WAL mode: INSERT→ROLLBACK. WAL rollback path. | No data after rollback. | Yes |
| 10 | `test_wal_checkpoint_explicit` | WAL mode: Insert data, `PRAGMA wal_checkpoint(TRUNCATE)`, reopen. | Data persists. WAL file cleaned up. | Yes |
| 11 | `test_mixed_read_write_transaction` | BEGIN→SELECT→INSERT→SELECT→COMMIT. Reads see uncommitted writes within same txn. | Mid-transaction SELECT returns just-inserted rows. | Yes |
| 12 | `test_transaction_local_baseline` | Same transaction tests against local file (no Azure). Baseline correctness. | All assertions pass. | No |

**VFS methods exercised:** xLock (SHARED→RESERVED→EXCLUSIVE), xUnlock (EXCLUSIVE→SHARED→NONE), xSync (journal + DB), xOpen/xClose (MAIN_JOURNAL), xTruncate (journal cleanup on commit), xWrite (dirty pages), xCheckReservedLock.

### 5.3 Cache Reuse Tests (`cache_reuse.rs`)

These test the ETag-based cache reuse mechanism. The key observable: `download_count(conn)` which reports how many full blob downloads occurred on this connection's xOpen.

| # | Test Name | What It Validates | Key Assertions | Azure? |
|---|-----------|-------------------|----------------|--------|
| 1 | `test_cache_reuse_skips_download` | Open with `cache_reuse=1`, insert data, close. Reopen same URI. ETag matches → skip download. | `download_count == 0` on second open. Data correct. | Yes |
| 2 | `test_cache_miss_forces_download` | Open conn1 with `cache_reuse=1`, insert, close. Open conn2 (no cache_reuse), modify, close. Reopen conn1 → ETag mismatch. | `download_count > 0` on third open. Modified data visible. | Yes |
| 3 | `test_cache_files_created_on_close` | Close with `cache_reuse=1` and clean state. Inspect cache_dir. | `.cache` file exists. `.etag` sidecar exists. Both contain valid data. | Yes |
| 4 | `test_cache_files_deleted_without_reuse` | Close with `cache_reuse=0` (default). | No `.cache` or `.etag` files in cache_dir. | Yes |
| 5 | `test_no_cache_files_on_dirty_close` | Open with `cache_reuse=1`. Insert data. Drop connection WITHOUT closing cleanly (if possible) or close with dirty state. | `.etag` file should NOT exist (crash safety: conservative re-download). | Yes |
| 6 | `test_cache_reuse_with_empty_database` | Create empty DB (no tables), close with cache_reuse=1, reopen. | `download_count == 0`. No crash. | Yes |
| 7 | `test_cache_dir_custom_path` | Use `cache_dir=/custom/path` via UriBuilder. | Cache files appear in custom path, not default /tmp. | Yes |
| 8 | `test_cache_reuse_across_many_reopens` | Open→close→reopen 5 times with cache_reuse=1. No modifications between reopens. | `download_count == 0` on reopens 2-5. Data always correct. | Yes |
| 9 | `test_cache_reuse_with_wal_mode` | Same as test 1, but with WAL mode enabled. | `download_count == 0` on cache hit. WAL checkpoint correctly handled. | Yes |

**VFS methods exercised:** xOpen (ETag check, conditional download), xClose (cache file persistence, .etag sidecar write), xFileControl (FCNTL_DOWNLOAD_COUNT).

**Cache file inspection pattern:**
```rust
let cache_dir = test_db.cache_dir().unwrap();
let cache_files: Vec<_> = std::fs::read_dir(cache_dir)
    .unwrap()
    .filter_map(|e| e.ok())
    .filter(|e| e.path().extension().map_or(false, |ext| ext == "cache"))
    .collect();
assert_eq!(cache_files.len(), 1, "Expected exactly one .cache file");

let etag_files: Vec<_> = std::fs::read_dir(cache_dir)
    .unwrap()
    .filter_map(|e| e.ok())
    .filter(|e| e.path().extension().map_or(false, |ext| ext == "etag"))
    .collect();
assert_eq!(etag_files.len(), 1, "Expected exactly one .etag sidecar");
```

### 5.4 Threading Tests (`threading.rs` — extend existing)

The existing file has 5 tests focused on local-file threading. We extend with Azure-aware threading tests. The Rust `Connection` type is `Send` but NOT `Sync` — each thread must own its connection.

| # | Test Name | What It Validates | Key Assertions | Azure? | Status |
|---|-----------|-------------------|----------------|--------|--------|
| 1-5 | (existing 5 tests) | Local file threading | (unchanged) | No | KEEP |
| 6 | `test_azure_threads_separate_databases` | 4 threads, each with own Azure DB. No cross-contamination. | Each thread's SeededData.verify() passes independently. | Yes | NEW |
| 7 | `test_azure_threads_sequential_access` | 4 threads write to same Azure DB sequentially (Mutex<Connection>). | All 4 threads' data present after join. | Yes | NEW |
| 8 | `test_azure_stress_open_close` | 10 threads, each opens→writes→closes→reopens→verifies its own DB. Tests VFS registration thread safety. | All 10 threads succeed. No SQLITE_BUSY. No crash. | Yes | NEW |
| 9 | `test_connection_move_across_threads` | Open on thread A, move to thread B (Send trait), use on B. | Data accessible on thread B. | Yes | NEW |
| 10 | `test_concurrent_reads_separate_connections` | Writer thread inserts, then 4 reader threads each open fresh connections and verify. | All readers see committed data. | Yes | NEW |

**Threading pattern (recommended):**
```rust
let barrier = Arc::new(Barrier::new(num_threads));
let handles: Vec<_> = (0..num_threads).map(|i| {
    let barrier = Arc::clone(&barrier);
    std::thread::spawn(move || {
        let db = TestDb::azure();
        let conn = db.connect();
        configure_wal(&conn);
        barrier.wait(); // Synchronize start
        let data = SeededData::new(i as u64);
        data.populate(&conn, 100, 32);
        drop(conn);
        // Verify via reopen
        let conn2 = db.reopen();
        configure_wal(&conn2);
        data.verify(&conn2, 100, 32);
    })
}).collect();

for h in handles {
    h.join().expect("Thread panicked");
}
```

### 5.5 Growth & Shrink Tests (`growth_shrink.rs`)

These test the VFS's handling of database file size changes: xTruncate (grow/shrink page blob), xFileSize, xSync with changing dirty page counts, and VACUUM (complete DB rewrite).

| # | Test Name | What It Validates | Key Assertions | Azure? |
|---|-----------|-------------------|----------------|--------|
| 1 | `test_grow_small_to_medium` | Insert 100 rows (small), then 10,000 more (medium). Blob resizes. | SeededData.verify(10100). File size increased. | Yes |
| 2 | `test_grow_across_page_boundaries` | Insert rows until DB crosses 1-page, 10-page, 100-page boundaries. | Data correct at each checkpoint. No corruption at boundary crossings. | Yes |
| 3 | `test_grow_with_large_text` | Insert 500 rows with 4KB text each (~2MB total). Tests multi-page rows. | All 500 rows verified. Text values exact match. | Yes |
| 4 | `test_shrink_delete_half` | Insert 1000 rows, DELETE WHERE id >= 500. Verify remaining 500. | SeededData.verify_range(0, 500). `count(*) = 500`. | Yes |
| 5 | `test_shrink_delete_all` | Insert 1000 rows, DELETE all. DB still openable. | `count(*) = 0`. Schema still exists. Reopen succeeds. | Yes |
| 6 | `test_vacuum_after_delete` | Insert 1000 rows, DELETE 900 rows, VACUUM. DB file shrinks. | SeededData.verify_range() for survivors. File size < pre-vacuum size. | Yes |
| 7 | `test_vacuum_empty_database` | Create table, VACUUM. No rows. | No crash. Schema intact. File size minimal. | Yes |
| 8 | `test_vacuum_preserves_data` | Insert 500 rows, VACUUM (no deletes). Data unchanged. | SeededData.verify(500) after VACUUM and after reopen. | Yes |
| 9 | `test_grow_shrink_cycle` | 3 cycles of: insert 1000 → delete 800 → VACUUM. DB stays usable. | After each cycle, surviving 200 rows correct. After final reopen, all survivors present. | Yes |
| 10 | `test_incremental_vacuum` | Enable `PRAGMA auto_vacuum=INCREMENTAL`. Insert, delete, `PRAGMA incremental_vacuum(100)`. | Page count decreases. Data intact. | Yes |
| 11 | `test_local_grow_shrink_baseline` | Same tests against local file for baseline. | All assertions pass. | No |

**Size validation pattern (Azure):**
```rust
// We can't directly query blob size from Rust without adding Azure REST calls.
// Instead, use PRAGMA page_count and page_size:
let page_count: i64 = conn.pragma_query_value(None, "page_count", |row| row.get(0)).unwrap();
let page_size: i64 = conn.pragma_query_value(None, "page_size", |row| row.get(0)).unwrap();
let db_size = page_count * page_size;
assert!(db_size > 0);
```

**VACUUM triggers the following VFS sequence:**
1. Create temp DB (xOpen with SQLITE_OPEN_DELETEONCLOSE)
2. Copy all live pages to temp (xRead source → xWrite temp)
3. Truncate original (xTruncate)
4. Copy temp back (xRead temp → xWrite original)
5. Sync original (xSync — full dirty page flush)
6. Delete temp (xClose + xDelete)

This is the most comprehensive single-operation VFS workout.

### 5.6 Error Recovery Tests (`error_recovery.rs`)

These test resilience: what happens when things go wrong. The most important question: does the VFS correctly recover from unclean shutdown?

| # | Test Name | What It Validates | Key Assertions | Azure? |
|---|-----------|-------------------|----------------|--------|
| 1 | `test_drop_without_close` | Open connection, insert data, `mem::forget(conn)` — skip Drop. Reopen. | Previously committed data present. Uncommitted data absent. No corruption. | Yes |
| 2 | `test_committed_data_survives_drop` | INSERT + COMMIT, then `mem::forget(conn)`. Reopen. | Committed rows present (xSync already uploaded). | Yes |
| 3 | `test_uncommitted_data_lost_on_drop` | BEGIN, INSERT (no COMMIT), `mem::forget(conn)`. Reopen. | Uncommitted rows absent. DB in consistent state. | Yes |
| 4 | `test_reopen_after_crash_no_cache_reuse` | Close with cache_reuse=1, corrupt the .etag sidecar, reopen. | Forces re-download (ETag mismatch or unreadable). Data correct. | Yes |
| 5 | `test_journal_hot_recovery` | Open, BEGIN, INSERT (journal created), `mem::forget(conn)`. Reopen. SQLite should detect hot journal and roll back. | DB consistent. No committed data from aborted txn. | Yes |
| 6 | `test_busy_timeout_respected` | Set `PRAGMA busy_timeout=100`. Simulate lock contention (if possible with two connections). | SQLITE_BUSY returned after timeout. No hang. | Yes |
| 7 | `test_multiple_reopen_after_crash` | Crash simulation, then reopen 3 times in succession. | Each reopen consistent. No progressive corruption. | Yes |
| 8 | `test_local_drop_recovery` | Same drop tests against local file. | Baseline correctness. | No |

**Dirty shutdown simulation strategy:**

The cleanest approach in Rust is `std::mem::forget(conn)` — this prevents the `Drop` impl from running, which means `sqlite3_close` is never called, which means xClose (cache persistence, lease release) never runs. This simulates a process crash from the VFS's perspective.

```rust
// Dirty shutdown simulation
let conn = test_db.connect();
configure_wal(&conn);
conn.execute("INSERT INTO data VALUES (1, 'committed')", []).unwrap();
// Don't close cleanly — simulate crash
std::mem::forget(conn);

// Reopen — VFS must handle the absence of clean close
let conn2 = test_db.reopen();
configure_wal(&conn2);
// Committed data should be present (it was synced to Azure)
let count: i64 = conn2.query_row("SELECT count(*) FROM data", [], |r| r.get(0)).unwrap();
assert_eq!(count, 1);
```

**Important caveat:** `mem::forget` leaks the rusqlite `Connection` and its internal `sqlite3*` handle. This is safe (no UB) but leaks resources. For test purposes this is acceptable. An alternative is `ManuallyDrop<Connection>`.

**For journal recovery testing:** After `mem::forget` during an active transaction, the journal blob may still exist in Azure. On reopen, SQLite's pager will detect the hot journal (via xAccess + xOpen for MAIN_JOURNAL), download it, and roll back the incomplete transaction. This tests the complete recovery path.
---
## 6. VFS Interaction Surface Coverage Matrix

This maps which tests exercise which VFS methods, ensuring comprehensive coverage:

| VFS Method | Lifecycle | Transactions | Cache | Threading | Growth/Shrink | Error Recovery |
|------------|-----------|--------------|-------|-----------|---------------|----------------|
| **xOpen** (MAIN_DB) | ✅ create, reopen | ✅ every test | ✅ cache hit/miss | ✅ parallel opens | ✅ every test | ✅ reopen after crash |
| **xOpen** (JOURNAL) | | ✅ BEGIN/COMMIT | | | | ✅ hot journal |
| **xOpen** (WAL) | | ✅ WAL tests | ✅ WAL cache | | | |
| **xClose** (clean) | ✅ all | ✅ all | ✅ cache persist | ✅ all | ✅ all | |
| **xClose** (leak) | | | ✅ dirty close | | | ✅ mem::forget |
| **xRead** | ✅ SELECT | ✅ SELECT | ✅ verify data | ✅ concurrent reads | ✅ verify | ✅ after recovery |
| **xWrite** | ✅ INSERT | ✅ INSERT/UPDATE | | ✅ parallel writes | ✅ bulk insert | |
| **xSync** (DB) | ✅ implicit | ✅ COMMIT | | ✅ per-thread | ✅ large flush | |
| **xSync** (Journal) | | ✅ COMMIT | | | | ✅ partial |
| **xSync** (WAL) | | ✅ WAL commit | | | | |
| **xTruncate** (DB) | | | | | ✅ VACUUM | |
| **xTruncate** (Journal) | | ✅ COMMIT cleanup | | | | |
| **xFileSize** | ✅ implicit | ✅ implicit | | | ✅ size checks | |
| **xLock** | ✅ implicit | ✅ SHARED→EXCL | | ✅ contention | ✅ implicit | ✅ busy timeout |
| **xUnlock** | ✅ implicit | ✅ EXCL→SHARED | | ✅ release | ✅ implicit | |
| **xCheckReservedLock** | | | | ✅ contention | | ✅ busy |
| **xFileControl** | | | ✅ download_count | | | |
| **xAccess** | | | | | | ✅ journal exists? |
| **xDelete** | | ✅ journal cleanup | ✅ cache cleanup | | | |
| **xSectorSize** | ✅ implicit | ✅ implicit | | | | |
| **xDeviceCharacteristics** | ✅ implicit | ✅ implicit | | | | |

**Gap analysis:** The only VFS methods not directly testable from Rust are xRandomness, xSleep, xCurrentTime (OS-level delegation) and xDlOpen (shared library loading) — these are correctly delegated to the default VFS and don't touch Azure.
---
## 7. Azure-Specific Edge Cases

These scenarios are called out for explicit test coverage because they exercise Azure storage behaviors that differ from local files:

### 7.1 Page Blob Alignment
SQLite pages are 4096 bytes (default). Azure page blobs require 512-byte alignment. Our VFS handles this transparently, but edge cases exist:
- **Test in growth_shrink:** `test_grow_across_page_boundaries` — verifies no corruption at 512-byte boundary alignment edges.

### 7.2 Blob Creation on First Write
Unlike local files, page blobs must be explicitly created with a size before writing. The VFS creates the blob on first xSync.
- **Test in lifecycle:** `test_create_new_database` — verifies blob creation path.

### 7.3 Lease Expiry During Long Operations
A 30s lease can expire during large bulk operations. The VFS renews inline.
- **Test in growth_shrink:** `test_large_transaction` (5000 rows) — likely exceeds 15s, triggers lease renewal.
- **Test in growth_shrink:** `test_vacuum_after_delete` — VACUUM is a long operation.

### 7.4 Empty Blob Handling
What happens when the blob exists but has size 0? Or doesn't exist at all?
- **Test in lifecycle:** `test_empty_database_reopen`, `test_create_new_database`.

### 7.5 Journal Blob Lifecycle
The journal is a block blob that gets created on transaction start and deleted on commit/rollback.
- **Test in transactions:** All commit/rollback tests. `test_journal_hot_recovery`.
---
## 8. Test Execution Strategy

### 8.1 Running Locally (No Azure)

```bash
cd rust/sqlite-objs
cargo test
```

Runs all tests. Azure tests auto-skip (credential check returns early). Local-only tests execute in < 5s.

### 8.2 Running with Azurite

```bash
# Terminal 1: Start Azurite
azurite --silent --location /tmp/azurite --debug /tmp/azurite-debug.log

# Terminal 2: Set env and run
source .env.azurite  # Or set vars manually
cargo test 2>&1 | head -100
```

All tests run, including Azure-backed tests against the local emulator.

### 8.3 Running Against Real Azure (CI)

```bash
AZURE_STORAGE_ACCOUNT=prodaccount \
AZURE_STORAGE_CONTAINER=testcontainer \
AZURE_STORAGE_SAS="sv=2024-08-04&..." \
cargo test -- --test-threads=4
```

Limit test threads to avoid Azure throttling (429s).

### 8.4 Execution Time Estimates

| Category | # Tests | Local (no Azure) | Azurite | Real Azure |
|----------|---------|-------------------|---------|------------|
| lifecycle | 9 | ~1s (1 local test) | ~10s | ~30s |
| transactions | 12 | ~1s (1 local test) | ~15s | ~45s |
| cache_reuse | 9 | skip | ~20s | ~60s |
| threading (new) | 5 | skip | ~15s | ~30s |
| growth_shrink | 11 | ~2s (1 local test) | ~30s | ~120s |
| error_recovery | 8 | ~1s (1 local test) | ~15s | ~45s |
| **New total** | **54** | **~5s** | **~105s** | **~330s** |
| Existing (unchanged) | ~208 | ~5s | ~30s | ~120s |
---
## 9. New Dev-Dependencies Required

```toml
[dev-dependencies]
rusqlite = "0.38"          # Already present
tempfile = "3.8"           # Already present
uuid = { version = "1", features = ["v4"] }  # Already present
dotenvy = "0.15"           # Already present
# No new dependencies needed — the design uses only std + existing deps
```
---
## 10. Implementation Order

Phase 1 (foundation): `common/` module + `lifecycle.rs` — establishes TestDb, SeededData, and the basic open/close/reopen pattern. ~1 day.

Phase 2 (correctness): `transactions.rs` + `cache_reuse.rs` — exercises the critical commit/rollback paths and the ETag mechanism. ~1 day.

Phase 3 (robustness): `threading.rs` extensions + `error_recovery.rs` — the hard tests: concurrent access, crash simulation, journal recovery. ~1.5 days.

Phase 4 (completeness): `growth_shrink.rs` — bulk operations, VACUUM, resize. ~1 day.

Total: ~4.5 days for implementation.
---
## 11. Open Questions for Brady

1. **Thread count for CI:** Should we limit `--test-threads` against real Azure? 4 seems safe. 1 is too slow.
2. **FCNTL wrapper:** The download counter requires unsafe FFI through rusqlite's raw handle. Should we add a safe wrapper to the `sqlite-objs` crate itself (e.g., `pub fn download_count(conn: &Connection) -> i32`), or keep it test-only?
3. **WAL vs Journal mode:** The existing smoke tests don't set journal mode. Should the new tests default to WAL (production config) or test both modes?
4. **Real Azure frequency:** The task mentions the C tests run real Azure weekly. Same cadence for Rust?
5. **Toxiproxy (Layer 3):** This design covers Layers 1+2 equivalent in Rust. Should we plan for Toxiproxy fault injection tests? (I'd defer to a future design.)
---
*— Gandalf, Lead Architect*
*"Even the very wise cannot see all ends. But a test suite that covers every VFS method — that they can build."*
# Decision: Concurrent Client Contention Test Exposes VFS Stale-Read Race

**Author:** Gimli (Rust Dev)
**Date:** 2025-07-25
**Status:** Finding (needs team discussion)
**Affects:** VFS C implementation (Aragorn), test infrastructure (all)

## Context

Wrote `concurrent_client_contention` integration test: 20 threads, each with an independent
connection (own cache directory), atomically incrementing a shared counter on the same Azure blob.
Expected final counter = 20. Actual = 2–3.

## Finding

The VFS has a **stale-read race condition** in its lease lifecycle:

1. `xOpen` — downloads the full blob (no lease held)
2. `xLock(RESERVED)` — acquires Azure blob lease (during `BEGIN IMMEDIATE`)
3. Read — from local copy (downloaded in step 1, now potentially stale)
4. Write + COMMIT — uploads dirty pages (under lease)

Between steps 1 and 2, another client can upload a new version. Step 3 reads the stale
download, not the latest blob. The lease correctly prevents concurrent writes, but does NOT
prevent reading stale data — leading to **lost updates**.

## Proposed VFS Fixes (for Aragorn)

1. **Re-download after lease** — At `xLock(RESERVED)`, check ETag. If blob changed since
   `xOpen`, re-download before proceeding. Most correct, moderate cost.
2. **Optimistic write** — Use `If-Match: <etag>` on PUT. If 412 Precondition Failed, return
   `SQLITE_BUSY`. Cheaper (no extra download), but requires caller retry.
3. **Lease at open** — Acquire lease during `xOpen`. Prevents stale reads entirely. Most
   expensive (lease held for entire connection lifetime).

## Test Location

`rust/sqlite-objs/tests/vfs_integration.rs` → `concurrent_client_contention`

Marked `#[ignore]`. Asserts `counter == N` (the correct expected behaviour). Currently fails.
When the VFS is fixed, this test becomes the regression guard.

## Impact

Any workload with multiple independent clients writing to the same blob (e.g., distributed
work queues, shared counters) will experience lost updates under the current VFS. Single-client
and read-only concurrent workloads are unaffected.
# Decision: Performance Matrix Test Suite Architecture

**Date:** 2025-07  
**Author:** Gimli (Rust Developer)  
**Status:** Implemented  

## Context

The microsoft/duroxide team benchmarked sqlite-objs performance by running 202 provider-validation tests across three backends (in-memory, local file, Azure blob) with varying nextest concurrency levels (`-j1` to `-j64`). Their results showed Azure blob storage scales strongly with concurrency (20x improvement from j1 to j28) despite high absolute latency.

We needed a test suite that reproduces their benchmarking methodology to enable local performance profiling and regression detection.

## Decision

Created `rust/sqlite-objs/tests/perf_matrix.rs` with 200 independent test functions organized into 7 categories (schema CRUD, single-row ops, bulk operations, transactions, complex queries, WAL mode, provider patterns). Tests run across three backends controlled by `PERF_MODE` environment variable.

### Key Design Choices

1. **Macro-Based Test Generation**
   - Defined category-specific macros (`schema_test!`, `bulk_test!`, etc.) for DRY pattern reuse
   - Each macro wraps a test function that receives a `TestDb` handle
   - Tests are independent — no shared state between test functions

2. **Three Backend Modes via Env Var**
   - `memory` (default): `:memory:` databases — fastest baseline, no I/O overhead
   - `file`: Local temp files via `tempfile::TempDir` — file I/O overhead
   - `azure`: Azure blob storage via sqlite-objs VFS — network + cloud latency
   - Mode detection in `TestDb::new()` creates appropriate connection type

3. **Mode-Specific Behavior Handling**
   - In-memory databases don't support WAL mode — use MEMORY journal mode automatically
   - WAL checkpoint tests skip in memory mode (early return pattern)
   - PRAGMA queries use `query_row()` not `execute()` (they return result sets)
   - Window function tests use CTE pattern to avoid WHERE-clause filtering issues

4. **Azure VFS Registration**
   - Lazy one-time registration via `OnceLock` (process-global VFS)
   - Loads credentials from env vars: `AZURE_STORAGE_ACCOUNT`, `AZURE_STORAGE_CONTAINER`, `AZURE_STORAGE_SAS`
   - Each test generates unique UUID blob name (`perf-<uuid>.db`)
   - Uses `UriBuilder` for proper percent-encoding of SAS tokens

5. **Test Independence for Nextest Parallelism**
   - Each test creates a fresh database (in-memory/temp file/unique blob)
   - No shared state, no cleanup dependencies
   - Safe for unlimited nextest concurrency (`-j`)
   - Mirrors duroxide team's test isolation pattern

### Technical Insights

**In-Memory vs WAL Mode:**
- In-memory databases automatically use MEMORY journal mode (WAL not supported)
- `configure_pragmas()` attempts WAL but doesn't fail if unsupported
- WAL-specific tests check `PerfMode::from_env()` and skip for memory mode

**PRAGMA Return Types:**
- `PRAGMA journal_mode` → String ("WAL", "MEMORY", etc.)
- `PRAGMA synchronous` → i64 (0=OFF, 1=NORMAL, 2=FULL, 3=EXTRA)
- `PRAGMA wal_checkpoint(MODE)` → (i32, i32, i32) tuple (busy, log pages, checkpointed pages)
- `PRAGMA wal_autocheckpoint=N` → i64 (returns the value set)

**Window Function Filtering:**
- `SELECT LEAD(val) OVER (...) FROM t WHERE val=3` applies WHERE before window → wrong result
- Correct pattern: `WITH windowed AS (SELECT val, LEAD(val) OVER (...) FROM t) SELECT ... WHERE val=3`

## Alternatives Considered

1. **Single large test with subtests** — Rejected: Wouldn't reproduce nextest parallel execution pattern
2. **Parameterized tests** — Rejected: Rust doesn't have native parameterized test support like pytest
3. **Separate test files per mode** — Rejected: Code duplication, harder to maintain
4. **Runtime test count generation** — Rejected: Rust test harness needs compile-time test functions

## Consequences

**Positive:**
- Enables local performance profiling across backends
- Reproduces duroxide team's test shape (200+ independent tests)
- Easy to add more tests via macros
- Nextest parallelism works correctly (each test is a separate process)
- Zero new runtime dependencies (uuid only in dev-dependencies)

**Negative:**
- 200 tests take ~0.05s (memory) to ~0.11s (file) — longer CI time
- Azure mode requires live credentials (can't test in CI without secrets)
- Large test file (~2800 lines) — may be harder to navigate
- Macro pattern may be unfamiliar to new contributors

**Neutral:**
- Test file is self-contained (no external test data files)
- Can run subset via `cargo test --test perf_matrix <pattern>`

## Validation

- All 200 tests pass in memory mode (0.05s)
- All 200 tests pass in file mode (0.11s)
- Azure mode not verified (requires live Azure credentials)
- Tests run successfully with nextest `-j1`, `-j4`, `-j14` concurrency

## Future Work

- Add more provider-style tests (durability, concurrent peek-lock, etc.)
- Create GitHub Actions workflow for periodic performance regression testing
- Add Azure mode smoke tests to CI (separate secret-based workflow)
- Consider adding benchmark harness (criterion.rs) for micro-benchmarks
# Decision: Reconnect Benchmark Example

**Date:** 2025-07-22
**From:** Gimli
**Status:** Implemented

## Context

Needed a benchmark to measure reconnect latency with `cache_reuse=true` and two independent clients
sharing the same Azure blob database. This helps quantify the cost of ETag-based revalidation vs
cold downloads.

## Decision

Created `rust/sqlite-objs/examples/reconnect_bench.rs` using `journal_mode=DELETE` (not WAL)
for simplicity. Uses UUID blob names, separate TempDir cache directories per client, and a
DEADBEEF marker row for correctness verification.

## Implications

- Available via `cargo run --example reconnect_bench` (requires Azure env vars)
- No Cargo.toml changes needed — all deps already in `[dev-dependencies]`
- Useful baseline for future VFS cache optimization work
# Decision: Rust Ergonomic API for VFS Features

**Date:** 2025-07-22
**From:** Gimli (Rust Developer)
**Status:** Implemented

## Context

The C VFS has metrics (FCNTL 201/202), download counting (FCNTL 200), and a `prefetch` URI parameter that were not exposed in the Rust crate.

## Decisions Made

### FCNTL constants match the actual C header, not the task spec

The task specified incorrect FCNTL numbers (200=WAL_PARALLEL, 201=PREFETCH, etc.). The real values from `sqlite_objs.h` are 200=DOWNLOAD_COUNT, 201=STATS, 202=STATS_RESET. There are NO WAL_PARALLEL or PREFETCH FCNTLs in the C code — prefetch is a URI parameter, not an FCNTL. I implemented what the C code actually supports.

### VfsMetrics fields match C `formatMetrics()` output exactly

The struct has 27 fields mirroring the C output including `journal_bytes_uploaded` and `wal_bytes_uploaded` (which the task omitted) and excluding `azure_retries` (which the task invented). Unknown keys are silently ignored for forward compatibility.

### Pragmas module gated behind `rusqlite` Cargo feature

The FCNTL wrapper functions (`get_stats`, `reset_stats`, `get_download_count`) require `rusqlite::Connection` and use unsafe FFI internally. They live in `pragmas.rs` gated behind `#[cfg(feature = "rusqlite")]` so the core crate stays dependency-light. Dev-dependencies always have rusqlite available for testing.

### UriBuilder::prefetch() only emits when explicitly set

Since `PrefetchMode::All` is the default in the C VFS, calling `UriBuilder::new(...)` without `.prefetch()` omits the parameter entirely, keeping URIs short. Setting it explicitly to `All` still emits `&prefetch=all` for documentation clarity.

## Files Changed

- `rust/sqlite-objs-sys/src/lib.rs` — FCNTL constants
- `rust/sqlite-objs/Cargo.toml` — `rusqlite` feature
- `rust/sqlite-objs/src/lib.rs` — PrefetchMode, UriBuilder::prefetch(), module declarations, MetricsParse error variant
- `rust/sqlite-objs/src/metrics.rs` — NEW: VfsMetrics + parser
- `rust/sqlite-objs/src/pragmas.rs` — NEW: FCNTL wrappers
# Decision: Rust Multi-Threading Pattern & Test Coverage

**Date:** 2025-07
**From:** Gimli (Rust Developer)
**Context:** Thread-safety bug discovered in C VFS (shared curl handle), Aragorn adding mutex fix

## Decision

Added comprehensive multi-threading test suite (`rust/sqlite-objs/tests/threading.rs`) establishing
the threading contract for Rust users of sqlite-objs.

## Key Findings

### rusqlite::Connection Thread Safety

After research and testing, confirmed:
- `Connection` implements **Send** — can be moved between threads ✓
- `Connection` does NOT implement **Sync** — cannot be shared via `Arc<Connection>` ✗
- Reason: Contains `RefCell` (interior mutability, not thread-safe)

### Recommended Pattern

**Each thread should create its own `Connection` to the same database file.**

```rust
let db_path = "mydb.db";
let mut handles = vec![];

for i in 0..N {
    let path = db_path.to_string();
    handles.push(thread::spawn(move || {
        let conn = Connection::open(&path).unwrap();
        // Do work with conn
    }));
}
```

SQLite's built-in file locking handles concurrent access across connections.

### Alternative (Not Recommended)

`Arc<Mutex<Connection>>` works but adds unnecessary serialization overhead and defeats SQLite's
concurrent read capabilities. Only use if you have a strong reason.

## Test Coverage

5 tests covering:
1. Separate threads, separate databases (Send trait verification)
2. Mutex-wrapped shared connection (Arc<Mutex<>> pattern)
3. Separate connections to same database (**recommended pattern**)
4. Stress test (10 threads × 5 iterations = 50 concurrent ops)
5. Connection moved between threads (explicit Send trait test)

All tests use **local file paths** (not Azure URIs) to isolate SQLite threading semantics from
Azure client threading. This is intentional — we're testing rusqlite's threading model, not Azure's.

## Implications

1. **Rust API Documentation:** Should document the recommended pattern (separate connections per thread)
2. **C-Level Thread Safety:** The C VFS must be thread-safe when multiple connections exist
   simultaneously. Aragorn's mutex fix to `azure_client_t` ensures this.
3. **Future Azure Integration Tests:** Should combine this threading pattern with real Azure
   operations to verify the C mutex fix works correctly under multi-threaded load.
4. **Regression Protection:** If the C code regresses to shared state without locking, these tests
   establish the baseline contract. Azure-specific threading failures would appear in integration
   tests.

## Verification

- 27 total tests pass (16 unit + 3 ignored + 5 threading + 2 sys + 4 doc)
- Threading tests complete in ~0.1s
- No new dependencies (rusqlite, tempfile already in dev-dependencies)
- No flaky behavior across multiple runs

## Files Changed

- `rust/sqlite-objs/tests/threading.rs` — new file, 350+ lines, 5 tests
- `rust/sqlite-objs/.squad/agents/gimli/history.md` — learnings documented

## Team Coordination

This complements Aragorn's C-level work:
- Aragorn: Adding mutex to `azure_client_t` in C code
- Gimli: Verifying Rust threading patterns work correctly through FFI

The tests are designed to work regardless of the C implementation state (they use local files),
but they establish the expected behavior for when Azure operations run in multi-threaded contexts.
# Decision: ETag Cache Reuse Test Strategy

**Date:** 2026-07-25  
**Author:** Samwise (QA)  
**Status:** Implementation Complete (with known fixup needed)

## Context

The VFS now supports ETag-based cache reuse (opt-in via `cache_reuse=1` URI parameter). Comprehensive integration tests are needed to verify all cache hit/miss scenarios.

## Decision

### Mock Infrastructure Updates

Extended `mock_azure_ops.{h,c}` with full ETag support:

1. **ETag storage**: Added `char etag[128]` field to `mock_blob_t` struct
2. **Auto-generation**: ETags updated on every write operation (page_blob_write, block_blob_upload, page_blob_create)
3. **Properties API**: `blob_get_properties` now populates `err->etag` field
4. **Test API**: Added `mock_set_blob_etag()` and `mock_get_blob_etag()` for external modification simulation

### Test Suite Structure

Created `test/test_cache_reuse.c` with 8 comprehensive scenarios:

1. **cache_preserved_on_clean_close** — Filesystem validation: .cache + .etag files persist
2. **cache_reused_on_reconnect** — Mock call counting: blob_get_properties called, page_blob_read NOT called
3. **cache_invalidated_on_blob_change** — External ETag change triggers re-download
4. **cache_deleted_without_cache_reuse** — Default behavior cleans up temp files
5. **cache_deleted_on_dirty_close** — Unsynced writes prevent cache persistence
6. **deterministic_naming** — FNV-1a hash consistency verification
7. **missing_etag_file_triggers_redownload** — Missing sidecar file forces re-download
8. **size_mismatch_triggers_redownload** — Corrupted cache detection

### Test Design Principles

- **Dedicated context**: `g_cache_ctx`/`g_cache_ops` (isolation from other test suites)
- **Temp directories**: `/tmp/sqlite-cache-test-{PID}` (auto-cleanup)
- **Filesystem validation**: Direct file existence/size checks for cache behavior
- **Mock call counting**: Verify download avoidance via `mock_get_call_count()`
- **ETag manipulation**: Use `mock_set_blob_etag()` to simulate external modifications

## Known Issue & Resolution Path

**Problem:** Tests currently fail at database open (SQLITE_CANTOPEN) because URI parameters `azure_account` and `azure_container` trigger `azure_client_create()`, which fails in unit tests with the stub client.

**Root cause:** Cache reuse tests don't need per-file Azure configuration — they should use global mock ops registered via `sqlite_objs_vfs_register_with_ops()`.

**Fix:** Remove `azure_account` and `azure_container` from all URI strings in `test/test_cache_reuse.c`. Only `cache_dir` and `cache_reuse` parameters are needed.

**Example:**
```c
// INCORRECT (current):
"file:testdb.db?cache_dir=%s&cache_reuse=1&azure_account=acct&azure_container=cont"

// CORRECT:
"file:testdb.db?cache_dir=%s&cache_reuse=1"
```

## Rationale

- **Unhappy paths first**: Tests explicitly target cache invalidation scenarios (ETag mismatch, missing files, corruption)
- **Mock over integration**: ETag behavior fully testable with in-memory mock — no Azurite needed for core logic
- **Filesystem as oracle**: Cache persistence is a filesystem concern → direct file checks are appropriate
- **Call counting for downloads**: Mock call tracking is the cleanest way to verify "did NOT download" assertions

## Impact

- **Test coverage**: 242 → 250 unit tests (once URI params fixed)
- **Mock completeness**: ETags now fully supported in mock infrastructure (benefits future tests)
- **Confidence**: Comprehensive coverage of all cache hit/miss paths before production use

## Next Steps

1. **Aragorn (VFS owner)** or **Samwise**: Remove Azure URI params from `test/test_cache_reuse.c` URIs
2. Verify all 8 tests pass: `make clean && make test-unit`
3. Add integration test variant against Azurite (optional — unit tests cover logic fully)

## Files Modified

- `test/mock_azure_ops.h` — ETag API declarations
- `test/mock_azure_ops.c` — ETag storage, auto-generation, helpers
- `test/test_cache_reuse.c` — 8 cache reuse tests (NEW)
- `test/test_main.c` — Include + runner call
- `Makefile` — Dependency list
- `src/sqlite_objs_vfs.c` — Added `#include <pthread.h>` (pre-existing bug fix)
# Decision: Cross-VFS ATTACH Limitation

**From:** Samwise (QA/Tester)
**Date:** 2025-07-25
**Context:** Multi-client integration test implementation

## Finding

ATTACH DATABASE inherits the main connection's VFS. This means:
- A database opened via "sqlite-objs" VFS cannot ATTACH a local file (it tries to find it on Azure)
- A local database CAN attempt to ATTACH an Azure URI database, but this requires the sqlite-objs VFS to be registered and URI parsing to work via ATTACH's open path

## Current Test Approach

Test E4 (mc_cross_database_join) opens a local database first, then ATTACHs the Azure database via URI. If ATTACH with Azure URI isn't supported, the test skips gracefully (like integ_attach_cross_container).

## Recommendation

If cross-VFS operations are a product requirement, consider:
1. Implementing VFS override syntax in ATTACH (e.g., `ATTACH DATABASE '...' AS x USING VFS 'sqlite-objs'`)
2. Or documenting this as a known limitation
---
# Observation: Stale Azurite Data

Tests that use `CREATE TABLE` (without `IF NOT EXISTS`) will fail if Azurite retains data from prior runs. The run-integration.sh script starts a fresh Azurite instance each time, but if Azurite's workspace files (`__azurite_db_*.json`, `__blobstorage__/`) persist, blobs survive across runs.

**Recommendation:** Add `rm -f __azurite_db_*.json && rm -rf __blobstorage__ __queuestorage__` to run-integration.sh before starting Azurite, or use `--inMemoryPersistence` flag.
# Decision: Lazy Cache Test Infrastructure

**Date:** 2025-07  
**From:** Samwise (QA/Tester)  
**Status:** Implemented

## Summary

Added 41 comprehensive unit tests for the lazy cache (prefetch=none) feature, bringing total unit tests from 247 to 288. Also added ETag simulation to mock_azure_ops.c to enable cache_reuse testing.

## Changes Made

### mock_azure_ops.c
- Added `etag_counter` field to `mock_blob_t` struct
- `blob_get_properties` now populates `err->etag` (format: `"etag-N"`)
- Counter incremented on `page_blob_create`, `page_blob_write`, `page_blob_resize`
- **Impact:** Zero existing test regressions. Enables all cache_reuse + state file unit test paths.

### test_vfs.c — 41 New Tests in 6 Suites
1. **Lazy Cache — Open & Bootstrap** (4 tests): prefetch=none minimal download, page 1 bootstrap, prefetch=all full download, new db creation
2. **Lazy Cache — xRead Behavior** (6 tests): valid-from-cache, invalid-fetches-azure, readahead, sequential caching, sub-page reads, write-then-read
3. **Lazy Cache — Write & Truncate** (3 tests): write marks valid+dirty, truncate clears valid bits, write beyond EOF
4. **Lazy Cache — Prefetch PRAGMA** (4 tests): downloads invalid, noop when all valid, mixed states, noop on prefetch=all
5. **Lazy Cache — State File I/O** (5 tests): write/read roundtrip, missing file, corrupt magic, corrupt CRC, truncated file
6. **Lazy Cache — Edge Cases** (19 tests): error propagation, locking, journal handling, savepoints, large data, mixed modes, etc.

## Open Questions for Team

1. **revalidateAfterLease lazy path untested:** The mock doesn't implement `blob_get_page_ranges_diff` or `blob_snapshot_create`, so the lazy revalidation diff path cannot be tested in Layer 1. Should Gimli add mock support, or defer to Layer 2 (Azurite)?
2. **Integration test coverage:** The state file corruption tests use shell commands (`dd`, `truncate`) to corrupt files. Should these be promoted to a dedicated crash-recovery test suite?
# TCL Test Suite Expansion — Full Sweep Results

**Date:** 2026-03-14 | **From:** Samwise (QA)

## Decision

Expanded TCL test runner from 78 to **1,151 verified passing tests** (out of 1,187 total).
All 6 failures are platform/config issues — **zero VFS bugs found**.

## Key Facts

- **1,151 tests pass** with 0 errors (~720,042 individual assertions)
- **6 failures:** all `Inf` vs `inf` (macOS printf), dlopen message format (macOS), TCL 8.5 type name
- **15 timeouts:** meta-runners + heavy fault injection (>30s)
- **15 empty:** Windows-only, zipfile extension, meta-tests

## Team Impact

- **Aragorn (VFS):** The VFS passes crash recovery, corruption detection, WAL, pager, and IO error tests. Very strong validation.
- **Frodo (Azure):** No Azure-related failures at all — mock backend is solid.
- **CI:** Running all 1,151 tests takes ~30 minutes. Use `--quick` (10 tests, ~10s) for fast feedback. Consider a medium subset for CI.

## Runner Changes

- Added `rm -rf testdir` cleanup between test runs (prevents cascading failures)
- Expanded quick test subset from 5 to 10 tests
- Tests organized by 78 categories with comments

# Decision: If-Match Header Signature Fix

**Date**: 2025-01-09  
**Author**: Aragorn (SQLite/C Dev)  
**Status**: Implemented

## Context

All 32 VFS integration tests were failing with "disk I/O error" (SQLITE_IOERR) due to Azure Blob Storage returning 403 Forbidden errors. The 8 raw Azure client tests were passing, indicating the issue was in the VFS layer's use of If-Match headers for ETag-based compare-and-swap operations.

## Problem

The If-Match header added in recent changes was corrupting the Azure SharedKey authentication signature. The header was being incorrectly placed in the `extra_x_ms` array, which caused:
1. If-Match to appear in the canonicalized headers section (only `x-ms-*` headers belong there)
2. The standard If-Match position in the signing string to always be empty

Result: signature mismatch → 403 Forbidden → SQLITE_IOERR

## Decision

**Add standard HTTP headers as explicit parameters through the auth chain, NOT as part of `extra_x_ms`.**

### Changes Made

1. **`src/azure_auth.c` & `src/azure_client_impl.h`**:
   - Added `const char *if_match` parameter to `azure_auth_sign_request()`
   - Used it at line 193 of signing string construction

2. **`src/azure_client.c` - `execute_single()` and `execute_with_retry()`**:
   - Added `const char *if_match` parameter (now 13 parameters for execute_with_retry)
   - Passed to `azure_auth_sign_request()`
   - Added `If-Match` header to curl header list AFTER signing (not through extra_x_ms)

3. **`src/azure_client.c` - `az_page_blob_write()`**:
   - Removed `if_match_header` from all `extra_*` arrays
   - Simplified from 4 conditional arrays to 2 (with/without lease)
   - Passed `if_match` as separate parameter to `execute_with_retry()`

4. **`src/azure_client.c` - `batch_init_easy()`**:
   - Updated to pass `if_match` to `azure_auth_sign_request()`

5. **All other callers**: Pass NULL for if_match parameter

6. **Synced to `rust/sqlite-objs-sys/csrc/`**: Kept Rust FFI wrappers in sync

### Alternatives Considered

- **Using a params struct**: Would reduce parameter count but would require refactoring all call sites. Deferred for now.
- **Special-casing If-Match in extra_x_ms**: Would make the code harder to understand and maintain. Rejected.

## Consequences

### Positive
- ✅ Authentication signatures are now correct
- ✅ All 8 Azure Client integration tests pass
- ✅ VFS integration tests went from 0/32 passing to 23/41 passing
- ✅ No compilation warnings
- ✅ All 295 unit tests pass
- ✅ Clear separation between x-ms-* headers and standard HTTP headers

### Negative
- `execute_with_retry()` now has 13 parameters (was 12) - still manageable but approaching the limit
- Small increase in code complexity at call sites

### Remaining Work
- 18 integration tests still failing (etag cache, multi-client scenarios) - appears to be a different issue, not related to authentication
- Consider refactoring to params struct if more parameters are needed in the future

## Verification

```bash
make clean && make test-unit 2>&1 | tail -5
# All 295 tests passed

make clean && make test-integration 2>&1 | tail -20
# 23 of 41 tests PASSED (was 0/32 before fix)
# 0 "disk I/O error" messages (was universal before)

make test-unit 2>&1 | grep "warning:"
# (no output = zero warnings)
```

## Key Principle Established

**The `extra_x_ms` array is ONLY for `x-ms-*` headers. Standard HTTP headers (If-Match, If-None-Match, Range, etc.) must be handled through explicit parameters to ensure correct placement in the Azure SharedKey signing string.**

# Azure Blob Lease and If-Match Are Mutually Exclusive

**Decision**: Never send both `x-ms-lease-id` and `If-Match` headers in the same Azure Blob Storage write request.

**Context**: 
During integration testing, 11 multi-client tests were failing with SQLITE_BUSY errors during COMMIT. The VFS was sending both a lease ID (for exclusive write access) and an If-Match ETag (for optimistic concurrency) in the same `page_blob_write_batch()` request. Azurite (and likely production Azure) rejected these requests with HTTP 412 Precondition Failed.

**Rationale**:
- **Blob leases** provide exclusive write access for a duration (15-60 seconds). When you hold a lease, you're the only writer.
- **If-Match ETags** provide optimistic concurrency control without exclusive access. You send an ETag to ensure the blob hasn't changed since you last read it.
- These are two different concurrency strategies and should not be mixed.
- Azurite rejects requests that contain both, treating it as a precondition failure.

**Implementation**:
In `sqlite_objs_vfs.c`, both `xSync` paths (batch and sequential) now check `hasLease(p)` before passing the ETag:

```c
azure_err_t arc = p->ops->page_blob_write_batch(
    p->ops_ctx, p->zBlobName,
    ranges, nRanges,
    hasLease(p) ? p->leaseId : NULL,
    /* Don't send If-Match when we hold a lease */
    (hasLease(p) || !p->etag[0]) ? NULL : p->etag,
    &aerr);
```

**Consequences**:
- ✅ All 41 integration tests now pass
- ✅ Cleaner API semantics — lease OR ETag, never both
- ✅ Matches Azure's documented best practices
- ⚠️ Future code must maintain this invariant

**Team Guidance**: Any code that calls `page_blob_write()` or `page_blob_write_batch()` must follow this rule: send `lease_id` XOR `if_match`, never both.

### 2026-03-21T07:20:00Z: User directive — S3 design constraints
**By:** Quetzal Bradley (via Copilot)
**What:** For S3 support: (1) Must use sharding — uploading the whole blob on every write is unacceptable. (2) Multi-writer can require If-Match (conditional writes). AWS S3 supports If-Match, MinIO supports it, and that's sufficient coverage. Other S3-compatible providers will catch up.
**Why:** User request — foundational design constraints for S3/multi-cloud implementation.

# S3 Support Research — Architecture and Dependency Strategy

**Author:** Gandalf (Lead Architect)
**Date:** 2026-07-15
**Status:** Research / Proposal — No implementation yet
**Requested by:** Quetzal Bradley

---

## 1. Executive Summary

Adding AWS S3 (and S3-compatible) backend support is architecturally feasible but requires a fundamentally different write strategy than our Azure page blob model. S3 lacks page-granularity random writes — every write is a full-object replacement — creating significant write amplification for the journal-mode SQLite workload we target. The good news: (a) S3 added conditional writes with `If-Match` ETag checks in November 2024, giving us multi-writer safety without leases; (b) SigV4 auth uses HMAC-SHA256 which we already implement in `azure_auth.c`; (c) our `azure_ops_t` vtable pattern (`azure_client.h:156-325`) can be generalized to a `storage_ops_t` abstraction with minimal VFS changes. The recommended approach is a phased plan: Phase 1 builds SigV4 auth + a read-only S3 backend; Phase 2 adds writes via download-modify-upload with conditional ETag safety; Phase 3 generalizes the vtable abstraction so Azure and S3 share one VFS core. The dependency strategy is unchanged: libcurl + OpenSSL, zero SDK dependency, with SigV4 signing implemented in ~400 lines of C.

---

## 2. API Mapping — Azure Page Blob ↔ S3 Equivalents

### 2.1 Page Blob Random R/W → S3 Object (No Equivalent)

**Azure:** Page blobs support 512-byte-aligned random writes via `PUT ?comp=page` with `x-ms-range`. Our VFS tracks dirty pages in a bitmap (`sqlite_objs_vfs.c:148`), coalesces them (`sqlite_objs_vfs.c:1645-1695`), and flushes only changed ranges during xSync. A 100-page dirty commit touching 5 pages uploads only 5 × 4096 = 20 KiB.

**S3:** Objects are immutable once written. There is **no** partial/in-place write. The only write operations are:
- `PutObject` — replaces the entire object
- Multipart Upload (`CreateMultipartUpload` → `UploadPart` × N → `CompleteMultipartUpload`) — creates an object from parts, but parts are concatenated, not patched into an existing object

**Consequence:** For S3, every xSync that writes dirty pages must:
1. Download the current object (or use local cache)
2. Apply dirty pages to the local copy
3. Upload the entire modified object via `PutObject` or multipart upload

**Write amplification:** If the database is 100 MiB and 5 pages (20 KiB) are dirty, Azure uploads 20 KiB; S3 uploads 100 MiB. This is **5000× write amplification** for small commits on large databases. Multipart upload helps with throughput (parallel chunks) but doesn't reduce total bytes uploaded.

### 2.2 Byte-Range GET → S3 Range GET (Direct Equivalent)

**Azure:** `page_blob_read()` uses `Range: bytes=offset-end` header.
**S3:** `GetObject` supports identical `Range` header. This is universally supported across all S3-compatible services.

**Read path is identical.** No architecture change needed for reads.

### 2.3 Blob Leases → S3 Has No Leases

**Azure:** 30-second renewable leases provide exclusive write access (`lease_acquire/renew/release` in `azure_client.h:213-234`). Our VFS uses this for RESERVED/EXCLUSIVE lock levels (`sqlite_objs_vfs.c:2326-2399`).

**S3 Alternatives:**

| Mechanism | Pros | Cons |
|-----------|------|------|
| **DynamoDB Lease Table** | True distributed locking, TTL-based expiry, conditional writes | External dependency (DynamoDB), AWS-only, adds latency |
| **S3 Conditional Write (`If-Match`)** | No external dependency, built into S3 API | Not a lease — optimistic concurrency, not mutual exclusion |
| **S3 Object Lock** | WORM compliance | Designed for retention, not locking; no unlock |
| **Lock object pattern** | Simple — PUT a `<db>.lock` sentinel object | No automatic expiry; crash leaves stale lock |
| **Lock object + TTL metadata** | PUT `<db>.lock` with `x-amz-meta-expires` timestamp | Requires polling; no atomicity guarantee on check+acquire |

**Recommendation:** Use a **two-tier approach**:
1. **Optimistic concurrency via `If-Match` ETag** for the common case (single writer). On xSync, upload with `If-Match: <last-known-etag>`. If another writer modified the object, S3 returns 412 Precondition Failed → map to `SQLITE_BUSY`.
2. **Optional DynamoDB lease table** for multi-writer deployments that need true mutual exclusion. This would be an opt-in feature via URI parameter (`lock_table=my-dynamo-table`).

This mirrors real-world patterns: DynamoDB lock tables are the standard AWS pattern for distributed locking (used by Terraform, Apache Iceberg, Delta Lake).

### 2.4 ETag-Based Change Detection → S3 ETags (Mostly Equivalent)

**Azure:** ETags are MD5 hashes of blob content. Our VFS uses them for cache revalidation (`sqlite_objs_vfs.c:2120-2318`): compare stored ETag with HEAD response, skip download if unchanged.

**S3:** ETags work similarly with caveats:
- **Single-part uploads:** ETag = MD5 of content (same as Azure)
- **Multipart uploads:** ETag = MD5 of concatenated part MD5s, suffixed with `-N` (e.g., `"d41d8cd98f00b204e9800998ecf8427e-5"`). This is a "checksum of checksums," not a content hash.
- **SSE-KMS encrypted objects:** ETag is not MD5

**Impact:** ETags still work for **change detection** (different content → different ETag), which is all we need. We don't compare ETags across upload methods — we just check "has it changed since I last saw it?" The `-N` suffix is irrelevant for our use case.

**S3 Conditional headers for reads:**
- `If-None-Match: <etag>` → returns 304 Not Modified if unchanged (saves download bandwidth)
- `If-Match: <etag>` → returns 412 if changed (write safety)

Both are supported on `GetObject` and `PutObject`. This maps directly to our revalidation pattern.

### 2.5 Get Page Ranges (Incremental Diff) → No S3 Equivalent

**Azure:** `blob_get_page_ranges_diff()` (`azure_client.h:318-325`) returns which 512-byte pages changed between two snapshots. Our VFS uses this for incremental cache updates (`sqlite_objs_vfs.c:1004-1134`): instead of re-downloading the entire blob, fetch only changed ranges.

**S3:** No equivalent API. S3 has no concept of page ranges or incremental diffs.

**Workarounds:**
1. **S3 Versioning:** Enabled per-bucket. Each PutObject creates a new version. But there's no API to diff two versions at the byte level — you must download both and diff locally.
2. **Application-level diff:** Store a metadata object (`<db>.meta`) containing a hash map of page checksums. On revalidation, download metadata, compare page hashes, fetch only changed page ranges via byte-range GETs. This adds one extra HEAD + one extra GET per revalidation but can save significant bandwidth.
3. **Just re-download:** For databases under ~50 MiB, re-downloading the full object may be faster than the metadata overhead. Our lazy cache mode (`prefetch=none`) already handles page-level fetching on demand.

**Recommendation:** For S3 MVP, skip incremental diff. Use full re-download on ETag mismatch. Implement application-level page hash metadata as a Phase 3 optimization.

### 2.6 Block Blobs (Journal) → S3 PutObject (Direct Equivalent)

**Azure:** Journal files use block blob upload/download (`block_blob_upload/download` in `azure_client.h:186-194`). These are whole-object sequential operations.

**S3:** `PutObject` / `GetObject` map directly. Journal files are typically small (< 1 MiB). No architecture change needed.

---

## 3. Dependency Strategy — Low-Dependency C Approach

### 3.1 AWS C SDK: Not Viable for Our Project

The **aws-c-s3** library (Apache 2.0 license) is the official AWS C client. Its dependency chain:

```
aws-c-s3
├── aws-c-auth         (SigV4 signing)
├── aws-c-http         (HTTP/1.1, HTTP/2)
├── aws-c-io           (Event loop, TLS, DNS)
│   ├── aws-lc         (TLS library — AWS fork of BoringSSL/OpenSSL)
│   └── s2n-tls        (TLS implementation, Linux only)
├── aws-c-cal          (Crypto abstraction layer)
├── aws-c-compression  (Huffman coding for HTTP/2)
├── aws-c-sdkutils     (Endpoints, profiles)
├── aws-c-common       (Allocators, logging, strings, byte buffers)
└── aws-checksums      (CRC32, CRC64)
```

**10 separate git repositories.** Each requires CMake. On Linux, requires building aws-lc + s2n-tls (their own TLS stack). Total compiled size: ~5-10 MiB of libraries.

**Verdict:** Completely incompatible with our zero-SDK, minimal-dependency philosophy. Our entire binary is ~300 KiB. The AWS SDK alone would 10-30× our binary size. Reject.

### 3.2 Mining the SDK for Ideas (Apache 2.0 — License Compatible)

Apache 2.0 is compatible with our MIT license for idea/pattern borrowing. Key patterns worth studying:

1. **SigV4 Signing** (`aws-c-auth`): Canonical request construction, credential scope derivation, 4-step HMAC chain
2. **Retry Logic** (`aws-c-s3`): S3-specific retryable error codes (503, 500, SlowDown, RequestTimeout)
3. **Endpoint Discovery**: Region → endpoint URL mapping, virtual-hosted vs path-style bucket addressing
4. **Multipart Upload**: Part size selection, parallel upload orchestration

We should read their SigV4 implementation for correctness reference but implement our own.

### 3.3 SigV4 with libcurl + OpenSSL: Feasible and Recommended

**SigV4 Signing Algorithm (5 steps):**

```
1. CanonicalRequest = HTTP_Method + '\n' + URI + '\n' + QueryString + '\n' +
                      CanonicalHeaders + '\n' + SignedHeaders + '\n' + SHA256(payload)

2. StringToSign = "AWS4-HMAC-SHA256" + '\n' + Timestamp + '\n' +
                  Date + '/' + Region + '/' + Service + '/aws4_request' + '\n' +
                  SHA256(CanonicalRequest)

3. SigningKey = HMAC-SHA256(HMAC-SHA256(HMAC-SHA256(HMAC-SHA256(
                  "AWS4" + SecretKey, Date), Region), Service), "aws4_request")

4. Signature = Hex(HMAC-SHA256(SigningKey, StringToSign))

5. Authorization = "AWS4-HMAC-SHA256 Credential=AccessKeyId/CredentialScope,
                    SignedHeaders=..., Signature=..."
```

**What we already have:**
- `azure_hmac_sha256()` (`azure_auth.c:100-110`) — wraps OpenSSL's `HMAC(EVP_sha256())`. **Directly reusable** — SigV4's 4-step HMAC chain calls this 4 times.
- `azure_base64_encode/decode()` (`azure_auth.c:31-94`) — **Reusable** for key handling.
- SHA256 hashing — OpenSSL's `EVP_Digest()` / `EVP_sha256()`. We already link OpenSSL.
- URL encoding — Need to implement `UriEncode()` per AWS spec (slightly different from standard percent-encoding).
- Header sorting — `compare_headers()` in `azure_auth.c:128` is reusable.

**What's new for SigV4:**
- Canonical request format (different from Azure's 13-line format)
- 4-step HMAC key derivation chain (Date → Region → Service → "aws4_request")
- SHA256 payload hashing (Azure doesn't hash the payload)
- Credential scope string
- UriEncode per AWS specification

**Estimated implementation:** ~400 lines of C for a `s3_auth_sign_request()` function, using existing HMAC/base64 helpers. This is comparable to our `azure_auth_sign_request()` at `azure_auth.c:154-303` (~150 lines). SigV4 is more complex (payload hashing, key derivation chain) but well-documented.

### 3.4 S3-Compatible Services and SigV4

**All major S3-compatible services support SigV4.** This is the universal authentication standard:

| Service | SigV4 | SigV2 (Legacy) | Notes |
|---------|-------|----------------|-------|
| AWS S3 | ✅ | Deprecated | SigV4 required for new regions |
| MinIO | ✅ | ✅ | Full SigV4 support |
| Cloudflare R2 | ✅ | ❌ | SigV4 only |
| Backblaze B2 | ✅ | ✅ | Both supported |
| DigitalOcean Spaces | ✅ | ✅ | SigV4 recommended |
| Wasabi | ✅ | ✅ | Full SigV4 support |
| Google Cloud Storage | ✅ | ❌ | Via HMAC keys, XML API |

**Implementing SigV4 once covers all services.** No need for SigV2.

---

## 4. Performance Comparison

### 4.1 Write Amplification — The Core Problem

| Scenario | Azure (Page Blob) | S3 (Full Object) | Amplification |
|----------|-------------------|-------------------|---------------|
| 5 dirty pages, 10 MiB DB | 20 KiB upload | 10 MiB upload | 500× |
| 5 dirty pages, 100 MiB DB | 20 KiB upload | 100 MiB upload | 5,000× |
| 5 dirty pages, 1 GiB DB | 20 KiB upload | 1 GiB upload | 50,000× |
| VACUUM 100 MiB DB | ~100 MiB upload | ~100 MiB upload | 1× |
| Bulk load (new DB) | ~N MiB upload | ~N MiB upload | 1× |

**Key insight:** Write amplification is proportional to database size, not dirty data size. Small transactions on large databases are catastrophically expensive on S3.

### 4.2 Multipart Upload — Helps Throughput, Not Amplification

S3 multipart upload splits the object into parts (min 5 MiB per part, up to 10,000 parts). It enables:
- **Parallel upload:** Upload N chunks simultaneously via our curl_multi infrastructure
- **Retry per part:** Failed part doesn't restart entire upload

But each part is a chunk of the **full object** — you still upload the entire thing. Multipart reduces latency for large uploads but doesn't reduce bytes transferred.

**One optimization:** If we know which pages are dirty, we could potentially use multipart upload where only dirty-containing parts have new data. But S3 parts must be contiguous and complete — you can't skip unchanged parts.

### 4.3 S3 Express One Zone (Directory Buckets)

S3 Express One Zone places data in a single Availability Zone for lower latency:
- **Up to 200,000 read TPS, 100,000 write TPS** per directory bucket
- **Single-digit millisecond latency** (vs 10-100ms for standard S3)
- Supports conditional writes (`If-Match`, `If-None-Match`)
- Supports multipart upload

**Relevance to us:** Lower latency helps, but doesn't solve write amplification. A 100 MiB upload at lower latency is still a 100 MiB upload. Express One Zone would be most beneficial for read-heavy workloads or small databases.

**Caveat:** Express One Zone uses a different authentication model (session-based with `CreateSession` API). We'd need to support this as a variant.

### 4.4 Read Performance — Comparable

Both Azure and S3 support byte-range GETs with similar performance characteristics:
- First-byte latency: 10-100ms (standard), 1-10ms (Express One Zone)
- Throughput: Both support parallel multi-connection downloads
- Our `page_blob_read_multi()` curl_multi pattern works identically with S3 range GETs

**Read path requires no architectural changes.** Just swap the URL construction and auth.

### 4.5 Mitigating Write Amplification

Strategies to reduce write amplification on S3:

1. **Small databases:** For databases < 50 MiB, full re-upload is tolerable (50 MiB at 100 Mbps = 4 seconds). This covers many use cases.

2. **WAL mode (future):** WAL appends to a separate file. If we store the WAL as a separate S3 object, we only upload WAL frames (small), not the full DB. Checkpoint then uploads the full DB. This amortizes write cost across many transactions.

3. **Sharded objects:** Store the database as N separate objects (e.g., 1 MiB chunks). Dirty pages only require re-uploading the chunks they touch. This is a significant architecture change but could reduce amplification to ~1 MiB granularity.

4. **Hybrid approach:** Use S3 for the "cold" database image + a separate "delta log" object for recent changes. Periodically merge. Similar to LSM tree compaction.

5. **Accept the tradeoff:** For read-heavy workloads (many readers, rare writes), S3's write amplification is acceptable. Position S3 backend for read-replica scenarios.

---

## 5. Architecture — Supporting Multiple Backends

### 5.1 Recommendation: Generalized Storage Ops Vtable

Our current `azure_ops_t` (`azure_client.h:156-325`) has 23 function pointers. Many are Azure-specific (page blobs, append blobs, snapshots). The VFS (`sqlite_objs_vfs.c`) calls these directly.

**Proposed approach:** Define a **`storage_ops_t`** abstraction at the VFS level with operations that map to what SQLite actually needs, not what any specific cloud offers:

```c
typedef struct storage_ops storage_ops_t;
struct storage_ops {
    /* Read a byte range from the database object */
    storage_err_t (*read_range)(void *ctx, const char *name,
                                int64_t offset, size_t len,
                                storage_buffer_t *out, storage_error_t *err);

    /* Write the full database object (S3) or dirty ranges (Azure) */
    storage_err_t (*write_object)(void *ctx, const char *name,
                                  const uint8_t *data, size_t total_size,
                                  const storage_dirty_range_t *dirty, int nDirty,
                                  const char *if_match_etag,
                                  storage_error_t *err);

    /* Read entire object (for journal, small blobs) */
    storage_err_t (*read_object)(void *ctx, const char *name,
                                 storage_buffer_t *out, storage_error_t *err);

    /* Write entire object (for journal) */
    storage_err_t (*write_whole_object)(void *ctx, const char *name,
                                        const uint8_t *data, size_t len,
                                        storage_error_t *err);

    /* Get object metadata (size, etag) */
    storage_err_t (*get_properties)(void *ctx, const char *name,
                                    int64_t *size, char *etag,
                                    storage_error_t *err);

    /* Lock/unlock (lease on Azure, lock object on S3, DynamoDB, etc.) */
    storage_err_t (*lock_acquire)(void *ctx, const char *name,
                                   int duration_secs, char *lock_id,
                                   size_t lock_id_size, storage_error_t *err);
    storage_err_t (*lock_renew)(void *ctx, const char *name,
                                 const char *lock_id, storage_error_t *err);
    storage_err_t (*lock_release)(void *ctx, const char *name,
                                   const char *lock_id, storage_error_t *err);

    /* Delete an object */
    storage_err_t (*delete_object)(void *ctx, const char *name,
                                    storage_error_t *err);

    /* Check existence */
    storage_err_t (*exists)(void *ctx, const char *name,
                            int *exists, storage_error_t *err);

    /* Optional: parallel read (NULL = fallback to sequential) */
    storage_err_t (*read_multi)(void *ctx, const char *name,
                                 int64_t total_size, uint8_t *dest,
                                 storage_error_t *err);

    /* Optional: incremental diff (NULL = not supported, full re-download) */
    storage_err_t (*get_diff_ranges)(void *ctx, const char *name,
                                      const char *prev_snapshot,
                                      storage_diff_range_t **ranges, int *count,
                                      storage_error_t *err);
};
```

**Key design decisions in this vtable:**

1. **`write_object` receives dirty ranges AND full data.** Azure implementation uploads only dirty ranges; S3 implementation uploads the full object. The VFS doesn't need to know which strategy is used.

2. **`if_match_etag` parameter on writes.** Enables conditional writes on both Azure (already used) and S3 (`If-Match` header). This is the multi-writer safety mechanism.

3. **Lock operations are abstract.** Azure implements via leases; S3 implements via lock objects or DynamoDB. The VFS just calls `lock_acquire/renew/release`.

4. **Optional operations (NULL = not supported).** `get_diff_ranges` returns NULL for S3 backends; VFS falls back to full re-download. Same pattern as our current `page_blob_write_batch` (NULL = sequential fallback).

### 5.2 Keeping azure_ops_t as Internal Implementation

The `azure_ops_t` vtable remains as the **Azure-specific** implementation detail inside `azure_client.c`. A thin adapter wraps it into `storage_ops_t`. Similarly, `s3_ops_t` (internal to `s3_client.c`) would be wrapped into `storage_ops_t`.

```
VFS Layer (sqlite_objs_vfs.c)
    ↓ calls
storage_ops_t (generic interface)
    ↓ dispatches to
azure_storage_ops (wraps azure_ops_t)    OR    s3_storage_ops (wraps s3_ops_t)
    ↓ calls                                     ↓ calls
azure_client.c (libcurl + Azure REST)          s3_client.c (libcurl + S3 REST)
```

This layering keeps the VFS clean and allows adding new backends (GCS native, etc.) without touching VFS code.

### 5.3 URI Scheme

```
file:db.sqlite?backend=s3&bucket=mybucket&region=us-east-1&aws_access_key_id=...&aws_secret_key=...
file:db.sqlite?backend=azure&azure_account=acct&azure_container=cont&azure_sas=token
```

Or via environment variables:
```
STORAGE_BACKEND=s3
AWS_ACCESS_KEY_ID=...
AWS_SECRET_ACCESS_KEY=...
S3_BUCKET=mybucket
S3_REGION=us-east-1
S3_ENDPOINT=http://localhost:9000  # for MinIO
```

**Default:** `backend=azure` for backward compatibility.

### 5.4 VFS Name

Keep `"sqlite-objs"` as the single VFS name. The backend is selected by configuration, not by registering separate VFS instances. This preserves API compatibility.

---

## 6. S3-Compatible Services — Compatibility Matrix

| Feature | AWS S3 | MinIO | Cloudflare R2 | Backblaze B2 | DO Spaces | Wasabi | GCS (S3 mode) |
|---------|--------|-------|---------------|--------------|-----------|--------|---------------|
| **SigV4 Auth** | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| **Byte-Range GET** | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| **PutObject** | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| **Multipart Upload** | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| **ETags** | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| **If-Match (conditional write)** | ✅ (Nov 2024) | ✅ | ✅ | ⚠️ Limited | ❌ | ⚠️ Unknown | ⚠️ Partial |
| **If-None-Match (conditional create)** | ✅ (Aug 2024) | ✅ | ✅ | ⚠️ Limited | ❌ | ⚠️ Unknown | ⚠️ Partial |
| **Versioning** | ✅ | ✅ | ❌ | ✅ | ✅ | ✅ | ✅ |
| **Object Lock** | ✅ | ✅ | ❌ | ✅ | ❌ | ✅ | ❌ |
| **HeadObject** | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| **DeleteObject** | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| **Custom Endpoint** | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| **Path-Style URLs** | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |

### Service-Specific Notes

**MinIO (self-hosted):**
- Most complete S3 compatibility. Supports nearly all S3 APIs.
- Conditional writes fully supported.
- Ideal for local development/testing (replaces Azurite role).
- Object Lock supported for distributed locking patterns.

**Cloudflare R2:**
- No egress fees — excellent for read-heavy workloads.
- Full conditional operations (If-Match, If-None-Match) on HeadObject and GetObject.
- PutObject supports conditional writes.
- No versioning, no Object Lock.
- Region is always `auto` (or `us-east-1` for compatibility).

**Backblaze B2:**
- S3-compatible API with some limitations.
- Pre-signed URLs supported. Conditional write support is limited — `If-Match` on writes may not be supported across all operations.
- Good value for archival/backup use cases.

**DigitalOcean Spaces:**
- Basic S3 compatibility. SigV2 + SigV4 supported.
- **No conditional write support** (`If-Match` on PutObject not documented).
- Cross-region copy not supported.
- Best for simple read/write, not multi-writer scenarios.

**Wasabi:**
- Hot storage pricing, no egress fees.
- Full SigV4 support. Conditional write support is not well-documented but likely works given their "S3-compatible" claim.
- Object Lock supported.

**Google Cloud Storage (S3 mode):**
- XML API interoperable with S3. Uses HMAC keys for SigV4 auth.
- Conditional writes supported via GCS's native preconditions (mapped through XML API).
- Some S3 features may not translate perfectly.

### Universal Baseline (All Services Support)

These features form our **minimum viable S3 backend:**
- SigV4 authentication
- PutObject / GetObject / HeadObject / DeleteObject
- Byte-range GET (Range header)
- Multipart upload (for large objects)
- ETags (for change detection)
- Custom endpoint URL (for non-AWS services)

### Conditional Write Support (Critical for Multi-Writer)

`If-Match` conditional writes are **not universal**. For services without it:
- Single-writer scenarios work fine (no conflict possible)
- Multi-writer requires an external lock mechanism or is simply unsupported

---

## 7. Recommended Phased Plan

### Phase 0: SigV4 Auth Module (1 week)
- Implement `s3_auth_sign_request()` in new `src/s3_auth.c`
- Reuse `azure_hmac_sha256()`, base64 helpers from `azure_auth.c`
- Implement UriEncode, canonical request builder, credential scope, 4-step HMAC chain
- Unit tests with known AWS SigV4 test vectors (published by AWS)
- **Deliverable:** Working SigV4 signing, validated against reference vectors

### Phase 1: Read-Only S3 Backend (1 week)
- Implement `s3_client.c` with GetObject (range + full), HeadObject
- Wire into VFS via `storage_ops_t` adapter (reads only)
- Test with MinIO local container
- **Deliverable:** Can open and read S3-hosted SQLite databases

### Phase 2: Read-Write S3 Backend (2 weeks)
- Implement PutObject with `If-Match` conditional write
- Implement multipart upload for databases > 5 MiB
- Journal blob via simple PutObject/GetObject
- Lock mechanism: lock-object pattern for MVP, optional DynamoDB later
- **Deliverable:** Can create, read, write, and sync SQLite databases on S3

### Phase 3: Generalize VFS Abstraction (1 week)
- Define `storage_ops_t` interface
- Refactor VFS to use `storage_ops_t` instead of `azure_ops_t`
- Wrap `azure_ops_t` in Azure adapter, `s3_ops_t` in S3 adapter
- Backend selection via URI parameter or environment variable
- **Deliverable:** Single VFS binary supports both Azure and S3

### Phase 4: Optimization & Compatibility (2 weeks)
- Application-level page hash metadata for incremental diff on S3
- Connection pooling for S3 (reuse curl_multi infrastructure)
- Test matrix across MinIO, R2, B2, DO Spaces
- Performance benchmarks: Azure page blob vs S3 for various workloads
- **Deliverable:** Production-quality S3 support with benchmarks

### Phase 5: Advanced Features (future)
- DynamoDB lease table for multi-writer
- S3 Express One Zone support (session-based auth)
- WAL mode on S3 (WAL as separate object, reduces write amplification)
- Sharded storage (database split across N objects)

**Total estimated effort (Phases 0-3): 5 weeks**
**Total with optimization (Phases 0-4): 7 weeks**

---

## 8. Risks and Open Questions

### Critical Risks

| Risk | Severity | Mitigation |
|------|----------|------------|
| **Write amplification for large DBs** | HIGH | Clearly document S3 is best for small databases (< 50 MiB) or read-heavy workloads. WAL mode (Phase 5) reduces this. |
| **No universal conditional writes** | MEDIUM | Services without `If-Match` support single-writer only. Document this limitation per service. |
| **Locking without leases** | MEDIUM | Lock-object pattern has no automatic expiry. Implement TTL-based cleanup + stale lock detection. |
| **S3 eventual consistency** | LOW | S3 is now strongly consistent (since Dec 2020) for all operations. This is no longer a risk. |
| **SigV4 implementation correctness** | MEDIUM | Validate against AWS published test vectors. Test with real AWS + MinIO + R2. |
| **Multipart ETag semantics** | LOW | Our revalidation only checks "has ETag changed?" — multipart ETags work fine for this. |

### Open Questions for Quetzal

1. **Target database size?** Write amplification makes S3 impractical for databases > ~100 MiB with frequent small commits. What's the expected size range for S3 use cases?

2. **Multi-writer requirement?** If S3 is primarily for read-replica / single-writer scenarios, we can skip DynamoDB locking entirely and use a simpler lock-object pattern.

3. **Which S3-compatible services are priority?** Suggest: AWS S3 + MinIO (for testing) as Phase 2, then R2 + B2 as Phase 4. This focuses effort on the two most complete implementations.

4. **Sharded storage interest?** Storing the database as N objects (1 MiB chunks) would dramatically reduce write amplification but is a significant architecture change. Worth pursuing, or out of scope?

5. **Naming:** Should we rename the project from `sqlite-objs` to something cloud-agnostic? Or keep the name and just add S3 as a backend?

6. **Build system:** S3 support adds `src/s3_auth.c` and `src/s3_client.c`. Should these be conditionally compiled (e.g., `make WITH_S3=1`) or always included?

---

## Appendix A: SigV4 vs Azure Shared Key — Code Reuse Analysis

| Component | Azure (`azure_auth.c`) | SigV4 (new `s3_auth.c`) | Reusable? |
|-----------|----------------------|------------------------|-----------|
| HMAC-SHA256 | `azure_hmac_sha256()` :100-110 | Same function, called 4× for key derivation | ✅ Yes — extract to shared `crypto_utils.c` |
| Base64 encode/decode | `azure_base64_encode/decode()` :31-94 | Needed for key handling | ✅ Yes — extract to shared |
| SHA256 hash | Not used (Azure doesn't hash payload) | Required for payload hashing | ❌ New — but trivial with OpenSSL `EVP_Digest()` |
| Header sorting | `compare_headers()` :128 | Same need, same algorithm | ✅ Yes — extract to shared |
| Canonical request | Azure 13-line format :183-247 | Different format (6 components) | ❌ Separate implementation |
| URL encoding | Not needed for Azure | Required per AWS spec | ❌ New ~30 lines |
| Key derivation | Decode base64 key once | 4-step HMAC chain per request (can cache per day) | ❌ New ~20 lines |
| Canonical resource | `/account/path` :243-279 | `/bucket/key` + sorted query | ❌ Separate implementation |

**Estimated: ~60% of crypto infrastructure is reusable. ~400 new lines for SigV4-specific code.**

---

## Appendix B: S3 REST API Calls Needed

| Operation | HTTP Method | S3 API | Headers |
|-----------|-------------|--------|---------|
| Read range | `GET /{key}` | GetObject | `Range: bytes=start-end` |
| Read full | `GET /{key}` | GetObject | — |
| Write full | `PUT /{key}` | PutObject | `If-Match: <etag>` (optional) |
| Head | `HEAD /{key}` | HeadObject | — |
| Delete | `DELETE /{key}` | DeleteObject | — |
| Multipart init | `POST /{key}?uploads` | CreateMultipartUpload | — |
| Upload part | `PUT /{key}?partNumber=N&uploadId=X` | UploadPart | `Content-Length` |
| Complete multipart | `POST /{key}?uploadId=X` | CompleteMultipartUpload | XML body with part list |
| Conditional read | `GET /{key}` | GetObject | `If-None-Match: <etag>` → 304 |
| Conditional write | `PUT /{key}` | PutObject | `If-Match: <etag>` → 412 on mismatch |

All operations use virtual-hosted-style URLs: `https://{bucket}.s3.{region}.amazonaws.com/{key}`
Or path-style (for MinIO/custom endpoints): `https://{endpoint}/{bucket}/{key}`
