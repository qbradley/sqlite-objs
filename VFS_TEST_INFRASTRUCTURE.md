# SQLite-Objs VFS Test Infrastructure Analysis

## Overview
The test infrastructure for sqlite-objs VFS consists of:
1. **test_harness.h** — Minimal C test framework with macro-based assertions
2. **mock_azure_ops.h/c** — Complete in-memory mock of Azure Blob Storage API
3. **test_vfs.c** — ~2,561 lines of comprehensive test cases for VFS and mock operations
4. **sqlite_objs_vfs.c** — Production VFS implementation (~2,357 lines)

---

## 1. TEST FRAMEWORK (test_harness.h)

### Test Declaration & Execution
- **Lines 58**: `#define TEST(name)` — Declares a test function as `static void test_##name(void)`
- **Lines 75-90**: `#define RUN_TEST(name)` — Executes a single test with:
  - Increments `th_total` counter
  - Sets `th_current_test` name
  - Uses `setjmp/longjmp` to catch assertion failures
  - Prints PASS/FAIL with colored output
  - Tracks pass/fail counts

### Suite Organization
- **Lines 62-66**: `#define TEST_SUITE_BEGIN(name)` — Prints suite header with bold yellow text
- **Lines 68-71**: `#define TEST_SUITE_END()` — Clears current suite
- Example from test_vfs.c:
  ```c
  TEST_SUITE_BEGIN("Mock Page Blob Operations");
  RUN_TEST(page_blob_create_basic);
  RUN_TEST(page_blob_write_basic);
  TEST_SUITE_END();
  ```

### Assertion Macros

#### Comparison Assertions
- **ASSERT_EQ(a, b)** — Line 128: Assert equality (casts to long long)
- **ASSERT_NE(a, b)** — Line 134: Assert not equal
- **ASSERT_GT(a, b)** — Line 140: Assert greater than
- **ASSERT_GE(a, b)** — Line 146: Assert greater than or equal
- **ASSERT_LT(a, b)** — Line 152: Assert less than
- **ASSERT_LE(a, b)** — Line 158: Assert less than or equal

#### Boolean/Pointer Assertions
- **ASSERT_TRUE(x)** — Line 122: Assert x is truthy
- **ASSERT_FALSE(x)** — Line 125: Assert x is falsy
- **ASSERT_NULL(x)** — Line 164: Assert x is NULL
- **ASSERT_NOT_NULL(x)** — Line 167: Assert x is not NULL

#### String/Memory Assertions
- **ASSERT_STR_EQ(a, b)** — Line 170: String equality with NULL-safe comparison
- **ASSERT_STR_NE(a, b)** — Line 178: String inequality
- **ASSERT_MEM_EQ(a, b, len)** — Line 185: Memory comparison (memcmp)

#### SQLite/Azure Error Assertions
- **ASSERT_OK(rc)** — Line 195: Assert sqlite3 return code is SQLITE_OK (0)
- **ASSERT_ERR(rc, expected)** — Line 202: Assert specific SQLite error code
- **ASSERT_AZURE_OK(rc)** — Line 211: Assert azure_err_t is AZURE_OK (0)
- **ASSERT_AZURE_ERR(rc, expected)** — Line 218: Assert specific Azure error code

### Global Test State
- **th_total**: Total number of tests run
- **th_passed**: Number of passing tests
- **th_failed**: Number of failing tests
- **th_current_failed**: Flag for current test failure
- **th_jump**: jmp_buf for assertion longjmp

### Error Handling
- **Lines 110-118**: `#define TH_FAIL(...)` — Assertion failure handler:
  - Prints formatted error message with file/line
  - Sets `th_current_failed = 1`
  - Calls `longjmp(th_jump, 1)` to exit test early
  - Return code: 0 if all passed, 1 if any failed

---

## 2. MOCK AZURE OPERATIONS (mock_azure_ops.h & mock_azure_ops.c)

### mock_azure_ops.h Structure

#### Type Definitions
- **Lines 29-33**: `mock_lease_state_t` enum:
  - `LEASE_AVAILABLE = 0`
  - `LEASE_LEASED = 1`
  - `LEASE_BREAKING = 2`

- **Lines 40**: Forward declaration: `typedef struct mock_azure_ctx mock_azure_ctx_t;`

#### Lifecycle API (Lines 47-56)
```c
mock_azure_ctx_t *mock_azure_create(void);     /* Create & init context */
void mock_azure_destroy(mock_azure_ctx_t *ctx); /* Free all resources */
azure_ops_t *mock_azure_get_ops(void);         /* Get vtable pointer */
void mock_reset(mock_azure_ctx_t *ctx);        /* Clear blobs/leases/counters */
```

