# Lazy Cache Filling Implementation

**Date:** 2026-03-22 | **Author:** Aragorn | **Status:** Implemented

## Summary

Implemented lazy cache filling in `src/sqlite_objs_vfs.c` per the approved design decisions. The feature is opt-in via `prefetch=none` URI parameter; `prefetch=all` remains the default with zero behavioral changes.

## Implementation Decisions

### D-LC1: Validity Bitmap Structure
Parallel to dirty bitmap — same allocation strategy (separate `unsigned char *aValid` array), same helper function pattern. `bitmapsEnsureCapacity()` wrapper keeps both in sync.

### D-LC2: State File Binary Format
Fixed format: `SQOS` magic (4B) + version LE32 (4B) + pageSize LE32 (4B) + fileSize LE64 (8B) + bitmapSize LE32 (4B) + bitmap (N bytes) + CRC32 LE32 (4B). Total header = 24 bytes. CRC32 covers header + bitmap. Atomic write via rename.

### D-LC3: Page 1 Bootstrap Size
65536 bytes (max legal SQLite page size) downloaded at xOpen in lazy mode. This handles all page size values (512–65536) without a second round-trip.

### D-LC4: Readahead Window
Fixed 16 pages. `fetchPagesFromAzure()` issues a single `page_blob_read` for pages N through N+15, clamped to file size.

### D-LC5: Lazy Revalidation Strategy
In lazy mode, `revalidateAfterLease()` uses incremental diff to **invalidate** changed pages (clear valid bits) instead of downloading them. Downloads only happen on subsequent xRead misses. If diff exceeds 50% of pages, falls back to full download for efficiency.

### D-LC6: Write Order at Close
cache fsync → .state file (atomic rename) → .etag file → .snapshot file. This ensures the valid bitmap is persisted before the ETag that gates cache reuse.

## Files Changed
- `src/sqlite_objs_vfs.c` — ~500 lines added (bitmap helpers, state I/O, VFS method modifications)
- `src/sqlite_objs.h` — Added `prefetch` URI parameter documentation
- `test/test_integration.c` — Updated `cleanup_cache_files()` for `.state`/`.snapshot` removal
- `rust/sqlite-objs-sys/csrc/` — Synced all source files

## Test Results
- 247 unit tests: PASS
- 17 integration tests: PASS (264 total)
- Zero regressions in default `prefetch=all` mode
