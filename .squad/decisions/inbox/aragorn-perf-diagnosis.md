# Performance Diagnosis: Azure VFS TPC-C Latency

**Date:** 2026-03-11
**Author:** Aragorn (SQLite/VFS)
**Benchmark:** TPC-C, 1 warehouse, 5s duration, Azure (West US 2)

## Measured Results

- **Local:** ~1694 tps, avg New Order 0.9ms
- **Azure:** ~2.9 tps, avg New Order 377ms, avg Payment 327ms
- **Slowdown factor:** ~400x

## Per-Transaction Timing Breakdown

Each TPC-C write transaction (New Order / Payment) requires **9 sequential HTTP round-trips** averaging **~28ms each** on a reused connection. Here is where the time goes:

| Phase | Avg (ms) | % of Total | HTTP Calls | Notes |
|-------|----------|------------|------------|-------|
| HEAD checks (journal/wal existence) | 111ms | 23.0% | ~4 | SQLite probes journal and WAL twice |
| xSync(journal) - block blob upload | 130ms | 26.9% | 1 | Journal uploaded before DB pages |
| xSync(db) - page blob writes | 144ms | 29.8% | 1 resize + 1 batch | Batch is parallel but resize is serial |
| - resize (page_blob_resize) | 45ms | 9.3% | 1 | Every sync does resize even if size unchanged |
| - write_batch (curl_multi) | 99ms | 20.5% | N parallel | 5-24 pages per txn, batched well |
| lease_acquire | 38ms | 7.8% | 1 | PUT comp=lease |
| lease_release | 32ms | 6.5% | 1 | PUT comp=lease |
| journal DELETE | 29ms | 5.9% | 1 | DELETE after commit |
| **TOTAL** | **~483ms** | **100%** | **~9** | |

## Key Findings

### 1. Connection Pool IS Working
TLS is reused on 99%+ of requests after initial handshake (104ms for first request, ~25-35ms for subsequent). The connection pool optimization from Phase 2 is effective.

### 2. Root Cause: Sequential HTTP Round-Trip Count
The fundamental bottleneck is **9 sequential HTTP round-trips per transaction**, each costing ~25-45ms of pure network latency. The batch write (curl_multi) does parallelize the page writes, but everything else is serial:
- 4 HEAD checks (serial, ~111ms total)
- 1 lease acquire (serial, ~38ms)
- 1 journal upload (serial, ~130ms)
- 1 resize (serial, ~45ms)
- N page writes (PARALLEL via curl_multi, ~99ms)
- 1 journal delete (serial, ~29ms)
- 1 lease release (serial, ~32ms)

### 3. Redundant HEAD Checks Are 23% of Latency
SQLite calls xAccess to check journal and WAL existence multiple times per transaction. Each HEAD request costs ~25ms. These are pure overhead for the single-writer case.

### 4. Resize-Every-Sync Is 9% of Latency
`sqlite-objsSync` calls `page_blob_resize` on every sync regardless of whether the blob actually needs to grow. This is a wasted HTTP round-trip when the blob size has not changed.

### 5. Journal Upload Is Serial and Expensive
The journal (block blob upload) takes ~130ms average. It must complete BEFORE the db page writes begin (crash safety). With only ~21-111KB of journal data, the time is dominated by HTTP latency, not bandwidth.

### 6. Parallel Batch Writes Work But Cannot Help Enough
The curl_multi batch write averages ~99ms for 5-24 parallel PUT Page requests. For small transactions (5 dirty pages), it still takes ~60-80ms because the parallelism benefit is minimal when all requests finish in roughly one round-trip time.

### 7. Lease Overhead Is 14% of Latency
Acquire + release = ~70ms per transaction. These are unavoidable for correctness but could be amortized.

## Root Cause Analysis

The ~400x slowdown from local (0.9ms) to Azure (377ms) is **not** caused by bandwidth, TLS overhead, or missing optimizations. It is the **fundamental cost of 9 sequential HTTPS round-trips to a datacenter ~25ms away**.

For TPC-C's tiny transactions (2-5 dirty pages each), the page coalescing and parallel writes from Phase 1-3 have minimal impact because the dominant cost is **per-transaction fixed overhead** (lease, journal, HEAD checks, delete), not per-page write cost.

