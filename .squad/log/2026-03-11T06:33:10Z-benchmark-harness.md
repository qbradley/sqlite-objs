# Session Log — Benchmark Harness

**Timestamp:** 2026-03-11T06:33:10Z

## Summary

Benchmark harness completed. Three-binary subprocess architecture. speedtest1 (vanilla) and speedtest1-azure (with azqlite VFS) run as isolated processes. Main harness measures wall-clock time via `gettimeofday()`. Supports --local-only, --azure-only, --size N, --output csv/text.

**Files:**
- `benchmark/benchmark.c` — Main harness
- `benchmark/speedtest1.c` — Official SQLite speedtest1 (unmodified upstream)
- `benchmark/speedtest1_wrapper.c` — VFS registration wrapper
- `benchmark/Makefile` — Build system
- `benchmark/README.md` — User documentation

**Status:** ✅ Local-only benchmarks functional. Azure benchmarks require production build and env vars.

## Decision Reference

D14: Benchmark Harness Design
