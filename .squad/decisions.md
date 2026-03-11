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

### D6: VFS Name "azqlite", Non-Default, Delegating
**Date:** 2026-03-10 | **From:** Gandalf (Design Review)

Register as "azqlite" via `sqlite3_vfs_register(pVfs, 0)`. Delegate xDlOpen/xRandomness/xSleep/xCurrentTime to default VFS. Route temp files to default VFS xOpen.

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

**IN:** Page blob DB, block blob journal, in-memory cache+write buffer, lease locking, journal mode, azure_ops_t vtable, SAS+SharedKey auth, retry logic, temp file delegation, VFS registration, Layer 1+2 tests, Makefile, azqlite-shell.

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
- **C1 (azqlite_vfs.c:693):** Change device flags from `ATOMIC512|SAFE_APPEND` to `SEQUENTIAL|POWERSAFE_OVERWRITE|SUBPAGE_READ` (data corruption prevention)
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

Three-binary architecture: lightweight harness that shells out to speedtest1 subprocesses. speedtest1 (standard SQLite) and speedtest1-azure (with azqlite VFS registered as default) run as isolated processes measured via `system()` and `gettimeofday()`. speedtest1.c used unmodified from SQLite upstream. Rejected embedding via #define main trick (speedtest1's `exit()` terminates harness before results capture) and patching upstream (maintenance burden).

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

**Solution:** Implemented `coalesceDirtyRanges()` in `azqlite_vfs.c` — scans dirty bitmap left-to-right, merges contiguous dirty pages into single `azure_page_range_t` ranges, caps each at 4 MiB (Azure Put Page limit). Sequential fallback iterates coalesced ranges instead of individual pages.

**Files modified:**
- `src/azure_client.h` — Added `azure_page_range_t` struct and `AZQLITE_MAX_PARALLEL_PUTS` define. Added `page_blob_write_batch` to end of `azure_ops_t` vtable (NULL until Phase 2 curl_multi).
- `src/azqlite_vfs.c` — Added `coalesceDirtyRanges()`, rewrote `azqliteSync` MAIN_DB path: coalesce → try batch (NULL for now) → sequential fallback with lease renewal every 50 ranges.
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

- **Two binaries:** `tpcc-local` (local SQLite only, no Azure deps), `tpcc-azure` (with azqlite VFS)
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
- **maxranges_overflow test:** Requires `azqlite_test_coalesce_dirty_ranges()` exposed behind `AZQLITE_TESTING` flag for direct algorithm testing

**Impact:** Write recording seam enables coalescing algorithm verification without exposing internals. All 9 coalescing tests pass; 1 gated test awaits algorithm exposure.

---

### UD5: User Directive — Use claude-opus-4.6 for Performance Optimization
**Date:** 2026-03-11 | **From:** Quetzal Bradley (via Copilot)

Use claude-opus-4.6 for all agents during performance optimization implementation. Complicated feature requires highest quality reasoning.

---

## Governance

- All meaningful changes require team consensus
- Document architectural decisions here
- Keep history focused on work, decisions focused on direction
