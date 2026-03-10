# Project Context

- **Owner:** Quetzal Bradley
- **Project:** Azure Blob-backed SQLite (azqlite) — a drop-in replacement for SQLite where all storage is backed by Azure Blob Storage, implemented as a custom VFS layer. MIT licensed.
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
