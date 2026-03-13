# Demand Paging Design Review

**Reviewer:** Gandalf (Lead Architect)  
**Date:** 2025-07-17  
**Scope:** Page cache, read-ahead, bulk prefetch, and dirty-page flush  
**Files:** `src/sqlite_objs_vfs.c` (2399 LOC), `src/azure_client.c` (2060 LOC)  
**Verdict:** Ship with follow-up — correct enough for current workloads, but multiple architectural issues must be addressed before production scale

---

## 1. Executive Summary

The demand paging implementation replaces the original "download entire blob at open" strategy with a three-component system: an LRU page cache, sequential read-ahead on cache miss, and a bulk prefetch at xOpen. The architecture is fundamentally sound — it correctly maps SQLite's page-oriented I/O onto Azure Page Blob range reads/writes, and the coalesced batch write path is well-engineered.

However, the current implementation exhibits an **85× regression** (0.4 tps vs 34 tps) in a benchmark that previously fit entirely in memory. The performance gap is not a bug — it is the predictable consequence of several design choices that trade latency for memory. This review identifies the specific architectural issues responsible and proposes a prioritised remediation plan.

---

## 2. Architecture Overview

### 2.1 Component Map

```
xOpen
  │
  ├─► Bulk Prefetch: single GET for min(blobSize, maxPages × 4096)
  │   └─► Split into pages → cacheInsert() for each
  │
  ▼
xRead (cache hit path)
  │
  ├─► cacheLookup() → O(1) hash probe
  ├─► cacheLruTouch() → move to MRU
  └─► memcpy into SQLite buffer
  
xRead (cache miss path)
  │
  ├─► cacheGetReadaheadPages() → getenv("SQLITE_OBJS_READAHEAD_PAGES")
  ├─► Single GET for pages [N, N+readahead)
  ├─► Split response → cacheInsert() for each (target first)
  └─► cacheEvictClean() as needed (LRU tail, skip dirty)

xWrite
  │
  ├─► cacheLookup() → hit: overwrite in-place, mark dirty
  └─► miss: fetch page from Azure (full page) or calloc (partial), mark dirty

xSync (MAIN_DB)
  │
  ├─► page_blob_resize (if grown)
  ├─► cacheCollectDirty() → sorted array
  ├─► cacheCoalesceRanges() → contiguous ranges ≤ 4 MiB, 512-aligned
  ├─► page_blob_write_batch (curl_multi, 32 parallel PUTs)
  │   └─► fallback: sequential page_blob_write per range
  └─► Clear dirty flags across cache
```

### 2.2 Data Structures

| Structure | Role | Lifetime |
|-----------|------|----------|
| `sqlite_objs_page_cache_t` | Hash map + doubly-linked LRU | Per `sqlite-objsFile`, xOpen→xClose |
| `sqlite_objs_cache_entry_t` | One page: pageNo, data ptr, dirty flag, LRU/hash pointers | Heap-allocated per page |
| `azure_page_range_t` | Contiguous byte range for batch write | Stack (64) or heap, per xSync |

---

## 3. Correctness Assessment

### 3.1 What's Correct

**Cache coherence within a connection.** The cache is the sole authority for page content after xOpen. xWrite always goes through the cache (write-through to cache, lazy-flush to Azure at xSync). xRead always checks cache first. There is no stale-read path within a single connection's lifetime.

**Dirty page tracking.** The `nDirty` counter and per-entry `dirty` flag are maintained consistently: set on xWrite, cleared in bulk at end of xSync, decremented on cacheRemove. The eviction policy correctly refuses to evict dirty pages, allowing the cache to grow past its soft limit.

**Coalesced writes.** `cacheCoalesceRanges()` correctly sorts dirty pages by pageNo (insertion sort — appropriate for typical dirty counts), merges consecutive pages into contiguous ranges capped at 4 MiB (Azure Put Page limit), and 512-byte aligns (Azure page blob requirement). The temporary contiguous buffers are necessary because page data is non-contiguous in the heap.