#### Failure Injection (Lines 59-91)
```c
void mock_set_fail_at(mock_azure_ctx_t *ctx, int call_number,
                      azure_err_t error_code);
  /* Make the Nth overall call fail (1-based, one-shot) */

void mock_set_fail_operation(mock_azure_ctx_t *ctx, const char *op_name,
                             azure_err_t error_code);
  /* Make ALL calls to a specific operation fail */

void mock_set_fail_operation_at(mock_azure_ctx_t *ctx, const char *op_name,
                                int op_call_number, azure_err_t error_code);
  /* Make Nth call to specific operation fail (one-shot) */

void mock_clear_failures(mock_azure_ctx_t *ctx);
  /* Disable all failure injection rules */
```

#### Call Counting (Lines 95-104)
```c
int mock_get_call_count(mock_azure_ctx_t *ctx, const char *op_name);
  /* Get count for specific operation (e.g., "page_blob_write") */

int mock_get_total_call_count(mock_azure_ctx_t *ctx);
  /* Get total calls across all operations */

void mock_reset_call_counts(mock_azure_ctx_t *ctx);
  /* Reset all counters to zero */
```

#### State Inspection — Page Blobs (Lines 109-120)
```c
const uint8_t *mock_get_page_blob_data(mock_azure_ctx_t *ctx, const char *name);
  /* Get raw blob data pointer (DO NOT free) */

int64_t mock_get_page_blob_size(mock_azure_ctx_t *ctx, const char *name);
  /* Get logical blob size, returns -1 if not found */

int mock_is_leased(mock_azure_ctx_t *ctx, const char *name);
  /* 1 if leased, 0 if not */

mock_lease_state_t mock_get_lease_state(mock_azure_ctx_t *ctx, const char *name);
  /* Returns LEASE_AVAILABLE, LEASE_LEASED, or LEASE_BREAKING */

const char *mock_get_lease_id(mock_azure_ctx_t *ctx, const char *name);
  /* Returns lease ID string (NULL if not leased) */
```

#### State Inspection — Block Blobs (Lines 130-140)
```c
int64_t mock_get_block_blob_size(mock_azure_ctx_t *ctx, const char *name);
  /* Returns -1 if not found */

const uint8_t *mock_get_block_blob_data(mock_azure_ctx_t *ctx, const char *name);
  /* Returns NULL if not found */

int mock_blob_exists(mock_azure_ctx_t *ctx, const char *name);
  /* Check any blob type (1=exists, 0=not) */
```

#### Write Recording (Lines 146-160)
Used to track page_blob_write calls for coalescing tests:
```c
typedef struct {
    int64_t offset;
    size_t  len;
} mock_write_record_t;  /* Line 148-151 */

int mock_get_write_record_count(mock_azure_ctx_t *ctx);
const uint8_t *mock_get_write_record(mock_azure_ctx_t *ctx, int idx);
void mock_clear_write_records(mock_azure_ctx_t *ctx);
```

#### Append Recording (Lines 168-180)
Used to track append_blob_append calls for WAL chunking tests:
```c
typedef struct {
    size_t len;
} mock_append_record_t;  /* Line 169-171 */

int mock_get_append_record_count(mock_azure_ctx_t *ctx);
mock_append_record_t mock_get_append_record(mock_azure_ctx_t *ctx, int idx);
void mock_clear_append_records(mock_azure_ctx_t *ctx);
```

#### Append Blob State (Lines 186-194)
For WAL mode tests:
```c
const unsigned char *mock_get_append_data(mock_azure_ctx_t *ctx, const char *name);
  /* Returns NULL if not found */

int64_t mock_get_append_size(mock_azure_ctx_t *ctx, const char *name);
  /* Returns -1 if not found */

void mock_reset_append_data(mock_azure_ctx_t *ctx, const char *name);
  /* Reset to empty but keep blob alive */
```

### mock_azure_ops.c Implementation

#### Constants (Lines 16-24)
- `PAGE_BLOB_ALIGNMENT = 512` — Enforced for page blob offsets and sizes
- `MAX_BLOBS = 128` — Maximum blobs in-memory
- `MAX_FAIL_RULES = 32` — Maximum failure injection rules
- `LEASE_ID_LEN = 37` — "UUID-like" lease ID format

