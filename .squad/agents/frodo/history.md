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

### Azure REST API PoC (2026-03-10)

- **API version:** Target `2024-08-04` (latest stable). Set via `x-ms-version` header.
- **Auth signing:** StringToSign has 12 standard header lines + canonicalized x-ms-* headers (sorted, lowercase) + canonicalized resource. Date line must be EMPTY when x-ms-date is used. Content-Length is empty string for zero-length, not "0".
- **Page blobs are the right choice** for SQLite DB files. 512-byte pages align perfectly with all SQLite page sizes (512, 1024, 4096, 8192 — all powers of 2 ≥ 512). Max write is 4 MiB/request. Reads don't require alignment.
- **Block blobs** for journal/WAL files. Sequential access pattern, cheapest storage, no alignment constraints.
- **Append blobs rejected** for WAL — can't truncate, so checkpoint would require delete+recreate. No advantage over block blobs.
- **Leases map to SQLite locks:** No lease = NONE/SHARED, acquired lease = RESERVED/EXCLUSIVE. Reads never require leases. 30-second duration with renewal recommended.
- **Retry policy:** 5 retries, 500ms base, exponential backoff + jitter. Transient: 408/429/500/502/503/504. Matches Azure SDK patterns.
- **SAS tokens recommended for MVP** over Shared Key — simpler, more secure, scoped permissions.
- **PoC location:** `research/azure-poc/` — compiles on macOS, all offline tests pass.
- **Dependencies confirmed:** Only libcurl + OpenSSL needed. No Azure SDK dependency.
- **Missing for production:** Retry-After header support, Content-MD5 integrity checks, connection pooling, HTTP/2.

### Design Review — MVP 1 Architecture (2026-03-10)

- **Full design review at `research/design-review.md`.** 11 decisions (D1-D11) covering blob types, locking, caching, WAL, testing, build system, error handling, auth, naming, VFS registration, and MVP scope. All approved.
- **My Azure REST API PoC findings were validated:** Page blobs (D1), block blobs for journal (D1), SAS+SharedKey auth (D9), retry policy (D8), lease-based locking (D3).
- **Design decisions directly affecting my code:**
  - **D7:** Filename maps to blob name, container from env var. Journal blob = `<name>-journal`.
  - **D8:** Error mapping: 409/429 → SQLITE_BUSY, all else after retry → SQLITE_IOERR_*
  - **D9:** Both SAS (AZURE_STORAGE_SAS) and Shared Key (AZURE_STORAGE_KEY) auth
  - **D10:** Makefile build system with targets: all, test-unit, test-integration, test, clean, amalgamation
- **Key architectural constraint: azure_ops_t vtable.** Design decision D5 mandates that I refactor my PoC into a production azure_client with a swappable function pointer interface. This is both the production code AND the test seam (Samwise will inject mocks).

### Cross-Agent Context: Key Interface Contract — azure_ops_t (2026-03-10)

- **I (Frodo) must deliver:** Refactored azure_client exporting azure_ops_t vtable with these functions:
  - `azure_blob_read(ctx, blob_name, offset, size, buffer)` — GET with Range header
  - `azure_blob_write(ctx, blob_name, offset, size, buffer)` — PUT Page, 512-byte aligned, max 4 MiB/call
  - `azure_blob_size(ctx, blob_name)` — HEAD request, returns Content-Length
  - `azure_blob_truncate(ctx, blob_name, new_size)` — Set Blob Properties
  - `azure_lease_acquire(ctx, blob_name, duration)` — Acquire lease, return lease ID
  - `azure_lease_release(ctx, blob_name, lease_id)` — Release lease
  - `azure_lease_check(ctx, blob_name)` — HEAD request, return 0 if no lease, 1 if held
- **How it's used:** Aragorn's VFS layer receives a pointer to azure_ops_t at startup, calls functions through it. For production: real Azure. For tests: Samwise's mock.
- **PoC location:** `research/azure-poc/` already has the basic patterns. Refactor into `src/azure_client.c` with function pointers.
- **See design-review.md Appendix A** for full contract definition and usage patterns.

### Production Azure Client Layer (2026-03-10)

