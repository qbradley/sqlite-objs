# Project Context

- **Owner:** Quetzal Bradley
- **Project:** Azure Blob-backed SQLite (sqlite-objs) — a drop-in replacement for SQLite where all storage is backed by Azure Blob Storage, implemented as a custom VFS layer. MIT licensed.
- **Stack:** C, SQLite VFS API, Azure Blob Storage REST API, libcurl, OpenSSL
- **SQLite source:** `sqlite-autoconf-3520000/` (do not modify unless absolutely necessary)
- **Created:** 2026-03-10

## Key Context

- MVP 1: Drop-in replacement, single machine, remote storage, committed txns survive machine loss
- MVP 2: In-memory read cache
- MVP 3: Multi-machine reads (one writer, many readers on different machines)
- MVP 4: Multi-machine writes (correct but not necessarily performant)
- Dependencies: SQLite, OpenSSL (system), libcurl. No Azure SDK — direct REST API.
- License: MIT

## Learnings

<!-- Append new learnings below. Each entry is something lasting about the project. -->

### Prior Art Survey (2026-03-10)

- **Cloud Backed SQLite (CBS)** is the most directly relevant prior art — built by the SQLite team, supports Azure Blob Storage, uses block-based chunking (4MB blocks) with a manifest. However, it uses block blobs (not page blobs) and has no built-in locking. See `research/prior-art.md`.
- **Azure Page Blobs are our key differentiator.** Every S3-based project (sqlite-s3vfs, CBS, Litestream) struggles with immutable objects — either one object per page (too many requests) or chunked blocks (write amplification). Page blobs support 512-byte aligned random writes, meaning we can read/write individual SQLite pages directly at their offsets in a single blob. No prior project exploits this.
- **Every project is single-writer per database.** This is universal — CBS, Litestream, LiteFS, mvsqlite, rqlite, dqlite all enforce single writer. The only exception is cr-sqlite (CRDTs) which trades write performance (~2.5x overhead) for convergent multi-writer.
- **WAL mode is impossible across machines** without custom shared-memory implementation. Rollback journal is the safe default for MVP 1. WAL with EXCLUSIVE locking mode is viable for single-machine.
- **Locking must be built into the VFS** — projects that defer locking to "the application" (CBS, sqlite-s3vfs) see corruption in practice. Azure Blob Leases (60s renewable) are the right primitive.
- **Local caching is mandatory for usable performance.** Uncached cloud reads add 10-100ms per page. Every successful project caches aggressively. MVP 2 is correctly sequenced.
- **Don't fork SQLite.** libSQL/Turso shows both the power and maintenance burden. Our VFS-only approach is validated by CBS and mvsqlite.
- **License-compatible projects found:** rqlite (MIT), libSQL (MIT), sqlite-s3vfs (MIT), sqlite3vfshttp (MIT), gRPSQLite (MIT), go-cloud-sqlite-vfs (MIT), ncruces/go-sqlite3 (MIT). CBS is public domain. dqlite is LGPL (incompatible for static linking).

### Design Review — MVP 1 Architecture (2026-03-10)