#### Operation Index Enum (Lines 27-45)
```c
typedef enum {
    OP_PAGE_BLOB_CREATE = 0,
    OP_PAGE_BLOB_WRITE,
    OP_PAGE_BLOB_READ,
    OP_PAGE_BLOB_RESIZE,
    OP_BLOCK_BLOB_UPLOAD,
    OP_BLOCK_BLOB_DOWNLOAD,
    OP_BLOB_GET_PROPERTIES,
    OP_BLOB_DELETE,
    OP_BLOB_EXISTS,
    OP_LEASE_ACQUIRE,
    OP_LEASE_RENEW,
    OP_LEASE_RELEASE,
    OP_LEASE_BREAK,
    OP_APPEND_BLOB_CREATE,
    OP_APPEND_BLOB_APPEND,
    OP_APPEND_BLOB_DELETE,
    OP_COUNT
} op_index_t;
```

#### Blob Storage (Lines 75-95)
```c
typedef enum {
    BLOB_TYPE_NONE   = 0,
    BLOB_TYPE_PAGE   = 1,
    BLOB_TYPE_BLOCK  = 2,
    BLOB_TYPE_APPEND = 3,
} blob_type_t;

typedef struct {
    char            name[BLOB_NAME_LEN];
    blob_type_t     type;
    uint8_t        *data;            /* Raw blob data */
    int64_t         size;            /* Logical size */
    int64_t         capacity;        /* Allocated capacity */
    
    /* Lease state machine */
    mock_lease_state_t lease_state;
    char               lease_id[LEASE_ID_LEN];
    int                lease_duration;
    time_t             lease_acquired_at;
    int                break_period;
} mock_blob_t;
```

#### Mock Context (Lines 109-131)
```c
struct mock_azure_ctx {
    mock_blob_t  blobs[MAX_BLOBS];
    int          blob_count;
    
    /* Call counting per operation */
    int          call_counts[OP_COUNT];
    int          total_calls;
    
    /* Failure injection rules */
    fail_rule_t  fail_rules[MAX_FAIL_RULES];
    int          fail_rule_count;
    
    /* Lease ID generation counter */
    int          next_lease_num;
    
    /* Write recording for coalescing tests */
    mock_write_record_t write_records[MOCK_MAX_WRITE_RECORDS];
    int                 write_record_count;
    
    /* Append call recording for WAL chunking tests */
    mock_append_record_t append_records[MOCK_MAX_APPEND_RECORDS];
    int                  append_record_count;
};
```

#### Key Implementation Details

**Page Blob Read (Lines 333-377)**
- Supports range reads with offset & size parameters
- Returns data in `azure_buffer_t *out` (caller must handle reallocation)
- **Line 347-350**: Clamping: if offset > blob size, returns error
- **Line 352-354**: Available = min(requested_len, blob_size - offset)
- **Line 356-365**: Allocates/reallocates output buffer to match requested length
- **Line 367-373**: Copies available data, zero-fills remainder (short read pattern)
- **Line 374**: Sets `out->size = requested_len` (full length requested, not actual data)

**Page Blob Write (Lines 284-331)**
- **Lines 293-303**: Validates 512-byte alignment on both offset AND length
- **Line 305-309**: Fails if blob doesn't exist (AZURE_ERR_NOT_FOUND)
- **Lines 312-319**: Auto-grows blob if write extends past current size
- **Line 321**: Direct memcpy of data at offset
- **Lines 323-328**: Records write (offset, len) for coalescing tests
- Return: `AZURE_OK` on success

**Failure Injection Logic (Lines 178-210)**
- **pre_call()** at line 213: Increments counters, checks failures
- **check_failures()** at line 178:
  - Iterates fail_rules, returns first matching error
  - One-shot rules: `r->active = 0` after match (line 189)
  - Three rule types:
    1. By overall call number (line 188): `r->call_number == current_call`
    2. By Nth call to specific op (line 195): `r->op_call_number == op_call_num`
    3. Always for operation (line 204): `r->call_number == 0 && r->op_call_number == 0`

**Lease State Machine (Lines 551-688)**
- **acquire** (551-590): AVAILABLE → LEASED, generates UUID-like lease_id
- **renew** (592-619): Validates lease_state == LEASED && lease_id matches, updates timestamp
- **release** (621-649): Validates state and ID, clears lease to AVAILABLE
- **break** (651-688):
  - `break_period <= 0`: Immediate to AVAILABLE
  - `break_period > 0`: Transitions to BREAKING (tests can inspect state before next call)

