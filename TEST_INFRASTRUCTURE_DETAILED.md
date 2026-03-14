# sqliteObjs Test Infrastructure Guide

This guide shows how to write new integration tests that fit the existing patterns.

---

## 1. Test Directory Structure

```
/Users/qbradley/src/sqlite/test/
├── test_harness.h              # Minimal C test framework (TEST, RUN_TEST, ASSERT macros)
├── test_main.c                 # Test runner entry point (includes all test files)
├── mock_azure_ops.h            # Mock Azure ops interface (public API)
├── mock_azure_ops.c            # Mock implementation (in-memory blob storage)
├── test_vfs.c                  # Unit tests for VFS (mock-based)
├── test_azure_client.c         # Azure client wrapper tests
├── test_integration.c          # Integration tests (real Azure/Azurite)
├── test_coalesce.c             # Coalescing write optimization tests
├── test_wal.c                  # WAL mode tests
├── test_uri.c                  # URI-based per-file config tests
├── run-integration.sh           # Integration test wrapper (manages Azurite)
└── README-INTEGRATION.md        # Integration test documentation
```

---

## 2. Example Test Pattern: Opening Database with URI Parameters

From **test_uri.c** (line 86-123) — test that uses `sqlite3_open_v2()` with URI params:

```c
TEST(uri_fallback_to_global) {
    /* Setup: create fresh mock context and get operations vtable */
    if (uri_ctx) mock_reset(uri_ctx);
    else         uri_ctx = mock_azure_create();
    uri_ops = mock_azure_get_ops();

    /* Register VFS with global mock ops */
    int rc = sqlite_objs_vfs_register_with_ops(uri_ops, uri_ctx, 0);
    ASSERT_OK(rc);

    /* Open database without URI params → uses global mock ops */
    sqlite3 *db = NULL;
    rc = sqlite3_open_v2("fallback_test.db", &db,
                          SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                          "sqlite-objs");
    ASSERT_OK(rc);
    ASSERT_NOT_NULL(db);

    /* Create table and insert data */
    char *errmsg = NULL;
    rc = sqlite3_exec(db,
        "CREATE TABLE t1 (id INTEGER PRIMARY KEY, val TEXT);"
        "INSERT INTO t1 VALUES (1, 'hello');",
        NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "  SQL error: %s\n", errmsg ? errmsg : "unknown");
        sqlite3_free(errmsg);
    }
    ASSERT_OK(rc);

    /* Verify data reads back */
    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(db, "SELECT val FROM t1 WHERE id=1;", -1, &stmt, NULL);
    ASSERT_OK(rc);
    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_ROW);
    ASSERT_STR_EQ(sqlite3_column_text(stmt, 0), "hello");
    sqlite3_finalize(stmt);

    sqlite3_close(db);
}
```

**Key Points:**
- Tests open with `SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI` flags
- URI params syntax: `"file:db.db?azure_account=acct&azure_container=cont&azure_key=key"`
- VFS registered via `sqlite_objs_vfs_register_with_ops(ops, ctx, flags)`
- Assertions use `ASSERT_OK()` for SQLite, `ASSERT_AZURE_OK()` for Azure errors

---

## 3. Test Stub/Mock System

### Overview
The mock system simulates Azure Blob Storage entirely **in-memory**. Tests do NOT contact Azure.

### How Tests Register the VFS with Stub Ops

From **test_uri.c** setup pattern (lines 24-31):

```c
static mock_azure_ctx_t *uri_ctx = NULL;   /* In-memory blob storage */
static azure_ops_t      *uri_ops = NULL;   /* VFS operations vtable */

static void uri_setup(void) {
    if (uri_ctx) mock_reset(uri_ctx);      /* Clear previous state */
    else         uri_ctx = mock_azure_create();  /* Allocate fresh context */
    uri_ops = mock_azure_get_ops();        /* Get pointer to mock ops vtable */
}

/* In test: */
int rc = sqlite_objs_vfs_register_with_ops(uri_ops, uri_ctx, 0);
ASSERT_OK(rc);
```

### How Data is Stored (In-Memory Blobs)

**mock_blob_t structure** (mock_azure_ops.c, lines 82-95):

