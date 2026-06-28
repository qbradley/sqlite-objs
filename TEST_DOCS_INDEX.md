# sqliteObjs Test Infrastructure Documentation Index

## 🎯 Quick Navigation

**Starting out?** → Start with TEST_QUICK_REFERENCE.md (5 min, fast gate commands)
**Need details?** → See section-specific docs below
**Release prep?** → See "Extended Testing & Validation" section

---

## Available Documentation

### 1. **TEST_QUICK_REFERENCE.md** (400+ lines)
   Quick-start guide with fast gate commands:
   - ⚡ Pre-commit validation commands
   - Minimal C test example
   - How to add tests to test_main.c
   - Failure injection & mock inspection
   - Common assertion patterns
   - Build and run commands

### 2. **TEST_INFRASTRUCTURE_DETAILED.md** (770 lines)
   Comprehensive guide covering:
   - Test directory structure (9 files)
   - Complete example test with URI parameters
   - How mock system stores data in-memory
   - Read/write operations with code snippets
   - blob_get_properties implementation
   - Test harness framework
   - Persistence/reopening patterns
   - Complete "Writing New Tests" checklist

### 3. **This Document (TEST_DOCS_INDEX.md)**
   Navigation guide to all test documentation

### 4. **test/SECURITY-TESTING.md** (600+ lines)
   Security and sanitizer details:
   - **AddressSanitizer + UBSan** (runtime memory safety) ← Use `make sanitize`
   - **MemorySanitizer** (uninitialized memory detection)
   - **ThreadSanitizer** (data race detection)
   - Static analysis tools (cppcheck, clang-tidy, scan-build)
   - Fuzzing targets (libFuzzer, AFL++)
   - Code coverage (gcov/lcov, llvm-cov)
   - Credential handling and hardening flags

### 5. **test/tcl-test-status.md** (1200+ lines)
   Status of official SQLite TCL test suite:
   - 1151 passing test files
   - ~720,042 individual assertions
   - Coverage across SELECT, DML, transactions, indexes, schema, functions, JSON, FTS5

### 6. **VFS_TEST_INFRASTRUCTURE.md** (260 lines)
   Quick reference for VFS architecture:
   - Page cache (LRU demand-paging)
   - Read/write/sync flows
   - xOpen initialization
   - Lease state machine
   - Mock page blob implementation

### 7. **TEST_INFRASTRUCTURE_QUICK_REFERENCE.md** (260 lines)
   Compact VFS reference:
   - Essential files and test execution pattern
   - Mock context usage and failure injection
   - Page cache architecture and write recording
   - Environment variables (SQLITE_OBJS_CACHE_PAGES, SQLITE_OBJS_DEBUG_TIMING)
   - Common test patterns

---

## Extended Testing & Validation

### ⚡ **Fast Gate Commands** (Run Before Commit)

```bash
# Unit tests + sanitizers (1-2 minutes)
make test-unit
make sanitize

# Add Rust binding tests if modified Rust code
cd rust && cargo test
```

### 🔄 **Stress Testing** (Pre-Release)

```bash
# Normal stress: 2× concurrent writers, 3 iterations (~5 min)
make test-stress

# Heavy stress: 4× concurrent writers, 5 iterations (~30 min)
make test-stress-heavy

# Extended property testing: 500+ operations per test (~10 min)
make test-integration-extended

# Custom multipliers via environment variables:
SQLITE_OBJS_STRESS_MULTIPLIER=3 make test-integration
PROP_TEST_OPS=1000 make test-integration
```

**What these test:**
- Concurrent lease acquisition and renewal
- Large transaction flushes (100+ dirty pages)
- Rapid open/close cycles
- Mixed read/write workloads
- Page cache eviction under load
- Dirty page coalescing edge cases

### 🧪 **Sanitizer Validation** (Memory Safety)

```bash
# Build and run unit tests with AddressSanitizer + UBSan
make sanitize

# What it catches:
#  - Buffer overflows (heap, stack, global)
#  - Use-after-free bugs
#  - Memory leaks
#  - Uninitialized memory reads
#  - Undefined behavior (signed overflow, etc.)
#  - Invalid memory access patterns
```

**Details:** See `test/SECURITY-TESTING.md` Section 2 for all sanitizer options.