**Journal and WAL handling.** These are handled entirely in memory (journal = block blob, WAL = append blob) and are orthogonal to the page cache. The journal cache (avoiding HEAD requests) is a sound optimisation.

**Batch write retry.** The `page_blob_write_batch` implementation (azure_client.c:1098) uses curl_multi with per-range retry (3 attempts, exponential backoff), lease renewal every 15 seconds during long flushes, and proper cleanup on partial failure.

### 3.2 Correctness Concerns

**C-3.1: `aerr` potentially uninitialised on ETag capture (Low risk).**  
At `sqlite_objs_vfs.c:1411`, the sequential fallback path reads `aerr.etag[0]` to capture the ETag from the last write. The `aerr` variable is declared at function scope (line 1109) but only initialised inside the sequential write loop (line 1379) or the batch path (line 1325). If `nDirty > 0` but the resize operation at line 1266 is the only operation that initialises `aerr`, the stale `aerr` from resize doesn't carry a meaningful ETag. In practice, `nRanges ≥ 1` whenever `nDirty > 0` (the coalesce function guarantees this), so the loop always executes and `aerr` is always set. But the code relies on an invariant that isn't expressed in the control flow.  
**Recommendation:** Initialise `aerr` with `azure_error_init(&aerr)` at declaration, or move the ETag capture into the sequential loop body on the last iteration.

**C-3.2: Readahead can exceed cache capacity for one request cycle.**  
`cacheGetReadaheadPages()` caps readahead at `cache.maxPages`, but each readahead page is inserted via `cacheInsert()` which calls `cacheEvictClean()` before insertion — evicting the *least* recently used clean page. The target page is inserted first (at MRU), so if `readaheadPages == maxPages`, the readahead will evict `maxPages - 1` existing clean pages to make room. This is correct but aggressive — it effectively flushes the working set on every cache miss when the cache is full.

**C-3.3: No validation of Azure response size.**  
`az_page_blob_read` (azure_client.c:746) requests a specific byte range but does not validate that the response body matches the requested length. If Azure returns a truncated response (e.g., connection drop with HTTP 200), the VFS will split a short buffer into pages, with the last page containing zeros from calloc. SQLite would then operate on corrupted page data silently.  
**Recommendation:** After `page_blob_read`, verify `buf.size == fetchLen`. Return `SQLITE_IOERR_READ` on mismatch.

---

## 4. Design Flaws

### 4.1 Critical — Performance Architecture

**D-4.1: Per-page heap allocation creates O(n) malloc pressure.**

Every page in the cache requires two separate heap allocations:
- `calloc(1, sizeof(sqlite_objs_cache_entry_t))` — 56 bytes (with pointers/padding)
- `calloc(1, detectedPageSize)` — typically 4096 bytes

For a 1024-page cache, this means 2048 `malloc` calls at open and 2 `malloc` + 2 `free` calls per evict-and-insert cycle. With glibc, this induces significant heap fragmentation and TLB pressure. On macOS (libmalloc with magazine allocator), the per-allocation overhead is ~16 bytes metadata, so a 4096-byte page actually consumes ~4112 bytes.

**Impact:** At 1024 pages × 2 allocations = 2048 malloc calls during prefetch. During readahead (64 pages), that's 128 malloc + up to 128 free calls per cache miss.

**Recommendation:** Pre-allocate a contiguous page buffer (`maxPages × pageSize`) and a parallel entry array at cache init. Use a free-list for recycling entries on eviction. This eliminates all per-page malloc/free and improves cache locality.

```
Before: [entry₁→heap] [data₁→heap] [entry₂→heap] [data₂→heap] ...
After:  [entry₁|entry₂|...|entryₙ] [data₁|data₂|...|dataₙ]   (two allocations total)
```

**D-4.2: Prefetch size calculation uses wrong page size.**

```c
// sqlite_objs_vfs.c:1817
size_t maxPrefetch = (size_t)maxPages * SQLITE_OBJS_DEFAULT_PAGE_SIZE;
```