```c
typedef struct {
    char            name[BLOB_NAME_LEN];    /* "db.db", "db.db-journal", etc. */
    blob_type_t     type;                   /* BLOB_TYPE_PAGE, BLOCK, APPEND */
    uint8_t        *data;                   /* Heap-allocated data buffer */
    int64_t         size;                   /* Logical size */
    int64_t         capacity;               /* Allocated capacity */

    /* Lease state machine */
    mock_lease_state_t lease_state;         /* AVAILABLE, LEASED, BREAKING */
    char               lease_id[LEASE_ID_LEN];  /* Unique lease ID */
    int                lease_duration;
    time_t             lease_acquired_at;
    int                break_period;
} mock_blob_t;
```

**Storage:** All blobs stored in array in `mock_azure_ctx_t`:

```c
struct mock_azure_ctx {
    mock_blob_t  blobs[MAX_BLOBS];          /* Up to 128 blobs */
    int          blob_count;                /* Number of active blobs */
    /* ... call counting, failure injection, write recording ... */
};
```

### How Reads/Writes Work

**Page blob write** (mock_azure_ops.c, lines 280-340):

```c
static azure_err_t mock_page_blob_write(...) {
    mock_azure_ctx_t *ctx = (mock_azure_ctx_t *)vctx;
    azure_err_t rc = pre_call(ctx, OP_PAGE_BLOB_WRITE, err);
    if (rc != AZURE_OK) return rc;

    /* Find blob by name */
    mock_blob_t *b = find_blob(ctx, name);
    if (!b) {
        set_error(err, 404, "BlobNotFound", "...");
        return AZURE_ERR_NOT_FOUND;
    }

    /* Validate alignment (512-byte) */
    if (offset % PAGE_BLOB_ALIGNMENT != 0 || len % PAGE_BLOB_ALIGNMENT != 0) {
        set_error(err, 400, "InvalidArgument", "...");
        return AZURE_ERR_ALIGNMENT;
    }

    /* Resize blob if needed */
    if (offset + len > b->capacity) {
        int64_t new_cap = ((offset + len + PAGE_BLOB_ALIGNMENT - 1) /
                           PAGE_BLOB_ALIGNMENT) * PAGE_BLOB_ALIGNMENT;
        b->data = realloc(b->data, new_cap);
        b->capacity = new_cap;
    }

    /* Copy data into the buffer */
    memcpy(b->data + offset, data, len);
    if (offset + len > b->size) {
        b->size = offset + len;  /* Update logical size */
    }

    /* Record write for coalescing tests */
    if (ctx->write_record_count < MOCK_MAX_WRITE_RECORDS) {
        ctx->write_records[ctx->write_record_count++] = 
            (mock_write_record_t){offset, len};
    }

    return AZURE_OK;
}
```

**Page blob read** (mock_azure_ops.c, lines 373-410):

```c
static azure_err_t mock_page_blob_read(...) {
    mock_azure_ctx_t *ctx = (mock_azure_ctx_t *)vctx;
    azure_err_t rc = pre_call(ctx, OP_PAGE_BLOB_READ, err);
    if (rc != AZURE_OK) return rc;

    mock_blob_t *b = find_blob(ctx, name);
    if (!b) {
        set_error(err, 404, "BlobNotFound", "...");
        return AZURE_ERR_NOT_FOUND;
    }

    /* Allocate output buffer */
    out->data = (uint8_t *)malloc(len);
    out->capacity = len;
    out->size = 0;

    /* Read from blob, zero-fill if past EOF */
    if (offset >= b->size) {
        memset(out->data, 0, len);
        out->size = len;
    } else if (offset + len <= b->size) {
        memcpy(out->data, b->data + offset, len);
        out->size = len;
    } else {
        /* Partial read + zero fill */
        size_t avail = b->size - offset;
        memcpy(out->data, b->data + offset, avail);
        memset(out->data + avail, 0, len - avail);
        out->size = len;
    }

    return AZURE_OK;
}
```

---

## 4. blob_get_properties Implementation

From **mock_azure_ops.c** (lines 480-513):

