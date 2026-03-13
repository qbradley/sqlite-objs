# SQLite-Objs VFS Test Infrastructure — Quick Reference

## Essential Files
- `/Users/qbradley/src/sqlite/test/test_harness.h` — Test macros & framework
- `/Users/qbradley/src/sqlite/test/mock_azure_ops.h/c` — Mock Azure ops
- `/Users/qbradley/src/sqlite/test/test_vfs.c` — 105 integration tests
- `/Users/qbradley/src/sqlite/src/sqlite_objs_vfs.c` — Production VFS implementation

## Test Execution Pattern

```c
TEST(my_test) {
    setup();  // Reset mock context
    // Test code here
    ASSERT_* assertions
}

int main() {
    TEST_SUITE_BEGIN("Suite Name");
    RUN_TEST(my_test);
    TEST_SUITE_END();
    return test_harness_summary();
}
```

## Mock Context Usage

```c
mock_azure_ctx_t *ctx = mock_azure_create();
azure_ops_t *ops = mock_azure_get_ops();

// Failure injection
mock_set_fail_at(ctx, 3, AZURE_ERR_NETWORK);           // Fail call #3
mock_set_fail_operation(ctx, "page_blob_write", ...);  // Fail all writes
mock_set_fail_operation_at(ctx, "page_blob_write", 2, ...); // Fail 2nd write

// State inspection
int count = mock_get_call_count(ctx, "page_blob_write");
int64_t size = mock_get_page_blob_size(ctx, "test.db");
const uint8_t *data = mock_get_page_blob_data(ctx, "test.db");
int leased = mock_is_leased(ctx, "test.db");

// Reset
mock_reset(ctx);
mock_clear_write_records(ctx);
```

## Page Cache Architecture

**LRU Demand-Paging Cache**
- Default: 1024 pages
- Override: `export SQLITE_OBJS_CACHE_PAGES=2048`
- Hash table for O(1) lookup
- Doubly-linked LRU list for eviction

**Read Flow**:
```
xRead(offset, len)
  → pageNo = offset / pageSize
  → cacheLookup(pageNo)
     ✓ Hit: cacheLruTouch(), copy data
     ✗ Miss: page_blob_read() from Azure
          → calloc page buffer
          → insert into cache
          → evict clean pages if needed
```

**Write Flow**:
```
xWrite(offset, len, data)
  → pageNo = offset / pageSize
  → cacheLookup(pageNo)
     ✓ Hit: memcpy, mark dirty (entry->dirty = 1, cache.nDirty++)
     ✗ Miss: For partial writes → fetch existing page from Azure first
          → memcpy new data into page
          → mark dirty
```

**Sync Flow**:
```
xSync()
  → cacheCollectDirty() — gather all dirty entries
  → cacheCoalesceRanges() — merge consecutive pages (max 4 MiB per range)
  → For each range:
       → Create temp buffer
       → Copy page data into contiguous buffer
       → page_blob_write() to Azure
       → Free temp buffer
  → Clear dirty flags
  → Renew lease if >100 dirty pages (use 60s lease instead of 30s)
```

## Key Constants

```c
SQLITE_OBJS_DEFAULT_PAGE_SIZE       4096        // SQLite page size
SQLITE_OBJS_LEASE_DURATION          30          // Standard lease (seconds)
SQLITE_OBJS_LEASE_DURATION_LONG     60          // Extended lease for large flushes
SQLITE_OBJS_DIRTY_PAGE_THRESHOLD    100         // Triggers extended lease
SQLITE_OBJS_DEFAULT_CACHE_PAGES     1024        // LRU cache size
PAGE_BLOB_ALIGNMENT             512         // Enforced on offset & length
SQLITE_OBJS_MAX_PUT_PAGE            (4*1024*1024) // 4 MiB max per page_blob_write range
```

## VFS Operations Routing

| File Type | Handler | Storage |
|-----------|---------|---------|
| MAIN_DB | `xRead/xWrite/xSync/xTruncate` | Azure Page Blob |
| MAIN_JOURNAL | `xRead/xWrite/xSync/xTruncate` | Azure Block Blob |
| WAL | `xRead/xWrite/xSync/xTruncate` | Azure Append Blob |
| Temp/sub-journals | Delegate | Platform default VFS |

## xOpen Initialization

**MAIN_DB**:
1. Call `blob_get_properties()` for single HEAD request
2. If exists: Fetch first page to detect page size from SQLite header
3. Initialize cache with detected page size
4. Insert first page(s) into cache
5. Record initial blob size in `lastSyncedSize` (R2 optimization)

**MAIN_JOURNAL**:
1. Initialize journal buffer (start empty)

**WAL**:
1. Initialize WAL buffer (start empty)
2. Track `nWalSynced` separately for incremental appends