- **Files created:** `src/azure_client.c`, `src/azure_auth.c`, `src/azure_error.c`, `src/azure_client_impl.h`
- **Architecture:** All 13 azure_ops_t vtable functions implemented as static `az_*` functions, exposed via `azure_client_get_ops()` returning a const static vtable pointer. Every HTTP call goes through `execute_with_retry` → `execute_single`, so retry is guaranteed for all operations.
- **Retry-After support added:** Response header `Retry-After` (seconds format) is now parsed and used to override exponential backoff. This was a TODO from the PoC.
- **AZURE_ERR_THROTTLED added:** New error code separating 429 (→ SQLITE_BUSY) from 5xx transient errors (→ SQLITE_IOERR). This is critical for Decision 8 error mapping.
- **Client lifecycle changed:** `azure_client_create(account, container, sas_token, shared_key)` takes explicit params (PoC read from env vars). VFS layer handles env var reading. SAS token leading `?` is auto-stripped.
- **Connection reuse:** Single curl handle per client with `curl_easy_reset()` between requests. TCP keep-alive enabled (60s idle, 30s interval). This avoids TLS handshake overhead on sequential operations.
- **Key security:** `azure_client_destroy()` scrubs key material (key_raw, key_b64, sas_token) with memset before free.
- **blob_exists uses blob_get_properties:** Lightweight HEAD request; 404 mapped to exists=0 (not an error). Error struct cleared on "not found" so VFS sees clean AZURE_OK.
- **curl timeout handling:** CURLE_OPERATION_TIMEDOUT and CURLE_COULDNT_CONNECT classified as AZURE_ERR_TRANSIENT (retryable), not AZURE_ERR_CURL (fatal).
- **Internal header `azure_client_impl.h`:** Contains all public types (azure_ops_t, azure_err_t, azure_error_t, azure_buffer_t) temporarily. When Aragorn creates `azure_client.h`, public types move there. Reconciliation should be straightforward — types follow Appendix A exactly.
- **Compiles clean** with `-Wall -Wextra -pedantic -std=c11`. Links against `-lcurl -lssl -lcrypto`.

### Production Build System — pkg-config Integration (2026-03-10)

- **Problem:** `make all-production` failed on macOS because OpenSSL headers weren't found. Homebrew installs OpenSSL in non-standard paths (`/opt/homebrew/opt/openssl@3`), not `/usr/local`.
- **Solution:** Refactored Makefile to use pkg-config for OpenSSL and libcurl with graceful fallbacks:
  - `OPENSSL_CFLAGS := $(shell pkg-config --cflags openssl 2>/dev/null || echo "")`
  - `OPENSSL_LDFLAGS := $(shell pkg-config --libs openssl 2>/dev/null || echo "-lssl -lcrypto")`
  - `CURL_CFLAGS` and `CURL_LDFLAGS` similarly
  - New `CFLAGS_PROD` and `LDFLAGS_PROD` variables combine base flags with pkg-config results
- **Makefile structure:** Production object files (`azure_client.o`, `azure_auth.o`, `azure_error.o`) now use `CFLAGS_PROD`. The `all-production` target uses `CFLAGS_PROD` and `LDFLAGS_PROD`. Stub build (`make all`) remains unchanged — no OpenSSL/curl dependency.
- **Portability:** Works on macOS (Homebrew), Linux (system packages), and any environment where pkg-config is available. Falls back to hardcoded flags if pkg-config is missing.
- **Format warning fixed:** `azure_client.c:439` had `%ld` for `err->http_status` (int). Changed to `%d`.
- **Error enum completeness:** Added missing cases to `azure_err_str()` switch: `AZURE_ERR_LEASE_EXPIRED`, `AZURE_ERR_IO`, `AZURE_ERR_TIMEOUT`, `AZURE_ERR_ALIGNMENT`. Eliminated compiler warnings.
- **Compatibility alias cleanup:** Removed duplicate switch cases caused by `#define AZURE_ERR_OPENSSL AZURE_ERR_AUTH` etc. The `azure_err_str` function now uses only canonical error codes from `azure_client.h`.
- **Build verification:** Both `make all` (stub) and `make all-production` compile cleanly with zero errors. All 148 unit tests pass.

### Demo Script for Azure Integration (2026-03-10)

- **Created:** `demo/azure-demo.sh` — executable bash script demonstrating sqlite-objs with real Azure Blob Storage.
- **Functionality:**
  1. Validates environment variables (AZURE_STORAGE_ACCOUNT, AZURE_STORAGE_CONTAINER, AZURE_STORAGE_SAS or AZURE_STORAGE_KEY)
  2. Builds `sqlite-objs-shell` with `make all-production`
  3. Runs a SQLite session that creates a table, inserts rows, queries, and updates
  4. Displays results and cleanup instructions
- **Documentation:** `demo/README.md` provides setup instructions, prerequisites (libcurl/OpenSSL installation), troubleshooting guide, and explanation of what gets created in Azure.
- **Configuration flow confirmed:** `sqlite_objs_shell.c` reads env vars in `main()` and passes them to `sqlite_objs_vfs_register(1)`. The VFS internally calls `sqlite_objs_vfs_register_with_config()` which creates an `azure_client_t` with the credentials. No changes needed to shell — it already works correctly.
- **Ready to test:** Script is executable, production build works, and all prerequisites are documented. Waiting for real Azure credentials to verify end-to-end.