- **Full design review at `research/design-review.md`.** 11 decisions covering blob types, locking, caching, WAL, testing, build system, error handling, auth, naming, VFS registration, and MVP scope.
- **Corrected my own prior-art recommendation:** Originally deferred caching to MVP 2. Aragorn proved uncached reads make VFS untestable (~5s for 100 pages). Cache (full blob download) is now mandatory for MVP 1.
- **Overrode Aragorn's nolock proposal:** Two-level lease-based locking from day 1 (SHARED=no lease, RESERVED+=acquire lease). Prior art shows deferred locking leads to corruption.
- **Key architecture: azure_ops_t vtable.** The swappable function pointer table between VFS and Azure client is both the production interface and the test seam. Defined in design-review.md Appendix A. This is the critical boundary between Aragorn and Frodo's code.
- **io_methods iVersion=1** — no WAL, no mmap. Eliminates 6 method implementations.
- **File type routing:** MAIN_DB and MAIN_JOURNAL → Azure. Everything else → default local VFS. Detected via flags in xOpen.
- **Journal handled as block blob** (sequential write/read pattern). DB as page blob (random R/W).
- **Write buffer with dirty page bitmap:** xWrite→memcpy+dirty bit, xSync→PUT Page per dirty page. Batches writes to sync time.
- **Error handling:** 429/lease-conflict → SQLITE_BUSY (retryable). All else after retry → SQLITE_IOERR_* (fatal). 5 retries, 500ms exponential backoff + jitter.
- **Device characteristics:** `SQLITE_IOCAP_SEQUENTIAL | SQLITE_IOCAP_POWERSAFE_OVERWRITE | SQLITE_IOCAP_SUBPAGE_READ`. NOT ATOMIC — journal safety is needed.
- **VFS name "sqlite-objs"**, non-default registration. Usage: `sqlite3_open_v2(name, &db, flags, "sqlite-objs")`.
- **Source layout:** `src/` (sqlite_objs_vfs.c, azure_client.c/.h, azure_auth.c, azure_error.c, sqlite_objs.h), `test/` (test_main.c, test_vfs.c, mock_azure_ops.c, test_integration.c).
- **Top risks:** (1) Lease expiry during long transactions, (2) Partial sync failure — both mitigated by journal-first ordering and inline lease renewal.
- **Decisions written to:** `.squad/decisions.md` — all 11 decisions (D1-D11) now approved and merged from inbox.

### MVP 1 Code Review — Reviewer Gate (2026-03-10)

- **Full review at `research/code-review.md`.** Reviewed all 7 source files against D1–D11.
- **Overall verdict: APPROVE WITH CONDITIONS** — 2 critical issues must fix before demo, 5 important issues for follow-up.
- **C1 (CRITICAL): Device characteristics in `sqlite_objs_vfs.c` claim ATOMIC512 + SAFE_APPEND.** Design spec (D4) says NOT ATOMIC. ATOMIC512 tells SQLite it can skip journal entries for 512-byte writes, which is dangerous — our multi-page xSync is not atomic. Must change to `SEQUENTIAL | POWERSAFE_OVERWRITE | SUBPAGE_READ`.
- **C2 (CRITICAL): URL construction in `azure_client.c` uses unbounded `strcat` on a 2048-byte stack buffer.** SAS tokens + long paths can overflow. Must use snprintf with bounds checking.
- **Everything else is architecturally sound.** The azure_ops_t vtable is correctly implemented (all 13 operations, signatures match Appendix A). Lease-based locking has no race conditions. Journal workflow is correct. Error mapping follows D8. Both auth paths work.
- **Key learning:** ATOMIC in SQLite device characteristics is a loaded term — it's about whether the pager can skip journal entries, not about whether individual writes are reliable. Always cross-check io capabilities against pager behavior, not just the underlying storage semantics.

### Agent-11: Code Review Complete (2026-03-10 — 07:43:07Z)

Completed comprehensive design review (D1-D11). Verdict: **APPROVE WITH CONDITIONS**.

**Blockers (must fix before demo):**
- C1: Device flags (Aragorn) — change `ATOMIC512|SAFE_APPEND` to `SEQUENTIAL|POWERSAFE_OVERWRITE|SUBPAGE_READ`
- C2: URL buffer (Frodo) — replace `strcat` with bounds-checked `snprintf`

**Status:** SUCCESS. Awaiting C1/C2 fixes. Code review re-check not required — fixes are mechanical.

### Performance Optimization Design (2026-03-11)

