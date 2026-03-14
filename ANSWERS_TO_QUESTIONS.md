# Direct Answers to Your 7 Questions

## Question 1: Show the test directory structure — `ls test/`

**Answer:**
```
/Users/qbradley/src/sqlite/test/
├── test_harness.h              226 lines  — Minimal C test framework
├── test_main.c                  50 lines  — Test runner (includes all test files)
├── mock_azure_ops.h            200 lines  — Mock Azure ops public API
├── mock_azure_ops.c           1040 lines  — In-memory blob storage implementation
├── test_vfs.c                 2500+ lines — Unit tests (VFS, mock-based)
├── test_azure_client.c         ~1000 lines — Azure client wrapper tests
├── test_coalesce.c             ~1000 lines — Write coalescing tests
├── test_wal.c                  ~1200 lines — WAL mode tests
├── test_uri.c                   ~400 lines — URI per-file config tests
├── test_integration.c           ~950 lines — Integration tests (real Azurite)
├── run-integration.sh             451 lines — Integration test wrapper
├── README-INTEGRATION.md        ~100 lines — Integration test guide
├── SECURITY-TESTING.md          ~700 lines — Security testing guide
└── TEST-MATRIX.md              ~300 lines — Test coverage matrix
```

---

## Question 2: Find an existing test that opens a database with URI parameters; show a complete test function

**Answer:** File: `/Users/qbradley/src/sqlite/test/test_uri.c`, lines 86-123

```c
TEST(uri_fallback_to_global) {
    /* Setup: create fresh mock context */
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

    /* Verify data is readable */
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

**Key Pattern:**
- Mock setup: `ctx = mock_azure_create(); ops = mock_azure_get_ops();`
- VFS registration: `sqlite_objs_vfs_register_with_ops(ops, ctx, 0);`
- Database open: `sqlite3_open_v2(name, &db, flags, "sqlite-objs");`
- SQL execution: `sqlite3_exec(db, "CREATE TABLE...", ...);`
- Data verification: `sqlite3_prepare_v2() + sqlite3_step() + ASSERT_STR_EQ()`

---

## Question 3: How does the test stub/mock work? Show VFS registration and how stub simulates blob storage

**Answer:**

### Registration Pattern (test_uri.c, lines 27-31):
```c
static void uri_setup(void) {
    if (uri_ctx) mock_reset(uri_ctx);       /* Clear previous state */
    else         uri_ctx = mock_azure_create();  /* Allocate context */
    uri_ops = mock_azure_get_ops();         /* Get ops vtable */
}

/* Then in test: */
int rc = sqlite_objs_vfs_register_with_ops(uri_ops, uri_ctx, 0);
ASSERT_OK(rc);
```

### Mock Blob Storage (mock_azure_ops.c, lines 82-95):
```c
typedef struct {
    char            name[BLOB_NAME_LEN];    /* "db.db", "db.db-journal" */
    blob_type_t     type;                   /* PAGE, BLOCK, or APPEND */
    uint8_t        *data;                   /* Heap-allocated buffer */
    int64_t         size;                   /* Logical size in bytes */
    int64_t         capacity;               /* Allocated capacity */

    /* Lease state machine for locking */
    mock_lease_state_t lease_state;         /* AVAILABLE, LEASED, BREAKING */
    char               lease_id[LEASE_ID_LEN];  /* 37 bytes (UUID-like) */
    int                lease_duration;
    time_t             lease_acquired_at;
    int                break_period;
} mock_blob_t;
```

### Storage Location (mock_azure_ops.c, lines 109-131):
```c
struct mock_azure_ctx {
    mock_blob_t  blobs[MAX_BLOBS];          /* Array of up to 128 blobs */
    int          blob_count;                /* Number of active blobs */

    /* Call counting for debugging */
    int          call_counts[OP_COUNT];
    int          total_calls;

    /* Failure injection for error simulation */
    fail_rule_t  fail_rules[MAX_FAIL_RULES];
    int          fail_rule_count;

    /* For testing write coalescing */
    mock_write_record_t write_records[MOCK_MAX_WRITE_RECORDS];
    int                 write_record_count;

