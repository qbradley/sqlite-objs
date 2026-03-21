# Project Context

- **Owner:** Quetzal Bradley
- **Project:** Azure Blob-backed SQLite (sqlite-objs) — a drop-in replacement for SQLite where all storage is backed by Azure Blob Storage, implemented as a custom VFS layer. MIT licensed.
- **Stack:** C, SQLite VFS API, Azure Blob Storage REST API, libcurl, OpenSSL
- **SQLite source:** `sqlite-autoconf-3520000/` (do not modify unless absolutely necessary)
- **Created:** 2026-03-10

## Key Context

- Azure REST API implemented directly — no SDK dependency
- OpenSSL for HMAC-SHA256 auth signing, libcurl for HTTP
- Research existing Azure SDKs (C, Python, Go, .NET) for best practices
- Open question: page blobs vs block blobs vs append blobs?
- License: MIT

## Learnings

<!-- Append new learnings below. Each entry is something lasting about the project. -->

### Phase 1 Immediate Wins (2026-07)

- **If-Match conditional headers:** Added `if_match` parameter to `page_blob_write` and `page_blob_write_batch` vtable signatures. `If-Match` header is included in SharedKey StringToSign (line 9 — between If-Modified-Since and If-None-Match). `execute_single` and `execute_with_retry` now accept and thread `if_match` through. `batch_init_easy` includes it in both signing and HTTP headers. 412 → `AZURE_ERR_PRECONDITION` → `SQLITE_BUSY` (non-retryable). VFS passes `p->etag` during xSync writes for belt-and-suspenders concurrency with leases.
- **Undelete Blob API:** Added `az_blob_undelete()` → `PUT ?comp=undelete`. Added `blob_undelete` to `azure_ops_t` vtable. Requires account-level soft delete enablement (portal/CLI, not our code).
- **Last Access Time:** Account-level setting only, zero code changes needed. Document as deployment recommendation.
- **Vtable changes affect ALL callers:** Adding a parameter to `page_blob_write` required updating the stub (`azure_client_stub.c`), mock (`mock_azure_ops.c`), and all 44+ test call sites. Use designated initializers in vtable structs to avoid positional mismatches.
- **If-Match is a standard HTTP header (not x-ms-\*):** It goes in the StringToSign at a fixed position (line 9), NOT in canonicalized headers. The `extra_x_ms` mechanism is only for `x-ms-*` headers.


## Core Context Summary

**Azure Client & Performance Lead (2026-03-10 through 2026-07):**
Frodo designed and implemented Azure REST client (libcurl-based), SAS+SharedKey auth, write batching (curl_multi), and connection pooling. From initial PoC to production-ready: ~3KLOC. Code reviews APPROVED for Phase 1+2 performance work.

**Azure Client Implementation:**
- **REST API:** GET (page blobs, page ranges), PUT (page writes, blob properties), HEAD (blob size/ETag), DELETE (blob deletion), LEASE (acquire/release/renew).
- **Auth (two paths):** SAS tokens (preferred, AZURE_STORAGE_SAS) and Shared Key (fallback, AZURE_STORAGE_KEY with HMAC-SHA256 signing). Both implemented; zero additional cost.
- **Error handling:** Lease conflicts (409) and throttling (429) → SQLITE_BUSY. All other errors after 5 retries (500ms exponential backoff + jitter) → SQLITE_IOERR_*.
- **Connection pooling:** Reuse curl handles across operations, reduce SSL handshake overhead.

**Performance Optimization (Phase 1+2):**
- **Phase 1 (Coalescing):** Batch dirty pages into 4 MiB chunks before upload. Reduces write amplification from page-at-a-time (512 B) to coalesced batch (~4 MB). 5–10× improvement for sequential writes.
- **Phase 2 (curl_multi):** Parallel page uploads via single-threaded event loop (32 connections default). Key insight: curl_multi eliminates threading hazards (mutexes, data races) vs pthreads. Btree mutex held during xSync ensures aData stability → zero-copy batch design safe. Projected 50–1000× improvement for bulk writes.
- **Key decision: PRAGMA synchronous=NORMAL default.** Saves one journal upload per commit (FULL costs extra round-trip for marginal safety).

**Code Review Outcomes:**
- Phase 1+2: APPROVED. Batch retry uses pure exponential backoff (recommend adding jitter for consistency before Phase 3).
- azure_ops_t vtable: 13 operations (blob_read, blob_write, blob_size, blob_truncate, page_blob_write_batch, lease_*). Frodo refactored azure_client.c to export these.

**Recent Work & Context:**
- curl_multi event loop single-threaded design preferred over pthreads for I/O-bound work (no mutex contention, trivial lease renewal integration).
- Lease renewal at 15s intervals on 30s lease (adequate safety margin).
- SharedKey signing requires x-ms header ordering, implemented correctly.
- Partial batch failure handling: retry loop cleans up all resources, no leaks.

- **No migration concerns:** First sync after code change automatically overwrites any existing append blob WAL with a block blob.
- **Full analysis:** `.squad/decisions/inbox/frodo-wal-blob-strategy.md`

### HTTP/2 Multiplexing Enabled (2026-06-13)

