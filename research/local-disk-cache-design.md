# Local Disk Cache Design

**Author:** Gandalf 🏗️  
**Status:** Draft  
**Date:** 2025-03-13

## Problem Statement

When sqlite-objs opens an Azure-backed database, it must download pages from
blob storage before any reads can proceed. Even with `prefetch=all`, a
90MB database takes several seconds to download. On reconnect from the
same machine, the entire download repeats — even if nothing changed.

This is the dominant cost in the common workflow: a developer or service
connects to the same database repeatedly from the same machine. Every
connection pays the full download cost.

**Goal:** Make reconnection essentially instant when the remote database
hasn't changed, and make first-connection as fast as the network allows.

## Design Overview

Replace the in-memory LRU page cache with a **persistent memory-mapped
local disk cache**. The OS virtual memory subsystem manages which pages
are resident in physical RAM — we don't need to size buffers or
implement eviction.

Two memory-mapped files per database:

1. **Metadata file** (`.meta`): ETag, bitmaps, checksums, header
2. **Page store file** (`.pages`): Raw page data, indexed by page number

A **background prefetch thread** lazily downloads missing or stale pages
after open. An FCNTL allows the caller to wait for prefetch completion
or query progress. This single mechanism serves all use cases: callers
who want instant availability call the FCNTL wait immediately after open;
callers who want low-latency open skip the wait and let demand-loading
handle immediate needs while the background thread fills in the rest.

### Key Invariants

1. **The blob is the source of truth.** The local cache is always
   expendable — it can be deleted or invalidated at any time and
   rebuilt from the blob.
2. **No pointers in mapped files.** All references within the mmap
   regions use byte offsets or page indices. This makes remapping safe
   when files grow — no pointer fixup needed.
3. **V1 constraint:** The database must fit on local disk. The page
   store is indexed directly by page number (page N at offset
   `N × pageSize`). Hash-table-based storage for partial caching is
   a V2 concern.

## Scenarios

| Scenario | Behavior |
|----------|----------|
| **First connect, same machine** | HEAD to get size + ETag. Create cache files. Background thread downloads entire DB via parallel GETs. Demand-load on miss until prefetch completes. |
| **Reconnect, no remote changes** | HEAD to get ETag (~22ms). ETag matches local cache. mmap existing files. Everything is local — zero downloads. |
| **Reconnect, remote changes** | HEAD shows ETag mismatch. Invalidate entire bitmap. Background thread re-downloads all pages via parallel GETs. |
| **Reconnect, we were the last writer** | Our stored ETag matches remote ETag. Cache is valid. Zero downloads. |
| **Different machine, small query** | No local cache exists. `prefetch=off` or demand-load handles it. Falls back to per-page GETs with readahead. |
| **Power loss / crash recovery** | On open, validate via checksums (deferred) or ETag. If metadata looks corrupt (bad magic, wrong version), delete cache and rebuild. |

## File Layout

### Cache Directory

Configurable via URI parameter `cache_dir=<path>`, defaulting to a
platform-appropriate location (e.g., `~/.sqlite-objs/cache/`). The
directory is created on first use.

Each remote database maps to a pair of files. The filename is derived
from a hash of `account + container + blob_name` to avoid path
collisions and filesystem-unfriendly characters:

```
~/.sqlite-objs/cache/
  <hash>.meta     — Metadata: header, bitmaps, checksums
  <hash>.pages    — Page data store
```

The hash function (SHA-256 truncated to 16 hex chars, or similar) must
be deterministic so the same remote blob always resolves to the same
local cache files.

### Metadata File Layout

All multi-byte integers are little-endian (matching the host on x86 and
ARM). All offsets are byte offsets from the start of the file.

