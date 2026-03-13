# Azure Blob Storage Analysis for sqlite-objs

> Research findings from the Azure REST API proof-of-concept.
> Author: Frodo (Azure Expert) | Date: 2026-03-10

## Executive Summary

**Verdict: Direct Azure REST API implementation in C is fully viable.**

The PoC (`research/azure-poc/`) compiles and runs on macOS with only libcurl and OpenSSL. All offline tests pass — HMAC-SHA256 auth signing, base64 round-trips, error XML parsing, retry logic, and alignment validation. The code is structured for production use.

Key recommendation: **Use page blobs for the main database file, block blobs for journal/WAL files, and Azure leases for SQLite's locking model.**

---

## 1. API Version Decision

**Target: `2024-08-04`** (latest stable as of March 2026)

Rationale:
- Latest stable version in the Azure REST API specs on GitHub
- Supports all features we need (page blobs, leases, large block blobs)
- Put Blob for block blobs supports up to 5000 MiB in a single operation
- No deprecated features we depend on
- Set via `x-ms-version` header on every request

---

## 2. Authentication

### 2.1 Shared Key (HMAC-SHA256)

The PoC implements complete Shared Key authentication:

1. **StringToSign construction** — 12 standard header lines + canonicalized x-ms-* headers + canonicalized resource path with sorted query parameters
2. **HMAC-SHA256 signing** — using OpenSSL's `HMAC()` function with the base64-decoded account key
3. **Authorization header** — `SharedKey <account>:<base64-signature>`

**Edge cases discovered from SDK source code review:**
- `Date` header line must be EMPTY when `x-ms-date` is used (avoids proxy mangling)
- `Content-Length` must be the string representation of the body length, or empty string for zero-length bodies (NOT "0")
- x-ms-* headers must be sorted case-insensitively, emitted as lowercase
- Query parameters in canonicalized resource must be sorted by parameter name
- Multiple values for the same query parameter must be comma-separated (rare in blob operations)

### 2.2 SAS Tokens (Recommended for MVP)

SAS (Shared Access Signatures) tokens are simpler and more secure for client-side use:
- **No signing code needed** — just append `?<sas_token>` to the URL
- **Scoped permissions** — can limit to specific containers, blobs, operations, time windows
- **No account key exposure** — the key never leaves the server that generates the SAS
- **The PoC supports both** — set `AZURE_STORAGE_SAS` to use SAS, or `AZURE_STORAGE_KEY` for Shared Key

**Recommendation:** Use SAS tokens for the MVP. Implement Shared Key for server-side tools or when SAS generation isn't practical. Both are implemented in the PoC.

---

## 3. Blob Type Analysis

### 3.1 Page Blobs — **Primary choice for SQLite database files**

| Property | Value |
|---|---|
| Max size | 8 TiB |
| Page size | 512 bytes (fixed) |
| Write alignment | Must be 512-byte aligned (offset AND length) |
| Max write per request | 4 MiB |
| Read alignment | Not required (Range reads work at any offset) |
| Random access | Yes — native support |
| Latency | Low (designed for IOPS) |
| Cost | Higher than block blobs |

**Why page blobs are perfect for SQLite:**
- SQLite's minimum page size is 512 bytes — exact match with Azure page size
- SQLite's default page size is 4096 bytes — 8× Azure page size, perfectly aligned
- Random read/write is the core operation model for database files
- Resize operation exists (for growing the database)
- Page blob max of 8 TiB exceeds SQLite's practical limits

**Alignment math:**
```
SQLite page 512  → 1 Azure page  (512 / 512 = 1)
SQLite page 1024 → 2 Azure pages (1024 / 512 = 2)
SQLite page 4096 → 8 Azure pages (4096 / 512 = 8)
SQLite page 8192 → 16 Azure pages (8192 / 512 = 16)
```

All SQLite page sizes are powers of 2 ≥ 512, so they always align perfectly.

### 3.2 Block Blobs — **Good for journal and WAL files**

| Property | Value |
|---|---|
| Max size | ~190.7 TiB |
| Block size | Up to 4000 MiB per block |
| Access pattern | Sequential / whole-object |
| Random write | Not supported (must rewrite blocks) |
| Cost | Lowest |

**Use for:**
- Journal files (written sequentially, read entirely on recovery)
- WAL files (append-heavy, read on checkpoint)
- The entire journal can be uploaded as a single block blob

**Not for:** Main database file (no random write support)

### 3.3 Append Blobs — **Interesting but limited**

| Property | Value |
|---|---|
| Max size | ~195 GiB |
| Append block max | 4 MiB |
| Access pattern | Append-only |
| Random write | Not supported |
| Modification | Cannot modify existing data |