### 📊 **Coverage Analysis**

```bash
make coverage
# Opens: build/coverage-report/index.html

# Aim for ≥80% line coverage on src/ files
# Check uncovered paths in:
#  - Error handling (failure injection tests)
#  - Lease renewal logic
#  - Page cache eviction
#  - Alignment calculations
```

### 🔬 **TCL Test Suite** (Official SQLite Tests)

```bash
# Smoke test (5 test files, ~1 min)
make test-tcl-quick

# Full suite (1187 test files, ~10 min)
make test-tcl

# What it validates:
#  - 1151 passing test files
#  - SELECT, DML, transactions, indexes, schema, functions, JSON, FTS5
#  - ~720,042 individual assertions
```

See `test/tcl-test-status.md` for complete test breakdown and known skips.

### 🦀 **Rust Binding Tests**

```bash
# Low-level FFI tests
cd rust && cargo test -p sqlite-objs-sys

# High-level API tests
cd rust && cargo test -p sqlite-objs

# Integration tests (if Azurite running)
cd rust && cargo test --tests -- --nocapture
```

**Current status:**
- `sqlite-objs-sys`: Config size, register_uri, const verification
- `sqlite-objs`: URI builder, error handling
- `tests/perf_matrix.rs`: Performance benchmarks (requires Azurite)

---

## Known Testing Gaps & External Dependencies

### ⚠️ **External Azure / Network Chaos Gaps**

**Status:** Azurite-backed integration tests run locally via `make test-integration`, but production Azure retry-loop behavior still needs external fault injection.

- Use `make test-integration` for local Azurite validation.
- Use `./scripts/release-gate.sh --azure` when real Azure credentials are available.
- Toxiproxy-style network chaos remains deferred for future work.

**Next steps for contributors:**
1. Run `make test-integration` for local Azurite coverage.
2. Set AZURE_STORAGE_ACCOUNT, AZURE_STORAGE_CONTAINER, AZURE_STORAGE_SAS for real Azure validation.
3. Run `./scripts/release-gate.sh --azure`.

### 🚫 **Toxiproxy (Not Implemented)**

**What it would test:** Network chaos (latency, packet loss, connection resets)

**Why not critical for MVP:**
- AddressSanitizer catches memory corruption
- Unit tests cover failure injection (mock layer)
- Real Azure provides network reliability guarantees

**Future consideration:** Add after MVP 1 release for chaos testing.

### 🔐 **Real Azure Testing (Manual)**

**For final validation before release:**

```bash
# Set real Azure credentials
export AZURE_STORAGE_ACCOUNT="your-account"
export AZURE_STORAGE_CONTAINER="your-container"
export AZURE_STORAGE_SAS="sv=2024-..."  # or AZURE_STORAGE_KEY

# Run release gate against real Azure-backed Rust integration tests
./scripts/release-gate.sh --azure
```

**Cost:** ~$0.01 per test run (minimal storage operations)

---

## By Task

### "I want to write a C test"
→ Read: TEST_QUICK_REFERENCE.md (Section 1)
→ Add to: test/test_main.c
→ Build: `make test-unit` or `make sanitize`

### "I want to validate memory safety"
→ Run: `make sanitize`
→ Details: `test/SECURITY-TESTING.md`, Section 2

### "I want to run stress tests before release"
→ Run: `make test-stress` (5 min, 2× multiplier)
→ Or: `make test-stress-heavy` (30 min, 4× multiplier)
→ Custom: `SQLITE_OBJS_STRESS_MULTIPLIER=N make test-integration`

### "I want to check code coverage"
→ Run: `make coverage`
→ View: `build/coverage-report/index.html`
→ Details: `test/SECURITY-TESTING.md`, Section 4

### "I want to test the TCL suite"
→ Run: `make test-tcl-quick` (smoke, 1 min)
→ Or: `make test-tcl` (full, 10 min)
→ Status: `test/tcl-test-status.md`

### "I want to write a Rust test"
→ Create: `rust/sqlite-objs/tests/integration_test.rs`
→ Build: `cd rust && cargo test`
→ Example: `rust/sqlite-objs/examples/basic.rs`
→ Details: rust/README.md

