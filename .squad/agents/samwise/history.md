# Project Context

- **Owner:** Quetzal Bradley
- **Project:** Azure Blob-backed SQLite (azqlite) — a drop-in replacement for SQLite where all storage is backed by Azure Blob Storage, implemented as a custom VFS layer. MIT licensed.
- **Stack:** C, SQLite VFS API, Azure Blob Storage REST API, libcurl, OpenSSL
- **SQLite source:** `sqlite-autoconf-3520000/` (do not modify unless absolutely necessary)
- **Created:** 2026-03-10

## Key Context

- Test crash recovery: commit → blob verify → machine loss → reconnect → data intact
- Test network failures: Azure unreachable mid-write, partial writes, auth failures
- Test locking: single writer, many readers (MVP 1), multi-machine (MVP 3-4)
- License: MIT

## Learnings

<!-- Append new learnings below. Each entry is something lasting about the project. -->

### Layer 1 Test Infrastructure Delivered (2026-03-10)

- **124 tests, all passing.** Zero external dependencies — pure C, no test frameworks needed.
- **Files created:**
  - `test/test_harness.h` — Minimal test framework (TEST, ASSERT_*, RUN_TEST, color output, summary). Uses setjmp/longjmp for assertion failure recovery.
  - `test/mock_azure_ops.h` — Canonical `azure_ops_t` interface definition + mock API. Also defines `azure_err_t`, `azure_error_t`, `azure_buffer_t`. This is the interface contract until Frodo delivers `azure_client.h`.
  - `test/mock_azure_ops.c` — Full mock implementation: page blobs (512-byte alignment enforced), block blobs (key-value), lease state machine (AVAILABLE→LEASED→BREAKING), failure injection (by call number or operation name), call counting, state inspection.
  - `test/test_vfs.c` — 89 active tests (mock infrastructure) + 28 VFS integration tests behind `ENABLE_VFS_INTEGRATION` flag (waiting for Aragorn's VFS).
  - `test/test_azure_client.c` — 35 active tests + 9 auth tests behind `ENABLE_AZURE_CLIENT_TESTS` flag (waiting for Frodo's client).
  - `test/test_main.c` — Test runner. Includes test files directly (shared static counters pattern).
- **Build command:** `cc -o test_runner test/test_main.c test/mock_azure_ops.c sqlite-autoconf-3520000/sqlite3.c -I sqlite-autoconf-3520000 -I test -lpthread -ldl -lm`
- **Key patterns:**
  - Test files use `#include "test_file.c"` in test_main.c (not separate compilation) to share test_harness.h statics.
  - VFS integration tests gated behind `ENABLE_VFS_INTEGRATION` — define it when linking with azqlite_vfs.o.
  - Azure client tests gated behind `ENABLE_AZURE_CLIENT_TESTS` — define it when linking with azure_client.o.
  - mock_azure_ops.h IS the authoritative interface definition until reconciliation with azure_client.h.
- **Lease state machine:** AVAILABLE → LEASED (on acquire) → BREAKING (on break with period > 0) → AVAILABLE. Immediate break (period=0) goes straight to AVAILABLE. Acquire during BREAKING returns CONFLICT.

### Testing Strategy Research (2026-03-10)

- **Four-layer test pyramid recommended:** (1) In-process C mocks via `azure_ops_t` vtable, (2) Azurite integration tests, (3) Toxiproxy fault injection, (4) Real Azure validation in CI.
- **Azurite** (MIT licensed, v3.33-3.35): Supports all our operations — page blobs, leases, block blobs, shared key auth. Known fidelity gaps: Range header edge cases (#1682), lease timing under load, IP-style URLs. Cannot simulate network failures.
- **Toxiproxy** (Shopify, MIT): TCP proxy for fault injection — latency, timeouts, connection resets, blackholes. Works between our code and Azurite. Controlled via HTTP API (port 8474).
- **Key architectural requirement:** VFS layer must accept a swappable Azure operations vtable (`azure_ops_t`) for testability. This is how SQLite's own VFS tests work (system call override pattern) and what the Azure SDK for C recommends.
- **SQLite test patterns found:** `sqlite3FaultSim()` callback system for numbered fault injection points, system call override via `xSetSystemCall()`, memdb VFS as template for in-memory testing, kvvfs as template for non-filesystem backend.
- **No C-specific Azure mock libraries exist.** Azure SDK for C uses hand-written HTTP transport mocks. We do the same but at the Azure client API level (higher, more useful).
- **LiteFS/rqlite patterns:** LiteFS passes full SQLite TCL test suite through FUSE — we should aim for same through our VFS. rqlite uses Python E2E tests for multi-node scenarios.
- **Cost:** ~$0/month for layers 1-3 (all local/free tools). ~$1/month for optional real Azure CI tests.
- **Always run Azurite in strict mode** (no `--loose`, no `--skipApiVersionCheck`). Loose mode hides bugs.
- **Full findings in:** `research/testing-strategy.md`

### Design Review — MVP 1 Architecture (2026-03-10)

- **Full design review at `research/design-review.md`.** 11 decisions (D1-D11) covering all aspects. All approved by Gandalf.
- **My testing pyramid proposal was approved (D5):** Layers 1+2 in MVP 1 (unit mocks + Azurite), layers 3+4 in MVP 2+.
- **Key architectural constraint I raised was accepted:** VFS layer MUST accept swappable azure_ops_t vtable. This is non-negotiable for testability. Gandalf used it as design gate.
- **MVP 1 test scope (from D11):** ~300 unit tests (Layer 1) + ~75 integration tests (Layer 2, Azurite). ALL MUST PASS before implementation phase ends.
- **Layer 1 deliverable:** mock_azure_ops.c with swappable implementation of azure_ops_t for in-process testing without network.
- **Layer 2 deliverable:** Integration tests against Azurite in Docker (MIT licensed). Test auth, API compatibility, end-to-end SQLite operations.

### Cross-Agent Context: Working with Aragorn and Frodo (2026-03-10)

- **I (Samwise) provide:** mock_azure_ops.c for testing, test harness that accepts both real and mock azure_ops_t.
- **Aragorn (VFS) provides:** azqlite_vfs.c that accepts azure_ops_t* pointer at init, calls functions through it.
- **Frodo (Azure client) provides:** Real azure_client.c with azure_ops_t vtable pointing to actual Azure REST API calls.
- **How it works:** At compile time, link with either mock_azure_ops.o (unit tests) or azure_client.o (integration/real). Both export identical azure_ops_t interface.
- **Test structure:** test_vfs.c calls azqlite VFS methods, which internally call whatever azure_ops_t is linked in. For unit tests: deterministic behavior. For integration: real Azurite emulator.

### Critical Interface: azure_ops_t Vtable (2026-03-10)

- **See design-review.md Appendix A** for full function signatures and semantics.
- **Functions I (Samwise) must mock:**
  - `azure_blob_read(ctx, blob_name, offset, size, buffer)` → return bytes read or error code
  - `azure_blob_write(ctx, blob_name, offset, size, buffer)` → return bytes written or error code
  - `azure_blob_size(ctx, blob_name)` → return blob size or error
  - `azure_blob_truncate(ctx, blob_name, new_size)` → return success/error
  - `azure_lease_acquire(ctx, blob_name, duration)` → return lease_id or error
  - `azure_lease_release(ctx, blob_name, lease_id)` → return success/error
  - `azure_lease_check(ctx, blob_name)` → return 1 if held, 0 if not, <0 if error
- **Mock testing strategies:** Fault injection (simulate Azure errors), latency (inject delays), state (track blob contents, leases), edge cases (EOF reads, partial writes).
