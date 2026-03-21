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

### S3 Backend Research (2026-07-15)

- **Produced comprehensive S3 architecture research:** `.squad/decisions/inbox/gandalf-s3-research.md` — covers API mapping, dependency strategy, performance analysis, architecture recommendation, S3-compatible services matrix, and phased implementation plan.
- **Key finding — write amplification:** S3 lacks page-granularity random writes. Every xSync requires full-object re-upload, creating up to 5000× write amplification for small commits on large databases vs Azure page blobs. This is the fundamental architectural constraint.
- **Key finding — conditional writes exist:** As of November 2024, S3 supports `If-Match` ETag-based conditional writes on PutObject and CompleteMultipartUpload. This provides multi-writer safety without leases — maps to our existing revalidation pattern.
- **Key finding — SigV4 is feasible with existing stack:** SigV4 uses HMAC-SHA256 (we already have `azure_hmac_sha256()` in `azure_auth.c:100-110`). Estimated ~400 new lines of C for the S3-specific signing code. All S3-compatible services support SigV4.
- **Key finding — AWS C SDK is rejected:** 10 separate repositories, requires building aws-lc + s2n-tls, would 10-30× our binary size. Incompatible with zero-SDK philosophy.
- **Architecture recommendation:** Generalize `azure_ops_t` to `storage_ops_t` abstraction. `write_object()` receives both dirty ranges and full data — Azure backend uploads only dirty ranges, S3 backend uploads full object. VFS stays identical.
- **Locking without leases:** Recommended two-tier approach: (1) optimistic concurrency via `If-Match` ETag for single-writer, (2) optional DynamoDB lease table for multi-writer deployments.
- **No incremental diff on S3:** Azure's `blob_get_page_ranges_diff()` has no S3 equivalent. MVP uses full re-download on ETag mismatch; future optimization via application-level page hash metadata.
- **S3-compatible services:** Byte-range GET, PutObject, multipart upload, ETags, SigV4 are universal. Conditional writes (`If-Match`) are NOT universal — DigitalOcean Spaces lacks it entirely. MinIO and R2 support it fully.
- **Phased plan:** Phase 0 (SigV4 auth, 1 week) → Phase 1 (read-only S3, 1 week) → Phase 2 (read-write S3, 2 weeks) → Phase 3 (generalize vtable, 1 week) → Phase 4 (optimization, 2 weeks). Total: 5-7 weeks.
- **Open questions for Quetzal:** Target database size? Multi-writer requirement? Priority S3-compatible services? Sharded storage interest? Project naming? Conditional compilation?

## Core Context Summary

**Lead Architect & Design Review (2026-03-10 through 2026-07):**
Gandalf led MVP 1 architecture, design review (D1–D11, 11 decisions), code review gate, performance design, and lazy cache filling analysis. Core contribution: articulated tradeoffs, validated prior art, and ensured decisions backed by architectural reasoning (not guessing).

**MVP 1 Design Review (2026-03-10):**
11 decisions (D1–D11) covering blob strategy (page blobs for DB, block blobs for journal), locking (two-level lease), caching (full-blob from day 1, not deferred), WAL (impossible for remote, journal mode correct), testing (4-layer pyramid with azure_ops_t vtable), build (GNU Makefile), error handling (429/409→SQLITE_BUSY, else→SQLITE_IOERR_*), auth (SAS+SharedKey), naming (sqlite-objs), VFS registration (non-default), and MVP scope (11 items IN, 8 OUT). Key reversals: (1) Overrode Aragorn's nolock → two-level lease (prior art shows deferred locking → corruption). (2) Overrode MVP 2 deferral on caching → mandatory day 1 (uncached reads untestable at ~5s/100 pages). (3) azure_ops_t vtable mandatory for testability (critical boundary).

**Code Review Gate (2026-03-10):**
Verdict: APPROVE WITH CONDITIONS. Two critical blockers: C1 (device flags ATOMIC512|SAFE_APPEND incorrect, must be SEQUENTIAL|POWERSAFE_OVERWRITE|SUBPAGE_READ), C2 (URL buffer overflow strcat, must use snprintf). All else architecturally sound. Key learning: ATOMIC in SQLite device characteristics is about whether pager can skip journal entries, not storage semantics.