- **Corrects earlier research:** Azure Parallel Write Research noted "HTTP/2: Dead end — Azure Blob Storage only supports HTTP/1.1." This was wrong; Azure Blob Storage does support HTTP/2 via TLS ALPN negotiation.
- **Changes made to `src/azure_client.c`** (4 locations):
  1. `execute_single()`: Added `CURLOPT_HTTP_VERSION = CURL_HTTP_VERSION_2TLS` after `curl_easy_reset()`. All single HTTP requests now negotiate HTTP/2 over TLS.
  2. `ensure_multi_handle()`: Added `CURLMOPT_PIPELINING = CURLPIPE_MULTIPLEX` on the persistent CURLM handle. Enables true HTTP/2 stream multiplexing for batch operations.
  3. `batch_init_easy()` (write batch): Added `CURLOPT_HTTP_VERSION = CURL_HTTP_VERSION_2TLS` and `CURLOPT_PIPEWAIT = 1L` on each easy handle. PIPEWAIT tells curl to wait for an existing multiplexed connection.
  4. `read_batch_init_easy()` (read batch): Same HTTP/2 + PIPEWAIT settings as write batch.
- **Graceful fallback:** `CURL_HTTP_VERSION_2TLS` means negotiate HTTP/2 via ALPN during TLS handshake; if server or libcurl doesn't support it, silently falls back to HTTP/1.1. No crash, no error.
- **nghttp2 dependency:** HTTP/2 requires libcurl built with nghttp2. macOS Homebrew curl includes it. If absent, curl falls back silently.
- **Synced to `rust/sqlite-objs-sys/csrc/azure_client.c`.**
- **Build:** Zero warnings. All 242 unit tests pass.

### Lazy Cache HTTP Integration (2026-03-22)

Lazy cache implementation completed. HTTP layer implications:

- **Page 1 bootstrap:** New `page_blob_read(blob_client, offset=0, length=65536)` call at xOpen in lazy mode. Single 64KB request fetches max SQLite page size without round-trip for header parsing.
- **Readahead batching:** `fetchPagesFromAzure()` now issues single `page_blob_read` for 16-page windows (pages N:N+15) instead of individual requests. Reduces HTTP call count by 16× on sequential reads.
- **State file I/O:** New `.state` sidecar file written via `put_blob()` after cache fsync. Atomic rename pattern on local disk. No new Azure API requirements.
- **Error handling:** Readahead window fetch failures (404, 429, etc.) map to existing SQLITE_BUSY/SQLITE_IOERR logic. Lazy mode doesn't introduce new error scenarios.
- **Caching implications:** Valid bitmap persistence in `.state` file allows reconnect scenario: no need to re-fetch pages already in cache if state file exists. ETag matching guarantees file unchanged.

**Testing:** Integration tests validate `page_blob_read` offset/length contracts at HTTP mock layer.

### Azure Capabilities Research (2026-01-15)

Comprehensive analysis of Azure Blob Storage features we're not using vs. what we currently leverage. Key findings:

**What we use (19 operations):**
- Page blobs: Create, Put Page (single + batch), Get Page Ranges, Resize, Get Page Ranges (diff), Snapshot
- Block blobs: Put Blob, Get Blob, Put Block + Put Block List (parallel upload)
- Append blobs: Create, Append Block (WAL mode)
- Common: Get Properties, Delete, HEAD (exists check)
- Leases: Acquire, Renew, Release, Break
- HTTP/2 multiplexing, connection pooling, retry logic with backoff

**High-priority opportunities:**
1. **Put Page From URL** — Server-side page copy between blobs (snapshot recovery without bandwidth cost)
2. **Conditional headers (If-Match)** — Optimistic concurrency for multi-writer scenarios (trivial add)
3. **Blob index tags** — Queryable metadata for multi-tenant discovery (10 tags per blob, free)
4. **Last access time tracking** — Enables automated tiering policies (Cool/Archive)
5. **Soft delete** — Accidental deletion recovery (7-day retention typical)
6. **Incremental Copy Blob** — Differential page blob backups (only changed pages)

**Medium-priority:**
- Blob versioning (automatic vs manual snapshots), change feed (audit log), access tier management, customer-managed keys (CMK), object replication (cross-region for block blobs only)

**Not recommended:**
- HNS/ADLS Gen2 (breaks page blob versioning compatibility)
- CDN/Query Acceleration (not applicable to transactional DB workloads)
- Customer-provided keys (too risky vs CMK)

**Immediate wins (low effort):**
- Enable soft delete at account level, add If-Match headers to Put Page, enable last access time tracking

Full analysis: `.squad/decisions/inbox/frodo-azure-capabilities.md`

## Phase 1 Orchestration — 2026-03-21T06:42:13Z

**Completed work:**
- If-Match conditional headers on PUT Page: prevents race condition (412 → SQLITE_BUSY)
- Undelete Blob API: `az_blob_undelete()` via REST `?comp=undelete`, added to vtable
- Last Access Time Tracking: sidecar `.atime` format documented

**Test status:** 294/295 passing (1 unrelated intermittent)

**Cross-agent notes:**
- Samwise's write-read handoff tests validate If-Match implementation
- Gimli's pragmas module will expose FCNTL metrics for debugging
- Aragorn's next phase: incremental downloads using page snapshot diffs

**Remaining opportunities:**
- Extend If-Match to journal writes (currently page-blob only)
- Use `.atime` for cache eviction heuristics (Aragorn's prefetch optimization)
- Put Page From URL (server-side copy) for future backup/clone scenarios
