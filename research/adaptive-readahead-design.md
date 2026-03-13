# Adaptive Readahead Design

**Author:** Gandalf (Lead Architect)  
**Date:** 2025-07-17  
**Status:** DRAFT — Awaiting Brady's Review  
**Addresses:** D-4.4 from `research/demand-paging-design-review.md`  
**Prerequisite reading:** `research/demand-paging-design-review.md` §4.1 (D-4.4), §5 (Performance Analysis), §7.2 (Linux Readahead)

---

## 1. Problem Statement

The current readahead implementation fetches a fixed number of pages (default 64) on every cache miss, regardless of access pattern. This is suboptimal for every workload:

| Access Pattern | What Happens Now | What Should Happen |
|---|---|---|
| Sequential scan (table scan, VACUUM) | 64-page readahead, adequate but not optimal | Readahead should grow to 1024+ pages, minimising HTTP GETs |
| Index B-tree traversal (root → internal → leaf) | 64-page readahead, ~60 pages wasted per miss | Readahead should be 1 page (demand-only) |
| Random point queries | 64-page readahead, pure waste | Readahead should be 1 page |
| Interleaved (scan then lookup) | 64-page readahead always | Should adapt per-phase |

**Impact with current 1GB default cache (262144 pages):**

Most databases are <1GB and will be fully cached after the xOpen prefetch. In this regime, readahead is irrelevant — there are no cache misses after warmup. Readahead matters primarily for:

1. **Databases larger than the cache** (>1GB with default settings)
2. **The initial warmup scan** when prefetch doesn't cover the full database
3. **Connections with smaller configured cache sizes**

For these cases, the cost is significant. A sequential scan of a 2GB database with 1GB cache requires demand-paging ~262,144 pages. At 64 pages per GET: **4,096 HTTP GETs × 22ms = 90 seconds** of pure latency. Adaptive readahead growing to 1024 pages per GET reduces this to **~280 GETs × ~35ms = ~10 seconds** — a **9× improvement**.

For index lookups on the same database, each B-tree traversal touches 3–5 scattered pages. Fixed 64-page readahead wastes 59–61 pages per miss, polluting the cache with unrequested data and accelerating eviction of useful pages. With demand-only (readahead=1), the same 3–5 pages cost the same latency but waste nothing.

---

## 2. State Machine

### 2.1 States

```
┌─────────────┐          ┌──────────────┐          ┌─────────────┐
│   INITIAL   │──miss──▶ │  SEQUENTIAL  │◀─────────│   RANDOM    │
│  (no history│          │  (growing    │ consecutive│  (demand-   │
│   yet)      │          │   window)    │   miss     │   only)     │
└─────────────┘          └──────┬───────┘          └──────┬──────┘
                                │                         │
                                │ non-sequential miss     │
                                └─────────────────────────┘
```

| State | Meaning | Readahead Window |
|---|---|---|
| `INITIAL` | No cache miss history for this file. Entered at xOpen. | N/A (no miss yet) |
| `SEQUENTIAL` | Consecutive misses follow a forward-sequential pattern. | Grows: 4 → 8 → 16 → 32 → 64 → 128 → 256 → 512 → 1024 |
| `RANDOM` | Last miss broke the sequential pattern. | Fixed at 1 (demand-only) |

### 2.2 Transitions

| From | To | Trigger | Action |
|---|---|---|---|
| `INITIAL` | `SEQUENTIAL` | First cache miss at any page P | Set window = `RA_INITIAL_WINDOW` (4). Fetch [P, P+4). |
| `SEQUENTIAL` | `SEQUENTIAL` | Miss at page P where P is a *sequential continuation* (see §3.2) | Double window: `W = min(W×2, RA_MAX_WINDOW)`. Fetch [P, P+W). |
| `SEQUENTIAL` | `SEQUENTIAL` | Miss at page P *inside* the last readahead window (eviction pressure) | Keep window unchanged. Fetch [P, P+W). |
| `SEQUENTIAL` | `RANDOM` | Miss at page P that is *not* a sequential continuation and not inside the window | Set window = 1. Fetch page P only. |
| `RANDOM` | `SEQUENTIAL` | Miss at page P where `P == lastMissPage + 1` (adjacent forward miss) | Set window = `RA_INITIAL_WINDOW` (4). Fetch [P, P+4). |
| `RANDOM` | `RANDOM` | Miss at page P where `P ≠ lastMissPage + 1` | Keep window = 1. Fetch page P only. |

### 2.3 State Diagram (ASCII)