```
Offset    Size     Field
──────    ────     ─────
0         8        magic           "AZQCACHE" (0x4548434143515A41)
8         4        version         1
12        4        page_size       4096 (from SQLite header)
16        4        page_count      Number of pages allocated in page store
20        4        flags           Bit 0: session_active (set on open, cleared on close)
24        128      etag            ETag from last successful sync/open (null-terminated)
152       8        blob_size       Remote blob size in bytes at last sync
160       96       reserved        Alignment padding to 256 bytes
256       var      valid_bitmap    ceil(page_count / 8) bytes — 1 = page is valid
256+V     var      dirty_bitmap    ceil(page_count / 8) bytes — 1 = page is dirty
256+2V    var      checksums       page_count × 4 bytes (CRC32C per page, 0 = not computed)
```

Where `V = ceil(page_count / 8)`.

The metadata file is grown by `ftruncate` + re-`mmap` when the remote
database grows. Because no pointers are stored in the mapped region,
remapping is safe at any time — all access uses computed offsets from
the base address.

**Access helpers** (computed at open or after remap):

```c
/* All derived from meta_base (mmap address) and page_count */
uint8_t *valid_bitmap  = meta_base + 256;
uint8_t *dirty_bitmap  = meta_base + 256 + bitmap_bytes;
uint32_t *checksums    = (uint32_t *)(meta_base + 256 + 2 * bitmap_bytes);
```

These are recomputed after every remap. They are local stack/struct
variables, never stored in the mapped file itself.

### Page Store File Layout

```
Offset              Size        Content
──────              ────        ───────
0                   pageSize    Page 0 data
pageSize            pageSize    Page 1 data
2 × pageSize        pageSize    Page 2 data
...
N × pageSize        pageSize    Page N data
```

Page N is at byte offset `N × pageSize`. The file size is
`page_count × pageSize`. Grown by `ftruncate` + re-`mmap` when new
pages are needed.

This direct-indexed layout is only possible when the entire database
fits on local disk (V1 constraint). For databases larger than local
storage, a hash-table-based page store is needed (see V2 section).

## ETag Capture (Prerequisite)

The current codebase has ETag fields stubbed but never populated.
Before the local cache can work, we must capture ETags from Azure
HTTP responses.

### Changes to azure_client.c

Add `etag` field to `azure_response_headers_t`:

```c
typedef struct {
    char    lease_id[64];
    char    lease_state[32];
    char    lease_status[32];
    char    request_id[64];
    char    error_code[128];
    int64_t content_length;
    int     lease_time;
    int     retry_after;
    char    etag[128];              /* NEW: ETag from response */
} azure_response_headers_t;
```

Add to `curl_header_cb`:

```c
CAPTURE_HEADER("ETag", 4, h->etag)
```

### Changes to blob_get_properties

Add `etag` output parameter to `blob_get_properties`:

```c
azure_err_t (*blob_get_properties)(void *ctx, const char *name,
                                   int64_t *size,
                                   char *lease_state,
                                   char *lease_status,
                                   char *etag,          /* NEW */
                                   azure_error_t *err);
```

Populate from response headers after HEAD request.

### Changes to page_blob_write / write_batch

After successful writes, copy the ETag from the response headers into
`azure_error_t.etag` (already has the field, just needs population).
The VFS captures this in xSync and stores it in both `p->etag` and
the metadata file.

## Cache Lifecycle

### Open

```
1. Parse URI: extract cache_dir, blob identity (account/container/name)
2. Compute cache file paths: hash(account + container + blob) → .meta, .pages
3. HEAD request → get blob_size, etag (single HTTP round-trip, ~22ms)
4. If .meta exists and magic/version valid:
   a. mmap .meta read-write
   b. Compare stored etag with remote etag
   c. If match → cache is valid. mmap .pages. Done — zero downloads.
   d. If mismatch → clear entire valid_bitmap (memset 0). Update stored
      etag. Grow files if blob grew. mmap .pages.
5. If .meta does not exist or is corrupt:
   a. Create .meta with header, empty bitmaps, checksums
   b. Create .pages sized to page_count × pageSize
   c. ftruncate + mmap both files
   d. Store etag from HEAD response
6. Set flags.session_active = 1
7. Start background prefetch thread (see Prefetch section)
8. Return — open is complete. SQLite can begin querying immediately.
```

