# Prefetch Strategies Design

**Author:** Gandalf 🏗️  
**Status:** Draft  
**Date:** 2025-03-13

## Problem Statement

When azqlite opens an existing Azure-backed database, it must warm the
local page cache before queries can execute efficiently. Without
prefetching, every page access is a cold cache miss requiring an HTTP
GET (~22ms to Azure). A TPC-C benchmark with ~22,000 pages showed
~540ms per transaction due to random cache misses.

The current implementation downloads a fixed number of pages at open
time (`prefetch` URI parameter, default 1024 pages = 4MB). This is
insufficient for databases larger than the prefetch window.

Different workloads need different strategies:

| Workload | DB Size | Access Pattern | Ideal Prefetch |
|----------|---------|---------------|----------------|
| OLTP small | < cache | Random point lookups | Load entire DB |
| OLTP large | > cache | Random, hot working set | Load index pages |
| Analytics | Any | Sequential scans | Readahead handles it |
| Reconnect | Any | Same as last session | Reload previous working set |
| Latency-critical | Any | First query must be fast | Minimal, background warm |

## URI Parameter Design

```
file:db.db?vfs=azqlite&prefetch=<strategy>
```

### Strategy Values

| Value | Behavior |
|-------|----------|
| `off` | No prefetch. Only page 0 (header) is loaded at open. All pages fetched on demand via readahead. |
| `<N>` | Fixed: download the first N pages at open. Current behavior. Default: 1024 (4MB). |
| `all` | Download the entire database if it fits in the cache. Falls back to fixed prefetch if DB > cache. |
| `index` | Parse the SQLite btree structure and prefetch only interior (non-leaf) pages. These are the index and table root/branch pages that every query must traverse. |
| `warm` | Load pages from a locally-saved working set file (`.azqlite-warm`) written at close time. On first open (no warm file), falls back to `all` if DB fits, else `index`. |

### Default

`prefetch=1024` (backward compatible, 4MB warmup). Production
deployments should use `prefetch=all` or `prefetch=warm` depending on
DB size.

## Core Constraint: Minimize HTTP Round-Trips

HTTP latency to Azure is ~22ms per request regardless of payload size.
Bandwidth is cheap; round-trips are expensive. A single 90MB GET takes
~400ms (mostly transfer time), but 90 × 1MB GETs take ~2000ms (90 ×
22ms latency alone, plus transfer). **Every prefetch strategy must
download data in the fewest, largest possible HTTP requests.**

This means:
- **Never fetch one page at a time** during prefetch. Always coalesce
  into large contiguous range GETs.
- **Prefer 1 large GET over N small GETs.** A single GET of the entire
  DB is almost always faster than fetching ranges.
- **Parallel chunking** (splitting into 4 × 23MB chunks) only helps
  when the blob is large enough that transfer time dominates latency.
  For blobs < ~64MB, a single GET is faster than parallel chunks due
  to connection setup overhead.
- For `prefetch=index` where pages are scattered, **sort page numbers
  and coalesce into contiguous ranges**, then fetch each range in one
  GET. If the ranges are small and numerous, it may be cheaper to just
  download the entire enclosing region and discard unwanted pages.

**Rule of thumb:** If the gap between two needed pages is < 256 pages
(1MB), fetch the entire range including the gap. The wasted bandwidth
costs less than an extra HTTP round-trip.

## Strategy Details

### 1. `prefetch=off`

**Implementation:** Skip the prefetch block entirely. Still fetch page
0 for header detection. The adaptive readahead state machine handles
all subsequent fetches.

**Use case:** Large databases where only a small fraction is accessed,
or when the application has its own warmup logic.

**Complexity:** Trivial — guard the existing prefetch with
`if (prefetchPages > 0)`.

### 2. `prefetch=<N>` (current, enhanced)

**Implementation:** Download the first N pages (or the entire DB if
< N pages) in a single HTTP GET. Already implemented.

**Enhancement:** Accept byte-scale suffixes: `prefetch=64m` (64MB),
`prefetch=1g` (1GB). Internally converted to page count.

**Complexity:** Low — parsing enhancement only.

### 3. `prefetch=all`

**Implementation:**

```
if (blobSize <= cacheMaxBytes):
    fetchLen = blobSize           # entire DB
else:
    fetchLen = prefetchDefault    # fall back to fixed
```

Single HTTP GET of `fetchLen` bytes starting at offset 0. Same
cache-insertion loop as today.

For very large blobs, a single GET may be slow or fail. Consider
chunking into 64MB requests if `blobSize > 64MB` and downloading
in parallel using the curl multi handle.