```c
static azure_err_t mock_blob_get_properties(void *vctx, const char *name,
                                           int64_t *size,
                                           char *lease_state_out,
                                           char *lease_status_out,
                                           azure_error_t *err) {
    mock_azure_ctx_t *ctx = (mock_azure_ctx_t *)vctx;
    azure_err_t rc = pre_call(ctx, OP_BLOB_GET_PROPERTIES, err);
    if (rc != AZURE_OK) return rc;

    /* Find blob */
    mock_blob_t *b = find_blob(ctx, name);
    if (!b) {
        set_error(err, 404, "BlobNotFound", "Blob not found");
        return AZURE_ERR_NOT_FOUND;
    }

    /* Return size */
    if (size) *size = b->size;

    /* Return lease state as string */
    if (lease_state_out) {
        switch (b->lease_state) {
            case LEASE_AVAILABLE: strcpy(lease_state_out, "available"); break;
            case LEASE_LEASED:    strcpy(lease_state_out, "leased"); break;
            case LEASE_BREAKING:  strcpy(lease_state_out, "breaking"); break;
        }
    }

    /* Return lease status */
    if (lease_status_out) {
        if (b->lease_state == LEASE_LEASED || b->lease_state == LEASE_BREAKING)
            strcpy(lease_status_out, "locked");
        else
            strcpy(lease_status_out, "unlocked");
    }

    return AZURE_OK;
}
```

**Key Points:**
- **Returns size**: Yes, via `*size = b->size`
- **Sets ETag on azure_error_t**: The mock does NOT set etag (Phase 3 feature, not yet implemented)
- **Error detail**: Uses `set_error()` helper to populate HTTP status, error code, and message
- **No network call**: Pure in-memory lookup and copy

---

## 5. Test Organization and Registration

### Test Harness Framework

From **test_harness.h**:

```c
/* Define a test function */
#define TEST(name) static void test_##name(void)

/* Run a test (increments counters, handles errors via setjmp/longjmp) */
#define RUN_TEST(name) \
    do { \
        th_total++; \
        th_current_test = #name; \
        th_current_failed = 0; \
        if (setjmp(th_jump) == 0) { \
            test_##name();  /* Call the test function */ \
        } \
        if (th_current_failed) { \
            th_failed++; \
            fprintf(stdout, "  %s%sFAIL%s  %s\n", ..., #name); \
        } else { \
            th_passed++; \
            fprintf(stdout, "  %sPASS%s  %s\n", ..., #name); \
        } \
    } while (0)

/* Group tests into suites */
#define TEST_SUITE_BEGIN(name) \
    do { \
        th_current_suite = (name); \
        fprintf(stdout, "\n%s%s=== %s ===%s\n", ..., (name), ...); \
    } while (0)
```

### Test Runner (test_main.c)

The main runner includes ALL test files directly to share test harness global counters:

```c
#include "test_harness.h"
#include "test_vfs.c"
#include "test_azure_client.c"
#include "test_coalesce.c"
#include "test_wal.c"
#include "test_uri.c"

int main(int argc, char **argv) {
    fprintf(stdout, "\n╔══════════════════════════════════════╗\n"
                    "║   sqliteObjs Layer 1 Test Suite     ║\n"
                    "╚══════════════════════════════════════╝\n");

    run_vfs_tests();           /* Each test file exports a run_*_tests() function */
    run_azure_client_tests();
    run_coalesce_tests();
    run_wal_tests();
    run_uri_tests();

    return test_harness_summary();  /* Report: X passed, Y failed */
}
```

### Individual Test File Organization

From **test_vfs.c** (lines 2316-2560):

```c
void run_vfs_tests(void) {
    /* ═══ Section 1: Page Blob Operations ═══ */
    TEST_SUITE_BEGIN("Page Blob Operations");
    RUN_TEST(page_blob_create_basic);
    RUN_TEST(page_blob_create_zero_size);
    RUN_TEST(page_blob_create_unaligned_fails);
    RUN_TEST(page_blob_create_already_exists_resizes);
    RUN_TEST(page_blob_create_large);
    RUN_TEST(page_blob_write_basic);
    RUN_TEST(page_blob_write_at_offset);
    RUN_TEST(page_blob_write_unaligned_offset_fails);
    RUN_TEST(page_blob_write_unaligned_length_fails);
    RUN_TEST(page_blob_write_nonexistent_fails);
    RUN_TEST(page_blob_write_grows_blob);
    /* ... more page blob tests ... */
    TEST_SUITE_END();

    /* ═══ Section 2: Block Blob Operations ═══ */
    TEST_SUITE_BEGIN("Block Blob Operations");
    RUN_TEST(block_blob_upload_basic);
    RUN_TEST(block_blob_upload_replaces);
    /* ... more block blob tests ... */
    TEST_SUITE_END();

    /* ═══ Section 3+: More sections ═══ */
    TEST_SUITE_BEGIN("Lease State Machine");
    RUN_TEST(lease_acquire_basic);
    /* ... lease tests ... */
    TEST_SUITE_END();

    /* ... more sections ... */
}
```