## Lease State Machine

```
┌────────────────┐
│   AVAILABLE    │ ← acquire() or release() or break(period≤0)
└────────────────┘
        ↑
        │ acquire(duration)
        ↓
┌────────────────┐
│    LEASED      │ ← renew() (updates timestamp)
└────────────────┘
        ↑
        │ break(period>0)
        ↓
┌────────────────┐
│   BREAKING     │ (inspection only, tests verify before next call)
└────────────────┘
        ↓
     AVAILABLE (implicit, no callback in tests)
```

## Assertion Macros (test_harness.h)

```c
ASSERT_EQ(a, b)              // a == b
ASSERT_NE(a, b)              // a != b
ASSERT_GT(a, b)              // a > b
ASSERT_GE(a, b)              // a >= b
ASSERT_LT(a, b)              // a < b
ASSERT_LE(a, b)              // a <= b
ASSERT_TRUE(x)               // x is truthy
ASSERT_FALSE(x)              // x is falsy
ASSERT_NULL(x)               // x == NULL
ASSERT_NOT_NULL(x)           // x != NULL
ASSERT_STR_EQ(a, b)          // strcmp(a, b) == 0 (NULL-safe)
ASSERT_STR_NE(a, b)          // strcmp(a, b) != 0
ASSERT_MEM_EQ(a, b, len)     // memcmp(a, b, len) == 0
ASSERT_OK(rc)                // SQLite rc == SQLITE_OK (0)
ASSERT_ERR(rc, exp)          // SQLite rc == expected
ASSERT_AZURE_OK(rc)          // Azure rc == AZURE_OK (0)
ASSERT_AZURE_ERR(rc, exp)    // Azure rc == expected
```

Failure handling: Prints file:line + message, uses longjmp to exit test early.

## Mock Page Blob Read

**Signature**:
```c
azure_err_t page_blob_read(void *ctx, const char *name,
                           int64_t offset, size_t len,
                           azure_buffer_t *out, azure_error_t *err);
```

**Behavior**:
- Supports range reads with offset & size
- **Clamping**: if `offset > blob_size`, returns `AZURE_ERR_INVALID_ARG`
- **Short read**: copies available data, zero-fills remainder
- **Buffer allocation**: Reallocs `out->data` to match requested `len`
- **Return size**: Sets `out->size = len` (requested length, not actual bytes)
- **Zero-fill**: Remainder of buffer beyond available data

**Example**:
```c
// Blob is 1000 bytes, read 512 bytes at offset 800:
azure_buffer_t buf = {0};
page_blob_read(ctx, "blob.db", 800, 512, &buf, &err);
// Result: buf.data contains bytes 800-999 (200 bytes), then 312 zero bytes
//         buf.size = 512
//         buf.capacity >= 512
```

## Write Recording (for coalescing tests)

```c
// Collect all page_blob_write calls
int count = mock_get_write_record_count(ctx);
for (int i = 0; i < count; i++) {
    mock_write_record_t rec = mock_get_write_record(ctx, i);
    printf("Write %d: offset=%lld, len=%zu\n", i, rec.offset, rec.len);
}

// Clear before test
mock_clear_write_records(ctx);
```

## Environment Variables

```bash
export SQLITE_OBJS_CACHE_PAGES=2048      # Override default 1024 pages
export SQLITE_OBJS_DEBUG_TIMING=1        # Enable timing output in xSync
```

## Common Test Patterns

**Mock an Azure failure at specific point**:
```c
setup();
mock_set_fail_operation_at(g_ctx, "page_blob_write", 2, AZURE_ERR_NETWORK);
// ... trigger 2 page_blob_writes ...
// Second write will fail
```

**Verify dirty page flushing**:
```c
mock_reset_call_counts(g_ctx);
sqlite3_exec(db, "INSERT INTO t VALUES(1);", NULL, NULL, NULL);
int writes = mock_get_call_count(g_ctx, "page_blob_write");
ASSERT_GT(writes, 0);
```

**Inspect final blob state**:
```c
const uint8_t *blob = mock_get_page_blob_data(g_ctx, "test.db");
int64_t size = mock_get_page_blob_size(g_ctx, "test.db");
ASSERT_MEM_EQ(blob, expected_data, size);
```

**Verify lease management**:
```c
sqlite3_exec(db, "INSERT INTO t VALUES(1);", NULL, NULL, NULL);
int leased = mock_is_leased(g_ctx, "test.db");
ASSERT_TRUE(leased);

sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
leased = mock_is_leased(g_ctx, "test.db");
ASSERT_FALSE(leased);
```

---

For full details, see: `/Users/qbradley/src/sqlite/VFS_TEST_INFRASTRUCTURE.md`