### "Integration tests fail with AZURE_ERR_NETWORK"
→ First check: `make test-integration` starts and manages local Azurite
→ If testing real Azure: run `./scripts/release-gate.sh --azure`
→ For retry/network chaos: Toxiproxy-style validation is still future work
→ Tracking: See "Known Testing Gaps" above

### "I want to debug a failed test"
→ Build with debugging symbols: `make clean && make CFLAGS="-g -O0" test-unit`
→ Run under gdb: `gdb ./build/test_main`
→ Or use AddressSanitizer output: `make sanitize 2>&1 | grep -A 5 "ERROR"`

---

## Key Files in Codebase

### Test Files
- `/workspace/qbradley/sqlite-objs/test/test_harness.h` — Framework (TEST, ASSERT macros)
- `/workspace/qbradley/sqlite-objs/test/test_main.c` — Entry point (includes all tests)
- `/workspace/qbradley/sqlite-objs/test/test_vfs.c` — Unit tests (2500+ lines)
- `/workspace/qbradley/sqlite-objs/test/test_integration.c` — Integration tests (Azurite)
- `/workspace/qbradley/sqlite-objs/test/test_uri.c` — URI config tests
- `/workspace/qbradley/sqlite-objs/test/test_coalesce.c` — Write coalescing tests
- `/workspace/qbradley/sqlite-objs/test/test_wal.c` — WAL mode tests
- `/workspace/qbradley/sqlite-objs/test/test_azure_client.c` — Azure client tests
- `/workspace/qbradley/sqlite-objs/test/test_chaos.c` — Failure injection tests

### Mock Implementation
- `/workspace/qbradley/sqlite-objs/test/mock_azure_ops.h` — Public API (200 lines)
- `/workspace/qbradley/sqlite-objs/test/mock_azure_ops.c` — Implementation (1040 lines)

### Rust Tests
- `/workspace/qbradley/sqlite-objs/rust/sqlite-objs-sys/src/lib.rs` — FFI tests
- `/workspace/qbradley/sqlite-objs/rust/sqlite-objs/src/lib.rs` — API tests
- `/workspace/qbradley/sqlite-objs/rust/sqlite-objs/tests/perf_matrix.rs` — Performance benchmarks

---

## Testing Workflow

### Development Workflow

```bash
# 1. Make changes
# 2. Quick validation (30 seconds)
make test-unit

# 3. Check memory safety (1 minute)
make sanitize

# 4. If Rust changes:
cd rust && cargo test

# 5. Before pushing PR:
make test-tcl-quick
make test-integration    # if Azure changes

# 6. Final validation (before merge to main):
make test-stress
make test-tcl
```

### Release Workflow

```bash
# 1. All CI tests must pass
make test-unit test-integration

# 2. Sanitizers clean
make sanitize

# 3. TCL suite passes
make test-tcl

# 4. Stress tests pass (2×, 3 iterations)
make test-stress

# 5. (Optional) Heavy stress + property testing
make test-stress-heavy
make test-integration-extended

# 6. Coverage ≥80% on src/
make coverage

# 7. Real Azure validation (if credentials available)
./scripts/release-gate.sh --azure
```

---

## Documentation Completeness

✓ Test directory structure
✓ Test declaration and registration
✓ Mock blob storage implementation
✓ Read/write operations with code
✓ blob_get_properties signature and behavior
✓ Failure injection mechanisms
✓ VFS registration patterns
✓ Persistence/reopening patterns
✓ All assertion macros
✓ Test harness framework
✓ Unit vs integration tests
✓ ✨ **EXTENDED: Fast gate commands** ← New (Phase 4)
✓ ✨ **EXTENDED: Stress testing modes** ← New (Phase 4)
✓ ✨ **EXTENDED: Sanitizer documentation** ← New (Phase 4)
✓ ✨ **EXTENDED: Rust test integration** ← New (Phase 4)
✓ ✨ **EXTENDED: Testing gaps & external deps** ← New (Phase 4)
✓ Build and run commands
✓ Complete working examples
✓ Quick reference guide
✓ Navigation index (this document)

---

## Support for Contributors

- **Ask questions:** Check the "By Task" section above
- **Report issues:** If tests fail unexpectedly, include:
  - `make test-unit` output
  - `make sanitize` output (if applicable)
  - OS and compiler version
  - Full error message with stack trace (if available)

---