**Performance Design (2026-03-11, D-PERF):**
Synthesized Frodo (curl_multi) + Aragorn (VFS async constraints). Four key decisions: (1) curl_multi over pthreads (single-threaded event loop, no threading hazards, trivial lease renewal). (2) 32 parallel connections default (halved Frodo's 64, exceeds 60 MiB/s blob limit with 4 MiB coalesces). (3) Zero-copy batch (btree mutex held → aData stable). (4) PRAGMA synchronous=NORMAL default (saves one journal upload/commit). Four implementation phases (1–2 days each): coalescing, curl_multi parallel, connection pooling, journal chunks. Projected 100-page commit: 10s → 200ms; 5K-page bulk load: 500s → 500ms; VACUUM 100MB: 2500s → 2s. Risk register (9 items): partial failure, lease expiry, throttling, Azurite compat, memory, vtable changes, TLS caching, alignment bugs, event loop starvation.

**Phase 1+2 Review (2026-03-12):**
Approved. Aragorn's `coalesceDirtyRanges()` handles all edge cases. Frodo's curl_multi event loop well-structured, zero-copy safe. Recommendation: add jitter to batch retry (currently pure exponential backoff).

**Lazy Cache Filling Analysis (2026-03-21, D23):**
Quetzal's proposal (download-on-demand with validity bitmap) architecturally sound. Three critical gaps: (1) Page-1 bootstrap for size detection. (2) Readahead-on-fault for cold-cache sequential reads (100–1000× slower without). (3) Bitmap persistence atomicity. Current disk-backed cache + dirty bitmap + ETag sidecars provide 80% scaffolding. Sweet-spot scenario (open 100MB, few lookups): 20× faster. Table scans: catastrophic without prefetch. Implementation estimate: 4 days (4 phases). Regression-free via opt-in `prefetch=none` URI parameter; default `prefetch=all` unchanged.

### Lazy Cache Filling Design Review (2026-03-21)

- **Produced comprehensive architectural analysis:** `.squad/decisions/inbox/gandalf-lazy-cache-analysis.md` (merged to D23 in decisions.md).
- **Verdict:** Proposal is architecturally sound but requires three critical additions: (1) Page-1 bootstrap, (2) Readahead-on-fault, (3) Bitmap persistence atomicity.
- **Key insight:** The sweet-spot scenario (open 100MB database, do a few lookups, close) achieves **20× faster open time**, but table scans become catastrophic without prefetch (~2500× slower for full table scans).
- **Regression guarantee:** Lazy cache is opt-in via `prefetch=none` URI parameter. Default `prefetch=all` preserves current behavior.
- **Implementation roadmap:** 4 phases across 4-6 days. Phase 1 (minimal lazy + page-1) deferred to Phase 2+.
- **Pending input:** 3 questions identified for Quetzal regarding scope constraints (interactive reads vs scans vs VACUUM), readahead strategy (fixed vs adaptive), and process-local cache sharing.
- **Context:** Synthesis of 4+ months prior analysis. Coordinated with Aragorn's code-level review.


### Lazy Cache Architecture Integration (2026-03-22)

Aragorn completed implementation of lazy cache filling feature per architecture review. Key architectural outcomes:

- **Design validation:** All 6 lazy cache design decisions (D-LC1 through D-LC6) proven sound in implementation. Bitmap persistence atomicity (D-LC6) correctly sequences .state before .etag writes.
- **Prefetch default:** Per UD9 user directive, `prefetch=all` remains default. Lazy mode opt-in via `prefetch=none` URI parameter. Regression-free: default behavior unchanged.
- **Code footprint:** ~500 lines new code + ~200 lines refactoring. Touches xOpen, xRead, xWrite, xSync, xClose, revalidateAfterLease(). No vtable changes.
- **Performance profile:** Cold-cache small-read workloads see 20× faster open latency. Sequential table scans remain slow without explicit prefetch (architectural limit, not regression).
- **Readahead simplicity:** Fixed 16-page window as designed (no adaptive state machine). Measured 16× reduction in HTTP requests for sequential reads.

**Impact on ongoing work:** 
- Lease renewal logic (`revalidateAfterLease`) refactored to support bitmap invalidation instead of forced downloads. Incremental diff now updates bitmap state.
- ETag + state file sidecar dependency chain enforced: cache fsync → .state (atomic) → .etag. Guards against desync on crash.
- Test infrastructure extended to validate bitmap consistency and .state file recovery.

**Deliverables validated:** 264 tests pass (247 unit + 17 integration). Zero regressions vs baseline.
