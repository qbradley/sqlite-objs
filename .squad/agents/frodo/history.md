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