```
                    miss at any P
    ┌─────────┐ ─────────────────────────► ┌─────────────┐
    │ INITIAL │                             │ SEQUENTIAL  │
    └─────────┘                             │  W=4        │
                                            └──────┬──────┘
                                                   │
                              ┌─────────────────────┤
                              │                     │
                    sequential continuation    non-sequential
                    (P ≈ L+W)                  (P far from L+W)
                              │                     │
                              ▼                     ▼
                       ┌─────────────┐       ┌─────────────┐
                       │ SEQUENTIAL  │       │   RANDOM    │
                       │  W=min(2W,  │       │   W=1       │
                       │     MAX)    │       └──────┬──────┘
                       └──────┬──────┘              │
                              │               P == L+1
                              │               (adjacent)
                              │                     │
                              │              ┌──────┘
                              │              ▼
                              │       ┌─────────────┐
                              │       │ SEQUENTIAL  │
                              │       │  W=4 (reset)│
                              │       └─────────────┘
                              │
                      continues growing
                      until MAX or pattern breaks
```

---

## 3. Algorithm

### 3.1 State Data

Added to the `sqlite-objsFile` struct:

```c
/* Adaptive readahead state — per-file, per-connection */
typedef struct sqlite_objs_readahead {
    int state;           /* RA_INITIAL, RA_SEQUENTIAL, or RA_RANDOM    */
    int lastMissPage;    /* Page number of last cache miss (-1 = none) */
    int window;          /* Current readahead window (pages)           */
} sqlite_objs_readahead_t;
```

**Size:** 12 bytes. Negligible overhead per connection.

**Lifetime:** Initialised at xOpen (state=INITIAL, lastMissPage=-1, window=0). No cleanup needed at xClose.

### 3.2 Sequential Continuation Detection

The key question: "Is this miss at page P a continuation of a sequential scan?"

After a readahead of W pages starting at page L, all pages [L, L+W) should be in cache. In a sequential scan, the next miss should be at page L+W — the first page *not* covered by readahead.

However, some pages near L+W may already be cached (from the xOpen prefetch, a prior readahead, or a previous query). When that happens, the actual next miss is at L+W+K for some small K.

**Detection rule:** The miss at P is a sequential continuation if:

```
P ≥ (L + W)  AND  P ≤ (L + W + tolerance)

where tolerance = max(W / 4, 4)
```

This allows a gap of up to 25% of the window size (minimum 4 pages) to account for pre-cached pages. The tolerance is proportional to the window because larger windows make larger gaps more likely.

**Examples:**

| Last miss (L) | Window (W) | Tolerance | Sequential range | Miss at P | Classification |
|---|---|---|---|---|---|
| 100 | 4 | 4 | [104, 108] | 104 | Sequential ✓ |
| 100 | 4 | 4 | [104, 108] | 107 | Sequential ✓ (3 pre-cached pages) |
| 100 | 4 | 4 | [104, 108] | 200 | Random ✗ |
| 100 | 64 | 16 | [164, 180] | 164 | Sequential ✓ |
| 100 | 64 | 16 | [164, 180] | 170 | Sequential ✓ (6 pre-cached pages) |
| 100 | 64 | 16 | [164, 180] | 300 | Random ✗ |
| 100 | 256 | 64 | [356, 420] | 356 | Sequential ✓ |
| 100 | 256 | 64 | [356, 420] | 400 | Sequential ✓ (44 pre-cached pages) |
| 100 | 256 | 64 | [356, 420] | 500 | Random ✗ |

### 3.3 Inside-Window Miss Detection

If P falls inside the last readahead window (L < P < L+W), a readahead page was evicted before being read. This indicates cache pressure but the access pattern is still forward. We stay in SEQUENTIAL but do not grow the window — cache pressure means larger readahead would be counterproductive.

### 3.4 Complete Pseudocode

```c
/*
** Determine readahead window for a cache miss at page P.
** Updates state in-place. Returns the number of pages to readahead.
*/
static int readaheadOnMiss(sqlite_objs_readahead_t *ra, int P, int maxWindow, int maxPages) {
    int L = ra->lastMissPage;
    int W = ra->window;
    int result;

    /* Cap readahead to cache capacity (existing guard) */
    if (maxWindow > maxPages) maxWindow = maxPages;

    if (ra->state == RA_INITIAL) {
        /* First miss — begin with small probe window */
        result = RA_INITIAL_WINDOW;
        if (result > maxWindow) result = maxWindow;
        ra->state = RA_SEQUENTIAL;
        ra->lastMissPage = P;
        ra->window = result;
        return result;
    }

    int distance = P - L;

    /* Case 1: Inside the last readahead window (eviction pressure) */
    if (distance > 0 && distance < W) {
        /* Stay sequential, keep window, don't grow */
        ra->lastMissPage = P;
        /* ra->window unchanged */
        /* ra->state unchanged (stays SEQUENTIAL) */
        result = W;
        if (result > maxWindow) result = maxWindow;
        return result;
    }

    /* Case 2: Sequential continuation — at or just past the window boundary */
    int tolerance = W / 4;
    if (tolerance < 4) tolerance = 4;

    if (distance >= W && distance <= W + tolerance) {
        /* Sequential! Double the window. */
        int newW = W * 2;
        if (newW > maxWindow) newW = maxWindow;
        if (newW < RA_INITIAL_WINDOW) newW = RA_INITIAL_WINDOW;
        ra->state = RA_SEQUENTIAL;
        ra->lastMissPage = P;
        ra->window = newW;
        return newW;
    }

    /* Case 3: Adjacent forward miss from RANDOM — new sequence detected */
    if (distance == 1 && ra->state == RA_RANDOM) {
        result = RA_INITIAL_WINDOW;
        if (result > maxWindow) result = maxWindow;
        ra->state = RA_SEQUENTIAL;
        ra->lastMissPage = P;
        ra->window = result;
        return result;
    }

    /* Case 4: Non-sequential — random access */
    ra->state = RA_RANDOM;
    ra->lastMissPage = P;
    ra->window = 1;
    return 1;
}
```