**Pattern:**
1. Each test file exports a `void run_*_tests(void)` function
2. Within that function: create TEST_SUITE_BEGIN/END blocks
3. Within each suite: call RUN_TEST(test_name) for each `TEST(test_name)` function
4. Tests are discovered by function naming convention (TEST(foo) → test_foo)

---

## 6. Stub Implementation Files

**Location:** `/Users/qbradley/src/sqlite/test/`

### mock_azure_ops.h (Header — Public API)

- Defines lifecycle: `mock_azure_create()`, `mock_azure_destroy()`, `mock_reset()`
- Failure injection: `mock_set_fail_at()`, `mock_set_fail_operation()`, `mock_clear_failures()`
- Call counting: `mock_get_call_count()`, `mock_get_total_call_count()`
- State inspection: `mock_get_page_blob_data()`, `mock_get_lease_state()`, `mock_is_leased()`, etc.
- Write recording: `mock_get_write_record_count()`, `mock_get_write_record()`

### mock_azure_ops.c (Implementation — 1040 lines)

**Structures (lines 73-131):**
- `blob_type_t` enum: PAGE, BLOCK, APPEND
- `mock_blob_t` struct: in-memory blob with data, size, lease state
- `mock_azure_ctx_t` struct: context holding up to 128 blobs

**Key Operations (all have matching public inspection functions):**
- Page blob: create, write, read, resize (lines ~200-450)
- Block blob: upload, download (lines ~450-480)
- Common: blob_get_properties, blob_delete, blob_exists (lines 480-560)
- Lease: acquire, renew, release, break (lines 560-750)
- Append blob: create, append, delete (lines 750-788)

**Failure Injection (lines 177-230):**
```c
/* Check if operation should fail */
static azure_err_t check_failures(mock_azure_ctx_t *ctx, op_index_t op,
                                  azure_error_t *err);
```

**Vtable (lines 788-810):**
```c
static azure_ops_t mock_ops = {
    .page_blob_create   = mock_page_blob_create,
    .page_blob_write    = mock_page_blob_write,
    .page_blob_read     = mock_page_blob_read,
    .page_blob_resize   = mock_page_blob_resize,
    .block_blob_upload  = mock_block_blob_upload,
    .block_blob_download = mock_block_blob_download,
    .blob_get_properties = mock_blob_get_properties,
    /* ... lease, append blob ops ... */
};
```

---

## 7. Persistence and Reopening Tests

### Persistence Pattern from test_integration.c (lines 690-719)

Test: **integ_multi_db_independent** — demonstrates data persistence:

```c
TEST(integ_multi_db_independent) {
    const char *db1_name = "multi_a.db";
    const char *db2_name = "multi_b.db";

    /* Setup: clean prior state */
    cleanup_blob(db1_name);
    cleanup_blob(db2_name);
    cleanup_blob("multi_a.db-journal");
    cleanup_blob("multi_b.db-journal");

    /* Register VFS with global ops */
    int rc = sqlite_objs_vfs_register_with_ops(g_ops, g_ctx, 0);
    ASSERT_OK(rc);

    /* Open two databases */
    sqlite3 *db1 = NULL, *db2 = NULL;
    rc = sqlite3_open_v2(db1_name, &db1, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                          "sqlite-objs");
    ASSERT_OK(rc);
    ASSERT_NOT_NULL(db1);

    rc = sqlite3_open_v2(db2_name, &db2, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                          "sqlite-objs");
    ASSERT_OK(rc);
    ASSERT_NOT_NULL(db2);

    /* Insert DIFFERENT data in each */
    char *errmsg = NULL;
    rc = sqlite3_exec(db1,
        "CREATE TABLE t (id INTEGER, val TEXT);"
        "INSERT INTO t VALUES (1, 'alpha');",
        NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "  db1 error: %s\n", errmsg ? errmsg : "unknown");
        sqlite3_free(errmsg);
    }
    ASSERT_OK(rc);

    rc = sqlite3_exec(db2,
        "CREATE TABLE t (id INTEGER, val TEXT);"
        "INSERT INTO t VALUES (1, 'beta');",
        NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "  db2 error: %s\n", errmsg ? errmsg : "unknown");
        sqlite3_free(errmsg);
    }
    ASSERT_OK(rc);

    /* ═══ VERIFY DATA IN BOTH ═══ */
    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(db1, "SELECT val FROM t WHERE id=1;", -1, &stmt, NULL);
    ASSERT_OK(rc);
    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_ROW);
    ASSERT_STR_EQ(sqlite3_column_text(stmt, 0), "alpha");  /* ← db1 has "alpha" */
    sqlite3_finalize(stmt);

    rc = sqlite3_prepare_v2(db2, "SELECT val FROM t WHERE id=1;", -1, &stmt, NULL);
    ASSERT_OK(rc);
    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_ROW);
    ASSERT_STR_EQ(sqlite3_column_text(stmt, 0), "beta");   /* ← db2 has "beta" */
    sqlite3_finalize(stmt);

    /* ═══ CLOSE BOTH DATABASES ═══ */
    sqlite3_close(db1);
    sqlite3_close(db2);

    /* ═══ REOPEN BOTH AND VERIFY PERSISTENCE ═══ */
    rc = sqlite3_open_v2(db1_name, &db1, SQLITE_OPEN_READWRITE, "sqlite-objs");
    ASSERT_OK(rc);
    rc = sqlite3_prepare_v2(db1, "SELECT val FROM t WHERE id=1;", -1, &stmt, NULL);
    ASSERT_OK(rc);
    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_ROW);
    ASSERT_STR_EQ(sqlite3_column_text(stmt, 0), "alpha");  /* ← STILL "alpha" after reopen */
    sqlite3_finalize(stmt);
    sqlite3_close(db1);

    rc = sqlite3_open_v2(db2_name, &db2, SQLITE_OPEN_READWRITE, "sqlite-objs");
    ASSERT_OK(rc);
    rc = sqlite3_prepare_v2(db2, "SELECT val FROM t WHERE id=1;", -1, &stmt, NULL);
    ASSERT_OK(rc);
    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_ROW);
    ASSERT_STR_EQ(sqlite3_column_text(stmt, 0), "beta");   /* ← STILL "beta" after reopen */
    sqlite3_finalize(stmt);
    sqlite3_close(db2);

    /* ═══ CLEANUP ═══ */
    cleanup_blob(db1_name);
    cleanup_blob("multi_a.db-journal");
    cleanup_blob(db2_name);
    cleanup_blob("multi_b.db-journal");
}
```

**Key Pattern:**
1. **Create/insert data** → close both
2. **Reopen both separately** → read data (proves persistence)
3. **Verify data unchanged** → proves data survives close/reopen
4. **Cleanup**: delete all blobs created by test

---

## Writing New Integration Tests: Checklist

When writing a new test to fit these patterns:

### 1. Setup
```c
TEST(my_new_test) {
    /* ─ Allocate mock context or reset existing ─ */
    if (g_ctx) mock_reset(g_ctx);
    else       g_ctx = mock_azure_create();
    g_ops = mock_azure_get_ops();

    /* ─ OR for integration tests (Azurite): ─ */
    /* Use setup_azure_client() at start of test_integration.c main() */
```

### 2. Register VFS
```c
    /* ─ Register with mock ops (unit test) ─ */
    int rc = sqlite_objs_vfs_register_with_ops(g_ops, g_ctx, 0);
    ASSERT_OK(rc);

    /* ─ OR register URI-only mode ─ */
    int rc = sqlite_objs_vfs_register_uri(0);
    ASSERT_OK(rc);
```

### 3. Open Database
```c
    sqlite3 *db = NULL;
    rc = sqlite3_open_v2("test.db", &db,
                          SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                          "sqlite-objs");
    ASSERT_OK(rc);
```

### 4. Execute SQL
```c
    char *errmsg = NULL;
    rc = sqlite3_exec(db,
        "CREATE TABLE t (id INTEGER, val TEXT);"
        "INSERT INTO t VALUES (1, 'test');",
        NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "  SQL error: %s\n", errmsg ? errmsg : "unknown");
        sqlite3_free(errmsg);
    }
    ASSERT_OK(rc);
```

### 5. Verify State
```c
    /* ─ Via SQLite query ─ */
    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(db, "SELECT val FROM t WHERE id=1;", -1, &stmt, NULL);
    ASSERT_OK(rc);
    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_ROW);
    ASSERT_STR_EQ(sqlite3_column_text(stmt, 0), "test");
    sqlite3_finalize(stmt);

    /* ─ OR via mock inspection ─ */
    const uint8_t *blob = mock_get_page_blob_data(g_ctx, "test.db");
    ASSERT_NOT_NULL(blob);
    /* ... examine blob contents ... */
```