**Mock ops Initialization (Lines 788-808)**
```c
static azure_ops_t mock_ops = {
    .page_blob_create    = mock_page_blob_create,
    .page_blob_write     = mock_page_blob_write,
    .page_blob_read      = mock_page_blob_read,
    .page_blob_resize    = mock_page_blob_resize,
    .block_blob_upload   = mock_block_blob_upload,
    .block_blob_download = mock_block_blob_download,
    .blob_get_properties = mock_blob_get_properties,
    .blob_delete         = mock_blob_delete_impl,
    .blob_exists         = mock_blob_exists_impl,
    .lease_acquire       = mock_lease_acquire_impl,
    .lease_renew         = mock_lease_renew_impl,
    .lease_release       = mock_lease_release_impl,
    .lease_break         = mock_lease_break_impl,
    .page_blob_write_batch = NULL,  /* Phase 2 */
    .append_blob_create  = mock_append_blob_create_impl,
    .append_blob_append  = mock_append_blob_append_impl,
    .append_blob_delete  = mock_append_blob_delete_impl,
};
```

---

## 3. TEST VFS FILE (test_vfs.c) 

### File Includes (Lines 17-21)
```c
#include "../sqlite-autoconf-3520000/sqlite3.h"
#include "mock_azure_ops.h"
#include "test_harness.h"
#include <string.h>
#include <stdlib.h>
#include <sqlite_objs.h>  /* Line 1326 */
```

### Global Setup/Teardown (Lines 25-37)
```c
static mock_azure_ctx_t *g_ctx = NULL;
static azure_ops_t      *g_ops = NULL;

static void setup(void) {
    if (g_ctx) mock_reset(g_ctx);
    else       g_ctx = mock_azure_create();
    g_ops = mock_azure_get_ops();
}

static void teardown(void) {
    /* Intentionally left alive across tests for efficiency.
    ** mock_reset in setup() clears state. */
}
```

**Pattern**: Each test calls `setup()` at beginning, reuses global context across tests

### VFS Test Helpers (Lines 1326-1339)
```c
static sqlite3 *open_test_db(mock_azure_ctx_t *mctx) {
    sqlite3 *db = NULL;
    sqlite_objs_vfs_register_with_ops(mock_azure_get_ops(), mctx, 0);
    int rc = sqlite3_open_v2("test.db", &db,
                              SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                              "sqlite-objs");
    return (rc == SQLITE_OK) ? db : NULL;
}

static void close_test_db(sqlite3 *db) {
    if (db) sqlite3_close(db);
}
```

### Test Sections & Count
Total: **105 test functions** organized in 17 suites:

#### SECTION 1: Mock Page Blob Operations (Lines 43-305)
1. **page_blob_create_basic** (43) — Creates 4KB page blob
2. **page_blob_create_zero_size** (51) — Creates 0-size blob
3. **page_blob_create_unaligned_fails** (59) — 1000 bytes → AZURE_ERR_ALIGNMENT
4. **page_blob_create_already_exists_resizes** (66) — Re-creating resizes blob
5. **page_blob_create_large** (75) — 512 KB creation
6. **page_blob_write_basic** (84) — Writes 512 bytes at offset 0
7. **page_blob_write_at_offset** (99) — Writes at offset 1024
8. **page_blob_write_unaligned_offset_fails** (119) — Offset 100 → alignment error
9. **page_blob_write_unaligned_length_fails** (129) — Length 100 bytes → alignment error
10. **page_blob_write_nonexistent_fails** (139) — Write to non-existent blob
11. **page_blob_write_grows_blob** (147) — Write extends blob size
12. **page_blob_write_multiple_pages** (160) — Multiple 512-byte writes
13. **page_blob_write_overwrite** (181) — Overwriting data
14. **page_blob_read_basic** (197) — Reads 512 bytes from offset 0
15. **page_blob_read_at_offset** (215) — Reads from offset 512
16. **page_blob_read_nonexistent_fails** (232) — Read from non-existent blob
17. **page_blob_read_past_eof_zero_fills** (242) — Read past EOF → zero-fill
18. **page_blob_read_offset_past_eof** (266) — Offset past EOF
19. **page_blob_write_then_read_roundtrip** (278) — Write then read same data
20. **page_blob_resize_grow** (306) — Resize 4KB → 8KB
21. **page_blob_resize_shrink** (315) — Shrink blob
22. **page_blob_resize_unaligned_fails** (324) — Unaligned size fails
23. **page_blob_resize_nonexistent_fails** (332) — Resize non-existent blob
24. **page_blob_resize_preserves_data** (339) — Data persists after resize
25. **page_blob_resize_to_zero** (357) — Resize to zero