### 3.5 Constants

```c
#define RA_INITIAL       0    /* No miss history          */
#define RA_SEQUENTIAL    1    /* Forward-sequential mode  */
#define RA_RANDOM        2    /* Demand-only mode         */

#define RA_INITIAL_WINDOW   4    /* Starting window for new sequences (pages) */
#define RA_MAX_WINDOW    1024    /* Maximum readahead window (pages = 4 MiB at 4K page size) */
```

**Rationale for RA_INITIAL_WINDOW = 4:**
- Small enough to not waste much on false sequential detection (4 pages = 16 KiB at 4K)
- Large enough to confirm a sequential pattern after 2–3 consecutive misses
- Linux starts at 4 pages (or the detected sequential-read request size)

**Rationale for RA_MAX_WINDOW = 1024:**
- At 4K page size: 1024 × 4096 = 4 MiB per GET
- HTTP latency budget: 22ms base + ~20ms transfer (at 200 MB/s) = ~42ms per GET
- Effective throughput: 4 MiB / 42ms ≈ 95 MB/s — near Azure Page Blob maximum
- Doubling to 2048 (8 MiB) saves only ~7ms per GET but doubles wasted bandwidth on false sequential detection
- The ramp from 4 → 1024 takes 8 doublings = 8 cache misses = ~8 × 22ms = ~176ms

### 3.6 Ramp-Up Profile

For a sequential scan starting from a cold cache:

| Miss # | Window (pages) | Pages fetched | Cumulative pages | Cumulative HTTP GETs |
|---|---|---|---|---|
| 1 | 4 | 4 | 4 | 1 |
| 2 | 8 | 8 | 12 | 2 |
| 3 | 16 | 16 | 28 | 3 |
| 4 | 32 | 32 | 60 | 4 |
| 5 | 64 | 64 | 124 | 5 |
| 6 | 128 | 128 | 252 | 6 |
| 7 | 256 | 256 | 508 | 7 |
| 8 | 512 | 512 | 1020 | 8 |
| 9 | 1024 | 1024 | 2044 | 9 |
| 10+ | 1024 | 1024 | 3068+ | 10+ |

After 9 GETs (~200ms total latency), the readahead reaches maximum. From that point, every GET fetches 4 MiB. A 1GB sequential scan at max window: 262,144 pages / 1024 pages per GET ≈ **256 GETs** (plus 9 ramp-up GETs) ≈ **265 GETs × ~35ms ≈ 9.3 seconds**.

Compare to current fixed 64-page readahead: 262,144 / 64 = **4,096 GETs × ~23ms ≈ 94 seconds**.

---

## 4. Integration with Existing Code

### 4.1 Struct Changes

```c
/* In sqlite-objsFile (src/sqlite_objs_vfs.c, ~line 297): */
typedef struct sqlite-objsFile {
    /* ... existing fields ... */

    /* Adaptive readahead state (per-file, per-connection) */
    sqlite_objs_readahead_t readahead;

    /* Readahead configuration */
    int readaheadMode;       /* 0=auto (adaptive), >0=fixed N pages */
    int readaheadMaxWindow;  /* Max window for adaptive mode */
} sqlite-objsFile;
```

### 4.2 xRead Modification

The change is surgical — replace the fixed `cacheGetReadaheadPages()` call with the adaptive state machine. The readahead fetch loop (lines 693–748 of current code) is unchanged.

```c
/* CURRENT CODE (sqlite_objs_vfs.c:693): */
int readaheadPages = cacheGetReadaheadPages();

/* REPLACEMENT: */
int readaheadPages;
if (p->readaheadMode > 0) {
    /* Fixed mode — user-specified via URI parameter */
    readaheadPages = p->readaheadMode;
} else {
    /* Adaptive mode — consult state machine */
    readaheadPages = readaheadOnMiss(
        &p->readahead, pageNo,
        p->readaheadMaxWindow, p->cache.maxPages);
}
```