### 6. Cleanup
```c
    sqlite3_close(db);

    /* For integration tests, cleanup blobs: */
    cleanup_blob("test.db");
    cleanup_blob("test.db-journal");
}
```

### 7. Register Test in Test Runner
In your test file, at the end, add `run_*_tests()`:

```c
void run_my_tests(void) {
    TEST_SUITE_BEGIN("My Feature");
    RUN_TEST(my_new_test);
    RUN_TEST(another_test);
    TEST_SUITE_END();
}
```

Then in `test_main.c`, add:
```c
#include "test_my_new_feature.c"

int main(...) {
    /* ... */
    run_my_tests();
    /* ... */
    return test_harness_summary();
}
```

---

## Key Assertion Macros

From **test_harness.h**:

```c
/* Integer comparisons */
ASSERT_EQ(a, b)              /* a == b */
ASSERT_NE(a, b)              /* a != b */
ASSERT_GT(a, b)              /* a > b */
ASSERT_GE(a, b)              /* a >= b */
ASSERT_LT(a, b)              /* a < b */
ASSERT_LE(a, b)              /* a <= b */

/* Boolean */
ASSERT_TRUE(x)               /* x is truthy */
ASSERT_FALSE(x)              /* x is falsy */

/* Pointers */
ASSERT_NULL(x)               /* x == NULL */
ASSERT_NOT_NULL(x)           /* x != NULL */

/* Strings */
ASSERT_STR_EQ(a, b)          /* strcmp(a, b) == 0 */
ASSERT_STR_NE(a, b)          /* strcmp(a, b) != 0 */

/* Memory */
ASSERT_MEM_EQ(a, b, len)     /* memcmp(a, b, len) == 0 */

/* SQLite-specific */
ASSERT_OK(rc)                /* rc == SQLITE_OK */
ASSERT_ERR(rc, expected)     /* rc == expected */

/* Azure-specific */
ASSERT_AZURE_OK(rc)          /* rc == AZURE_OK */
ASSERT_AZURE_ERR(rc, exp)    /* rc == expected */
```

---

## Running Tests

### Unit Tests (mock-based)
```bash
cd /Users/qbradley/src/sqlite
gcc -o test_runner test/test_main.c test/mock_azure_ops.c \
    sqlite-autoconf-3520000/sqlite3.c \
    -I sqlite-autoconf-3520000 -lpthread -ldl -lm
./test_runner
```

### Integration Tests (requires Azurite)
```bash
cd /Users/qbradley/src/sqlite
./test/run-integration.sh
```

The script:
1. Starts Azurite (Azure emulator)
2. Creates test containers
3. Runs integration tests
4. Shuts down Azurite
5. Reports results

---

## Example: Writing a New Test

**Goal:** Test that a database persists across close/reopen with URI params.

```c
TEST(uri_persist_with_container) {
    uri_setup();
    
    int rc = sqlite_objs_vfs_register_with_ops(uri_ops, uri_ctx, 0);
    ASSERT_OK(rc);

    /* ─ FIRST OPEN: create and insert ─ */
    sqlite3 *db = NULL;
    rc = sqlite3_open_v2("persist.db", &db,
                          SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                          "sqlite-objs");
    ASSERT_OK(rc);

    char *errmsg = NULL;
    rc = sqlite3_exec(db,
        "CREATE TABLE data (val TEXT);"
        "INSERT INTO data VALUES ('persisted');",
        NULL, NULL, &errmsg);
    ASSERT_OK(rc);
    if (errmsg) sqlite3_free(errmsg);

    sqlite3_close(db);

    /* ─ SECOND OPEN: verify data still there ─ */
    db = NULL;
    rc = sqlite3_open_v2("persist.db", &db,
                          SQLITE_OPEN_READWRITE,
                          "sqlite-objs");
    ASSERT_OK(rc);

    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(db, "SELECT val FROM data;", -1, &stmt, NULL);
    ASSERT_OK(rc);
    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_ROW);
    ASSERT_STR_EQ(sqlite3_column_text(stmt, 0), "persisted");
    sqlite3_finalize(stmt);

    sqlite3_close(db);
}

void run_uri_tests(void) {
    TEST_SUITE_BEGIN("URI Persistence");
    RUN_TEST(uri_persist_with_container);
    /* ... other tests ... */
    TEST_SUITE_END();
}
```