    /* For WAL mode tests */
    mock_append_record_t append_records[MOCK_MAX_APPEND_RECORDS];
    int                  append_record_count;
};
```

### How Data is Stored (In-Memory)
- **No network calls** — Everything in heap memory
- **Array of blobs** — Up to 128 named blobs (e.g., "test.db", "test.db-journal")
- **Per-blob data** — Each blob has `uint8_t *data` buffer (malloc'd)
- **Auto-expand** — Write beyond current size auto-reallocates
- **Zero-fill** — Reads past EOF return zero bytes

---

## Question 4: Show the stub's `blob_get_properties` implementation — does it return size? Does the stub set etag?

**Answer:** File: `/Users/qbradley/src/sqlite/test/mock_azure_ops.c`, lines 480-513

```c
static azure_err_t mock_blob_get_properties(void *vctx, const char *name,
                                           int64_t *size,
                                           char *lease_state_out,
                                           char *lease_status_out,
                                           azure_error_t *err) {
    mock_azure_ctx_t *ctx = (mock_azure_ctx_t *)vctx;
    azure_err_t rc = pre_call(ctx, OP_BLOB_GET_PROPERTIES, err);
    if (rc != AZURE_OK) return rc;

    /* Find the blob by name */
    mock_blob_t *b = find_blob(ctx, name);
    if (!b) {
        set_error(err, 404, "BlobNotFound", "Blob not found");
        return AZURE_ERR_NOT_FOUND;
    }

    /* ═══ RETURN SIZE ═══ */
    if (size) *size = b->size;

    /* ═══ RETURN LEASE STATE ═══ */
    if (lease_state_out) {
        switch (b->lease_state) {
            case LEASE_AVAILABLE: strcpy(lease_state_out, "available"); break;
            case LEASE_LEASED:    strcpy(lease_state_out, "leased"); break;
            case LEASE_BREAKING:  strcpy(lease_state_out, "breaking"); break;
        }
    }

    /* ═══ RETURN LEASE STATUS ═══ */
    if (lease_status_out) {
        if (b->lease_state == LEASE_LEASED || b->lease_state == LEASE_BREAKING)
            strcpy(lease_status_out, "locked");
        else
            strcpy(lease_status_out, "unlocked");
    }

    return AZURE_OK;
}
```

### Summary:
✓ **Returns size:** Yes — via `*size = b->size`
✓ **Returns lease state:** Yes — as string ("available", "leased", "breaking")
✓ **Returns lease status:** Yes — as string ("locked", "unlocked")
✗ **Does NOT set ETag:** No — ETag is Phase 3 feature (not yet implemented)

---

## Question 5: Show how tests are organized — test runner, registration, discovery

**Answer:**

### Test Harness Macros (test_harness.h, lines 58, 75):
```c
/* Define a test */
#define TEST(name) static void test_##name(void)

/* Run a test (with error handling via setjmp/longjmp) */
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
            fprintf(stdout, "  FAIL  %s\n", #name); \
        } else { \
            th_passed++; \
            fprintf(stdout, "  PASS  %s\n", #name); \
        } \
    } while (0)

/* Suite grouping */
#define TEST_SUITE_BEGIN(name) \
    do { \
        th_current_suite = (name); \
        fprintf(stdout, "\n%s=== %s ===%s\n", TH_BOLD, (name), TH_RESET); \
    } while (0)
```

### Test Runner Pattern (test_main.c, lines 30-48):
```c
#include "test_harness.h"
#include "test_vfs.c"          /* ← Include test file directly */
#include "test_azure_client.c"
#include "test_coalesce.c"
#include "test_wal.c"
#include "test_uri.c"

int main(int argc, char **argv) {
    fprintf(stdout, "╔══════════════════════════════════════╗\n"
                    "║   sqliteObjs Layer 1 Test Suite      ║\n"
                    "╚══════════════════════════════════════╝\n");

    run_vfs_tests();           /* Call run_*_tests() from each file */
    run_azure_client_tests();
    run_coalesce_tests();
    run_wal_tests();
    run_uri_tests();

    return test_harness_summary();  /* Report pass/fail counts */
}
```

### Test Registration in Test File (test_vfs.c, lines 2316-2344):
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
    /* ... more tests ... */
    TEST_SUITE_END();

    /* ═══ Section 2: Block Blob Operations ═══ */
    TEST_SUITE_BEGIN("Block Blob Operations");
    RUN_TEST(block_blob_upload_basic);
    RUN_TEST(block_blob_upload_replaces);
    /* ... more tests ... */
    TEST_SUITE_END();

    /* ... more sections ... */
}
```