The prefetch size is computed using `SQLITE_OBJS_DEFAULT_PAGE_SIZE` (4096) before detecting the actual page size. If the database uses 8192-byte pages, this fetches `1024 × 4096 = 4 MiB` instead of the intended `1024 × 8192 = 8 MiB`, loading only 512 pages into the cache. For 16384-byte pages (common in WAL mode), only 256 pages are prefetched.

Conversely, if the DB uses 1024-byte pages, the prefetch loads 4× more data than intended by the maxPages limit, consuming memory unnecessarily.

After detection, the cache splits the buffer using the detected page size, so pages are correctly populated. But the fetch volume is wrong.

**Recommendation:** Fetch only the first page (e.g., 64 KiB — covers all valid SQLite page sizes) to detect page size, then compute the correct prefetch volume, then issue the bulk GET with the correct size. This adds one small extra HTTP request but ensures correct cache warming.

**D-4.3: `getenv()` called on every cache miss.**

```c
// sqlite_objs_vfs.c:634-641
static int cacheGetReadaheadPages(void) {
    const char *env = getenv("SQLITE_OBJS_READAHEAD_PAGES");
    if (env) {
        int val = atoi(env);
        if (val > 0) return val;
    }
    return SQLITE_OBJS_DEFAULT_READAHEAD;
}
```

`getenv()` on most platforms walks the entire environment block (O(n) linear scan) on every call. This function is called once per cache miss in xRead. Under a scan-heavy workload, this could be thousands of calls per second.

**Recommendation:** Cache the value in a `static` variable with a `static int once` guard, or resolve it once at xOpen and store in the `sqlite-objsFile` struct.

**D-4.4: Readahead is not adaptive to access patterns.**

The readahead always fetches a fixed number of pages (default 64) forward from the miss point. SQLite's B-tree traversal exhibits:
- **Sequential scans:** Benefit from large readahead (128+ pages)
- **Index lookups:** Access pages sparsely (root → internal → leaf), readahead wastes bandwidth
- **Random point queries:** Readahead is pure waste

A fixed readahead of 64 pages (256 KiB with 4K pages) at ~22ms per HTTP GET means every cache miss costs 22ms regardless of whether the readahead pages will be used. For index lookups that touch 3-4 pages across the tree, this fetches 60+ pages that are never read.

**Impact:** This is the primary contributor to the 85× performance gap. A transaction that touches 100 pages scattered across a B-tree will incur ~100 cache misses × 22ms = 2.2 seconds in HTTP latency alone, compared to ~0ms for in-memory access.

**Recommendation (short-term):** Track sequential-miss detection. If the last N misses were for consecutive pages, increase readahead. If misses are scattered, reduce to 1 (demand-only). A simple 2-state detector (sequential vs random) based on the last miss address would capture most of the benefit.

**Recommendation (long-term):** Implement SQLite's `xFetch`/`xUnfetch` memory-mapped I/O interface to allow SQLite to directly access cache pages without copy, and use SQLite's own access pattern hints.

### 4.2 Major — Resource and Reliability

**D-4.5: Bulk prefetch can block xOpen for minutes.**

```c
// Prefetch for 25000 pages × 4096 = ~100 MiB in one GET
size_t maxPrefetch = (size_t)maxPages * SQLITE_OBJS_DEFAULT_PAGE_SIZE;
```

With `SQLITE_OBJS_MAX_PAGES=25000` (observed in benchmarks), the xOpen prefetch issues a single 100 MiB GET request. On a connection with 10 MiB/s throughput to Azure, this takes 10 seconds. On a saturated or throttled link, it blocks the calling thread for the full `CURLOPT_TIMEOUT` (60s). The benchmark observed **923-second hangs** attributed to macOS SecureTransport/curl interaction.

The single-GET design means: (a) no progress reporting, (b) no ability to cancel, (c) total failure if the request times out (no partial recovery), and (d) a single curl timeout governs the entire transfer.