**Time to open (reconnect, no changes):** ~22ms (one HEAD request).

**Time to open (reconnect, changes):** ~22ms + background download time.
SQLite can begin querying immediately via demand-load; prefetch fills
the cache in the background.

### Read (xRead)

```
1. Compute pageNo = offset / pageSize
2. Check valid_bitmap[pageNo]:
   a. If valid: memcpy from page store → output buffer. Done.
   b. If not valid: demand-load (see below)
3. Demand-load:
   a. Use adaptive readahead to determine fetch window [pageNo, endPage)
   b. HTTP GET for the byte range
   c. Acquire cache mutex
   d. For each page in response:
      - If valid_bitmap[page] is already set, skip (another thread filled it)
      - memcpy page data into page store at page × pageSize
      - Compute and store CRC32C checksum (if checksums enabled)
      - Set valid_bitmap[page] = 1
   e. Release cache mutex
   f. memcpy requested page from page store → output buffer
```

### Write (xWrite)

```
1. Compute pageNo = offset / pageSize
2. If page is not in cache and this is a partial-page write:
   a. Demand-load the page first (need existing content for merge)
3. Acquire cache mutex
4. memcpy write data into page store at appropriate offset
5. Set valid_bitmap[pageNo] = 1
6. Set dirty_bitmap[pageNo] = 1
7. Release cache mutex
```

### Sync (xSync)

```
1. Collect all dirty pages (scan dirty_bitmap)
2. Coalesce into contiguous ranges (existing logic)
3. Flush to blob via page_blob_write_batch (parallel writes)
4. On success:
   a. Capture ETag from write response
   b. Store ETag in metadata file
   c. Clear dirty_bitmap (memset 0)
   d. msync metadata file (ensure ETag + bitmaps persist)
```

### Close (xClose)

```
1. If dirty pages remain: flush via xSync path
2. Stop prefetch thread (signal + join)
3. Clear flags.session_active = 0
4. msync both files
5. munmap both files
6. Close file descriptors
7. Release Azure lease
```

## Background Prefetch Thread

### Design

A single background thread per open database. Started at open time,
stopped at close time. Downloads pages that are not yet valid in the
local cache, using parallel HTTP GETs for throughput.

```c
typedef struct {
    pthread_t thread;
    pthread_mutex_t mutex;      /* Protects page store + bitmaps */
    int stop_requested;         /* Set to 1 to signal shutdown */
    int pages_fetched;          /* Progress counter (atomic) */
    int pages_total;            /* Total pages to fetch */
    int complete;               /* 1 when prefetch is done */
    /* ... file pointers, mmap bases, azure_ops, etc. */
} sqlite_objs_prefetch_ctx_t;
```

### Algorithm

```
1. Scan valid_bitmap to find all invalid pages
2. Sort into contiguous runs
3. Coalesce runs (bridge gaps < 256 pages to reduce HTTP requests)
4. For each coalesced range:
   a. If range > 4MB, split into ~4MB chunks
   b. Issue parallel GETs via curl_multi (up to 32 concurrent)
   c. As each chunk completes:
      - Acquire mutex
      - For each page in chunk:
        - If valid_bitmap[page] already set → skip (written by foreground)
        - memcpy into page store
        - Compute CRC32C if enabled
        - Set valid_bitmap[page] = 1
      - Release mutex
      - Update pages_fetched counter (atomic)
   d. Check stop_requested — abort if true
5. Set complete = 1
```

### Chunk Size

Each HTTP GET fetches up to ~4MB (1024 pages at 4KB). This balances:
- **Throughput:** Large enough to amortize HTTP latency (~22ms)
- **Parallelism:** 32 concurrent 4MB GETs = 128MB in-flight
- **Responsiveness:** Small enough that stop_requested is checked
  frequently