**Everything else in the readahead fetch path remains identical.** The existing code already:
- Caps readahead at cache capacity (line 696–697)
- Caps at total pages / EOF (line 698–700)
- Computes fetch offset and length (line 702–705)
- Issues the single GET (line 711)
- Splits response into pages, skips already-cached pages (line 723–746)
- Evicts clean pages as needed (line 741–743)

None of this changes. Only the *number of pages* in the readahead window changes.

### 4.3 xOpen Initialisation

```c
/* In sqlite-objsOpen, after cache init: */
p->readahead.state = RA_INITIAL;
p->readahead.lastMissPage = -1;
p->readahead.window = 0;
p->readaheadMode = 0;            /* adaptive by default */
p->readaheadMaxWindow = RA_MAX_WINDOW;

/* Parse URI parameters (see §5) */
const char *raParam = sqlite3_uri_parameter(zName, "readahead");
if (raParam) {
    if (strcmp(raParam, "auto") == 0 || strcmp(raParam, "adaptive") == 0) {
        p->readaheadMode = 0;   /* adaptive */
    } else {
        int val = atoi(raParam);
        if (val >= 0) p->readaheadMode = val;  /* fixed or 0=disabled */
    }
}
const char *raMaxParam = sqlite3_uri_parameter(zName, "readahead_max");
if (raMaxParam) {
    int val = atoi(raMaxParam);
    if (val > 0) p->readaheadMaxWindow = val;
}
```

### 4.4 xSync Interaction

**No changes to xSync.** The readahead state machine operates entirely on the read path. Dirty page coalescing and batch writes are orthogonal.

One consideration: after xSync clears dirty flags, subsequent reads hit cached (now clean) pages. The readahead state is unaffected because it only tracks *misses*, not hits.

### 4.5 Removal of getenv() Calls

This change also addresses **D-4.3** (getenv called on every miss). The `cacheGetReadaheadPages()` function and `cacheGetMaxPages()` are replaced by per-file configuration parsed once at xOpen from URI parameters. No more `getenv()` on the hot path.

```c
/* DELETE these functions: */
static int cacheGetReadaheadPages(void) { ... }  /* lines 137-144 */
static int cacheGetMaxPages(void) { ... }         /* lines 128-135 */
```

Replace with URI parameter parsing in xOpen (see §5).

---

## 5. Configuration

### 5.1 URI Parameters

