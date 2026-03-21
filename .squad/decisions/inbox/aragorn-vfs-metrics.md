# Decision: VFS Activity Metrics with PRAGMA Exposure

**Date:** 2026-07 | **From:** Aragorn (VFS Implementation)

## Summary

Implemented per-connection VFS activity metrics (27 counters) exposed via `PRAGMA sqlite_objs_stats` and `SQLITE_OBJS_FCNTL_STATS`. Counters cover disk I/O, Azure blob I/O, cache behavior, lease lifecycle, sync operations, revalidation, journal/WAL uploads, and Azure errors.

## Key Decisions

1. **Per-connection, not global:** Metrics live in `sqliteObjsFile.metrics`, zeroed at xOpen. Avoids cross-connection contamination. Existing `g_xread_count` globals preserved for backward compatibility.

2. **Single text result format:** `PRAGMA sqlite_objs_stats` returns key=value text (one per line) rather than a virtual table result set. Trade-off: less SQL-native but drastically simpler implementation — no need for custom vtable, function registration, or result-set machinery.

3. **FCNTL op codes:** `SQLITE_OBJS_FCNTL_STATS` (201) returns `sqlite3_malloc`'d string; `SQLITE_OBJS_FCNTL_STATS_RESET` (202) zeroes all counters. Follows existing `SQLITE_OBJS_FCNTL_DOWNLOAD_COUNT` (200) pattern.

4. **sqlite3_int64 for all counters:** Even seemingly small counters (lease_acquires) use `sqlite3_int64` to avoid overflow in long-running benchmarks.

5. **Zero-overhead design:** All instrumentation is `p->metrics.field++` or `+= bytes`. No allocations, no branches, no locking.

## Files Changed

- `src/sqlite_objs.h` — Added `sqlite_objs_metrics` struct, `SQLITE_OBJS_FCNTL_STATS`, `SQLITE_OBJS_FCNTL_STATS_RESET`
- `src/sqlite_objs_vfs.c` — Added `metrics` field to `sqliteObjsFile`, `formatMetrics()` helper, instrumented 14 code paths, added PRAGMA/FCNTL handlers
- `test/test_vfs.c` — 7 new tests (Section 19: VFS Activity Metrics)
- `rust/sqlite-objs-sys/csrc/` — Synced

## Test Results

All 295 tests pass (288 existing + 7 new metrics tests).
