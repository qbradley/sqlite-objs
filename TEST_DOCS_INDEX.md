# sqliteObjs Test Infrastructure Documentation Index

## Available Documentation

### 1. **TEST_INFRASTRUCTURE_DETAILED.md** (770 lines)
   Comprehensive guide covering:
   - Test directory structure (9 files)
   - Complete example test with URI parameters (lines 86-123 of test_uri.c)
   - How mock system stores data in-memory
   - How reads/writes work (with code snippets)
   - blob_get_properties implementation (returns size, lease state/status, sets error detail)
   - Test harness framework and TEST_SUITE_BEGIN/RUN_TEST pattern
   - Individual test file organization (test_vfs.c as example)
   - Stub implementation files (mock_azure_ops.h and .c)
   - Persistence/reopening test pattern (integ_multi_db_independent, lines 690-719)
   - Complete "Writing New Tests" checklist
   - Key assertion macros
   - Running tests (unit vs integration)
   - Step-by-step example: URI persistence test

### 2. **TEST_QUICK_REFERENCE.md** (400+ lines)
   Quick-start guide with real code:
   - Minimal test example (3 tests: create, insert/query, persistence)
   - How to add test to test_main.c
   - Failure injection example
   - Mock state inspection example
   - URI-based test example
   - Common assertion patterns
   - Cleanup pattern
   - Build command
   - Test output example
   - Key functions quick reference

### 3. **This Document (TEST_DOCS_INDEX.md)**
   Navigation guide to all test documentation

---

## Quick Start (5 minutes)

1. Read: **TEST_QUICK_REFERENCE.md** Section 1 (Minimal Test Example)
2. Copy: Pattern into new test file
3. Build: `gcc -o test_runner test/test_main.c test/mock_azure_ops.c sqlite-autoconf-3520000/sqlite3.c -I sqlite-autoconf-3520000 -lpthread -ldl -lm`
4. Run: `./test_runner`

---

## By Task

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