**Last Updated:** June 28, 2026 (Phase 4 — Validation Documentation)
**Codebase:** /workspace/qbradley/sqlite-objs
**Contact:** Gimli (Rust Developer & Documentation Owner)

### "I want to write a test that opens a database with URI parameters"
→ Read: TEST_INFRASTRUCTURE_DETAILED.md, Section 2
→ Example: test_uri.c lines 86-123

### "I want to understand how the mock stores data"
→ Read: TEST_INFRASTRUCTURE_DETAILED.md, Section 3
→ Key structure: mock_blob_t (lines 82-95 of mock_azure_ops.c)
→ Storage: Array in mock_azure_ctx_t (128 max blobs)

### "I want to test persistence (data survives close/reopen)"
→ Read: TEST_INFRASTRUCTURE_DETAILED.md, Section 7
→ Example: test_integration.c lines 690-719
→ Or: TEST_QUICK_REFERENCE.md Section 1, Test 3 (persistence test)

### "I want to see blob_get_properties implementation"
→ Read: TEST_INFRASTRUCTURE_DETAILED.md, Section 4
→ Code: mock_azure_ops.c lines 480-513
→ Returns: size, lease_state ("available"/"leased"/"breaking"), lease_status ("locked"/"unlocked")
→ Does NOT: set ETag (Phase 3 feature)

### "I want to inject failures to test error handling"
→ Read: TEST_INFRASTRUCTURE_DETAILED.md, Section 3 (Failure Injection)
→ Or: TEST_QUICK_REFERENCE.md, Section 3
→ APIs: mock_set_fail_at(), mock_set_fail_operation(), mock_set_fail_operation_at()

### "I want to inspect blob data directly in tests"
→ Read: TEST_QUICK_REFERENCE.md, Section 4 (Mock Inspection)
→ Functions: mock_get_page_blob_data(), mock_get_page_blob_size(), mock_get_call_count()

### "I want to understand test registration"
→ Read: TEST_INFRASTRUCTURE_DETAILED.md, Section 5
→ Pattern: TEST() macro + run_*_tests() function + test_main.c
→ Example: test_vfs.c lines 2316-2560

### "I want to run integration tests against real Azure"
→ Read: TEST_INFRASTRUCTURE_DETAILED.md, Section 9
→ Command: `./test/run-integration.sh`
→ Requirements: Azurite running on 127.0.0.1:10000

---

## Key Files in Codebase

### Test Files
- `/Users/qbradley/src/sqlite/test/test_harness.h` — Framework (TEST, ASSERT macros)
- `/Users/qbradley/src/sqlite/test/test_main.c` — Entry point (includes all tests)
- `/Users/qbradley/src/sqlite/test/test_vfs.c` — Unit tests (2500+ lines)
- `/Users/qbradley/src/sqlite/test/test_integration.c` — Integration tests (Azurite)
- `/Users/qbradley/src/sqlite/test/test_uri.c` — URI config tests
- `/Users/qbradley/src/sqlite/test/test_coalesce.c` — Write coalescing tests
- `/Users/qbradley/src/sqlite/test/test_wal.c` — WAL mode tests

### Mock Implementation
- `/Users/qbradley/src/sqlite/test/mock_azure_ops.h` — Public API (200 lines)
- `/Users/qbradley/src/sqlite/test/mock_azure_ops.c` — Implementation (1040 lines)

### Key Functions in mock_azure_ops.c
- `mock_azure_create()` line 816 — Allocate context
- `mock_azure_destroy()` line 821 — Free context
- `mock_reset()` line 833 — Clear all state
- `mock_page_blob_write()` line ~280 — In-memory write simulation
- `mock_page_blob_read()` line ~373 — In-memory read simulation
- `mock_blob_get_properties()` line 480 — Get size and lease state
- Failure injection functions: lines 177-230

---

## Assertion Macros

From test_harness.h (all auto-fail with colored output):

**Integer/Order:**
- `ASSERT_EQ(a, b)` — a == b
- `ASSERT_NE(a, b)` — a != b
- `ASSERT_GT(a, b)` — a > b
- `ASSERT_GE(a, b)` — a >= b
- `ASSERT_LT(a, b)` — a < b
- `ASSERT_LE(a, b)` — a <= b