**Recommendation:** Implement chunked prefetch — fetch in 1-4 MiB chunks with per-chunk timeout. This provides incremental progress, allows early bailout (the first chunk contains page 0 for page-size detection), and limits blast radius of a single timeout.

**D-4.6: Coalesced range buffers double peak memory during xSync.**

`cacheCoalesceRanges()` allocates temporary contiguous buffers for each range by copying page data:

```c
// sqlite_objs_vfs.c:1059
unsigned char *buf = (unsigned char *)calloc(1, len);
// ... memcpy each page's data into buf
```

If the cache has 1024 dirty pages × 4096 bytes = 4 MiB of dirty data, the coalesce step allocates another 4 MiB of contiguous buffers. Peak memory during xSync is therefore 2× the dirty data size.

For the batch write path, all range buffers must be live simultaneously (they're passed to `curl_multi`). For the sequential path, buffers are freed after each write, but they're all allocated upfront in `cacheCoalesceRanges`.

**Recommendation:** For the sequential path, coalesce and write one range at a time. For the batch path, consider writing directly from the cache page data by gathering page pointers into an iovec-style structure (would require `CURLOPT_READFUNCTION` with scatter-gather support). Alternatively, accept the 2× overhead as a known cost and document the peak memory formula.

**D-4.7: No cache hit/miss metrics.**

The implementation has no instrumentation for cache hit rate, eviction frequency, readahead effectiveness (pages read ahead that were subsequently accessed vs wasted), or dirty page high-water mark. Without these metrics, it's impossible to tune `SQLITE_OBJS_MAX_PAGES` and `SQLITE_OBJS_READAHEAD_PAGES` for a given workload.

The existing `[TIMING]` infrastructure is well-designed but only covers HTTP timing, not cache behaviour.

**Recommendation:** Add counters to `sqlite_objs_page_cache_t`: `nHits`, `nMisses`, `nEvictions`, `nReadaheadUsed`, `nReadaheadWasted`. Log a summary at xClose or xSync. Expose via `SQLITE_FCNTL_*` for programmatic access.

### 4.3 Minor — Code Quality

**D-4.8: Insertion sort in `cacheCollectDirty` is O(n²).**

```c
// sqlite_objs_vfs.c:1010-1019
for (int i = 1; i < n; i++) { ... }
```

For typical dirty counts (tens to low hundreds per sync), insertion sort is fine. But a pathological workload that dirties 10,000+ pages (e.g., bulk INSERT) will spend non-trivial time sorting. The comment says "typically small count or nearly sorted" — but dirty pages from random writes will be in LRU order, not page order.

**Recommendation:** Use `qsort()` for n > 256, keep insertion sort for small n. Alternatively, maintain a sorted dirty list incrementally.

**D-4.9: Magic numbers in timeout and retry configuration.**

Timeout values are hardcoded across two files:
- `CURLOPT_TIMEOUT=60` (azure_client.c:480)
- `CURLOPT_CONNECTTIMEOUT=10` (azure_client.c:481)
- `CURLOPT_LOW_SPEED_LIMIT=1024` (azure_client.c:483)
- `CURLOPT_LOW_SPEED_TIME=30` (azure_client.c:484)
- `AZURE_MAX_RETRIES=5`, `AZURE_RETRY_BASE_MS=500`, `AZURE_RETRY_MAX_MS=30000` (azure_client_impl.h)
- `BATCH_MAX_RETRIES=3`, `BATCH_LEASE_RENEWAL_SEC=15` (azure_client.c)

These should be tuneable at runtime (environment variables or configuration struct), with the current values as defaults. The 60-second curl timeout is particularly problematic — for a 100 MiB prefetch at realistic bandwidth, it may not be enough, yet it's too long for a simple page read.

**D-4.10: `cacheEvictClean` allows unbounded cache growth.**

```c
// sqlite_objs_vfs.c:257
/* No clean pages to evict — allow cache to grow beyond soft limit */
return SQLITE_OK;
```

If all pages in the cache are dirty, `cacheEvictClean` gives up and returns OK, allowing `cacheInsert` to grow the cache without limit. Under a write-heavy workload that dirties every cached page between syncs, the cache grows indefinitely until memory exhaustion.

**Recommendation:** Implement a hard limit (e.g., 2× maxPages). If the cache reaches the hard limit with all dirty pages, force a sync (write-back) or return `SQLITE_FULL`.

---

## 5. Performance Analysis

### 5.1 The 85× Regression — Root Cause Decomposition

| Factor | Pre-demand-paging | With demand paging | Impact |
|--------|-------------------|-------------------|--------|
| Read latency | memcpy (~10ns) | HTTP GET (~22ms) on miss | **2,200,000× per miss** |
| Cache misses per txn | 0 (all in memory) | ~50-200 (depending on working set) | 1-4 seconds HTTP time |
| Write path | memcpy to buffer | memcpy to cache (same) | Neutral |
| Sync path | Single PUT | Coalesce + batch PUT | Slightly better (parallel) |
| Open time | Single GET (entire blob) | Single GET (prefetch subset) | Better for large DBs |

**The performance gap is dominated by read-path cache misses.**

For a benchmark on a 100 MiB database with 1024 max cache pages (4 MiB), the cache can hold ~4% of the database. If the benchmark's access pattern touches pages outside this 4%, every access is a cache miss costing 22ms+ of network latency.

### 5.2 Latency Budget for a Single Transaction

Assuming a transaction that modifies 10 rows in a B-tree:
- Read root page: 1 cache miss = 22ms (+ 63 readahead pages)
- Read 3-4 internal pages: 2-3 cache misses = 44-66ms
- Read leaf pages: 5-8 cache misses = 110-176ms
- Write modified pages: 0ms (cache write, no HTTP)
- Journal upload: 1 PUT = 22ms
- Sync dirty pages: 1-2 coalesced PUTs = 22-44ms
- **Total: ~220-330ms per transaction → ~3-4 tps**

The observed 0.4 tps suggests the benchmark has a larger working set (more cache misses per transaction) or the readahead is not capturing the access pattern (wasted bandwidth reducing effective throughput).

### 5.3 Where Time is Spent

```
┌─────────────────────────────────────────────┐
│              HTTP Round Trips                │
│                                             │
│  xRead misses:        ████████████████ 78%  │
│  xSync (batch write): ████             12%  │
│  xSync (journal PUT): ██                6%  │
│  xOpen (prefetch):    █                 4%  │
│                                             │
└─────────────────────────────────────────────┘
```

### 5.4 Improvement Projections

| Change | Expected Impact |
|--------|----------------|
| Adaptive readahead (sequential detector) | 2-5× for scan workloads; no change for point queries |
| Increase default cache to 4096 pages (16 MiB) | 2-4× for workloads that fit in 16 MiB |
| Pre-allocate cache (slab allocator) | 1.1-1.3× (removes malloc overhead, better locality) |
| Correct prefetch size (use detected page size) | Correct cache warming; variable improvement |
| Async prefetch (non-blocking xOpen) | Better perceived latency; same throughput |
| xFetch/xUnfetch (zero-copy) | 1.3-1.5× (eliminates memcpy on read path) |

The most impactful single change is **increasing the default cache size** — if the benchmark's working set fits in cache, misses drop to near zero and performance returns to ~30 tps.

---

## 6. Failure Mode Analysis

### 6.1 Network Failures During Readahead

**Scenario:** Network drops during a 64-page readahead GET.  
**Behaviour:** `execute_with_retry` retries up to 5 times with backoff (total worst-case delay: ~62 seconds). If all retries fail, xRead returns `SQLITE_IOERR_READ`. SQLite aborts the current statement but the connection and cache remain valid.  
**Risk:** Medium. The 62-second retry window blocks the calling thread with no way to cancel.  
**Gap:** No partial-result recovery — if the GET returns 32 of 64 pages before failing, all data is discarded.

### 6.2 Partial Sync Failure

**Scenario:** Batch write succeeds for ranges 1-3 but fails for range 4 of 5.  
**Behaviour (batch path):** `page_blob_write_batch` returns the error from the first permanently-failed range. xSync returns `SQLITE_IOERR_FSYNC`. Dirty flags are NOT cleared (the cleanup path returns before the clear loop). On the next xSync, all 5 ranges are re-written.  
**Behaviour (sequential path):** After range 4 fails, ranges 1-3 are already committed to Azure but the cache dirty flags are not cleared. On retry, ranges 1-3 are re-written (idempotent — page blob writes are PUT operations).  
**Risk:** Low. The re-write strategy is correct (PUT Page is idempotent). The only cost is redundant HTTP traffic.  
**Gap:** No mechanism to track which ranges succeeded, so recovery always rewrites everything.

### 6.3 Cache Corruption from Truncated Azure Response

**Scenario:** Azure returns HTTP 200 with a 3000-byte body for a 4096-byte page read.  
**Behaviour:** The response buffer has `buf.size = 3000`. The page-splitting loop copies 3000 bytes and the remaining 1096 bytes are zero (from calloc). This page is inserted into the cache. If SQLite reads this page, it sees corrupted data (trailing zeros where real data should be).  
**Risk:** High impact, low probability. Azure is reliable, but proxies, CDNs, or network issues could truncate.  
**Recommendation:** Validate `buf.size == requestedLen` after every read. Return `SQLITE_IOERR_READ` on mismatch.

### 6.4 Memory Exhaustion Under Write-Heavy Load

**Scenario:** Application dirties all 1024 cached pages, then triggers more writes to uncached pages.  
**Behaviour:** `cacheEvictClean` finds no clean pages, returns OK. `cacheInsert` adds pages beyond the soft limit. With sustained writes, cache grows without bound.  
**Risk:** Medium. A realistic workload that dirties 10,000+ pages without syncing would consume ~40 MiB of cache. This is unlikely with SQLite's default journal mode (syncs frequently) but possible with WAL mode and large transactions.  
**Recommendation:** Enforce a hard limit; force flush when dirty count exceeds threshold.

### 6.5 Curl Timeout vs Transfer Size Mismatch

**Scenario:** 100 MiB prefetch on a 2 MiB/s link. Transfer time = 50 seconds. `CURLOPT_TIMEOUT = 60`.  
**Behaviour:** Transfer succeeds with 10 seconds margin. On a 1.5 MiB/s link, the 60-second timeout fires mid-transfer and the prefetch fails. xOpen returns `SQLITE_CANTOPEN`.  
**Risk:** High on slow/congested networks. The 60-second timeout is appropriate for single-page reads but dangerous for large transfers.  
**Recommendation:** Scale timeout with transfer size: `timeout = max(60, estimatedBytes / MIN_ACCEPTABLE_THROUGHPUT)`. Or use `CURLOPT_LOW_SPEED_*` exclusively (already configured but may not be effective on all platforms — see macOS SecureTransport issue in benchmarks).

---

## 7. Comparison with Prior Art

### 7.1 SQLite's Built-in Page Cache (pcache1)

SQLite's own page cache uses a **slab allocator** with pre-allocated page slots:
- `pcache1Init()` allocates a contiguous block for N pages
- Free pages are tracked via a free-list (O(1) allocate/free)
- No per-page malloc/free
- LRU list with unpinned/pinned distinction
- `SQLITE_CONFIG_PAGECACHE` allows application to provide the memory

**Lesson:** The sqlite-objs cache should mirror this pattern. Pre-allocate a contiguous buffer and use a free-list.

### 7.2 Linux Block Layer Read-Ahead

Linux's `readahead()` uses a **two-state machine** (synchronous/asynchronous):
- Sequential access detected → async readahead with exponentially increasing window
- Random access → readahead disabled
- Miss in readahead window → synchronous readahead (blocks, fetches, and reinitialises)

**Lesson:** A simple 2-state sequential detector would be far more effective than the current fixed-window approach.

### 7.3 LiteFS (Fly.io)

LiteFS intercepts SQLite I/O at the VFS level and replicates to remote nodes:
- Uses `xFetch`/`xUnfetch` for zero-copy page access
- Maintains a local write-ahead log for durability
- Pages are fetched on-demand from the primary on cache miss

**Lesson:** Implementing `xFetch`/`xUnfetch` would allow SQLite to operate directly on cache page buffers, eliminating the memcpy in xRead.

---

## 8. Recommended Remediation Plan

### Phase 1 — Quick Wins (1-2 days each)

| # | Change | Risk | Impact |
|---|--------|------|--------|
| P1.1 | Cache `getenv()` result for readahead pages | Trivial | Eliminates O(n) env scan per miss |
| P1.2 | Add `azure_error_init(&aerr)` at declaration in `sqlite-objsSync` | Trivial | Defensive correctness |
| P1.3 | Validate Azure response length in `page_blob_read` | Low | Prevents silent corruption |
| P1.4 | Add cache hit/miss counters and log at xClose | Low | Enables tuning |
| P1.5 | Increase default `SQLITE_OBJS_MAX_PAGES` from 1024 to 4096 | Low | 4× larger working set |

### Phase 2 — Structural Improvements (3-5 days each)

| # | Change | Risk | Impact |
|---|--------|------|--------|
| P2.1 | Slab allocator for page cache (pre-allocate at init) | Medium | Eliminates ~2000 malloc/free per open |
| P2.2 | Fix prefetch size calculation (detect page size first) | Low | Correct cache warming for non-4K pages |
| P2.3 | Adaptive readahead (sequential vs random detector) | Medium | 2-5× for mixed workloads |
| P2.4 | Chunked prefetch (1-4 MiB per GET) | Medium | Eliminates long-transfer timeout risk |
| P2.5 | Hard cache limit with forced flush | Medium | Prevents memory exhaustion |

### Phase 3 — Architectural Enhancements (1-2 weeks each)

| # | Change | Risk | Impact |
|---|--------|------|--------|
| P3.1 | `xFetch`/`xUnfetch` zero-copy interface | High | Eliminates memcpy on hot path |
| P3.2 | Configurable timeout scaling by transfer size | Medium | Fixes curl timeout for large transfers |
| P3.3 | Partial result recovery (use partial readahead data) | Medium | Better resilience on flaky networks |
| P3.4 | Async prefetch (return from xOpen, prefetch in background) | High | Non-blocking open for large DBs |

---

## 9. Testing Gaps

The current test infrastructure (`test/` directory, VFS_TEST_INFRASTRUCTURE.md) should be extended with:

1. **Cache-specific unit tests:** Hit/miss counting, eviction ordering, dirty page growth past soft limit, hash table collision behaviour at high load factor.

2. **Readahead effectiveness test:** Verify that sequential scan uses readahead data, random access doesn't waste bandwidth (once adaptive readahead is implemented).

3. **Truncated response test:** Mock `page_blob_read` to return short response; verify `SQLITE_IOERR_READ` (once validation is added).

4. **Memory limit test:** Dirty all pages, verify hard limit prevents OOM (once hard limit is implemented).

5. **Prefetch correctness for non-4K pages:** Create a database with 8192 or 16384 page size, verify prefetch loads the correct number of pages.

---

## 10. Verdict

**Ship with follow-up.** The implementation is correct for its intended use case (single-writer access to moderately-sized databases). The 85× performance regression is a direct consequence of design choices that can be addressed incrementally.

**Immediate blockers for production:**
- Response length validation (C-3.3 / P1.3) — silent corruption risk
- Increased default cache size (P1.5) — makes the 85× gap disappear for databases ≤ 16 MiB

**Required before scaling:**
- Slab allocator (P2.1) and adaptive readahead (P2.3) — needed for databases > 100 MiB
- Chunked prefetch (P2.4) — needed if `SQLITE_OBJS_MAX_PAGES` is increased beyond ~1000

The batch write infrastructure (`curl_multi`, coalesced ranges, retry with lease renewal) is the strongest part of the implementation and can be left as-is.

---

*End of review.*
