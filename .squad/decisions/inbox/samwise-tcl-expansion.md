# TCL Test Suite Expansion — Full Sweep Results

**Date:** 2026-03-14 | **From:** Samwise (QA)

## Decision

Expanded TCL test runner from 78 to **1,151 verified passing tests** (out of 1,187 total).
All 6 failures are platform/config issues — **zero VFS bugs found**.

## Key Facts

- **1,151 tests pass** with 0 errors (~720,042 individual assertions)
- **6 failures:** all `Inf` vs `inf` (macOS printf), dlopen message format (macOS), TCL 8.5 type name
- **15 timeouts:** meta-runners + heavy fault injection (>30s)
- **15 empty:** Windows-only, zipfile extension, meta-tests

## Team Impact

- **Aragorn (VFS):** The VFS passes crash recovery, corruption detection, WAL, pager, and IO error tests. Very strong validation.
- **Frodo (Azure):** No Azure-related failures at all — mock backend is solid.
- **CI:** Running all 1,151 tests takes ~30 minutes. Use `--quick` (10 tests, ~10s) for fast feedback. Consider a medium subset for CI.

## Runner Changes

- Added `rm -rf testdir` cleanup between test runs (prevents cascading failures)
- Expanded quick test subset from 5 to 10 tests
- Tests organized by 78 categories with comments