**Analysis for WAL:** Append blobs are conceptually appealing for WAL (Write-Ahead Log) since WAL is append-heavy. However:
- WAL files need to be truncated/reset during checkpoints
- Append blobs can't be truncated — you'd need to delete and recreate
- The 195 GiB limit is lower than block blobs
- No clear advantage over block blobs for our use case

**Verdict:** Skip append blobs. Use block blobs for journal/WAL.

### 3.4 Cost Comparison (US East, as of 2024)

| Blob Type | Storage (GB/month) | Write (10K ops) | Read (10K ops) |
|---|---|---|---|
| Block (Hot) | $0.018 | $0.055 | $0.0044 |
| Block (Cool) | $0.01 | $0.10 | $0.01 |
| Page (Standard) | $0.045 | $0.005 | $0.005 |
| Page (Premium) | $0.15 | included* | included* |

\* Premium page blobs include a certain IOPS/throughput based on blob size.

**Key insight:** Page blob storage is ~2.5× more expensive than hot block blob storage, BUT page blob operations are cheaper per-op. For a database workload with many small reads/writes, page blobs may actually be cheaper overall despite higher storage cost.

---

## 4. Lease Operations for SQLite Locking

### 4.1 How Azure Leases Work

- **Duration:** 15-60 seconds, or infinite (-1)
- **Lease ID:** A GUID returned by Azure on acquire
- **States:** Available → Leased → (Release) → Available, or Leased → (Break) → Breaking → Broken → Available
- **Scope:** One lease per blob at a time. While leased, writes and deletes require the lease ID.

### 4.2 Mapping to SQLite Lock Model