#### SECTION 2: Block Blob Operations (Lines 370-450)
26. **block_blob_upload_basic** (370) — Upload data blob
27. **block_blob_upload_replaces** (380) — Upload replaces content
28. **block_blob_upload_empty** (394) — Upload empty blob
29. **block_blob_download_basic** (402) — Download blob data
30. **block_blob_download_nonexistent_fails** (417) — Download non-existent
31. **block_blob_upload_download_roundtrip** (427) — Upload then download

#### SECTION 3: Common Blob Operations (Lines 453-610)
32. **blob_exists_page_blob** (453) — Check page blob exists
33. **blob_exists_block_blob** (464) — Check block blob exists
34. **blob_exists_nonexistent** (476) — Non-existent blob
35. **blob_delete_page_blob** (485) — Delete page blob
36. **blob_delete_block_blob** (494) — Delete block blob
37. **blob_delete_nonexistent_fails** (504) — Delete non-existent fails
38. **blob_get_properties_page_blob** (511) — Get page blob size/lease state
39. **blob_get_properties_nonexistent_fails** (527) — Properties of non-existent blob
40. **blob_get_properties_with_lease** (536) — Properties with active lease

#### SECTION 4: Lease Operations (Lines 555-780)
41. **lease_acquire_basic** (555) — Acquire lease
42. **lease_acquire_already_leased_fails** (568) — Can't acquire leased blob
43. **lease_acquire_nonexistent_fails** (580) — Acquire on non-existent blob
44. **lease_renew_basic** (589) — Renew active lease
45. **lease_renew_wrong_id_fails** (602) — Wrong lease ID fails
46. **lease_renew_not_leased_fails** (614) — Renew without lease fails
47. **lease_release_basic** (623) — Release lease
48. **lease_release_wrong_id_fails** (637) — Release with wrong ID fails
49. **lease_release_not_leased_fails** (650) — Release non-leased blob
50. **lease_break_immediate** (659) — Break with period 0
51. **lease_break_with_period** (674) — Break with grace period
52. **lease_break_not_leased_fails** (689) — Break non-leased blob
53. **lease_acquire_after_release** (699) — Can re-acquire after release
54. **lease_acquire_after_immediate_break** (715) — Acquire after immediate break
55. **lease_acquire_during_breaking_fails** (730) — Can't acquire while breaking
56. **lease_state_inspection** (747) — Inspect lease state directly

#### SECTION 5: Failure Injection (Lines 773-917)
57. **fail_at_specific_call** (773) — Fail at call #3
58. **fail_at_is_one_shot** (788) — One-shot failure
59. **fail_operation_always** (802) — Fail all operation calls
60. **fail_operation_lease_acquire** (818) — Fail all lease_acquire
61. **fail_clear_restores_normal** (832) — Clear failures restores normal
62. **fail_injection_populates_error** (846) — Error structure populated
63. **fail_blob_read** (860) — Inject read failure
64. **fail_blob_delete** (874) — Inject delete failure
65. **fail_block_blob_download** (887) — Inject download failure
66. **fail_block_blob_upload** (902) — Inject upload failure

#### SECTION 6: Call Counting & State (Lines 917-1010)
67. **call_count_tracks_operations** (917) — Count across all ops
68. **call_count_per_operation** (929) — Count per-operation
69. **call_count_includes_failures** (945) — Count even when failed
70. **call_count_reset** (955) — Reset counters
71. **call_count_unknown_op** (967) — Unknown op returns 0

#### SECTION 7: Reset & State (Lines 976-1010)
72. **reset_clears_blobs** (976) — Reset removes all blobs
73. **reset_clears_leases** (986) — Reset clears lease state
74. **reset_clears_failures** (997) — Reset removes failure rules
75. **reset_clears_counters** (1008) — Reset zeroes counters

#### SECTION 8: Buffer Management (Lines 1021-1072)
76. **buffer_init** (1021) — Buffer initialization
77. **buffer_reuse** (1029) — Reusing buffer
78. **buffer_free_resets** (1055) — Free resets size

