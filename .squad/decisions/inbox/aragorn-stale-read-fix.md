# Decision: Stale-Read Race Condition Fix — ETag Revalidation After Lease

**Date:** 2026-03-15
**From:** Aragorn (SQLite/C Dev)
**Status:** Implemented and verified

## Context

Gimli's concurrent contention test (20 threads × independent connections to the same Azure blob) revealed a stale-read race condition in the VFS. Between `xOpen` (which downloads the blob without a lease) and `xLock` (which acquires the lease for writes), another client could modify the blob. The local cache from `xOpen` would then be stale, causing lost updates.

## Decision

Implemented **Approach 1: Re-download after lease acquisition** from Gimli's analysis.

When `sqliteObjsLock` acquires a new lease (transitioning from no-lease to having-lease), it calls `revalidateAfterLease()` which:

1. **Fast path:** Compares the ETag from the `lease_acquire` Azure response with the stored ETag from `xOpen`. If they match, the cache is valid — no extra I/O needed.
2. **Slow path:** If the lease response has no ETag, performs a HEAD request via `blob_get_properties` to get the current ETag and blob size.
3. **Re-download:** On ETag mismatch, re-downloads the full blob into the cache file, resets the dirty bitmap, and updates size/pageSize tracking.
4. **Error handling:** On failure, releases the just-acquired lease before returning the error to SQLite, preventing orphaned leases.

## Rationale

- Most correct of the three approaches — guarantees the client always reads the latest committed data after acquiring the lock
- Invisible to SQLite — the pager doesn't need to know about the re-download
- Minimal cost in the common (no-contention) case: just one string comparison of ETags from the lease response (zero extra HTTP calls)
- Only adds cost when contention actually occurs: one HEAD + one GET (re-download)

## Impact

- **Files changed:** `src/sqlite_objs_vfs.c` and `rust/sqlite-objs-sys/csrc/sqlite_objs_vfs.c` (both must be kept in sync — no automated mechanism exists)
- **Test:** `concurrent_client_contention` now passes — counter = 20 with zero lost updates
- **No breaking changes:** Existing unit tests (247) and sanitizer tests all pass unchanged

## Team Note

The Rust test suite compiles C sources from `rust/sqlite-objs-sys/csrc/`, not from `src/`. Any C changes must be applied to both locations. Consider adding a sync check to CI.
