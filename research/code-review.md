# Code Review: sqlite-objs MVP 1 — Reviewer Gate

> **Reviewer:** Gandalf (Lead/Architect)
> **Date:** 2026-03-10
> **Scope:** All `src/` files — VFS, Azure client, auth, error handling, public API, shell
> **Reference:** `research/design-review.md` decisions D1–D11
> **Purpose:** D10 reviewer gate — assess readiness for real Azure demo

---

## 1. `src/sqlite_objs_vfs.c` — Core VFS Implementation (Aragorn)

**Verdict: APPROVE WITH CONDITIONS**

### Critical Issues

**C1: Device characteristics contradict the design spec (D4)**

```c
// Line 693-694 — CURRENT (wrong)
return SQLITE_IOCAP_ATOMIC512 | SQLITE_IOCAP_SAFE_APPEND;
```

The design review explicitly states:

> Device characteristics: `SQLITE_IOCAP_SEQUENTIAL | SQLITE_IOCAP_POWERSAFE_OVERWRITE | SQLITE_IOCAP_SUBPAGE_READ`. NOT ATOMIC — journal safety is needed.

**Why this matters:** `SQLITE_IOCAP_ATOMIC512` tells SQLite that 512-byte aligned writes are guaranteed atomic, which allows SQLite to *skip journal entries* for certain operations. Our xSync flushes dirty pages one-at-a-time to Azure — if any PUT fails mid-flush, the database is partially written. Without a journal (because SQLite thought it wasn't needed), there's no recovery. This is a **data corruption risk.**

`SQLITE_IOCAP_SAFE_APPEND` is also wrong — our page_blob_resize and page_blob_write are separate HTTP calls, so an extension is not atomic.

**Fix:**
```c
return SQLITE_IOCAP_SEQUENTIAL
     | SQLITE_IOCAP_POWERSAFE_OVERWRITE
     | SQLITE_IOCAP_SUBPAGE_READ;
```

**Must fix before demo.** This is a correctness issue that could cause silent data loss.

### Important Issues

**I1: xSync unconditionally resizes the page blob on every flush (lines 486–494)**

Every `xSync` calls `page_blob_resize` regardless of whether the blob needs resizing. For a database that hasn't grown, this is a wasted HTTP request per sync — doubling the cost of every commit.

**Fix:** Track the blob's current Azure-side size and only resize when `nData` (aligned) exceeds it.

**I2: `dirtyEnsureCapacity` doesn't zero newly-grown bitmap bytes (lines 179–190)**

When the buffer grows (via `bufferEnsure → realloc`), `dirtyEnsureCapacity` reallocates the bitmap but only memsets to zero on *initial* allocation (`p->aDirty == NULL`). On growth, new bytes contain uninitialized memory.

In practice this is safe because `xSync` only iterates up to `nPages = ceil(nData/pageSize)`, and `nData` is properly bounded by actual writes. But it's a latent bug — if any code path ever checks dirty bits beyond `nData`, uninitialized bits could cause spurious writes. **Fix by zeroing the new region:**

```c
int oldNeeded = p->aDirty ? dirtyBitmapSize(/* old nAlloc */) : 0;
// ... realloc ...
if (needed > oldNeeded) {
    memset(pNew + oldNeeded, 0, needed - oldNeeded);
}
```

### Minor Issues

- **M1:** Unreachable `return SQLITE_NOTFOUND` at line 675 after the switch default clause.
- **M2:** Journal truncate zeroing arithmetic at line 428 is convoluted — the ternary guard is correct but hard to read. Consider simplifying.

### Design Compliance

| Decision | Status | Notes |
|----------|--------|-------|
| D2 (DELETE journal, no WAL) | ✅ Compliant | `iVersion=1`, WAL rejected in `xFileControl` |
| D3 (Lease-based locking) | ✅ Compliant | Two-level model: SHARED=no-op, RESERVED+=lease, release on unlock |
| D4 (Full-blob cache, dirty bitmap) | ⚠️ Partial | Cache/bitmap correct; **device characteristics wrong** (C1) |
| D6 (VFS registration, szOsFile, delegation) | ✅ Compliant | `szOsFile = max(ours, default)`, temp files delegated, methods delegated correctly |
| D7 (Filename = blob name) | ✅ Compliant | Direct mapping, `..` rejection, leading slash stripping |
| D8 (Error mapping) | ✅ Compliant | `azureErrToSqlite` correctly maps conflicts→BUSY, network→IOERR |
| Lock state machine | ✅ Sound | No race conditions — single-threaded, lease acquire is idempotent (checks `hasLease()` first) |
| Journal workflow | ✅ Correct | Journal xSync uploads block blob; DB xSync flushes dirty pages. SQLite guarantees ordering. |

---

## 2. `src/azure_client.h` — Interface Contract (Aragorn)

**Verdict: APPROVE**

### Critical Issues

None.

### Design Compliance

| Check | Status |
|-------|--------|
| All 13 operations present | ✅ 4 page blob + 2 block blob + 3 common + 4 lease = 13 |
| Signatures match Appendix A | ✅ Exact match |
| `azure_err_t` enum complete | ✅ 15 error codes covering all scenarios |
| `azure_error_t` fields | ✅ code, http_status, error_code, error_message, request_id |
| `azure_buffer_t` contract | ✅ Callee allocates data, caller frees |
| `AZURE_ERR_THROTTLE` compat alias | ✅ Bridges naming between VFS and client code |

Clean, well-documented, correct. The vtable is the most important interface in the system and it's right.

---

## 3. `src/azure_client.c` — Production Azure Client (Frodo)

**Verdict: APPROVE WITH CONDITIONS**

### Critical Issues

**C2: Stack buffer overflow risk in URL construction (line 173–187)**

```c
char url[2048];
build_blob_url(client, blob_name, url, sizeof(url));  // snprintf — safe

if (query && *query) {
    strcat(url, "?");      // UNSAFE — no bounds check
    strcat(url, query);    // UNSAFE
}
if (client->use_sas) {
    strcat(url, ...);      // UNSAFE — SAS tokens can be 500+ chars
    strcat(url, client->sas_token);  // UNSAFE
}
```

Account name (256 max) + container (256 max) + blob name (512 max) + base URL template (~50 chars) = ~1074 chars before query/SAS. SAS tokens are typically 300–500 chars. Query params add more. Total can exceed 2048.

**Fix:** Use `snprintf` with bounds checking for all URL assembly, or compute the required size first and heap-allocate.

### Important Issues

**I3: `azure_err_str` switch uses `#define` aliases that shadow enum values (via `azure_client_impl.h`)**

`azure_error.c` includes `azure_client_impl.h` which defines:
```c
#define AZURE_ERR_HTTP       AZURE_ERR_BAD_REQUEST
#define AZURE_ERR_TRANSIENT  AZURE_ERR_SERVER
#define AZURE_ERR_CURL       AZURE_ERR_NETWORK
#define AZURE_ERR_OPENSSL    AZURE_ERR_AUTH
```

The switch in `azure_err_str` uses these aliased names, which means:
- `AZURE_ERR_HTTP` → `AZURE_ERR_BAD_REQUEST` — so `AZURE_ERR_BAD_REQUEST` appears twice (once as the enum, once as the alias)
- `AZURE_ERR_OPENSSL` → `AZURE_ERR_AUTH` — `AZURE_ERR_AUTH` appears twice in the switch

This is fragile. If the enum values change, the switch could have duplicate case labels (compiler error) or miss cases. The aliases should be resolved — either use the canonical names in the switch or eliminate the aliases.

**I4: `CURLOPT_POSTFIELDS` with `CUSTOMREQUEST "PUT"` is fragile**

Lines 307–313 use `CURLOPT_POSTFIELDS` to send the request body for PUT requests. This works but is technically a POST body mechanism being overridden by `CUSTOMREQUEST`. The body will have `Transfer-Encoding` or `Content-Length` from POST semantics. In practice this works against Azure REST APIs, but `CURLOPT_UPLOAD = 1L` with a read callback would be more correct for PUT bodies.

Not blocking for demo, but document this as technical debt.

### Minor Issues

- **M3:** `rand()` for jitter (azure_error.c:186) is unseeded and not thread-safe. Call `srand(time(NULL))` at client initialization. For production, use `arc4random()` or read from `/dev/urandom`.
- **M4:** Single `curl_handle` per client means the client is not thread-safe. Fine for MVP 1 but should be documented in `azure_client.h`.

### Design Compliance

| Decision | Status | Notes |
|----------|--------|-------|
| D1 (Page blobs for DB, block blobs for journal) | ✅ Compliant | Both implemented correctly |
| D1 (512-byte alignment) | ✅ Compliant | `az_page_blob_write` validates offset and length alignment |
| D8 (Retry: 5 attempts, 500ms exponential, jitter) | ✅ Compliant | `AZURE_MAX_RETRIES=5`, `AZURE_RETRY_BASE_MS=500`, jitter added |
| D8 (Retry-After support) | ✅ Compliant | Header captured and used in `azure_compute_retry_delay` |
| D8 (429→BUSY, 5xx→IOERR) | ✅ Compliant | `azure_classify_http_error` maps correctly |
| D9 (SAS + Shared Key) | ✅ Compliant | SAS precedence, key base64-decoded at init |
| Resource cleanup | ✅ Good | Key material scrubbed on destroy, curl handle cleaned up |
| Connection reuse | ✅ | `curl_easy_reset` preserves connection, TCP keep-alive enabled |

---

## 4. `src/azure_auth.c` — HMAC-SHA256 Signing (Frodo)

**Verdict: APPROVE WITH NOTES**

### Important Issues

**I5: `snprintf` return value can advance pointer past buffer end (line 176)**

```c
p += snprintf(p, (size_t)(end - p), ...);
```

`snprintf` returns the number of characters that *would have been written* (excluding null terminator), not the actual count. If the output is truncated, `p` advances past `end`. Subsequent calls with `(size_t)(end - p)` produce a wrapped-around huge value (unsigned), causing a buffer overflow.

In practice, the 4096-byte `string_to_sign` buffer is generous enough for all realistic inputs (method names, headers, and paths are bounded). **But this is a latent security risk.** Fix by clamping:

```c
int n = snprintf(p, (size_t)(end - p), ...);
if (n < 0 || p + n >= end) return AZURE_ERR_INVALID_ARG;
p += n;
```

### Design Compliance

| Check | Status |
|-------|--------|
| StringToSign format (Azure spec) | ✅ Correct — 12 header lines, canonicalized headers, canonicalized resource |
| HMAC-SHA256 via OpenSSL | ✅ Using `HMAC()` with `EVP_sha256()` |
| Header canonicalization (lowercase, sorted) | ✅ `qsort` + `tolower` |
| Query parameter canonicalization (sorted) | ✅ `strtok_r` + `qsort` |
| Base64 encode/decode | ✅ OpenSSL BIO chain, no-newline flag |
| Buffer overflow risks | ⚠️ Latent (I5) — bounded in practice |
| SAS token handling | ✅ Leading `?` stripped in `azure_client_create` |

---

## 5. `src/azure_error.c` — Error Classification (Frodo)

**Verdict: APPROVE**

### Minor Issues

- **M5:** Switch in `azure_err_str` is not exhaustive — missing `AZURE_ERR_LEASE_EXPIRED`, `AZURE_ERR_NOMEM`, `AZURE_ERR_IO`, `AZURE_ERR_TIMEOUT`, `AZURE_ERR_ALIGNMENT`, `AZURE_ERR_UNKNOWN`, `AZURE_ERR_BAD_REQUEST`, `AZURE_ERR_SERVER`, `AZURE_ERR_NETWORK`. Some are covered by `#define` aliases but the canonical enum values aren't. Add a `default: return "Unknown error";` (already present, so it's functional, but explicit cases would be cleaner).