#### SECTION 9: Error Handling (Lines 1072-1303)
79. **error_populated_on_not_found** (1072) — Error struct populated on 404
80. **error_populated_on_conflict** (1087) — Error on conflict
81. **error_populated_on_alignment** (1103) — Error on alignment failure
82. **page_ops_on_block_blob_fails** (1117) — Type mismatch
83. **block_ops_on_page_blob_fails** (1130) — Type mismatch reverse
84. **create_page_blob_over_block_blob_fails** (1142) — Create over wrong type
85. **multiple_blobs_independent** (1156) — Multi-blob isolation
86. **blob_name_with_path_separators** (1176) — Name with '/'
87. **blob_name_with_special_chars** (1185) — Name with special chars
88. **lease_on_different_blobs** (1194) — Leases independent per blob
89. **page_blob_read_from_zero_blob** (1209) — Read from empty blob
90. **page_blob_write_4096_page_size** (1226) — 4KB page writes
91. **delete_and_recreate** (1244) — Delete then recreate blob
92. **journal_blob_naming_convention** (1266) — Journal blob names
93. **blob_properties_after_write** (1285) — Properties reflect writes
94. **concurrent_page_and_block_blobs** (1299) — Both blob types coexist

#### SECTION 9: VFS Registration (Lines 1343-1376)
95. **vfs_registers_with_correct_name** (1343) — VFS name "sqlite-objs"
96. **vfs_not_default_unless_requested** (1351) — Not default by default
97. **vfs_can_be_default** (1359) — Register as default
98. **vfs_open_with_name_parameter** (1366) — Open via VFS name

#### SECTION 10: VFS Basic Operations (Lines 1380-1473)
99. **vfs_create_new_database** (1380) — Create new DB
100. **vfs_create_table_insert_select** (1390) — CREATE TABLE, INSERT, SELECT
101. **vfs_transaction_commit_writes_to_blob** (1415) — Commit flushes pages
102. **vfs_multiple_transactions** (1433) — Multiple transactions
103. **vfs_data_survives_close_reopen** (1457) — Persistence across sessions

#### Additional VFS Test Suites (Lines 1477+)
104-105. More tests covering dirty pages, journals, locking, errors, pragmas, security, etc.

---

## 4. SQLITE_OBJS VFS IMPLEMENTATION (sqlite_objs_vfs.c)

### Core Constants (Lines 91-97)
```c
#define SQLITE_OBJS_DEFAULT_PAGE_SIZE       4096
#define SQLITE_OBJS_LEASE_DURATION          30      /* default lease (seconds) */
#define SQLITE_OBJS_LEASE_DURATION_LONG     60      /* extended lease for large flushes */
#define SQLITE_OBJS_DIRTY_PAGE_THRESHOLD    100     /* dirty pages triggering extended lease */
#define SQLITE_OBJS_MAX_PATHNAME            512
#define SQLITE_OBJS_INITIAL_ALLOC           (64*1024)  /* 64 KiB initial buffer */
#define SQLITE_OBJS_DEFAULT_CACHE_PAGES     1024    /* LRU cache size */
```

#### Cache Configuration via Environment Variable (Lines 127-134)
```c
static int cacheGetMaxPages(void) {
    const char *val = getenv("SQLITE_OBJS_CACHE_PAGES");
    if (val) {
        int n = atoi(val);
        if (n > 0) return n;
    }
    return SQLITE_OBJS_DEFAULT_CACHE_PAGES;  /* Default: 1024 pages */
}
```

### Page Cache Structure (Lines 106-125)

**Cache Entry** (lines 106-113)
```c
typedef struct sqlite_objs_cache_entry {
    int pageNo;                          /* 0-based page index */
    unsigned char *data;                 /* Allocated page data (pageSize bytes) */
    int dirty;                           /* 1 = needs flush to Azure */
    struct sqlite_objs_cache_entry *lruPrev; /* LRU: toward MRU end */
    struct sqlite_objs_cache_entry *lruNext; /* LRU: toward LRU end */
    struct sqlite_objs_cache_entry *hashNext;/* Hash chain for lookup */
} sqlite_objs_cache_entry_t;
```

**Cache Context** (lines 116-125)
```c
typedef struct sqlite_objs_page_cache {
    int maxPages;                        /* Soft limit for clean page eviction */
    int nPages;                          /* Current total pages in cache */
    int nDirty;                          /* Current dirty page count */
    int pageSize;                        /* Bytes per page (from header or 4096) */
    int hashSize;                        /* Hash table bucket count (power of 2) */
    sqlite_objs_cache_entry_t **hashTable;   /* Hash buckets array */
    sqlite_objs_cache_entry_t *lruHead;      /* Most recently used */
    sqlite_objs_cache_entry_t *lruTail;      /* Least recently used */
} sqlite_objs_page_cache_t;
```

### File Structure (Lines 287-332)

