# sqlite-objs Performance

Performance is a first-class design goal for sqlite-objs. The system cannot remove Azure network latency, but it should make the best possible use of each network round trip while preserving SQLite correctness and recovery guarantees.

This document captures performance principles, current techniques, non-obvious trade-offs, and future optimization directions that should remain relevant across refactors.

## Performance Goal

The goal is not to make Azure-backed SQLite behave like local SQLite. The goal is to minimize avoidable latency and bandwidth while keeping SQLite semantics correct.

The dominant performance challenge is that SQLite's VFS interface is synchronous and page-oriented, while Azure Blob Storage is remote and HTTP-oriented. Every unnecessary HEAD, GET, PUT, lease call, retry, or full re-download is expensive compared with local disk. The design therefore focuses on:

- amortizing network latency across larger operations,
- avoiding redundant remote calls,
- reusing local state safely,
- preserving connection/TLS reuse,
- batching independent work when SQLite gives enough information,
- making performance optimizations subordinate to recovery correctness.

## Performance Principles

### Correctness Comes First

Performance optimizations must not weaken recovery, locking, or snapshot invariants. A cached answer is valuable only when its proof remains valid across processes and crashes. If remote state may contain recovery artifacts, authoritative checks win over cached absence.

### Make Every Network Round Trip Count

Azure latency is unavoidable, so the implementation should avoid small, redundant operations and combine work where possible. This applies to reads, writes, blob metadata checks, lease operations, and cleanup paths.

### Prefer Local Reads After Safe Validation

Once cache state is proven valid for the current blob identity and ETag/snapshot, reads should be local. Remote reads should happen on open, cache miss, explicit prefetch, or revalidation.

### Upload Only What Changed

Sync should not upload the whole database unless necessary. Dirty page tracking and coalescing are central to write performance.

### Avoid Hidden Global Knobs

Performance-sensitive behavior should be discoverable and tied to the database URI or explicit APIs where possible. Hidden process environment configuration is harder to audit and reason about in libraries.

## Current Performance Techniques

### Local Disk Cache

The main database is cached locally. Reads and writes operate against the local cache file, not directly against Azure for every SQLite page request. This is the primary reason sqlite-objs can be usable despite network latency.

Key ideas:

- The blob remains the source of durable truth.
- Cache files are derived state and may be rebuilt.
- Dirty and valid bitmaps track what is locally modified and what is locally available.
- Cache reuse is allowed only when the cached file matches the current remote identity and ETag.

### Full-Prefetch Default

By default, sqlite-objs downloads the database blob on open. This front-loads latency but makes subsequent reads local, which is often best for small-to-medium databases and read-heavy workloads.

This is a workload trade-off:

- Good when most of the database will be touched.
- Bad when the database is large and a connection only reads a few pages.

### Lazy Fetch Mode

URI mode supports lazy fetching with `prefetch=none`. In this mode, the local cache starts sparse and pages are fetched on demand. A valid-page bitmap prevents repeated downloads of the same page.

Lazy mode is important for large databases and narrow queries. It trades predictable open-time cost for possible read-time latency.

### ETag-Based Cache Reuse

Persistent cache reuse avoids paying full download cost on reconnect when the remote blob has not changed. The core invariant is simple: cache reuse is valid only when the stored ETag matches the current remote ETag.

Cache reuse is especially valuable for:

- repeated local development sessions,
- services reconnecting to the same database from the same host,
- benchmark iterations,
- read-heavy workloads where the database changes infrequently.

### Incremental Revalidation

When cache reuse detects remote changes, the ideal is to apply only changed ranges rather than re-downloading the full blob. Snapshot/page-range diff support exists to make this possible when Azure can provide the needed information.

The design goal is to turn "remote changed" from "throw away the whole cache" into "patch the changed pages safely."

### Dirty Page Tracking

Writes mark pages dirty in the local cache. Sync uploads only dirty pages. This is one of the most important write-path optimizations because SQLite frequently modifies a small subset of pages during a transaction.

Dirty tracking must remain exact. Dropping a dirty bit is a correctness bug; uploading too many clean pages is a performance bug.

### Dirty Range Coalescing

Dirty pages are merged into contiguous Azure page-write ranges. Coalescing reduces request count and amortizes request overhead. Ranges are capped to Azure's page-write limits and padded to 512-byte alignment when needed.

This is a key latency optimization for write-heavy workloads because a transaction that dirties many adjacent pages can become a small number of remote writes.

### Parallel Batch Writes

When multiple dirty ranges exist, the production Azure client can upload them concurrently with libcurl multi. This turns many independent PUT Page requests into roughly one batch-latency window, subject to Azure throttling, bandwidth, and retry behavior.

Batch writes require careful resource ownership:

- range data must remain stable until the batch completes,
- each request owns its headers and response buffers,
- the persistent multi handle must be protected by the client mutex,
- lease renewal must not recursively acquire the same mutex.

### Parallel Full-Blob Reads

Large full-blob downloads can be split into chunks and fetched concurrently. This matters for open-time prefetch, cache refresh, and other paths that need the whole blob.

Parallel read is less helpful for single-page misses because SQLite's VFS API is synchronous and usually asks for one page at a time. It is most useful when the implementation already knows the total blob size and intends to download a large region.

### Skipping Redundant Resizes

The VFS tracks the last synced remote blob size. If a transaction changes existing pages without growing the database, sync can skip the remote resize operation.

This avoids a high-latency metadata operation on common update-heavy workloads.

### Lease Duration and Renewal Heuristics