### Design Compliance

| Decision | Status | Notes |
|----------|--------|-------|
| D8 (429→THROTTLED) | ✅ | Explicit check at line 123 |
| D8 (5xx→TRANSIENT) | ✅ | 408, 500, 502, 503, 504 all → TRANSIENT |
| D8 (401/403→AUTH) | ✅ | Lines 146–147 |
| D8 (404→NOT_FOUND) | ✅ | Line 148 |
| D8 (409→CONFLICT) | ✅ | Line 149 |
| D8 (412→PRECONDITION) | ✅ | Line 149 |
| Azure error codes (ServerBusy, etc.) | ✅ | Lines 138–140 |
| Retry-After parsing | ✅ | Captured in header callback, used in delay computation |
| Exponential backoff formula | ✅ | `base * 2^attempt + jitter`, capped at 30s |

---

## 6. `src/sqlite_objs.h` — Public API

**Verdict: APPROVE**

Clean, minimal, well-documented. Three registration functions covering all use cases:

1. `sqlite_objs_vfs_register()` — env var config (production)
2. `sqlite_objs_vfs_register_with_config()` — explicit config (programmatic)
3. `sqlite_objs_vfs_register_with_ops()` — mock injection (testing)

The test seam (`ops`/`ops_ctx` in `sqlite_objs_config_t`) is properly documented and does NOT compromise the production API. Users who don't use it never see it — the struct zero-initializes to NULL, which means "use production client." This is exactly right.