**sqlite-objsFile** — Per-file state
```c
typedef struct sqlite-objsFile {
    const sqlite3_io_methods *pMethod;  /* MUST be first */
    
    /* Azure operations */
    azure_ops_t *ops;                   /* Ops vtable (swappable for tests) */
    void *ops_ctx;                      /* Context for ops */
    char *zBlobName;                    /* Blob name in container */
    
    /* Page cache — MAIN_DB only */
    sqlite_objs_page_cache_t cache;         /* LRU demand-paged cache */
    sqlite3_int64 blobSize;             /* Current logical blob size */
    int lastSyncDirtyCount;             /* Dirty pages at last xSync (lease heuristic) */
    
    /* Lock state */
    int eLock;                          /* Current SQLite lock level */
    char leaseId[64];                   /* Azure lease ID (empty = no lease) */
    time_t leaseAcquiredAt;             /* For renewal timing */
    int leaseDuration;                  /* Actual lease duration acquired */
    
    /* Optimization tracking */
    sqlite3_int64 lastSyncedSize;       /* Blob size after last resize/open (R2) */
    char etag[128];                     /* Current blob ETag */
    
    /* File type */
    int eFileType;                      /* SQLITE_OPEN_MAIN_DB, MAIN_JOURNAL, WAL */
    
    /* Back-pointer to VFS shared state */
    sqlite-objsVfsData *pVfsData;
    
    /* Journal file — MAIN_JOURNAL only */
    unsigned char *aJrnlData;           /* Journal buffer */
    sqlite3_int64 nJrnlData;           /* Logical journal size */
    sqlite3_int64 nJrnlAlloc;          /* Allocated journal buffer */
    
    /* WAL file — WAL only */
    unsigned char *aWalData;            /* WAL buffer */
    sqlite3_int64 nWalData;            /* Logical WAL size */
    sqlite3_int64 nWalAlloc;           /* Allocated WAL buffer */
    sqlite3_int64 nWalSynced;          /* Bytes already synced to Azure */
    int walNeedFullResync;              /* 1 if writes invalidated synced data */
    
    /* Per-file client (optional) */
    azure_client_t *ownClient;
} sqlite-objsFile;
```

---

### xRead Implementation (Lines 619-747)

**Purpose**: Read from page cache (MAIN_DB) or in-memory buffers (journal/WAL)

**Key Behaviors**:
1. **WAL reads** (lines 624-639): Read from `aWalData` buffer, zero-fill on short read
2. **Journal reads** (lines 640-656): Read from `aJrnlData` buffer, zero-fill on short read
3. **MAIN_DB reads** (lines 658-746):
   - Demand-paging through LRU cache
   - Loop through requested bytes, page by page
   - Cache hit: touch LRU, copy data
   - Cache miss (line 681):
     - Fetch from Azure via `page_blob_read()` at offset `pageNo * pageSize`
     - Allocate page buffer (calloc)
     - Evict clean pages if cache at capacity
     - Insert into cache
   - Handle short reads past EOF: zero-fill remainder
   - Clamp fetch to blob size (lines 685-687)

**Short Read Handling**:
- Line 674: Past logical EOF → return SQLITE_IOERR_SHORT_READ with zeros
- Line 730: Partial page at EOF → return SQLITE_IOERR_SHORT_READ
- Lines 732-737: Valid copy + zero-fill remainder

---

### xWrite Implementation (Lines 754-865)

**Purpose**: Write to page cache (MAIN_DB) or in-memory buffers

**Key Behaviors**:
1. **WAL writes** (lines 759-771):
   - Ensure buffer capacity via `walBufferEnsure()`
   - memcpy to buffer at offset
   - If overwriting already-synced data, set `walNeedFullResync = 1`

2. **Journal writes** (lines 773-781):
   - Ensure buffer capacity via `jrnlBufferEnsure()`
   - memcpy to buffer at offset

3. **MAIN_DB writes** (lines 783-864):
   - Line 790-793: Renew lease if needed during long write sequences
   - Loop through pages:
     - Cache hit: touch LRU, mark dirty
     - Cache miss: 
       - For partial writes (line 809): Fetch existing page from Azure first
       - For full page writes (line 809): Skip fetch, just allocate
       - Read existing content if partial overwrite (lines 821-835)
       - Evict clean pages if at capacity
       - Insert into cache
     - Memcpy write data to page
     - Mark page dirty, increment `cache.nDirty`
   - Extend logical blob size if write past EOF (lines 860-862)

**Dirty Tracking**:
- Line 849-852: Mark cache entry as dirty, increment `cache.nDirty`

---

### xTruncate Implementation (Lines 870-946)

**Purpose**: Resize the file