### Agent-10: Production Build & Demo (2026-03-10 — 07:43:07Z)

Completed orchestration summary:
- ✅ `Makefile` refactored with pkg-config integration (D12)
- ✅ Format specifiers fixed in `azure_client.c` and `azure_error.c`
- ✅ Demo resources created (`demo/azure-demo.sh`, `demo/README.md`)
- ✅ All 148 unit tests passing
- ✅ Production build verified on macOS

**Status:** SUCCESS. Next: Await C2 review from Gandalf (URL buffer fix complete); watch for auth investigation results (Agent-13).

### Agent-13: Shared Key Auth Investigation (2026-03-10 — in progress)

**Issue:** Layer 2 integration tests (Samwise) discovered Shared Key auth fails with Azurite on blob modifications (403 Forbidden after first PUT). Needs investigation to determine if root cause is Azurite validator behavior or client implementation.

**Impact:** Blocks full Layer 2 validation and production auth path. Workaround: use SAS tokens for testing.

**Status:** IN PROGRESS. Research phase started.

### Azure Parallel Write Performance Research (2026-03-10)

- **Problem:** xSync flushes dirty pages one-by-one via sequential HTTP PUT. 5000 dirty pages × ~100ms = 500 seconds. Unacceptable.
- **Azure Put Page:** One contiguous range per request, max 4 MiB, 512-byte aligned. No multi-range support in a single call.
- **Azure Batch API:** Dead end — only supports Delete Blob and Set Blob Tier. Put Page NOT supported.
- **HTTP/2:** Dead end — Azure Blob Storage only supports HTTP/1.1. No multiplexing available.
- **Azure per-blob limits:** 500 requests/sec, 60 MiB/s throughput. Plenty of headroom.
- **Recommended optimization — Phase 1 (Coalesce):** Scan dirty bitmap, merge adjacent dirty pages into single PUT requests up to 4 MiB (1024 × 4KB pages per request). Application-level optimization. Could reduce 5000 PUTs to 5-50 if writes are clustered (common for SQLite B-tree mods).
- **Recommended optimization — Phase 2 (curl_multi):** Use libcurl's multi interface for parallel HTTP requests. Single-threaded event loop, 64 concurrent connections, connection pooling. Key settings: CURLMOPT_MAX_HOST_CONNECTIONS=64, CURLMOPT_MAXCONNECTS=64. Requires new CURLM handle + pool of easy handles (replaces single curl_easy reuse pattern).
- **New API proposed:** `page_blob_write_batch(ctx, name, ranges[], nRanges, lease_id, err)` with `azure_page_range_t` struct. Additive to azure_ops_t vtable — backward compatible. Mock can loop over ranges sequentially.
- **Combined impact:** Clustered 5000 pages: 500s → ~0.2-0.5s. Scattered 5000 pages: 500s → ~5-10s.
- **Full report:** `.squad/decisions/inbox/frodo-azure-parallel-writes.md`

### Phase 2: curl_multi Batch Write Implementation (2026-03-11)

- **Implemented:** `az_page_blob_write_batch()` in `src/azure_client.c` using libcurl's `curl_multi` interface for parallel PUT Page requests.
- **Architecture:** For nRanges ≤ 1, delegates to existing sequential `az_page_blob_write()`. For nRanges > 1, creates a `CURLM *multi` handle with one `CURL *easy` handle per range, all configured as PUT Page requests with independent auth headers.
- **Key design decisions:**
  - Each easy handle gets its own `struct curl_slist *` headers — cannot share between concurrent handles.
  - SharedKey auth: each request gets its own `Authorization` header (Content-Length and x-ms-range differ per range).
  - SAS auth: token appended to shared URL once (all ranges use same URL).
  - Body data via `CURLOPT_POSTFIELDS` (no copy — aData stable per D17 btree mutex guarantee).
  - `CURLMOPT_MAX_HOST_CONNECTIONS` and `CURLMOPT_MAXCONNECTS` set to `SQLITE_OBJS_MAX_PARALLEL_PUTS` (32).