**Boolean:**
- `ASSERT_TRUE(x)` — x is truthy
- `ASSERT_FALSE(x)` — x is falsy

**Pointers:**
- `ASSERT_NULL(x)` — x == NULL
- `ASSERT_NOT_NULL(x)` — x != NULL

**Strings:**
- `ASSERT_STR_EQ(a, b)` — strcmp(a, b) == 0
- `ASSERT_STR_NE(a, b)` — strcmp(a, b) != 0

**Memory:**
- `ASSERT_MEM_EQ(a, b, len)` — memcmp(a, b, len) == 0

**SQLite/Azure:**
- `ASSERT_OK(rc)` — rc == SQLITE_OK
- `ASSERT_ERR(rc, expected)` — rc == expected error code
- `ASSERT_AZURE_OK(rc)` — rc == AZURE_OK
- `ASSERT_AZURE_ERR(rc, expected)` — rc == expected Azure error

---

## Example: Complete Test File Template

```c
/* test_my_feature.c */
#include "../sqlite-autoconf-3520000/sqlite3.h"
#include "mock_azure_ops.h"
#include "test_harness.h"
#include "../src/sqlite_objs.h"

static mock_azure_ctx_t *g_ctx = NULL;
static azure_ops_t      *g_ops = NULL;

static void setup(void) {
    if (g_ctx) mock_reset(g_ctx);
    else       g_ctx = mock_azure_create();
    g_ops = mock_azure_get_ops();
}

/* Test definitions */
TEST(my_test_1) {
    setup();
    int rc = sqlite_objs_vfs_register_with_ops(g_ops, g_ctx, 0);
    ASSERT_OK(rc);
    sqlite3 *db = NULL;
    rc = sqlite3_open_v2("test.db", &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, "sqlite-objs");
    ASSERT_OK(rc);
    char *errmsg = NULL;
    rc = sqlite3_exec(db, "CREATE TABLE t (id INTEGER);", NULL, NULL, &errmsg);
    ASSERT_OK(rc);
    if (errmsg) sqlite3_free(errmsg);
    sqlite3_close(db);
}

TEST(my_test_2) {
    setup();
    /* ... another test ... */
}

/* Test runner */
void run_my_feature_tests(void) {
    TEST_SUITE_BEGIN("My Feature");
    RUN_TEST(my_test_1);
    RUN_TEST(my_test_2);
    TEST_SUITE_END();
}
```

Then add to test_main.c:
```c
#include "test_my_feature.c"

int main(void) {
    run_my_feature_tests();
    return test_harness_summary();
}
```

---

## Testing Workflow

### Unit Tests (Fast, no network)
```bash
gcc -o test_runner test/test_main.c test/mock_azure_ops.c \
    sqlite-autoconf-3520000/sqlite3.c \
    -I sqlite-autoconf-3520000 -lpthread -ldl -lm
./test_runner
```

### Integration Tests (Requires Azurite)
```bash
./test/run-integration.sh
```

---

## Architecture Overview

```
SQLite Application
       ↓
sqlite3_open_v2("db.db", flags, "sqlite-objs")
       ↓
sqlite_objs_vfs (Aragorn's VFS implementation)
       ↓
azure_ops_t vtable (swappable interface)
       ↓
    ┌──┴──┬────────────────────┐
    ↓     ↓                    ↓
  Real   Mock            Stub (Phase 1)
 Client (Frodo)    (for unit tests)    (fallback)
    ↓     ↓                    ↓
 libcurl  In-memory         Error
 + Azure  blob storage      returns
 REST API (no network)
```

**In Tests:** Use `azure_ops_t *ops = mock_azure_get_ops()` and register with:
```c
sqlite_objs_vfs_register_with_ops(ops, ctx, 0);
```

---

## Documentation Completeness

✓ Test directory structure
✓ Test declaration and registration
✓ Mock blob storage implementation
✓ Read/write operations with code
✓ blob_get_properties signature and behavior
✓ Failure injection mechanisms
✓ VFS registration patterns
✓ Persistence/reopening patterns
✓ All assertion macros
✓ Test harness framework
✓ Unit vs integration tests
✓ Build and run commands
✓ Complete working examples
✓ Quick reference guide
✓ Navigation index (this document)

---

**Last Updated:** March 14, 2025
**Codebase:** /Users/qbradley/src/sqlite