**WAL truncate** (lines 873-906):
- Size 0: Delete append blob, recreate empty
- Size < nWalData: Truncate buffer
- If size < nWalSynced: Set `walNeedFullResync = 1`

**Journal truncate** (lines 909-918):
- If size < nJrnlData: Truncate buffer and zero-fill remainder

**MAIN_DB truncate** (lines 920-945):
- Line 923: Align size to 512-byte boundary for Azure
- Call `page_blob_resize()` on Azure
- Line 934: Track new size in `lastSyncedSize` (R2 optimization)
- Invalidate cache entries beyond new size (lines 937-942)
- Update `blobSize`

---

### xSync Implementation (Lines 1073-1365+)

**Purpose**: Flush dirty pages/journal/WAL to Azure

#### WAL Sync (lines 1078-1178)
- Check if data pending: `nWalData > nWalSynced`
- If `walNeedFullResync`: Delete & recreate blob, upload entire WAL in chunks
- Else: Append only new data (`nWalData - nWalSynced`) in chunks
- Chunk size: Respect `AZURE_MAX_APPEND_SIZE` (likely 4 MiB)
- On partial append error: Delete & recreate blob, set `walNeedFullResync = 1`

#### Journal Sync (lines 1180-1200+)
- If `nJrnlData > 0`: Upload entire journal via `block_blob_upload()`
- Timing: Optional debug output if `SQLITE_OBJS_DEBUG_TIMING=1`

#### MAIN_DB Sync (lines 1200+)
- Collect dirty pages via `cacheCollectDirty()`
- Coalesce consecutive pages into ranges (max 4 MiB per range)
- For each range:
  - Create contiguous temp buffer
  - Copy dirty page data
  - Write via `page_blob_write()` (or batch if available)
  - Free temp buffer
- Handle lease renewal for large flushes (>100 dirty pages → 60s lease)
- Update dirty counts and clear dirty flags

---

### xOpen Implementation (Lines 1673-1850+)

**Purpose**: Open a file

**Routing** (lines 1677-1688):
- MAIN_DB, MAIN_JOURNAL, WAL → Azure-backed
- Everything else → delegate to default VFS

**Initialization** (lines 1682-1735):
- Memset file struct to zero
- Copy blob name
- Set up ops (from per-file URI config or global VFS ops)
- Set file type flag
- Initialize lock state

**MAIN_DB xOpen** (lines 1736-1860):
1. **Check blob existence** (lines 1743-1772):
   - Call `blob_get_properties()` as single HEAD request
   - Captures ETag if returned
   - Falls back to `blob_exists()` if needed

2. **Existing blob** (lines 1774-1840):
   - Fetch first page to detect page size from SQLite header (lines 1777-1790)
   - Call `detectPageSize()` on header bytes (line 1795)
   - Initialize cache with detected page size (line 1800)
   - Insert fetched page(s) into cache (lines 1808-1827)
   - Record initial blob size in `lastSyncedSize` (R2 optimization, line 1832)

3. **New blob** (lines 1842+):
   - Call `page_blob_create()` with size 0
   - Initialize cache with default page size

---

## Summary Table: Test Infrastructure Relationships

| Component | Role | Key Files | Lines |
|-----------|------|-----------|-------|
| **test_harness.h** | Test framework | Macros, assertions | 1-226 |
| **mock_azure_ops.h** | Mock API contract | Declarations, enums | 1-201 |
| **mock_azure_ops.c** | Mock implementation | Blobs, leases, failure injection | 1-1038 |
| **test_vfs.c** | Integration tests | 105 tests, 15 suites | 1-2561 |
| **sqlite_objs_vfs.c** | VFS implementation | xRead/Write/Sync/Truncate/xOpen | 1-2357 |

## Key Testing Patterns

1. **Setup/Teardown**: Global context reused, reset via `mock_reset()` each test
2. **Failure Injection**: Three modes — by call #, by operation, by operation call #
3. **State Inspection**: Query mock state directly (blob data, lease state, call counts)
4. **Write Recording**: Track page_blob_write calls for coalescing validation
5. **Page Cache Testing**: Demand-paging, LRU eviction, dirty marking
6. **Lease State Machine**: AVAILABLE ↔ LEASED ↔ BREAKING transitions
7. **Blob Type Safety**: Type mismatch errors (page ops on block blobs, etc.)

---

## Environment Variables

- `SQLITE_OBJS_CACHE_PAGES` — Override default 1024 page cache size
- `SQLITE_OBJS_DEBUG_TIMING` — Enable optional timing output in xSync