### Discovery Mechanism
1. **Function naming:** `TEST(my_test)` → expands to `static void test_my_test(void)`
2. **Manual registration:** Each test file exports `void run_*_tests(void)`
3. **Test runner calls:** `test_main.c` includes all test files and calls all `run_*_tests()`
4. **No reflection:** Discovery is manual (naming convention + explicit RUN_TEST calls)

---

## Question 6: Show the stub implementation files

**Answer:** Two files in `/Users/qbradley/src/sqlite/test/`:

### File 1: mock_azure_ops.h (200 lines — Public API)
Location: `/Users/qbradley/src/sqlite/test/mock_azure_ops.h`

**Key Functions:**
```c
/* Lifecycle */
mock_azure_ctx_t *mock_azure_create(void);
void mock_azure_destroy(mock_azure_ctx_t *ctx);
void mock_reset(mock_azure_ctx_t *ctx);
azure_ops_t *mock_azure_get_ops(void);

/* Failure injection */
void mock_set_fail_at(mock_azure_ctx_t *ctx, int call_number,
                      azure_err_t error_code);
void mock_set_fail_operation(mock_azure_ctx_t *ctx, const char *op_name,
                             azure_err_t error_code);
void mock_set_fail_operation_at(mock_azure_ctx_t *ctx, const char *op_name,
                                int op_call_number, azure_err_t error_code);
void mock_clear_failures(mock_azure_ctx_t *ctx);

/* Call counting */
int mock_get_call_count(mock_azure_ctx_t *ctx, const char *op_name);
int mock_get_total_call_count(mock_azure_ctx_t *ctx);

/* State inspection */
const uint8_t *mock_get_page_blob_data(mock_azure_ctx_t *ctx, const char *name);
int64_t mock_get_page_blob_size(mock_azure_ctx_t *ctx, const char *name);
int mock_is_leased(mock_azure_ctx_t *ctx, const char *name);
mock_lease_state_t mock_get_lease_state(mock_azure_ctx_t *ctx, const char *name);
const char *mock_get_lease_id(mock_azure_ctx_t *ctx, const char *name);
int mock_blob_exists(mock_azure_ctx_t *ctx, const char *name);

/* Write recording (for coalescing tests) */
int mock_get_write_record_count(mock_azure_ctx_t *ctx);
mock_write_record_t mock_get_write_record(mock_azure_ctx_t *ctx, int idx);

/* Append recording (for WAL tests) */
int mock_get_append_record_count(mock_azure_ctx_t *ctx);
const unsigned char *mock_get_append_data(mock_azure_ctx_t *ctx, const char *name);
int64_t mock_get_append_size(mock_azure_ctx_t *ctx, const char *name);
```

### File 2: mock_azure_ops.c (1040 lines — Implementation)
Location: `/Users/qbradley/src/sqlite/test/mock_azure_ops.c`

**Sections:**
- Lines 16-71: Operation index mapping (OP_PAGE_BLOB_CREATE, etc.)
- Lines 73-131: Data structures (mock_blob_t, mock_azure_ctx_t)
- Lines 133-160: Internal helpers (find_blob, create_blob, free_blob_data)
- Lines 177-230: Failure injection check
- Lines 200-450: Page blob operations (create, write, read, resize)
- Lines 450-480: Block blob operations (upload, download)
- Lines 480-560: Common operations (blob_get_properties, delete, exists)
- Lines 560-750: Lease operations (acquire, renew, release, break)
- Lines 750-788: Append blob operations (create, append, delete)
- Lines 788-810: Static vtable definition
- Lines 816-845: Public lifecycle API
- Lines 850+: Public query/inspection API

**Total:** 1040 lines of pure in-memory mock implementation