- **Produced comprehensive performance design:** `.squad/decisions/inbox/gandalf-performance-design.md` (D-PERF). Synthesized research from Frodo (Azure parallel writes, curl_multi, batch API) and Aragorn (VFS async constraints, threading model, xSync durability contract).
- **Key decision: curl_multi over pthreads.** Single-threaded event loop eliminates all threading hazards (no mutexes, no data races, no curl handle sharing issues). Lease renewal trivially integrates into the event loop. Frodo was right; Aragorn's pthread design is sound but unnecessary complexity for I/O-bound work.
- **Key decision: 32 parallel connections.** Halved Frodo's recommendation of 64 — 32 already exceeds Azure's 60 MiB/s per-blob throughput limit with 4 MiB coalesced writes. Tunable via compile-time `#define`.
- **Key decision: No page copies during xSync.** Aragorn proved btree mutex is held during xSync → aData is stable → zero-copy parallel flush is safe. This only holds because we use curl_multi (single-threaded), not pthreads.
- **Key decision: PRAGMA synchronous=NORMAL default.** Saves one journal upload per commit. FULL costs an extra HTTP round-trip for marginal safety gain (SQLite handles nRec recovery gracefully).
- **Key decision: No pre-flush.** Both researchers agree. Pre-flush creates corruption windows (journal not yet synced) and data races (xWrite vs in-flight uploads). Parallel-at-xSync delivers 50–1000× improvement.
- **Addressed user's write-through/write-back request directly.** sqlite-objs is already write-through (xWrite→memcpy). Pure async write-back is unsafe (xSync cannot lie). The design gives the user what they want: instant writes + parallel flush at sync time.
- **4-phase implementation plan:** (1) Coalescing only — 1–2 days, ≥5× for sequential, (2) curl_multi parallel — 3–5 days, ≥50× for bulk, (3) Connection pooling + lease hardening — 1–2 days, (4) Journal chunked upload — 2–3 days. Each phase delivers measurable value.
- **Projected performance:** 100-page clustered commit drops from 10s → ~200ms. 5,000-page bulk load drops from 500s → ~500ms. VACUUM 100MB drops from 2500s → ~2s.
- **9-item risk register** covering partial failure, lease expiry, throttling, Azurite compatibility, memory, vtable changes, TLS caching, alignment bugs, and event loop starvation.

### Phase 1+2 Code Review — Reviewer Gate (2026-03-12)

- **Verdict: APPROVED.** Both phases correctly implement the performance design. No blocking issues.
- **Phase 1 (Aragorn):** `coalesceDirtyRanges()` correctly handles all edge cases — empty, single, contiguous, 4 MiB boundary split, scattered, last-page 512-alignment. The post-loop `i++` for 4 MiB cap boundaries is subtle but correct (page already counted in runPages before break, advance past it for next iteration). Buffer overread from 512-alignment padding is safe because `bufferEnsure` grows geometrically (min 64 KiB doubling), so padding never exceeds nAlloc.
- **Phase 2 (Frodo):** `az_page_blob_write_batch()` curl_multi event loop is well-structured. Every exit path (success, partial failure, setup failure, lease loss) correctly cleans up all resources. Zero-copy design (CURLOPT_POSTFIELDS pointing into aData) is safe because btree mutex is held during xSync. Lease renewal at 15s intervals on 30s lease gives adequate safety margin. SharedKey signing has correct x-ms header ordering.
- **Key observation:** The batch retry uses pure exponential backoff without jitter (unlike sequential path). Non-blocking but should be fixed for consistency before Phase 3.
- **Missing test coverage:** No test for partial batch failure (some ranges succeed, others fail, then retry). Hard to test with current mock (batch=NULL). Tracked as Phase 3 follow-up.
- **Full review at:** `.squad/decisions/inbox/gandalf-phase12-review.md`

### Adaptive Readahead Design (2025-07-17)