All configuration via [SQLite URI filenames](https://www.sqlite.org/uri.html). No environment variables (per team directive).

| Parameter | Type | Default | Description |
|---|---|---|---|
| `readahead` | `auto` or integer | `auto` | `auto` = adaptive state machine. Integer N = fixed N-page readahead. `0` = demand-only (no readahead). |
| `readahead_max` | integer | 1024 | Maximum readahead window for adaptive mode (pages). Ignored in fixed mode. |
| `cache_pages` | integer | 262144 | Maximum cache pages (replaces `SQLITE_OBJS_CACHE_PAGES` env var). |

**Usage examples:**

```sql
-- Adaptive readahead (default, recommended)
.open file:mydb.db?vfs=sqlite-objs

-- Fixed 64-page readahead (legacy behaviour)
.open file:mydb.db?vfs=sqlite-objs&readahead=64

-- Demand-only, no readahead (for pure random workloads)
.open file:mydb.db?vfs=sqlite-objs&readahead=0

-- Aggressive readahead for known-sequential workloads
.open file:mydb.db?vfs=sqlite-objs&readahead_max=4096

-- Small cache with adaptive readahead
.open file:mydb.db?vfs=sqlite-objs&cache_pages=1024
```

### 5.2 sqlite3_file_control Interface

For programmatic control from application code:

```c
#define SQLITE_OBJS_FCNTL_READAHEAD_MODE  (SQLITE_FCNTL_SIZE + 100)
#define SQLITE_OBJS_FCNTL_READAHEAD_STATS (SQLITE_FCNTL_SIZE + 101)

/* Set readahead mode */
int mode = 0; /* 0=adaptive, >0=fixed N pages */
sqlite3_file_control(db, "main", SQLITE_OBJS_FCNTL_READAHEAD_MODE, &mode);

/* Read readahead statistics */
typedef struct sqlite_objs_readahead_stats {
    int currentState;     /* RA_INITIAL, RA_SEQUENTIAL, RA_RANDOM */
    int currentWindow;    /* Current readahead window (pages) */
    int lastMissPage;     /* Last miss page number */
    int totalMisses;      /* Total cache misses since xOpen */
    int sequentialMisses; /* Misses classified as sequential */
    int randomMisses;     /* Misses classified as random */
} sqlite_objs_readahead_stats_t;

sqlite_objs_readahead_stats_t stats;
sqlite3_file_control(db, "main", SQLITE_OBJS_FCNTL_READAHEAD_STATS, &stats);
```

### 5.3 Migration from Environment Variables

| Old (env var) | New (URI parameter) | Notes |
|---|---|---|
| `SQLITE_OBJS_READAHEAD_PAGES=64` | `readahead=64` | Exact equivalent |
| `SQLITE_OBJS_CACHE_PAGES=4096` | `cache_pages=4096` | Exact equivalent |
| (no equivalent) | `readahead=auto` | New adaptive mode (default) |
| (no equivalent) | `readahead_max=2048` | New max window control |

---

## 6. Edge Cases

### 6.1 End of File

**Scenario:** Sequential scan reaches the last few pages of the database.

**Behaviour:** The existing EOF guard (line 698–700) caps `endPage` at `totalPages`. If the readahead window extends past EOF, fewer pages are fetched. The state machine is unaffected — it doesn't know or care about EOF. The next miss (if any) will be classified normally.

**Example:** Database has 1000 pages. Sequential scan reaches page 990 with window=64. Readahead requests [990, 1000) — only 10 pages, not 64. Next miss won't happen (scan is complete). State persists but is idle.

**No special handling needed.**

### 6.2 Interleaved Sequential and Random Access

**Scenario:** Application does a table scan, then several point queries, then another table scan on a different range.

**Behaviour:**
1. Table scan: State ramps from INITIAL → SEQUENTIAL (window grows to max)
2. First point query: Miss at non-sequential page → state transitions to RANDOM (window=1)
3. Subsequent point queries: Stay in RANDOM (window=1, no waste)
4. Second table scan: First miss → RANDOM (window=1), second miss at adjacent page → SEQUENTIAL restart (window=4), ramps up again

**Cost of re-ramp:** 9 GETs to reach max window = ~200ms. Acceptable for a new scan phase.

**Optimisation deferred:** Could maintain a "sequential memory" (keep the last sequential state and restore it if a sequential pattern resumes). This adds complexity for marginal benefit — re-ramp cost is small relative to scan duration.

### 6.3 Very Small Databases (< 1 page of readahead)

**Scenario:** Database is 10 pages (40 KiB). Default cache holds 262,144 pages.

**Behaviour:** The xOpen prefetch loads the entire database into cache. Zero cache misses during operation. Readahead state stays INITIAL forever. No overhead.

**Even with a tiny cache (e.g., `cache_pages=4`):** First miss triggers readahead of 4 pages. That's the entire database. Second miss (if any) fetches the remaining pages. Total: 2–3 GETs for the whole database. The adaptive state machine works correctly but provides minimal value — the database is too small for readahead strategy to matter.

### 6.4 Single-Page Database (SQLite Header Only)

**Scenario:** Newly created database, only page 0 exists.

**Behaviour:** Prefetch loads page 0. All reads hit cache. No misses, no readahead activity. Works correctly.

### 6.5 Cache Fully Dirty (All Pages Modified Before Sync)

**Scenario:** Application modifies every cached page. On the next read, all pages are dirty (cannot be evicted). Readahead would insert new pages, growing the cache past its soft limit.

**Behaviour:** The existing `cacheEvictClean()` already allows growth past the soft limit when all pages are dirty (line 257–258). Adaptive readahead doesn't change this — it only changes *how many* pages are requested, not the eviction policy.

**Consideration:** In RANDOM mode (window=1), cache growth is minimal (1 page per miss). In SEQUENTIAL mode, larger readahead could grow the cache significantly. The existing cap (`readaheadPages > p->cache.maxPages` guard) limits this. This is acceptable — if the user configured a cache size, readahead respects it.

### 6.6 Database Grows During Operation

**Scenario:** Another connection (or the same connection) adds pages to the database. New pages are beyond the current blobSize.

**Behaviour:** `blobSize` is updated on xWrite when extending. On xRead, if `offset >= blobSize`, short read is returned. The readahead state machine doesn't know about database size — it operates purely on miss page numbers. New pages at the end are fetched normally on miss. No issue.

### 6.7 Backward Sequential Scans (ORDER BY ... DESC)

**Scenario:** Query does `SELECT * FROM t ORDER BY id DESC`, causing SQLite to traverse B-tree leaves in reverse.

**Behaviour:** Backward misses (P < lastMissPage) are classified as RANDOM (window=1). This is suboptimal — backward scans would benefit from readahead — but acceptable for v1.

**Why not handle it now:** Backward sequential detection doubles the state machine complexity (need to track direction, maintain separate windows, handle direction changes). SQLite's B-tree leaf pages are doubly-linked, so a backward scan still touches pages in a predictable pattern, but the benefit is modest: backward scans are rare compared to forward scans, and they're typically satisfied from cache after the forward scan that built the index.

**Future work:** Add `RA_SEQUENTIAL_BACKWARD` state with mirrored logic. Or, detect direction from the sign of `P - lastMissPage` and maintain symmetric state.

### 6.8 Multiple Attached Databases

**Scenario:** `ATTACH 'db2.db' AS aux;` — two databases on the same connection.

**Behaviour:** Each attached database has its own `sqlite-objsFile` with its own `readahead` state. The state machines are completely independent. A sequential scan on `main` doesn't affect readahead for `aux`. This is correct — access patterns are per-file.

### 6.9 Interleaved Reads and Writes to the Same Pages

**Scenario:** Read page 100, write page 100, read page 101, write page 101, ...

**Behaviour:** Writes go through the cache (xWrite updates the cached page and marks dirty). Subsequent reads hit the dirty cache entry. No cache miss → no readahead state change. The state machine only sees misses, which in this pattern are sequential (each first-read of a new page is a miss). Works correctly.

---

## 7. Performance Projections

All projections assume 4 KiB page size, 22ms base HTTP latency, 200 MB/s Azure bandwidth.

### 7.1 Sequential Scan (Table Scan, VACUUM, Backup)

**Database size:** 2 GB (524,288 pages). **Cache:** 1 GB (262,144 pages).  
**Pages to demand-page:** 262,144 (half the database, after 1 GB prefetch).

| Strategy | Window | HTTP GETs | Time (latency) | Time (latency + transfer) | Bandwidth used |
|---|---|---|---|---|---|
| Current (fixed 64) | 64 | 4,096 | 90.1s | 94.2s | 1 GB (all useful) |
| Adaptive (max 256) | 4→256 | ~1,032 | 22.7s | 27.9s | 1 GB (all useful) |
| Adaptive (max 1024) | 4→1024 | ~265 | 5.8s | 11.1s | 1 GB (all useful) |
| Adaptive (max 4096) | 4→4096 | ~72 | 1.6s | 7.3s | 1 GB (all useful) |

**Projected improvement:** **9–15× fewer HTTP GETs** with max window 1024.

### 7.2 Index B-Tree Lookup (Point Query)

**Pattern:** Root page → 1–2 internal pages → leaf page. 3–4 pages per query, scattered.

| Strategy | Readahead per miss | Pages fetched per query | HTTP GETs per query | Wasted pages |
|---|---|---|---|---|
| Current (fixed 64) | 64 | 192–256 | 3–4 | 188–252 (98%) |
| Adaptive | 1 (RANDOM) | 3–4 | 3–4 | 0 (0%) |

**Latency:** Same (3–4 × 22ms = 66–88ms either way). The improvement is **bandwidth efficiency** (98% waste eliminated) and **cache pollution** (no useless pages evicting useful ones).

### 7.3 Mixed Workload (OLTP: 70% Point Queries, 30% Range Scans)

**Scenario:** TPC-C-like workload on a 2 GB database.

| Strategy | Point query cost | Range scan cost | Overall |
|---|---|---|---|
| Current (fixed 64) | 66–88ms (wasteful) | 90s total (scan phase) | Steady 3–5 tps |
| Adaptive | 66–88ms (efficient) | ~10s total (scan phase) | Steady 5–8 tps initially, improving as cache warms |

**Key insight:** With 1 GB cache, repeated point queries quickly populate the hot set. After warmup, most point queries are cache hits regardless of readahead strategy. The adaptive improvement is primarily during the **warmup phase** and for **range scans** that exceed cache.

### 7.4 Summary

| Workload | Current | Adaptive | Improvement |
|---|---|---|---|
| Sequential scan (>cache) | Slow (fixed 64-page GETs) | Fast (growing to 1024-page GETs) | **9–15× fewer GETs** |
| Index point queries | Wasteful (64 pages per miss) | Efficient (1 page per miss) | **98% less wasted bandwidth** |
| Mixed OLTP | Adequate | Better cache utilisation | **1.5–2× during warmup** |
| Small DB (fits in cache) | Fine (all cached after open) | Fine (all cached after open) | **No change** |

---

## 8. Comparison with Linux Readahead

### 8.1 Linux Block Layer Readahead (`mm/readahead.c`)

Linux maintains per-file readahead state with three key fields:

```c
struct file_ra_state {
    pgoff_t start;           /* Start of current readahead window */
    unsigned int size;       /* Total pages in current window */
    unsigned int async_size; /* Async (prefetch) portion of window */
    /* ... */
};
```

**States:**
1. **Initial readahead:** First sequential read triggers synchronous readahead (blocks until pages are available).
2. **Async readahead:** When the application reads into the "async marker" region (the last `async_size` pages of the window), Linux triggers background readahead for the *next* window — overlapping I/O with computation.
3. **Random:** Non-sequential reads disable readahead entirely.

**Growth:** Window doubles on each sequential hit, capped at `ra_pages` (default 128 KiB = 32 pages at 4 KiB, or 256 KiB on many distros).

**Thrashing detection:** If pages in the readahead window are evicted before being read (detected via page flags), Linux reduces the window.

### 8.2 How sqlite-objs Differs and Why

| Aspect | Linux | sqlite-objs | Rationale |
|---|---|---|---|
| **Async readahead** | Background I/O via `submit_bio()` + page cache | Not supported (synchronous HTTP only) | sqlite-objs has no background I/O thread. HTTP GET blocks the calling thread. Async would require curl_multi or threads — significant complexity for read path. |
| **Async marker** | Triggers next readahead before current window is exhausted | N/A | Without async I/O, there's no benefit to early triggering. |
| **Window growth** | Doubles per sequential hit | Doubles per sequential miss (same effect) | In Linux, "sequential hit" means the page was in the readahead window. In sqlite-objs, we detect the same pattern by the miss being at the window boundary. |
| **Maximum window** | 128–256 KiB (32–64 pages) | 4 MiB (1024 pages) | Linux's small window reflects ~0.1ms disk latency. sqlite-objs's 22ms HTTP latency demands larger windows to amortise cost. The optimal window is proportional to `latency × bandwidth`. |
| **Thrashing detection** | Reduces window on eviction-before-read | Inside-window miss → don't grow | Same intent, different mechanism. sqlite-objs detects eviction pressure when a miss occurs inside the current readahead window. |
| **Backward detection** | Separate backward sequential detection | Not in v1 (treated as random) | Linux sees backward scans frequently (e.g., `tail -r`). SQLite backward scans are rare enough to defer. |
| **Per-file state** | `struct file_ra_state` per `struct file` | `sqlite_objs_readahead_t` per `sqlite-objsFile` | Same granularity. |
| **Direction handling** | Tracks forward and backward | Forward only (v1) | Simplicity; backward scans are rare in SQLite workloads. |

### 8.3 Why Not Copy Linux Exactly?

1. **No async I/O primitive.** Linux's key innovation is *overlapping* I/O with computation via the async marker. sqlite-objs would need a dedicated I/O thread or curl_multi integration on the read path. This is significant complexity (comparable to D-PERF Phase 2 for writes) and is better addressed by the long-term `xFetch`/`xUnfetch` plan.

2. **Different latency regime.** Linux readahead optimises for ~0.1ms SSD / ~5ms HDD latency. sqlite-objs faces 22ms minimum HTTP latency. This means:
   - Larger windows are needed (the cost of an HTTP GET is dominated by latency, not transfer size)
   - False sequential detection is more expensive (one wasted 4 MiB GET = 42ms, vs. one wasted 256 KiB read = 0.15ms on SSD)
   - The optimal initial window is smaller (to avoid waste on false detection)

3. **Simpler page cache model.** Linux has page-level eviction flags, page locks, and writeback state. sqlite-objs's cache is simpler (entry = clean or dirty, evict = remove). The simpler model means simpler detection logic suffices.

---

## 9. Observability

### 9.1 Readahead Statistics (Addresses D-4.7)

Add counters to `sqlite_objs_readahead_t`:

```c
typedef struct sqlite_objs_readahead {
    /* State machine (see §3.1) */
    int state;
    int lastMissPage;
    int window;

    /* Counters */
    int nMisses;             /* Total cache misses */
    int nSequentialMisses;   /* Misses classified as sequential */
    int nRandomMisses;       /* Misses classified as random */
    int nWindowGrows;        /* Times the window doubled */
    int nWindowResets;       /* Times the window reset to 1 */
    int peakWindow;          /* High-water mark of window size */
} sqlite_objs_readahead_t;
```

### 9.2 Debug Logging

When `SQLITE_OBJS_DEBUG_TIMING` is enabled, log state transitions:

```
[READAHEAD] miss page=1024 state=SEQUENTIAL window=64→128 (sequential continuation)
[READAHEAD] miss page=5000 state=RANDOM window=128→1 (non-sequential jump from 1152)
[READAHEAD] miss page=5001 state=SEQUENTIAL window=1→4 (new sequence detected)
```

### 9.3 Summary at xClose

```
[READAHEAD] Summary: 342 misses (298 sequential, 44 random), peak window=1024, 7 resets
```

---

## 10. Implementation Plan

### Phase 1: Core State Machine (1 day)

1. Add `sqlite_objs_readahead_t` to `sqlite-objsFile` struct
2. Implement `readaheadOnMiss()` function
3. Replace `cacheGetReadaheadPages()` call in xRead with state machine
4. Initialise state in xOpen
5. Unit tests: verify state transitions for sequential, random, interleaved, EOF patterns

### Phase 2: Configuration Migration (0.5 days)

1. Add `readahead`, `readahead_max`, `cache_pages` URI parameter parsing in xOpen
2. Remove `cacheGetReadaheadPages()` and `cacheGetMaxPages()` functions (delete `getenv()` calls)
3. Add `SQLITE_OBJS_FCNTL_READAHEAD_MODE` handling in xFileControl
4. Update any tests that set `SQLITE_OBJS_READAHEAD_PAGES` or `SQLITE_OBJS_CACHE_PAGES` env vars

### Phase 3: Observability (0.5 days)

1. Add counters to readahead state
2. Add `SQLITE_OBJS_FCNTL_READAHEAD_STATS` handling
3. Debug logging for state transitions
4. Summary logging at xClose

### Phase 4: Validation (1 day)

1. Benchmark: sequential scan with adaptive vs fixed (expect 9–15× improvement)
2. Benchmark: point queries with adaptive vs fixed (expect same latency, less waste)
3. Benchmark: mixed workload (expect 1.5–2× improvement during warmup)
4. Regression: existing test suite must pass unchanged
5. Edge case tests: EOF, tiny DB, interleaved patterns

**Total estimate: 3 days.**

---

## 11. Risks and Mitigations

| Risk | Probability | Impact | Mitigation |
|---|---|---|---|
| False sequential detection wastes bandwidth | Medium | Low (max 4 MiB per false positive) | Small initial window (4 pages = 16 KiB waste). Quick fallback to RANDOM on non-sequential miss. |
| Ramp-up too slow for large sequential scans | Low | Medium (200ms ramp to max) | 200ms is small relative to multi-second scans. Could start with larger initial window (16 pages) if benchmarks show it helps. |
| Inside-window misses cause stalls | Low | Low | Keeping window unchanged (not shrinking) is safe. The miss is served with the current window size. |
| URI parameter parsing bugs | Low | High | Defensive parsing with fallback to defaults. Existing `sqlite3_uri_parameter` is well-tested SQLite API. |
| Regression in existing tests using env vars | Medium | Medium | Phase 2 updates all affected tests. Can keep env var support as deprecated fallback if migration is risky. |

---

## 12. Future Work (Not In This Design)

1. **Async readahead via curl_multi on read path.** Would allow overlapping I/O with SQLite's page processing. High complexity but would bring sqlite-objs closer to Linux's async readahead performance.

2. **Backward sequential detection.** Add `RA_SEQUENTIAL_BACKWARD` state for ORDER BY ... DESC workloads.

3. **xFetch/xUnfetch (memory-mapped I/O interface).** SQLite's v3 io_methods. Would allow SQLite to directly reference cached page buffers, eliminating the xRead memcpy entirely. Requires careful lifetime management (pages must not be evicted while "fetched"). This is the long-term recommendation from D-4.4.

4. **Multi-stream detection.** Track multiple independent sequential streams (e.g., scan on table A + scan on table B interleaved by SQLite's query executor). Requires a small array of readahead states, matched by page range.

5. **Prefetch-aware readahead.** After xOpen prefetch, the state machine knows the prefetch boundary. Could start the first sequential readahead window at the prefetch boundary instead of INITIAL, saving the ramp-up.

---

## 13. Decision Record

**D-READAHEAD: Adaptive readahead state machine for demand-paging cache.**

- **Date:** 2025-07-17
- **Author:** Gandalf (Lead Architect)
- **Status:** PROPOSED — awaiting Brady's review
- **Addresses:** D-4.4 (readahead not adaptive) and D-4.3 (getenv on every miss)

**Decision:**
Replace the fixed-window readahead (default 64 pages) with a 3-state adaptive state machine (INITIAL / SEQUENTIAL / RANDOM) that doubles the readahead window on sequential cache misses (4 → 1024 pages) and drops to demand-only (1 page) on random access. Configuration moves from environment variables to URI parameters per team directive.

**Rationale:**
The fixed 64-page readahead is suboptimal for every workload. Sequential scans need larger windows (fewer HTTP GETs). Random/index access needs smaller windows (less waste). The 2-state detector (sequential vs random) captures most of the benefit with minimal complexity, following the proven Linux readahead model adapted for HTTP latency.

**Constraints:**
- No environment variables (team directive)
- No background threads on read path (architectural constraint)
- Must not change xSync or write path
- Must be backward-compatible (fixed mode available via URI parameter)

**Approved by:** _Pending Brady's review_