- **Memory:** Download buffer is temporary (freed after memcpy to mmap)

For a 90MB database with an empty cache:
- 23 chunks × 4MB = 92MB
- 32 concurrent GETs → ~1 round of parallelism
- Transfer time: ~2-3 seconds (limited by bandwidth, not latency)
- Compare: single GET = ~400ms but risks timeout; parallel chunks =
  more resilient + better utilization of multiple TCP connections

### Synchronization

A single `pthread_mutex_t` protects the critical section of writing
page data and updating bitmaps. The HTTP download (the expensive part)
happens **outside** the lock. The lock is held only during:

```c
pthread_mutex_lock(&ctx->mutex);
if (!bitmap_test(valid_bitmap, pageNo)) {     /* re-check under lock */
    memcpy(page_store + pageNo * pageSize, downloaded_data, pageSize);
    bitmap_set(valid_bitmap, pageNo);
}
pthread_mutex_unlock(&ctx->mutex);
```

Hold time: ~1μs (memcpy of 4KB + bitmap bit set). Contention is
negligible because:

- Foreground xRead holds the lock for the same ~1μs per demand-load
- xWrite holds it for ~1μs per page write
- The prefetch thread releases the lock between pages, allowing
  foreground operations to interleave

### FCNTL Interface

```c
/* Wait for prefetch to complete (blocks until done or timeout) */
#define SQLITE_OBJS_FCNTL_PREFETCH_WAIT     1002

/* Query prefetch progress (non-blocking) */
#define SQLITE_OBJS_FCNTL_PREFETCH_PROGRESS 1003

typedef struct {
    int pages_fetched;    /* Pages downloaded so far */
    int pages_total;      /* Total pages to download (0 = already cached) */
    int complete;         /* 1 if prefetch is done */
    double elapsed_ms;    /* Time since prefetch started */
} sqlite_objs_prefetch_progress_t;
```

Usage patterns:

```c
/* "prefetch=all" equivalent: open + wait */
sqlite3_open_v2("file:db.db?cache_dir=/tmp/cache", &db, flags, "sqlite-objs");
sqlite3_file_control(db, "main", SQLITE_OBJS_FCNTL_PREFETCH_WAIT, NULL);

/* Low-latency open: let prefetch run in background */
sqlite3_open_v2("file:db.db?cache_dir=/tmp/cache", &db, flags, "sqlite-objs");
/* Start querying immediately — demand-load handles misses */

/* Progress monitoring */
sqlite_objs_prefetch_progress_t prog;
sqlite3_file_control(db, "main", SQLITE_OBJS_FCNTL_PREFETCH_PROGRESS, &prog);
printf("Prefetch: %d/%d pages (%.1f%%)\n",
       prog.pages_fetched, prog.pages_total,
       100.0 * prog.pages_fetched / prog.pages_total);
```

## Replacing the In-Memory LRU Cache

The mmap-backed page store **replaces** the current `sqlite_objs_page_cache_t`
entirely. The following code is removed:

- `sqlite_objs_page_cache_t` struct and all methods (~300 lines)
- `cacheInit`, `cacheDestroy`, `cacheLookup`, `cacheInsert`
- `cacheRemove`, `cacheLruTouch`, `cacheEvictClean`
- `cacheCollectDirty` — replaced by dirty_bitmap scan
- Per-page `calloc` / `free` — replaced by fixed offset in mmap

**What replaces it:**

| Old (LRU cache) | New (mmap cache) |
|------------------|-------------------|
| `cacheLookup(page)` | `bitmap_test(valid_bitmap, page)` |
| `cacheInsert(page, data)` | `memcpy(page_store + page * pageSize, data, pageSize); bitmap_set(valid_bitmap, page)` |
| `entry->data` | `page_store + page * pageSize` |
| `entry->dirty` | `bitmap_test(dirty_bitmap, page)` |
| `cacheEvictClean()` | Not needed — OS handles memory pressure |
| `cacheCollectDirty()` | Scan dirty_bitmap |
| `cachePagesConfig` | Not needed — all pages have a slot |