**Parallel variant:** Split the blob into N chunks, issue N parallel
GETs via `curl_multi`, insert pages from each chunk into cache. This
uses the existing multi-handle infrastructure from `write_batch`.

**Performance:** For a 90MB DB, a single GET takes ~400ms (from the
timing data). Parallel 4×23MB chunks could complete in ~150ms.

**Complexity:** Medium. Single GET is trivial. Parallel chunking
requires a new `page_blob_read_parallel()` or reuse of multi-handle.

### 4. `prefetch=index`

**Implementation:**

SQLite's file format stores btree pages. Interior pages (non-leaf)
hold keys and child pointers. Leaf pages hold actual row data. For
index lookups, only interior pages need to be in memory — they
represent the "skeleton" of every table and index.

Algorithm:
1. Read page 1 (the schema table root page) — already prefetched
2. Parse `sqlite_master` to find root pages of all tables and indexes
3. For each root page, walk the btree structure:
   - If interior page: add to prefetch set, recurse into children
   - If leaf page: skip (don't prefetch)
4. Issue a single coalesced GET (or sorted range GETs) for the
   interior page set

**Estimation:** For a TPC-C W=1 database (~22,000 pages), the interior
pages are roughly sqrt(fanout × pages) ≈ a few hundred pages. At 4KB
each, that's ~1-2MB — but they're scattered throughout the file.

**Fetch strategy:** Sort the interior page numbers, coalesce into
contiguous ranges (bridging gaps < 256 pages to avoid extra GETs),
and issue one GET per range. For a typical B-tree layout, interior
pages cluster near the beginning of the file, so this often collapses
into 1-3 large range GETs. If the total span of interior pages is
< 50% of the DB, fetch ranges. Otherwise, just fetch the entire DB
(cheaper than many small GETs).

**SQLite page format (from file format docs):**
- Byte 0 of each page: page type flag
  - 0x02 = interior index btree
  - 0x05 = interior table btree
  - 0x0a = leaf index btree
  - 0x0d = leaf table btree
- Interior pages contain child page pointers (4 bytes each)

**Complexity:** High. Requires parsing SQLite btree page format.
However, the format is well-documented and stable. Could be implemented
incrementally: first pass does breadth-first traversal from page 1.

### 5. `prefetch=warm`

**Implementation:**

On `xClose`, write the set of cached page numbers to a local file:

```
.azqlite-warm/<blobname>.pages
```

Format: binary array of int32 page numbers, sorted. Header includes
the blob ETag or size at close time (for staleness detection).

On `xOpen`, if the warm file exists and the ETag matches:
1. Read the page number list
2. Sort into contiguous ranges, bridging gaps < 256 pages
3. If total span covers > 50% of DB, just GET the entire DB instead
4. Otherwise issue one large GET per coalesced range
5. Insert into cache

If the warm file is stale (ETag mismatch) or missing, fall back to
`all` (if DB fits) or `index`.

**Complexity:** Medium-high. File I/O at open/close, ETag tracking,
range coalescing. But the page-list format is simple.

**Cold start:** First open has no warm file → falls back. Second open
benefits from the saved working set.

## Implementation Priority

| Phase | Strategy | Effort | Impact |
|-------|----------|--------|--------|
| 1 | `off` | Trivial | Enables testing baseline |
| 1 | `all` (single GET) | Low | Fixes TPC-C immediately |
| 2 | `all` (parallel chunks) | Medium | Better for 100MB+ DBs |
| 2 | `index` | High | Best for large DBs that don't fit in cache |
| 3 | `warm` | Medium-high | Best for repeat connections |

**Phase 1** should ship first and unblock benchmarking.

## TPC-C Benchmark Integration

Add `--prefetch <strategy>` to `tpcc-azure` CLI. Pass through as URI
parameter:

```
./tpcc-azure --warehouses 1 --azure --duration 20 --wal --prefetch all
```

This appends `&prefetch=all` to the database URI.

## Observability

Add to the xClose readahead summary:

```
[PREFETCH] strategy=all fetched=22000 pages (89.8MB) in 412ms
```

Add FCNTL for querying prefetch stats:
- Pages prefetched at open
- Prefetch time (ms)
- Strategy used

## Open Questions

1. Should `prefetch=all` use parallel chunked GETs for large DBs?
   At what threshold? (Suggestion: >16MB → 4 parallel chunks)
2. For `prefetch=warm`, where should the warm file be stored?
   Current directory? XDG cache? Configurable?
3. Should `prefetch=index` also prefetch the first leaf page of each
   index (for range scan starts)?
4. Memory-mapped I/O: should `prefetch=all` for small DBs just mmap
   the downloaded buffer instead of copying into individual page
   allocations?