---

## Question 7: Show an existing test that verifies file persistence or reopening behavior

**Answer:** File: `/Users/qbradley/src/sqlite/test/test_integration.c`, lines 690-719

```c
TEST(integ_multi_db_independent) {
    const char *db1_name = "multi_a.db";
    const char *db2_name = "multi_b.db";

    /* Cleanup previous state */
    cleanup_blob(db1_name);
    cleanup_blob(db2_name);
    cleanup_blob("multi_a.db-journal");
    cleanup_blob("multi_b.db-journal");

    /* Register VFS */
    int rc = sqlite_objs_vfs_register_with_ops(g_ops, g_ctx, 0);
    ASSERT_OK(rc);

    /* ═══ PHASE 1: OPEN, CREATE, INSERT ═══ */
    sqlite3 *db1 = NULL, *db2 = NULL;
    rc = sqlite3_open_v2(db1_name, &db1, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                          "sqlite-objs");
    ASSERT_OK(rc);
    ASSERT_NOT_NULL(db1);

    rc = sqlite3_open_v2(db2_name, &db2, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                          "sqlite-objs");
    ASSERT_OK(rc);
    ASSERT_NOT_NULL(db2);

    /* Insert DIFFERENT data in each database */
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

    /* ═══ PHASE 2: VERIFY DATA IN BOTH (while open) ═══ */
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

    /* ═══ PHASE 3: CLOSE BOTH ═══ */
    sqlite3_close(db1);
    sqlite3_close(db2);

    /* ═══ PHASE 4: REOPEN AND VERIFY PERSISTENCE ═══ */
    rc = sqlite3_open_v2(db1_name, &db1, SQLITE_OPEN_READWRITE, "sqlite-objs");
    ASSERT_OK(rc);
    rc = sqlite3_prepare_v2(db1, "SELECT val FROM t WHERE id=1;", -1, &stmt, NULL);
    ASSERT_OK(rc);
    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_ROW);
    ASSERT_STR_EQ(sqlite3_column_text(stmt, 0), "alpha");  /* ← PERSISTED! */
    sqlite3_finalize(stmt);
    sqlite3_close(db1);

    rc = sqlite3_open_v2(db2_name, &db2, SQLITE_OPEN_READWRITE, "sqlite-objs");
    ASSERT_OK(rc);
    rc = sqlite3_prepare_v2(db2, "SELECT val FROM t WHERE id=1;", -1, &stmt, NULL);
    ASSERT_OK(rc);
    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_ROW);
    ASSERT_STR_EQ(sqlite3_column_text(stmt, 0), "beta");   /* ← PERSISTED! */
    sqlite3_finalize(stmt);
    sqlite3_close(db2);

    /* ═══ CLEANUP ═══ */
    cleanup_blob(db1_name);
    cleanup_blob("multi_a.db-journal");
    cleanup_blob(db2_name);
    cleanup_blob("multi_b.db-journal");
}
```

**Pattern:**
1. **Open two databases** with different names
2. **Insert different data** in each
3. **Verify data** (while still open)
4. **Close both databases**
5. **Reopen each database** WITHOUT the CREATE flag
6. **Re-query data** — must match what was inserted
7. **Verify persistence** — data survived close/reopen cycle

---

## Summary

| Question | Answer |
|----------|--------|
| **1. Directory structure** | 14 files: harness, runner, mock ops (1040 lines), 5 test files, wrapper |
| **2. URI test example** | test_uri.c:86-123 (uri_fallback_to_global test) |
| **3. Mock storage** | mock_blob_t array in mock_azure_ctx_t; data in malloc'd uint8_t buffer |
| **4. blob_get_properties** | Returns: size ✓, lease_state ✓, lease_status ✓; Does NOT set ETag (Phase 3) |
| **5. Test organization** | TEST() macro + run_*_tests() + test_main.c includes all files |
| **6. Stub files** | mock_azure_ops.h (200 lines) + mock_azure_ops.c (1040 lines) |
| **7. Persistence test** | test_integration.c:690-719 (integ_multi_db_independent) |

---

**You now have enough detail to write new tests that fit the existing patterns.**