| SQLite Lock | Azure Implementation |
|---|---|
| NONE | No lease held |
| SHARED | Read without lease (reads don't require lease) |
| RESERVED | Acquire lease (blocks other writers) |
| PENDING | Lease acquired, waiting for readers to finish |
| EXCLUSIVE | Same lease from RESERVED (already have exclusive write access) |
| Unlock | Release lease |

**Critical insight:** Azure leases are writer-exclusive but reader-permissive. Reads NEVER require a lease. This maps well to SQLite's model where SHARED locks allow concurrent reads.

### 4.3 Lease Duration Strategy

**Recommendation: Use 30-second leases with renewal.**

- 15 seconds is too short — network latency could cause lease expiry mid-transaction
- 60 seconds holds the lock too long on failure
- Infinite leases risk orphaned locks on client crash
- 30 seconds with a background renewal thread provides safety with reasonable timeout on failure

**Break lease** provides the escape hatch — if a client crashes, another client can break the lease (with a short break period) to recover.

### 4.4 Limitation: No Reader Counting

Azure leases don't support "shared" leases (multiple holders). This means:
- We can't enforce a strict SHARED → RESERVED → EXCLUSIVE lock escalation at the Azure level
- Reads always succeed (no SHARED lock enforcement needed)
- Writes require the lease (EXCLUSIVE enforcement works)
- For multi-writer coordination, leases are sufficient
- For reader-writer coordination with reader counting, we'd need a separate mechanism (e.g., a metadata blob tracking active readers)

**For MVP:** This is fine. SQLite in WAL mode allows concurrent readers with a single writer. The lease handles writer exclusion perfectly.

---

## 5. Error Handling and Retry Strategy

### 5.1 Error Response Format

Azure returns errors as XML with the `x-ms-error-code` response header:

```xml
<Error>
  <Code>ServerBusy</Code>
  <Message>The server is currently unable to receive requests.</Message>
</Error>
```

The PoC parses both the XML body and the header. The header is faster to check and always present.

### 5.2 Error Classification

| Category | HTTP Status | Azure Codes | Action |
|---|---|---|---|
| Transient | 408, 429, 500, 502, 503, 504 | ServerBusy, InternalError, OperationTimedOut | Retry with backoff |
| Auth failure | 401, 403 | AuthenticationFailed, AuthorizationFailure | Fail immediately |
| Not found | 404 | BlobNotFound, ContainerNotFound | Fail (or create) |
| Conflict | 409 | LeaseAlreadyPresent, BlobAlreadyExists | Fail or handle specifically |
| Precondition | 412 | ConditionNotMet, LeaseIdMismatch | Fail |
| Client error | 400 | Various | Fail (bug in our code) |

### 5.3 Retry Policy

Implemented as exponential backoff with jitter, matching Azure SDK patterns:

```
Attempt 0: 500ms  + jitter(0-500ms)
Attempt 1: 1000ms + jitter(0-500ms)
Attempt 2: 2000ms + jitter(0-500ms)
Attempt 3: 4000ms + jitter(0-500ms)
Attempt 4: 8000ms + jitter(0-500ms)
```

- Max 5 retries (configurable)
- Max delay capped at 30 seconds
- Only transient errors trigger retry
- Jitter prevents thundering herd on shared infrastructure

This matches the patterns found in azure-sdk-for-c, azure-sdk-for-go, and azure-sdk-for-python.

---

## 6. SDK Source Code Review Findings

### 6.1 Azure SDK for C (azure-sdk-for-c)

- Designed for embedded/constrained environments (microcontrollers)
- Retry logic is hardcoded loops, not pluggable policies
- Platform abstraction for `sleep()` — we need to provide our own (easy with `usleep()`)
- Uses `az_result` for error propagation — we adopted a similar `azure_err_t` enum
- Minimal — validates our approach of not depending on the SDK

### 6.2 Azure SDK for Go

- Uses a pipeline pattern with retry policies
- Default retry: 3 attempts, exponential backoff, jitter
- Classifies 408/429/500/502/503/504 as retriable
- Uses `x-ms-error-code` header for error classification (we do the same)

### 6.3 Azure SDK for Python

- Similar pipeline pattern
- Default: 3 retries, 0.8s initial backoff, 2× multiplier, 60s max
- Adds "retry-after" header support (we should add this — Azure sometimes tells you when to retry)

### 6.4 Azure SDK for .NET

- Most mature SDK
- Uses `Azure.Core` pipeline with retry handler
- Default: 3 retries, 0.8s backoff, exponential
- Has special handling for 409 (Conflict) on lease operations — treats some as retriable

**Key takeaway from all SDKs:** Our retry configuration (5 retries, 500ms base, exponential + jitter) is slightly more aggressive than the defaults (3 retries, 800ms base) but appropriate for a database-like workload where availability matters more.

### 6.5 Missing from PoC (Should Add for Production)

1. **`Retry-After` header support** — Azure sometimes includes this; we should respect it
2. **Request ID logging** — we capture `x-ms-request-id` but should log it consistently for support tickets
3. **Connection pooling** — libcurl supports this via `CURLOPT_TCP_KEEPALIVE`; important for latency
4. **HTTP/2** — libcurl supports it; reduces latency for multiple concurrent requests
5. **Content-MD5 verification** — Azure supports it for integrity; important for database pages

---

## 7. Architecture Recommendation

```
┌──────────────────────────────────────────┐
│              SQLite VFS Layer            │
│         (xRead, xWrite, xLock, ...)     │
├──────────────────────────────────────────┤
│           Azure Blob Client              │
│  ┌─────────────┐  ┌──────────────────┐  │
│  │ Page Blob    │  │ Block Blob       │  │
│  │ (main DB)    │  │ (journal/WAL)    │  │
│  └─────────────┘  └──────────────────┘  │
│  ┌─────────────┐  ┌──────────────────┐  │
│  │ Lease Mgr    │  │ Retry Engine     │  │
│  │ (locking)    │  │ (exp. backoff)   │  │
│  └─────────────┘  └──────────────────┘  │
├──────────────────────────────────────────┤
│  Auth: HMAC-SHA256 (OpenSSL)            │
│  HTTP: libcurl                           │
└──────────────────────────────────────────┘
```

---

## 8. Open Questions for Gandalf

1. **Local page cache** — Should we cache pages in memory (or local disk) to reduce Azure round-trips? A write-back cache would dramatically improve performance but adds complexity. This is an architecture question.

2. **WAL mode vs rollback journal** — WAL mode is better for concurrent reads, but WAL requires shared memory for reader coordination. With remote storage, how do we handle the WAL index (shm file)?

3. **Connection string format** — What should the VFS filename look like? Options:
   - `azure://<account>/<container>/<blob>`
   - `azblob://<account>.blob.core.windows.net/<container>/<blob>`
   - Standard URI with a registered VFS name

4. **Multi-region considerations** — Should we support geo-redundant storage (RA-GRS) for read replicas? This could give us read scaling for free.

---

## 9. Files Produced

| File | Description |
|---|---|
| `research/azure-poc/azure_blob.h` | Complete API header — all types, functions, constants |
| `research/azure-poc/azure_auth.c` | HMAC-SHA256 auth signing, base64, RFC 1123 dates |
| `research/azure-poc/azure_blob.c` | Page blob, block blob, lease operations via libcurl |
| `research/azure-poc/azure_error.c` | Error XML parsing, classification, retry with backoff |
| `research/azure-poc/azure_poc_main.c` | Test harness — offline + live tests |
| `research/azure-poc/Makefile` | Build system (macOS/Linux) |
| `research/azure-blob-analysis.md` | This document |

---

## 10. Confidence Level

**High confidence** that this approach works. The PoC:
- Compiles cleanly with `-Wall -Wextra -Wpedantic`
- All offline tests pass (auth signing, HMAC, base64, error parsing, retry logic)
- Code structure is production-ready (not throwaway)
- API contracts match Azure documentation exactly
- Ready for live testing with real Azure credentials