---

## 7. `src/sqlite_objs_shell.c` — CLI Wrapper

**Verdict: APPROVE**

The `#define main shell_main` / `#include "shell.c"` / `#undef main` pattern is the standard approach for wrapping the SQLite CLI (used by CBS and others). Clean, correct, and the error message clearly lists the required env vars.

One note: the VFS is registered as **default** (`makeDefault=1`), which is correct for the shell use case — `sqlite3_open()` (which the shell uses) needs the default VFS.

---

## 8. `src/azure_client_impl.h` — Internal Header

**Verdict: APPROVE WITH NOTES**

Well-structured separation: public types in `azure_client.h`, private internals here.

**Note:** The `#define` aliases (lines 39–44) mapping Frodo's original error code names to canonical enum values are a pragmatic solution but create the fragility noted in I3. Consider migrating to canonical names throughout and removing the aliases after MVP 1.

---

## Overall Verdict

### **APPROVE WITH CONDITIONS**

The code is architecturally sound, follows the design decisions faithfully in all major areas, and demonstrates strong engineering quality. The VFS layer, Azure client, and auth/error modules integrate cleanly through the `azure_ops_t` vtable exactly as designed. The codebase is ready for a real Azure demo **after fixing these 2 conditions:**

### Conditions (Must Fix Before Demo)

