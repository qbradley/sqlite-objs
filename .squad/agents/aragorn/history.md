# Aragorn's Learning History

## Learnings

### If-Match Header Authentication Signing Bug (2025-01-09)

**Problem**: All VFS integration tests were failing with "disk I/O error" (SQLITE_IOERR, SQLITE error 10) due to Azure Blob Storage returning 403 Forbidden. Root cause was the `If-Match` header being incorrectly included in the `extra_x_ms` array for authentication signing.

**Azure SharedKey Signing Structure**: The signing string has specific positions for standard HTTP headers (lines 183-200 in `azure_auth.c`):
```
VERB\n
Content-Encoding\n
Content-Language\n
Content-Length\n
Content-MD5\n
Content-Type\n
Date\n
If-Modified-Since\n
If-Match\n           ← standard header position (line 193)
If-None-Match\n
If-Unmodified-Since\n
Range\n
CanonicalizedHeaders\n   ← only x-ms-* headers belong here
CanonicalizedResource
```

**The Bug**: In `az_page_blob_write()`, the `If-Match` header was being placed in the `extra_x_ms` array, which caused TWO signature errors:
1. `azure_auth_sign_request()` included `If-Match:value` in the **canonicalized headers section** (wrong — only `x-ms-*` headers go there)
2. The standard If-Match field in the signing string (line 193) was always empty `"\n"` even when If-Match was being sent

**The Fix Pattern**:
- **Never** put standard HTTP headers (If-Match, If-None-Match, etc.) in the `extra_x_ms` array
- Add standard headers as explicit parameters through the auth and client chain
- Pass the value to `azure_auth_sign_request()` where it goes in the correct position in the signing string
- Add the HTTP header to the curl header list AFTER signing, not through `extra_x_ms`

**Implementation**:
1. Added `const char *if_match` parameter to `azure_auth_sign_request()` 
2. Used it at line 193: `if_match ? if_match : ""`
3. Added `if_match` parameter to both `execute_single()` and `execute_with_retry()`
4. Passed `if_match` through the call chain and to `azure_auth_sign_request()`
5. Added the `If-Match` header to curl's header list separately (after signing)
6. Simplified `az_page_blob_write()` from 4 conditional arrays to 2 (with/without lease)
7. Updated `batch_init_easy()` to pass `if_match` to signing function
8. All other callers pass NULL for `if_match`

**Result**: Authentication signatures now match correctly. Azure Client integration tests (8) all pass. VFS integration tests went from 0/32 passing to 23/41 passing.

**Key Principle**: The `extra_x_ms` array is ONLY for `x-ms-*` headers. Standard HTTP headers must be handled through explicit parameters to ensure they're placed correctly in the signing string.

### Azure Lease + If-Match Conflict with Azurite (2025-01-09)

**Problem**: 13 integration tests were failing after fixing the If-Match auth bug. 11 tests failed with "database is locked" (SQLITE_BUSY) during COMMIT, and 1 WAL test had a stale ETag sidecar after checkpoint.

**SQLITE_BUSY Root Cause**: The VFS was sending BOTH `x-ms-lease-id` AND `If-Match` headers in the same write request. When xSync called `page_blob_write_batch()`, Azurite rejected the request with HTTP 412 Precondition Failed (AZURE_ERR_PRECONDITION), which the VFS translated to SQLITE_BUSY. This happened because:
1. When we hold a lease, we already have exclusive write access
2. Sending If-Match with the lease is redundant and causes Azurite to reject the request
3. The precondition failure made SQLite think the database was locked by another process

**The Fix Pattern**:
- **Never send If-Match when you hold a lease** — the lease provides exclusive access
- Only use If-Match for optimistic concurrency when you DON'T have a lease
- In `xSync`, check `hasLease(p)` before passing `p->etag` to write functions:
  ```c
  /* Don't send If-Match when we hold a lease */
  (hasLease(p) || !p->etag[0]) ? NULL : p->etag
  ```

**ETag Sidecar Staleness**: After a WAL checkpoint + batch write, the ETag sidecar wasn't being updated until xClose. The fix was to call `writeEtagFile()` immediately after a successful batch write in xSync:
```c
if (aerr.etag[0] != '\0') {
    memcpy(p->etag, aerr.etag, sizeof(p->etag));
    if (p->cacheReuse && p->zCachePath) {
        writeEtagFile(p->zCachePath, p->etag);  // Persist immediately
    }
}
```

**Busy Timeout Addition**: Added `sqlite3_busy_timeout(db, 10000)` to the test helper as a safety measure for multi-client scenarios, though the real fix was removing If-Match with lease.

**Result**: All 41 integration tests now pass. The key insight is that Azure blob leases and If-Match ETags are mutually exclusive strategies for write protection — use one or the other, never both.

