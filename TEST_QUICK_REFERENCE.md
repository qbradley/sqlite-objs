# Test Infrastructure Quick Reference

## 1. Minimal Test Example

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

/* Test 1: Create table and insert data */
TEST(create_table) {
    setup();
    
    int rc = sqlite_objs_vfs_register_with_ops(g_ops, g_ctx, 0);
    ASSERT_OK(rc);

    sqlite3 *db = NULL;
    rc = sqlite3_open_v2("test.db", &db,
                          SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                          "sqlite-objs");
    ASSERT_OK(rc);
    ASSERT_NOT_NULL(db);

    char *errmsg = NULL;
    rc = sqlite3_exec(db,
        "CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT);",
        NULL, NULL, &errmsg);
    ASSERT_OK(rc);
    if (errmsg) sqlite3_free(errmsg);

    sqlite3_close(db);
}

/* Test 2: Insert and query */
TEST(insert_and_query) {
    setup();
    
    int rc = sqlite_objs_vfs_register_with_ops(g_ops, g_ctx, 0);
    ASSERT_OK(rc);

    sqlite3 *db = NULL;
    rc = sqlite3_open_v2("test2.db", &db,
                          SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                          "sqlite-objs");
    ASSERT_OK(rc);

    char *errmsg = NULL;
    rc = sqlite3_exec(db,
        "CREATE TABLE users (id INTEGER, name TEXT);"
        "INSERT INTO users VALUES (1, 'Alice');",
        NULL, NULL, &errmsg);
    ASSERT_OK(rc);
    if (errmsg) sqlite3_free(errmsg);

    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(db, "SELECT name FROM users WHERE id=1;", -1, &stmt, NULL);
    ASSERT_OK(rc);
    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_ROW);
    ASSERT_STR_EQ(sqlite3_column_text(stmt, 0), "Alice");
    sqlite3_finalize(stmt);

    sqlite3_close(db);
}

/* Test 3: Persistence (close and reopen) */
TEST(persistence) {
    setup();
    
    int rc = sqlite_objs_vfs_register_with_ops(g_ops, g_ctx, 0);
    ASSERT_OK(rc);

    /* First open: create and insert */
    sqlite3 *db = NULL;
    rc = sqlite3_open_v2("persist.db", &db,
                          SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                          "sqlite-objs");
    ASSERT_OK(rc);

    char *errmsg = NULL;
    rc = sqlite3_exec(db,
        "CREATE TABLE data (val TEXT);"
        "INSERT INTO data VALUES ('permanent');",
        NULL, NULL, &errmsg);
    ASSERT_OK(rc);
    if (errmsg) sqlite3_free(errmsg);

    sqlite3_close(db);

    /* Second open: verify data persisted */
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
    ASSERT_STR_EQ(sqlite3_column_text(stmt, 0), "permanent");
    sqlite3_finalize(stmt);

    sqlite3_close(db);
}

/* Test runner function */
void run_my_feature_tests(void) {
    TEST_SUITE_BEGIN("My Feature Tests");
    RUN_TEST(create_table);
    RUN_TEST(insert_and_query);
    RUN_TEST(persistence);
    TEST_SUITE_END();
}
```

## 2. Add Test to Main Runner

In `test_main.c`, add:

```c
#include "test_harness.h"
/* ... other includes ... */
#include "test_my_feature.c"  /* ← Add this */

int main(void) {
    fprintf(stdout, "╔══════════════════════════════════════╗\n"
                    "║   sqliteObjs Test Suite              ║\n"
                    "╚══════════════════════════════════════╝\n");

    run_vfs_tests();
    run_my_feature_tests();  /* ← Add this */
    
    return test_harness_summary();
}
```

## 3. Failure Injection Example

```c
TEST(handle_network_failure) {
    setup();
    
    /* Make the 2nd page_blob_write call fail with network error */
    mock_set_fail_operation_at(g_ctx, "page_blob_write", 2, AZURE_ERR_NETWORK);

    int rc = sqlite_objs_vfs_register_with_ops(g_ops, g_ctx, 0);
    ASSERT_OK(rc);

    sqlite3 *db = NULL;
    rc = sqlite3_open_v2("test.db", &db,
                          SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                          "sqlite-objs");
    ASSERT_OK(rc);

    /* This will fail because second write hits injected failure */
    char *errmsg = NULL;
    rc = sqlite3_exec(db,
        "CREATE TABLE t (data BLOB);",
        NULL, NULL, &errmsg);
    /* rc should be != SQLITE_OK due to network error */
    if (errmsg) sqlite3_free(errmsg);

    sqlite3_close(db);
}
```

## 4. Test with Mock Inspection

```c
TEST(verify_blob_storage) {
    setup();
    
    int rc = sqlite_objs_vfs_register_with_ops(g_ops, g_ctx, 0);
    ASSERT_OK(rc);

    sqlite3 *db = NULL;
    rc = sqlite3_open_v2("test.db", &db,
                          SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                          "sqlite-objs");
    ASSERT_OK(rc);

    /* Do some SQL operations */
    char *errmsg = NULL;
    rc = sqlite3_exec(db,
        "CREATE TABLE t (val INTEGER);"
        "INSERT INTO t VALUES (42);",
        NULL, NULL, &errmsg);
    ASSERT_OK(rc);
    if (errmsg) sqlite3_free(errmsg);

    sqlite3_close(db);

    /* Inspect the blob directly via mock API */
    const uint8_t *blob = mock_get_page_blob_data(g_ctx, "test.db");
    ASSERT_NOT_NULL(blob);

    int64_t size = mock_get_page_blob_size(g_ctx, "test.db");
    ASSERT_GT(size, 0);

    /* Verify call counts */
    int write_calls = mock_get_call_count(g_ctx, "page_blob_write");
    ASSERT_GT(write_calls, 0);
}
```

## 5. URI-Based Test

```c
TEST(uri_open_with_params) {
    if (g_ctx) mock_reset(g_ctx);
    else       g_ctx = mock_azure_create();
    g_ops = mock_azure_get_ops();

    /* Register VFS in URI-only mode (no global ops fallback) */
    int rc = sqlite_objs_vfs_register_uri(0);
    ASSERT_OK(rc);

    /* Open WITHOUT URI params → should fail */
    sqlite3 *db = NULL;
    rc = sqlite3_open_v2("test_no_uri.db", &db,
                          SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                          "sqlite-objs");
    ASSERT_EQ(rc, SQLITE_CANTOPEN);
    if (db) sqlite3_close(db);

    /* Register with global ops for fallback */
    rc = sqlite_objs_vfs_register_with_ops(g_ops, g_ctx, 0);
    ASSERT_OK(rc);

    /* Open WITHOUT URI params → uses global ops → succeeds */
    db = NULL;
    rc = sqlite3_open_v2("test_fallback.db", &db,
                          SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                          "sqlite-objs");
    ASSERT_OK(rc);

    char *errmsg = NULL;
    rc = sqlite3_exec(db,
        "CREATE TABLE t (id INTEGER);"
        "INSERT INTO t VALUES (1);",
        NULL, NULL, &errmsg);
    ASSERT_OK(rc);
    if (errmsg) sqlite3_free(errmsg);

    sqlite3_close(db);
}
```

## 6. Common Assertion Patterns

```c
/* Integer comparisons */
ASSERT_EQ(result, SQLITE_OK);
ASSERT_NE(status, -1);
ASSERT_GT(size, 0);
ASSERT_LE(count, 100);