| # | File | Issue | Severity | Effort |
|---|------|-------|----------|--------|
| **C1** | `sqlite_objs_vfs.c:693` | Device characteristics: replace `ATOMIC512 \| SAFE_APPEND` with `SEQUENTIAL \| POWERSAFE_OVERWRITE \| SUBPAGE_READ` per D4. **Data corruption risk.** | Critical | 5 min |
| **C2** | `azure_client.c:173-187` | URL construction: replace `strcat` with bounds-checked `snprintf` to prevent stack buffer overflow. | Critical | 30 min |

### Recommended Fixes (Soon After Demo)

| # | File | Issue | Priority |
|---|------|-------|----------|
| I1 | `sqlite_objs_vfs.c:486` | Track blob size, skip redundant resize in xSync | High |
| I2 | `sqlite_objs_vfs.c:179` | Zero newly-grown dirty bitmap bytes | Medium |
| I3 | `azure_error.c:26` | Resolve `#define` alias fragility in switch statement | Medium |
| I4 | `azure_client.c:307` | Document CURLOPT_POSTFIELDS+PUT as tech debt | Low |
| I5 | `azure_auth.c:176` | Clamp snprintf return to prevent pointer overshoot | Medium |

### What's Right

- **Architecture holds.** The `azure_ops_t` vtable cleanly separates VFS from client. Mock injection works without compromising production paths.
- **Locking is correct.** The two-level lease model (D3) is properly implemented with no race conditions. Lease renewal at write and sync boundaries covers the expiry risk.
- **Journal workflow is correct.** Journal sync → block blob upload. DB sync → dirty page flush. SQLite enforces ordering. Crash recovery via journal is sound.
- **Error handling is thorough.** Classification, retry logic, and SQLite error mapping all follow D8 faithfully.
- **Auth works for both paths.** SAS tokens and Shared Key HMAC-SHA256 are both implemented correctly.

### Assessment

Aragorn delivered a solid VFS implementation with one dangerous device-characteristics bug (C1) — likely a misunderstanding of what ATOMIC means in SQLite's pager context. Frodo's Azure client layer is production-quality with good retry logic and error handling; the URL overflow (C2) is the kind of thing that works in testing but could bite in production with long SAS tokens.

Fix C1 and C2, and this code is ready for a real Azure demo.

— Gandalf