Large flushes risk outliving a short lease. sqlite-objs uses lease renewal during long write sequences and batch writes. For large dirty-page counts, the design favors longer/renewed leases so the writer does not lose its lease mid-sync.

The performance goal is to renew early enough to avoid lease loss without adding unnecessary lease calls to small transactions.

### Connection and TLS Reuse

The Azure client is designed to reuse libcurl handles, connection pools, and TLS session state. This reduces handshake overhead for repeated requests and is important for both many small operations and batch operations.

Any refactor of request execution should preserve connection reuse.

### Metrics and Observability

Performance tuning requires visibility. sqlite-objs exposes counters for disk I/O, blob I/O, cache behavior, lease operations, sync activity, revalidation, journal/WAL uploads, and Azure errors.

When changing performance-sensitive code, use metrics to distinguish:

- fewer requests,
- fewer bytes,
- fewer full downloads,
- fewer resizes,
- better cache hit rate,
- fewer retries/errors.

## Non-Obvious Performance Trade-offs

### Authoritative Journal Checks Cost Latency

The system intentionally performs authoritative remote checks for recovery artifacts, even though this can add HEAD requests. This is a correctness-over-performance decision: cached journal absence is not safe across processes and crashes.

Future optimization may recover some of the lost performance, but only with a proof that remains valid across crash and multi-process scenarios.

### Full Download Can Be Faster Than Many Lazy Misses

For many workloads, a single full download is cheaper than hundreds or thousands of small page fetches. Full prefetch is not naive; it is often the best latency amortization strategy for databases that fit comfortably in local storage and are read broadly.

### Lazy Fetch Avoids Waste But Exposes Latency

Lazy mode avoids downloading unused pages, but each miss can put network latency on the SQLite read path. It is best for large databases with narrow access patterns, not for table scans.

### Batch Writes Improve Throughput But Complicate Failure Handling

Parallel writes reduce wall-clock sync time, but failure handling becomes more complex: partial range completion, retries, lease renewal, request cleanup, and error aggregation must all remain correct.

### Sanitizer and Gate Runtime Matter

Validation performance is part of developer performance. The release gate must be bounded and clear about skipped work. A gate that hangs or claims full readiness while skipping critical checks slows the project and creates release risk.

## Workload Guidance

### Best Fit

sqlite-objs performs best when:

- databases fit in local cache,
- connections are long-lived or can reuse cache,
- transactions batch many writes,
- workloads have read locality,
- writes are serialized naturally,
- deployments can tolerate Azure-level latency for commit/sync.

### Worst Fit

Expect poor performance when:

- every transaction is a tiny autocommit write,
- the database is much larger than local storage,
- reads are random and sparse with no cache reuse,
- many writers contend for the same database,
- every connection starts cold from a new machine,
- the workload requires low-latency shared-memory WAL semantics.

## Future Optimization Directions

### Adaptive Readahead

Fixed readahead is suboptimal: sequential scans want larger windows, while random/index lookups want demand-only reads. A future adaptive state machine can grow the readahead window for sequential access and shrink it for random access.

Important invariant: adaptive readahead must never return from `xRead` before the requested bytes are actually valid.

### Background Prefetch

A background prefetch thread could speculatively fetch pages after observing misses. This can help workloads with locality where the next pages are predictable, but it cannot help truly random access and must be coordinated carefully with cache invalidation.

### Tree-Aware Prefetch

SQLite B-tree interior pages contain child page pointers. After fetching an interior page, the VFS could prefetch likely child pages. This is more targeted than fixed readahead but requires careful parsing of SQLite page formats.

### Query-Level Hints

Application-level hooks could use statement tracing or query-plan inspection to prefetch table/index roots before SQLite asks for them. This is not transparent at the VFS level, but it may be useful for controlled applications or benchmarks.

### Shared No-Lock Azure Request Primitive

Batch lease renewal currently needs to avoid the normal lock-taking request helper. A future refactor could extract a shared request-building/execution primitive that supports both normal lock-owned requests and mutex-held batch paths without duplicating authentication, header, retry, and parsing logic.

### Smarter Journal Existence Optimization

The current design chooses authoritative recovery checks over cached absence. Future work may reduce HEAD calls by differentiating safe same-process transaction probes from recovery-sensitive cross-process checks. Any optimization must prove it cannot miss a journal created by another process before a crash.

### Renewal Timeout Budget

Lease renewal during large batch writes should have a bounded timeout strategy tied to lease duration. A future improvement could use shorter renewal-specific timeouts or drive renewal through the existing multi event loop.

### Cache Eviction and Size Policy

As databases grow, cache size policy becomes more important. The current disk-backed cache model can support lazy valid pages, but future work may need bounded disk usage, eviction, and smarter persistence policies.

## Performance Audit Checklist

When reviewing performance-sensitive changes, ask:

- Does this add a remote call on a common transaction path?
- Can the remote call be batched, skipped, cached, or delayed safely?
- Does the optimization remain valid across processes and crashes?
- Are dirty/valid bitmaps still exact?
- Does this preserve connection/TLS reuse?
- Does this hold the Azure client mutex across a potentially long operation?
- Does this change cache reuse or ETag validation behavior?
- Does this increase bytes transferred or request count?
- Do metrics expose the effect?
- Is a slower but safer path clearly documented as a correctness trade-off?

## Relationship to Correctness

Performance work in sqlite-objs is never independent of correctness. The fastest possible implementation would cache aggressively and skip remote checks; that would be wrong. The design challenge is to remove only avoidable latency while preserving the invariants that make SQLite recovery and Azure durability trustworthy.