/* String checks */
ASSERT_STR_EQ(sqlite3_column_text(stmt, 0), "expected");
ASSERT_STR_NE(name, "");

/* Pointer checks */
ASSERT_NOT_NULL(db);
ASSERT_NULL(error_msg);

/* Memory/blob checks */
const uint8_t *data = mock_get_page_blob_data(g_ctx, "test.db");
ASSERT_NOT_NULL(data);
ASSERT_MEM_EQ(data, expected_bytes, 512);

/* SQL-specific */
rc = sqlite3_step(stmt);
ASSERT_EQ(rc, SQLITE_ROW);  /* Got a row */
/* or */
ASSERT_EQ(rc, SQLITE_DONE);  /* All rows processed */

/* Azure-specific */
ASSERT_AZURE_OK(azure_rc);
ASSERT_AZURE_ERR(azure_rc, AZURE_ERR_NOT_FOUND);
```

## 7. Cleanup Pattern

```c
TEST(cleanup_example) {
    setup();
    
    const char *db1 = "test_a.db";
    const char *db2 = "test_b.db";

    /* Register and do work ... */

    int rc = sqlite_objs_vfs_register_with_ops(g_ops, g_ctx, 0);
    ASSERT_OK(rc);

    /* ... create databases and run operations ... */

    /* Mock reset clears all blobs for next test */
    /* (cleanup happens automatically in setup() via mock_reset) */
}
```

## 8. Build Command

```bash
cd /Users/qbradley/src/sqlite

gcc -o test_runner \
    test/test_main.c \
    test/mock_azure_ops.c \
    sqlite-autoconf-3520000/sqlite3.c \
    -I sqlite-autoconf-3520000 \
    -lpthread -ldl -lm

./test_runner
```

## 9. Test Output Example

```
╔══════════════════════════════════════╗
║   sqliteObjs Layer 1 Test Suite      ║
╚══════════════════════════════════════╝

=== My Feature Tests ===
  PASS  create_table
  PASS  insert_and_query
  PASS  persistence

────────────────────────────────────
All 3 tests passed
  Passed: 3  Failed: 0  Total: 3
```

## Key Functions Quick Reference

### Mock Context Lifecycle
```c
mock_azure_ctx_t *ctx = mock_azure_create();      /* Allocate */
mock_reset(ctx);                                   /* Clear all blobs */
mock_azure_destroy(ctx);                           /* Free */
```

### Get VFS Operations
```c
azure_ops_t *ops = mock_azure_get_ops();          /* Get vtable */
sqlite_objs_vfs_register_with_ops(ops, ctx, 0);   /* Register */
```

### Failure Injection
```c
mock_set_fail_at(ctx, 3, AZURE_ERR_NETWORK);
mock_set_fail_operation(ctx, "page_blob_write", AZURE_ERR_NETWORK);
mock_set_fail_operation_at(ctx, "lease_acquire", 2, AZURE_ERR_NETWORK);
mock_clear_failures(ctx);
```

### Call Counting
```c
int count = mock_get_call_count(ctx, "page_blob_write");
int total = mock_get_total_call_count(ctx);
```

### State Inspection
```c
const uint8_t *data = mock_get_page_blob_data(ctx, "name");
int64_t size = mock_get_page_blob_size(ctx, "name");
int is_leased = mock_is_leased(ctx, "name");
```

### Write Recording
```c
int count = mock_get_write_record_count(ctx);
mock_write_record_t rec = mock_get_write_record(ctx, 0);  /* offset, len */
```

