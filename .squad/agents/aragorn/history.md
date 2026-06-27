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


### Concurrency Bug — Snapshot Isolation Violation (2026-06-27)

**Problem**: Concurrent writers to the same sqlite-objs database experienced lost inserts, duplicate rowids, and silent data loss. Reproduction showed 40 inserts → 17 distinct IDs.

**Root Cause**: The `revalidateAfterLease()` function in `src/sqlite_objs_vfs.c` was re-downloading the entire blob when it detected an ETag mismatch during `xLock(RESERVED)`. This happened AFTER SQLite had already read pages at SHARED lock level, creating a **mixed snapshot** that violated SQLite's snapshot isolation guarantee.

**The Race**:
1. Connection A opens DB → downloads blob at ETag E1
2. Connection B opens DB → downloads same blob at ETag E1
3. Connection B: BEGIN → locks SHARED → reads pages (freelist, btree root, etc.)
4. Connection A: locks RESERVED → writes → commits → blob now at ETag E2
5. Connection B: tries to lock RESERVED
   - `revalidateAfterLease()` sees E1 (cached) vs E2 (blob)
   - **BUG**: Re-downloads blob at E2, overwrites cache
   - But SQLite's pager already has E1 pages cached!
   - Result: freelist/rowid state from E1, but blob data from E2
   - Outcome: duplicate rowids, corrupted btree, lost inserts

**The Fix**: Changed `revalidateAfterLease()` to **return SQLITE_BUSY immediately** when ETag mismatch is detected, instead of re-downloading. This forces the client to retry the transaction with a fresh connection that downloads a consistent snapshot in xOpen.

**Code Changes**:
- `src/sqlite_objs_vfs.c`: Modified `revalidateAfterLease()` to fail fast with SQLITE_BUSY
- Removed ~185 lines of unreachable re-download code (lazy diff, incremental diff, full download)
- `src/sqlite_objs.h`: Added `revalidation_busy` metric to track stale transaction failures

**Why This Is Correct**: SQLite requires snapshot isolation — all reads within a transaction must see a consistent point-in-time database state. Re-downloading mid-transaction breaks this. Failing with SQLITE_BUSY matches how local SQLite behaves with shared memory locking.

**Expected Behavior**: With `busy_timeout=5000`, concurrent writers will automatically retry. Connection gets SQLITE_BUSY → retries → re-opens with fresh snapshot → succeeds. All inserts persist with unique rowids.

**File References**:
- `src/sqlite_objs_vfs.c` lines 2098-2165 (`revalidateAfterLease()`)
- `src/sqlite_objs.h` line 185 (new `revalidation_busy` metric)
- See `FIX-SUMMARY.md` for full analysis

**Key Principle**: When blob storage state changes between xOpen and xLock(RESERVED), we must fail the transaction cleanly rather than attempting to fix it mid-flight. Snapshot isolation is non-negotiable.

### Correction — Refresh Before Reads, BUSY After Reads (2026-06-27)

The final implementation is more nuanced than fail-fast on every ETag mismatch. `xLock(SHARED)` now HEAD-checks and refreshes the MAIN_DB cache before SQLite reads pages, preserving existing sequential multi-client behavior. `revalidateAfterLease()` only returns SQLITE_BUSY when the blob changed after pages were read under the current SHARED lock; otherwise it can safely refresh before any pager-visible reads occur. This final design passes all 295 unit tests and all 42 Azurite integration tests.

### Concurrent Writer Snapshot Isolation Fix (2026-06-27)

**Problem:** Concurrent writers losing inserts due to mixed snapshot (stale pages in pager cache, fresh pages in VFS cache). Data corruption from stale cache overwriting committed changes.

**Solution implemented:** VFS-level snapshot isolation enforcement via stale-cache detection.

**Changes to `src/sqlite_objs_vfs.c`:**
- `xLock(SHARED)` calls `revalidateBeforeRead()` to HEAD-check blob and refresh cache before SQLite reads pages
- `sqliteObjsRead()` tracks MAIN_DB reads after current SHARED lock
- `revalidateAfterLease()` returns `SQLITE_BUSY` if ETag mismatch detected after pages already read
- Added `revalidation_busy` metric to track retries

**Key insight:** SQLite's transaction model requires snapshot isolation. Must refresh stale snapshots before reads, but fail fast (SQLITE_BUSY) if staleness detected after reads — cannot re-download mid-transaction.

**Result:** 42/42 integration tests pass, concurrent-writer regression test serializes all 4 writers with all 40 inserts persisted, no data loss.

### Phase 2 VFS Test Hooks — Sync Interleaving (2026-06-27)

**Context:** Phase 1 test infrastructure added chaos primitives (ETag clearing, time offset). Phase 2 extends this with deterministic crash/partial-write simulation via sync interleaving hooks.

**Implementation:** Added test-only hook API behind `SQLITE_OBJS_TEST` for observing/aborting xSync operations at critical points:
- `beforePageBlobResize` — before MAIN_DB page blob resize (line 1903)
- `beforeBatchPageWrite` — before batch page write via curl_multi (line 2001)
- `beforeSeqPageWrite` — before sequential page write fallback (line 2062)
- `beforeJournalUpload` — before journal block blob upload (line 1837)
- `beforeWalUpload` — before WAL block blob upload (line 1775)

**API Design:**
- Hook function signature: `int (*)(void *ctx, const char *blobName)`
- Return 0 = proceed normally, nonzero = abort with `SQLITE_IOERR_FSYNC`
- Single registration function `sqlite_objs_test_set_sync_hooks()` with ctx + 5 hook parameters
- Global hook state stored in static variables (test-only, zero production symbol leakage verified)

**Key Principles:**
- Hooks guarded by `#ifdef SQLITE_OBJS_TEST` at declaration, storage, registration, and call sites
- Return SQLite error codes (not process abort) to enable controlled failure injection
- No assert() for validation — test suites include mock/stub paths where preconditions may not hold
- Production builds have zero hook symbols (verified via `nm` on production object files)
- Test builds include hook symbols (verified via `nm` on test object files)

**Files Modified:**
- `src/sqlite_objs.h` lines 219-259: Hook typedef and registration API
- `src/sqlite_objs_vfs.c` lines 48-72: Hook storage and registration implementation
- `src/sqlite_objs_vfs.c` lines 1775, 1837, 1903, 2001, 2062: Hook call sites in xSync paths

**Validation:** All 312 unit tests pass. Production build has no hook symbols; test build has 6 hook symbols (`g_test_sync_hook_ctx` + 5 function pointers + `sqlite_objs_test_set_sync_hooks`).

**Use Case:** Enables deterministic chaos testing for crash recovery, partial writes, and multi-operation interleaving (e.g., fail after resize but before page write, fail journal upload after MAIN_DB sync).