- **Produced complete design document:** `research/adaptive-readahead-design.md` — addresses D-4.4 (readahead not adaptive) and D-4.3 (getenv on every miss) from the demand paging design review.
- **3-state machine:** INITIAL → SEQUENTIAL (window grows 4→1024 pages) or RANDOM (demand-only, window=1). Transitions based on whether cache misses follow the predicted sequential boundary.
- **Key design choice: tolerance-based sequential detection.** Unlike Linux's exact-boundary match, sqlite-objs allows a gap of `max(W/4, 4)` pages to handle pre-cached pages from xOpen prefetch. This is necessary because our prefetch populates arbitrary cache regions that Linux's page cache doesn't have.
- **Key design choice: no async readahead.** Linux's main innovation (overlapping I/O via async markers) requires background I/O threads. Deferred to future xFetch/xUnfetch implementation. The synchronous state machine captures most benefit with minimal complexity.
- **Configuration migrated from env vars to URI parameters:** `readahead=auto|N`, `readahead_max=N`, `cache_pages=N`. Per team directive, no environment variables.
- **Performance projections:** Sequential scans 9–15× fewer HTTP GETs (fixed 64 → adaptive max 1024). Index lookups eliminate 98% of bandwidth waste. Mixed OLTP 1.5–2× improvement during warmup.
- **Max window 1024 pages (4 MiB):** Chosen to balance HTTP latency amortisation (~95 MB/s effective throughput) against wasted bandwidth on false sequential detection. Tunable via `readahead_max` URI parameter.
- **Interaction with 1GB default cache:** For databases <1GB, readahead is mostly irrelevant (everything cached after prefetch). For >1GB databases, adaptive readahead is the primary performance lever.
- **Estimated 3 days to implement** across 4 phases: core state machine, config migration, observability, validation.
- **Status:** PROPOSED — awaiting Brady's review before implementation.

### Rust VFS Integration Test Architecture (2025-07-18)

- **Produced comprehensive test architecture:** `.squad/decisions/inbox/gandalf-vfs-test-architecture.md` (D-TEST). Designed 54 new Rust integration tests across 6 categories: lifecycle (9), transactions (12), cache reuse (9), threading extensions (5), growth/shrink (11), error recovery (8).
- **Key design: TestDb fixture + SeededData.** TestDb abstracts over local/Azurite/Azure backends with RAII cleanup. SeededData uses deterministic hash-based generation — given a seed and row index, regenerates expected values at verification time without storing them. No `#[ignore]` — Azure tests auto-skip when credentials absent.
- **Key design: FCNTL download counter wrapper.** The C VFS exposes `SQLITE_OBJS_FCNTL_DOWNLOAD_COUNT` (op 200). Wrapped via unsafe FFI through rusqlite's raw handle to validate ETag cache reuse: `download_count == 0` means cache hit, `> 0` means fresh download.
- **Key design: Dirty shutdown via `mem::forget`.** Prevents `Drop` → `sqlite3_close` → `xClose`, simulating process crash. Tests verify committed data survives (synced to Azure) while uncommitted data is lost. Journal hot recovery tested by forgetting mid-transaction.
- **VFS coverage matrix:** All 18 io_methods mapped to test categories. Only xRandomness/xSleep/xCurrentTime/xDlOpen uncovered (OS-level delegation, no Azure interaction).
- **File organization:** 6 new test files + shared `common/` module. Existing azure_smoke.rs, threading.rs, perf_matrix.rs unchanged. One file per concern, independently runnable via `cargo test --test <name>`.
- **Threading model validated:** Rust `Connection` is Send but NOT Sync. Each thread must own its connection. Barrier-based synchronization for coordinated test starts.
- **No new dependencies required.** All test infrastructure uses std + existing dev-deps (rusqlite, tempfile, uuid, dotenvy).
- **Implementation estimate:** 4.5 days across 4 phases. Phase 1 (foundation) → Phase 2 (correctness) → Phase 3 (robustness) → Phase 4 (completeness).
- **5 open questions for Brady:** Thread count for CI, FCNTL wrapper placement (test-only vs crate API), WAL vs journal default, real Azure cadence, Toxiproxy deferral.
- **Status:** PROPOSED — awaiting Brady's review before implementation.