The OS virtual memory system provides:
- **Automatic eviction:** Under memory pressure, the OS pages out
  least-recently-used mmap pages to disk. They fault back in on access.
- **No buffer sizing:** We don't need `cache_pages` configuration.
  The mmap can be as large as the database; the OS decides how much
  physical RAM to use.
- **Persistence:** Pages survive process restart (they're on disk).

## Checksums (Optional, Recommended)

CRC32C per page, stored in the metadata file. Computed when a page
is written to the cache (from download or from xWrite). Verified
on demand when a page is read from the cache.

### Rationale

- **Storage cost:** 4 bytes per page. For 1GB DB (262K pages) = 1MB.
  Negligible.
- **Compute cost:** CRC32C of 4KB ≈ 0.3μs with hardware acceleration
  (SSE4.2 on x86, CRC instructions on ARMv8). Both are available on
  modern macOS and Linux.
- **Benefit:** Detects silent disk corruption, incomplete writes from
  crashes, and mmap faults. If checksum fails → re-fetch from blob.
  This makes crash recovery automatic — no need for a separate
  "unclean shutdown" detection mechanism.

### Verification path

```
xRead for page P:
  1. valid_bitmap says valid
  2. Compute CRC32C of page data in mmap
  3. Compare with stored checksum[P]
  4. If match → use page (fast path)
  5. If mismatch → page is corrupt:
     a. Clear valid_bitmap[P]
     b. Demand-load from blob
     c. Store new checksum
     d. Return fresh data
```

The checksum verification adds ~0.3μs per page read. For a TPC-C
transaction reading 30 pages, that's ~9μs — invisible compared to
any other cost in the system.

### Deferred verification

To avoid unnecessary compute, checksums can be verified lazily:

- **On read:** Always verify (0.3μs is cheap)
- **On prefetch download:** Compute and store, but don't verify (the
  data just came from the network — it's inherently fresh)
- **On xWrite:** Compute and store new checksum for the written page

## File Growth

When the remote database grows (detected at open via `blob_size >
page_count × pageSize`, or during xWrite when a new page beyond
`page_count` is accessed):

```
1. Compute new_page_count from blob_size or write offset
2. ftruncate page store file to new_page_count × pageSize
3. ftruncate metadata file to accommodate larger bitmaps + checksums
4. munmap both files
5. mmap both files at new sizes
6. Recompute derived pointers (valid_bitmap, dirty_bitmap, checksums)
7. Initialize new bitmap regions to 0 (invalid)
8. Update page_count in metadata header
```

Because **no pointers are stored in the mapped files** — only page
indices and byte offsets — remapping is safe at any time. The derived
pointers (`valid_bitmap`, `dirty_bitmap`, `checksums`) are recomputed
from the mmap base address and `page_count` after every remap. They
live in the `sqlite-objsFile` struct (process memory), never in the mmap.

The prefetch thread must be paused during remap (it holds pointers to
the old mapping). Sequence: signal pause → join or wait for ack →
remap → update thread's base pointers → resume.

## Concurrency Model

### Single-process access (V1)

One process at a time owns the local cache files, coordinated by:

1. **`flock()`** on the metadata file — prevents two local processes
   from concurrently modifying the cache
2. **Azure lease** on the blob — prevents two machines from writing
   concurrently

If `flock()` fails (another process holds it), the VFS falls back to
in-memory-only mode (no local cache). This is the same behavior as
today's code.

### Thread safety within a process

- **SQLite's threading model:** With `SQLITE_OPEN_FULLMUTEX`, SQLite
  serializes all calls into the VFS. Only one xRead/xWrite/xSync is
  active at a time.
- **The prefetch thread** runs concurrently with SQLite's VFS calls.
- **The single mutex** described above protects the critical section
  (memcpy + bitmap update). Both SQLite's thread and the prefetch
  thread acquire this mutex when modifying the page store or bitmaps.

### Lock ordering

Only one lock exists (the cache mutex), so deadlock is impossible.
The Azure HTTP operations (which are slow) never hold the mutex.

## Parallel Read Infrastructure

The background prefetch thread needs to issue parallel HTTP GETs. This
requires a read-side equivalent of the existing `page_blob_write_batch`.

### New API

```c
typedef struct {
    int64_t offset;         /* Byte offset in blob */
    size_t len;             /* Bytes to read */
    azure_buffer_t *out;    /* Output buffer (allocated by callee) */
} azure_read_range_t;

azure_err_t (*page_blob_read_batch)(
    void *ctx, const char *name,
    const azure_read_range_t *ranges, int nRanges,
    azure_error_t *err);
```

### Implementation

Mirrors `page_blob_write_batch`: uses the existing `curl_multi` handle
and connection pool. Each range becomes a GET request with a `Range`
header. Up to `SQLITE_OBJS_MAX_PARALLEL_GETS` (32) concurrent requests.

The existing `ensure_multi_handle()`, `curl_multi_perform()` loop, and
lease renewal logic are reused directly.

## URI Parameters

| Parameter | Default | Description |
|-----------|---------|-------------|
| `cache_dir` | `~/.sqlite-objs/cache/` | Local directory for cache files |
| `cache` | `on` | `on` = use local disk cache, `off` = in-memory only |
| `prefetch` | `lazy` | `lazy` = background download, `off` = demand-load only |
| `checksum` | `on` | `on` = CRC32C per page, `off` = no checksums |

The old `prefetch=all` becomes: open with `prefetch=lazy`, then
immediately call `SQLITE_OBJS_FCNTL_PREFETCH_WAIT`. The old
`prefetch=off` maps directly.

The old `cache_pages` parameter is removed — the mmap handles
everything. The old `readahead` / `readahead_max` parameters remain
for controlling the demand-load window on cache misses.

## Observability

### Open logging

```
[CACHE] hit: etag match, 22873 valid pages (89.3MB) — 0 downloads needed
[CACHE] miss: etag mismatch, invalidated 22873 pages — prefetch started
[CACHE] new: created cache for tpcc.db (89.3MB, 22873 pages)
```

### Prefetch progress logging

```
[PREFETCH] started: 22873 pages (89.3MB), 23 chunks, 32 parallel GETs
[PREFETCH] complete: 22873 pages in 2847ms (31.4 MB/s)
```

### Close summary

```
[CACHE] close: 22873 valid, 0 dirty, etag="0x8DC..."
```

## V2: Hash-Table Page Store (Future)

For databases larger than local storage, the page store cannot be
directly indexed by page number. Instead, a hash table maps page
numbers to slots in a fixed-size page store.

### Design sketch

**Metadata file additions:**

```
hash_capacity       Number of hash buckets (2× max cached pages)
hash_buckets[]      Array of hash_capacity entries:
                      { int32 page_no,     (-1 = empty)
                        int32 slot_index,  (index into page store)
                        uint32 checksum }  (CRC32C)
```

**Page store:** Fixed size (e.g., 2GB). Slots are allocated from a
free list. When full, evict the least-recently-used clean page.

**Lookup:** `hash(page_no) % hash_capacity` → probe chain (open
addressing). Find matching `page_no` → `slot_index` → page data at
`slot_index × pageSize` in the page store.

**Why open addressing in mmap:** No pointers — just indices. Probing
uses arithmetic on the hash bucket array, which is a flat region in
the mapped file.

**Growth considerations:** The hash table size is fixed at creation.
If the working set exceeds capacity, eviction kicks in. The page store
never grows — it's a fixed-size cache. Only the mapping from page
numbers to slots changes.

This is significantly more complex than the V1 direct-indexed layout
but allows caching arbitrary subsets of arbitrarily large databases.
Defer until there is a concrete customer need.

## Implementation Phases

### Phase 1: ETag Capture

- Add `etag` to `azure_response_headers_t`
- Parse ETag in `curl_header_cb`
- Populate `azure_error_t.etag` after each Azure operation
- Pipe ETag through `blob_get_properties` to VFS
- Capture in `sqlite-objsFile.etag` on open and after sync
- **Tests:** Verify ETag captured from HEAD, GET, PUT responses

### Phase 2: Metadata + Page Store Files

- Implement cache file creation, mmap, growth, remap
- Metadata header with magic, version, page_count, etag
- Valid bitmap, dirty bitmap, checksum array
- Cache directory creation and file naming (hash of blob identity)
- `flock()` for single-process access
- **Tests:** Create, open, grow, remap, concurrent flock

### Phase 3: Replace LRU Cache with Mmap

- xRead: check valid_bitmap → memcpy from page store (hit) or
  demand-load → write to page store → set bitmap (miss)
- xWrite: write to page store → set valid + dirty bits
- xSync: scan dirty_bitmap → coalesce → write_batch → clear dirty
  → store ETag → msync
- xClose: msync + munmap + clear session_active
- Remove sqlite_objs_page_cache_t and all LRU code
- Single mutex for page store + bitmap operations
- **Tests:** Read/write/sync cycle, cache persistence across close/open

### Phase 4: Background Prefetch Thread

- Thread creation at open, shutdown at close
- Scan valid_bitmap → build download plan → parallel GETs
- Cache mutex synchronization with foreground operations
- FCNTL wait and progress query
- Stop-on-request for clean shutdown
- Pause during file remap
- **Tests:** Prefetch completion, concurrent read during prefetch,
  write during prefetch (no clobber), progress reporting, clean stop

### Phase 5: Parallel Read Infrastructure

- `page_blob_read_batch` using curl_multi (mirrors write_batch)
- Integrate into prefetch thread for chunked parallel downloads
- Integrate into demand-load path for tree-aware readahead (optional)
- **Tests:** Parallel reads, error handling, partial failure

### Phase 6: Checksums

- CRC32C computation (hardware-accelerated where available)
- Store on write to cache, verify on read from cache
- Auto-recovery: checksum mismatch → re-fetch from blob
- **Tests:** Corruption detection, auto-recovery, performance impact

## Open Questions

1. **ETag granularity on mismatch:** When the ETag doesn't match,
   V1 invalidates the entire cache and re-downloads everything.
   Azure's `GetPageRanges` API with `prevsnapshot` can identify
   which page ranges changed, enabling differential sync. Worth
   investigating for V2 but adds snapshot management complexity.

2. **Cache size limits:** Should there be a configurable max cache
   size (e.g., `cache_max=10g`) even in V1? If someone opens a 500GB
   database, we don't want to silently fill the disk. A size check
   at open time with a clear error message may be sufficient.

3. **Cache eviction policy:** When should old cache files be deleted?
   A `~/.sqlite-objs/cache/` directory could accumulate stale caches.
   Options: LRU eviction when total cache size exceeds a limit,
   manual cleanup, or TTL-based expiry.

4. **msync frequency:** Currently proposed only at xSync and xClose.
   If the process crashes between syncs, bitmap updates are lost
   (but checksums catch the inconsistency on next open). Should we
   msync more frequently? The cost is a syscall (~1μs) but the
   benefit is crash resilience without checksums.

5. **Multiple databases:** When the same process opens multiple
   Azure-backed databases, each gets its own prefetch thread, mutex,
   and mmap pair. This is correct but could use a lot of threads.
   A shared thread pool for prefetching across databases is a
   potential optimization.