## Recommendations (Ranked by Expected Impact)

### R1: Eliminate Redundant HEAD Checks (~23% savings, ~110ms/txn)
**Impact: HIGH** - Expected improvement from 2.9 tps to ~3.8 tps

SQLite calls `xAccess` for journal and WAL files repeatedly. Since we control both files via Azure:
- Cache journal existence state in the sqlite-objsFile struct
- Return cached result from `xAccess` for journal/WAL blobs that we created/deleted ourselves
- Only do a real HEAD when the cached state might be stale (e.g., on first access after xOpen)

### R2: Skip Resize When Size Unchanged (~9% savings, ~45ms/txn)
**Impact: MEDIUM** - Expected improvement of ~0.3 tps additional

Track `lastSyncedSize` in sqlite-objsFile. Only call `page_blob_resize` when `nData > lastSyncedSize`. Most TPC-C transactions modify existing pages without growing the file.

### R3: Transaction Batching / Deferred Lease Release (~14% savings)
**Impact: MEDIUM** - Expected improvement of ~0.5 tps additional

Hold the lease across multiple back-to-back transactions instead of acquire/release per transaction. SQLite does RESERVED->EXCLUSIVE->SHARED per transaction. If we detect rapid re-locking (within e.g. 100ms), defer the lease release and reuse it.

### R4: Parallel Journal Upload + Page Write (~27% savings, theoretical)
**Impact: HIGH but COMPLEX** - Expected improvement to ~5-6 tps

The journal must be durable before DB pages are written (crash safety). But we could:
- Upload journal via curl_multi simultaneously with the resize call
- Begin page writes immediately after journal upload completes
- This overlaps journal upload with resize, saving ~45ms

### R5: PRAGMA journal_mode=TRUNCATE Instead of DELETE (~6% savings)
**Impact: LOW** - Expected improvement of ~0.2 tps

Avoids the journal DELETE HTTP call (~29ms). SQLite truncates instead of deleting. For block blobs, we would upload an empty blob instead of DELETE + re-create.

### R6: Persistent Lease with Background Renewal
**Impact: MEDIUM-HIGH** - Expected improvement of ~0.5 tps

Acquire lease once at BEGIN, hold it across the entire session (with background renewal), release only on connection close. Saves acquire+release per transaction. Requires careful handling of idle timeout.

### R7: Accept Higher Latency, Optimize for Throughput
**Impact: REFRAMING**

For OLTP workloads, Azure blob storage is fundamentally limited by HTTP latency (~25ms per round-trip). Even with all optimizations, single-threaded TPC-C will max out at ~10-15 tps. The real path to higher throughput is:
- **Multi-connection parallelism** (multiple SQLite connections doing transactions concurrently)
- **Read replicas** (local read-only copies, remote writes only)
- **Application-level batching** (group multiple logical operations into fewer SQLite transactions)

## Expected Combined Impact

Implementing R1 + R2 + R3 (straightforward changes):
- Eliminates ~46% of overhead (~220ms/txn)
- Expected: ~5-6 tps (from 2.9 tps)

Adding R4 + R6 (moderate complexity):
- Additional ~40% reduction
- Expected: ~8-10 tps

Theoretical maximum with all optimizations:
- Minimum 2 serial HTTP calls per transaction (journal + pages, can overlap)
- Floor: ~50-80ms per transaction = ~12-20 tps
- This is the hard limit imposed by Azure HTTP latency

## Connection Reuse Statistics

- First request: 104ms (DNS 2ms + TCP 22ms + TLS 79ms)
- Subsequent requests: 20-45ms (connection reused, zero TLS overhead)
- Reuse rate: 99%+ (1 new connection out of 99+ requests)
- Connection pool is working correctly

## Instrumentation Added

Debug timing is controlled by `SQLITE_OBJS_DEBUG_TIMING=1` environment variable:
- `sqlite_objs_vfs.c`: lease acquire/release, xSync breakdown, xRead counts, coalesce timing
- `azure_client.c`: per-HTTP-call timing with DNS/TCP/TLS breakdown, connection reuse detection
- Zero overhead when env var is not set (checked once, cached)
- All 207 unit tests pass with instrumentation in place