- **Retry strategy:** Failed ranges retried up to 3 times with exponential backoff (500ms base). Only retryable errors (transient/throttle) trigger retry. Non-retryable errors (auth, bad request) abort immediately.
- **Lease renewal:** Checks elapsed time each event loop iteration. If >15 seconds since last renewal, calls `az_lease_renew` using client's own curl handle (NOT from multi pool). Lease loss aborts all in-flight requests.
- **Event loop:** `curl_multi_perform` + `curl_multi_wait` (1000ms timeout). Results collected via `curl_multi_info_read` with `CURLOPT_PRIVATE` linking each handle back to its `batch_req_t` context.
- **Wired into vtable:** `.page_blob_write_batch = az_page_blob_write_batch` — VFS now uses parallel path automatically.
- **Build:** Production compiles clean with zero warnings. All 205 unit tests pass.
- **Helper types:** `batch_req_t` (per-request context), `batch_init_easy()` (setup one handle), `batch_free_req()` (cleanup).

### Phase 3: Persistent CURLM Handle + Connection Pool Tuning (2026-03-11)

- **Problem:** Phase 2 created and destroyed a `CURLM *multi` handle on every `az_page_blob_write_batch()` call (and even per retry attempt). TLS sessions and TCP connections were lost between xSync calls, requiring full TLS handshakes each time.
- **Solution:** Added persistent `CURLM *multi_handle` field to `azure_client_t` struct. Lazily initialized on first batch write call via `ensure_multi_handle()`. Destroyed in `azure_client_destroy()`. Reused across all batch calls and retry attempts.
- **Files modified:** `src/azure_client_impl.h` (struct field), `src/azure_client.c` (lazy init, reuse, destroy, tuning, jitter)
- **Connection pool tuning (set once at CURLM creation):**
  - `CURLMOPT_MAX_HOST_CONNECTIONS = SQLITE_OBJS_MAX_PARALLEL_PUTS` (32)
  - `CURLMOPT_MAXCONNECTS = SQLITE_OBJS_MAX_PARALLEL_PUTS` (32)
- **TLS session caching:** Explicit `CURLOPT_SSL_SESSIONID_CACHE = 1` on each easy handle. CURLM connection pool manages TLS session reuse automatically across calls.
- **Keep-alive tuning updated:** `CURLOPT_TCP_KEEPIDLE = 30` (was 60), `CURLOPT_TCP_KEEPINTVL = 15` (was 30). Detects dead connections faster.
- **Retry jitter added:** Backoff formula now `base_delay * (1 << retry_count) + (rand() % 100)`. PRNG seeded once in `ensure_multi_handle()`. Prevents thundering herd on simultaneous retries.
- **Easy handle lifecycle unchanged:** Still created per-range, destroyed after each batch call. Only the CURLM multi handle persists — it owns the connection pool.
- **Thread-safety documented:** Safe because xSync is serialized by SQLite's btree mutex (D17). No concurrent access to the multi handle is possible.
- **Build:** Production compiles clean with zero warnings. All 205 unit tests pass.

### Append Blob Operations for WAL Mode (2026-03-11)

- **Files modified:** `src/azure_client.h`, `src/azure_client.c`, `src/azure_client_stub.c`, `test/mock_azure_ops.h`, `test/mock_azure_ops.c`
- **3 new azure_ops_t vtable entries:** `append_blob_create`, `append_blob_append`, `append_blob_delete` — added at END of struct after `page_blob_write_batch` for backward compatibility.
- **Production implementations in azure_client.c:**
  - `az_append_blob_create`: PUT with `x-ms-blob-type: AppendBlob`, Content-Length: 0. Optional lease header. Returns on 201.
  - `az_append_blob_append`: PUT `?comp=appendblock` with raw body. Max 4 MiB enforced. Optional lease header. Returns on 201.
  - `az_append_blob_delete`: DELETE with optional lease header. Reuses same `execute_with_retry` pattern as `az_blob_delete`. Returns on 202.
- **All 3 support optional lease_id** — NULL means no lease header, non-NULL adds `x-ms-lease-id` header. Matches WAL's exclusive-mode lease pattern.
- **Stub vtable:** All 3 set to NULL (same pattern as `page_blob_write_batch`).
- **Mock implementations in mock_azure_ops.c:**
  - New `BLOB_TYPE_APPEND` enum value for blob type tracking.
  - New op indices: `OP_APPEND_BLOB_CREATE`, `OP_APPEND_BLOB_APPEND`, `OP_APPEND_BLOB_DELETE` — integrated with failure injection and call counting.
  - `mock_append_blob_create_impl`: Creates append blob, or resets to empty if re-created (checkpoint pattern).
  - `mock_append_blob_append_impl`: Appends data to buffer via `ensure_capacity` + `memcpy`.
  - `mock_append_blob_delete_impl`: Removes blob from array (same logic as `mock_blob_delete_impl`).
  - Accessor functions: `mock_get_append_data()`, `mock_get_append_size()`, `mock_reset_append_data()`.
- **Build:** Stub and production compile clean. All 207 unit tests pass.
