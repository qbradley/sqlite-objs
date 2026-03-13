/*
** test_coalesce.c — Unit tests for Phase 1 page coalescing
**
** Validates the dirty-page coalescing optimization for xSync.
** Tests verify that contiguous dirty pages are merged into coalesced
** ranges (up to 4 MiB each) and that the resulting writes are correct.
**
** Two tiers:
**   1. VFS integration tests (ENABLE_VFS_INTEGRATION) — test through
**      xSync by writing data, syncing, and inspecting mock write records.
**      These work with both serial and coalesced implementations.
**
**   2. Direct algorithm tests (ENABLE_COALESCE_TESTS) — test
**      coalesceDirtyRanges() directly with crafted dirty bitmaps.
**      Gated until Aragorn exposes the function.
**
** Mock write recording (mock_get_write_record_count/mock_get_write_record)
** captures every page_blob_write call's offset and length for verification.
*/

#include "../sqlite-autoconf-3520000/sqlite3.h"
#include "mock_azure_ops.h"
#include "test_harness.h"
#include <string.h>
#include <stdlib.h>

/* ── Helpers ──────────────────────────────────────────────────────── */

static mock_azure_ctx_t *co_ctx = NULL;
static azure_ops_t      *co_ops = NULL;

static void co_setup(void) {
    if (co_ctx) mock_reset(co_ctx);
    else        co_ctx = mock_azure_create();
    co_ops = mock_azure_get_ops();
}

#ifdef ENABLE_VFS_INTEGRATION

#include "../src/sqlite_objs.h"

static sqlite3 *co_open_db(void) {
    sqlite3 *db = NULL;
    sqlite_objs_vfs_register_with_ops(co_ops, co_ctx, 0);
    int rc = sqlite3_open_v2("coalesce_test.db", &db,
                              SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                              "sqlite-objs");
    if (rc != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return NULL;
    }
    /* Use NORMAL synchronous for faster tests (still calls xSync for DB) */
    sqlite3_exec(db, "PRAGMA synchronous=NORMAL;", NULL, NULL, NULL);
    return db;
}

static void co_close_db(sqlite3 *db) {
    if (db) sqlite3_close(db);
}

/* Helper: execute SQL and assert SQLITE_OK */
static int co_exec(sqlite3 *db, const char *sql) {
    return sqlite3_exec(db, sql, NULL, NULL, NULL);
}

/* Helper: verify all write records have 512-byte aligned offset and length */
static void assert_writes_aligned(void) {
    int n = mock_get_write_record_count(co_ctx);
    for (int i = 0; i < n; i++) {
        mock_write_record_t rec = mock_get_write_record(co_ctx, i);
        if (rec.offset % 512 != 0) {
            TH_FAIL("Write record %d: offset %lld not 512-aligned",
                    i, (long long)rec.offset);
        }
        if (rec.len % 512 != 0) {
            TH_FAIL("Write record %d: len %zu not 512-aligned",
                    i, rec.len);
        }
    }
}

/* Helper: verify write records don't overlap and are in ascending order */
static void assert_writes_ordered_nonoverlapping(void) {
    int n = mock_get_write_record_count(co_ctx);
    for (int i = 1; i < n; i++) {
        mock_write_record_t prev = mock_get_write_record(co_ctx, i - 1);
        mock_write_record_t cur  = mock_get_write_record(co_ctx, i);
        if (cur.offset < prev.offset) {
            TH_FAIL("Write records not ordered: [%d].offset=%lld < [%d].offset=%lld",
                    i, (long long)cur.offset, i-1, (long long)prev.offset);
        }
        if (prev.offset + (int64_t)prev.len > cur.offset) {
            TH_FAIL("Write records overlap: [%d] ends at %lld, [%d] starts at %lld",
                    i-1, (long long)(prev.offset + prev.len),
                    i, (long long)cur.offset);
        }
    }
}

/* Helper: verify no write exceeds Azure's 4 MiB per-PUT limit */
static void assert_writes_within_4mb(void) {
    int n = mock_get_write_record_count(co_ctx);
    for (int i = 0; i < n; i++) {
        mock_write_record_t rec = mock_get_write_record(co_ctx, i);
        if (rec.len > 4 * 1024 * 1024) {
            TH_FAIL("Write record %d: len %zu exceeds 4 MiB limit",
                    i, rec.len);
        }
    }
}


/* ══════════════════════════════════════════════════════════════════════
** Test 1: test_coalesce_empty
** No dirty pages → 0 ranges / 0 writes
** ══════════════════════════════════════════════════════════════════════ */

TEST(coalesce_empty) {
    co_setup();
    sqlite3 *db = co_open_db();
    ASSERT_NOT_NULL(db);

    /* Create table to initialize the DB */
    ASSERT_OK(co_exec(db, "CREATE TABLE t(x);"));
    ASSERT_OK(co_exec(db, "INSERT INTO t VALUES(1);"));

    /* After commit, dirty pages are flushed. Now reset write records. */
    mock_clear_write_records(co_ctx);
    mock_reset_call_counts(co_ctx);

    /* Read-only operation — should not dirty any pages */
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, "SELECT * FROM t;", -1, &stmt, NULL);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    /* No page_blob_write calls should have occurred */
    ASSERT_EQ(mock_get_call_count(co_ctx, "page_blob_write"), 0);
    ASSERT_EQ(mock_get_write_record_count(co_ctx), 0);

    co_close_db(db);
}


/* ══════════════════════════════════════════════════════════════════════
** Test 2: test_coalesce_single
** Single dirty page → 1 range with correct offset/len
** ══════════════════════════════════════════════════════════════════════ */

TEST(coalesce_single) {
    co_setup();
    sqlite3 *db = co_open_db();
    ASSERT_NOT_NULL(db);

    /* Setup: create table and initial data */
    ASSERT_OK(co_exec(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, val TEXT);"));

    /* Clear writes from setup */
    mock_clear_write_records(co_ctx);
    mock_reset_call_counts(co_ctx);

    /* Single small insert — should dirty 1-2 pages */
    ASSERT_OK(co_exec(db, "INSERT INTO t VALUES(1, 'hello');"));

    int nWrites = mock_get_write_record_count(co_ctx);
    ASSERT_GT(nWrites, 0);

    /* Every write must be 512-aligned */
    assert_writes_aligned();

    /* Verify write offsets are page-aligned (4096 default) */
    for (int i = 0; i < nWrites; i++) {
        mock_write_record_t rec = mock_get_write_record(co_ctx, i);
        ASSERT_EQ(rec.offset % 4096, 0);
        ASSERT_GT(rec.len, 0);
    }

    co_close_db(db);
}


/* ══════════════════════════════════════════════════════════════════════
** Test 3: test_coalesce_contiguous
** Contiguous run (many pages) → should coalesce into few large writes
** ══════════════════════════════════════════════════════════════════════ */

TEST(coalesce_contiguous) {
    co_setup();
    sqlite3 *db = co_open_db();
    ASSERT_NOT_NULL(db);

    ASSERT_OK(co_exec(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, data BLOB);"));

    /* Clear writes from CREATE TABLE */
    mock_clear_write_records(co_ctx);
    mock_reset_call_counts(co_ctx);

    /*
    ** Insert enough data to fill ~100 contiguous pages (400 KiB).
    ** SQLite allocates pages sequentially in a new DB, so these will
    ** be contiguous in the file.
    */
    ASSERT_OK(co_exec(db, "BEGIN;"));
    for (int i = 0; i < 100; i++) {
        char sql[256];
        snprintf(sql, sizeof(sql),
                 "INSERT INTO t VALUES(%d, zeroblob(3900));", i);
        ASSERT_OK(co_exec(db, sql));
    }
    ASSERT_OK(co_exec(db, "COMMIT;"));

    int nWrites = mock_get_write_record_count(co_ctx);
    ASSERT_GT(nWrites, 0);

    /* All writes must be 512-aligned and within 4 MiB limit */
    assert_writes_aligned();
    assert_writes_within_4mb();
    assert_writes_ordered_nonoverlapping();

    /*
    ** Coalescing quality check: with ~100 contiguous dirty pages,
    ** coalescing should produce far fewer writes than page count.
    ** Without coalescing: ~100 writes. With coalescing: 1-2 writes.
    **
    ** After Aragorn implements coalescing, enable this assertion:
    */
#ifdef ENABLE_COALESCE_TESTS
    /* 100 contiguous 4K pages = 400 KiB < 4 MiB → should be 1 write */
    ASSERT_LE(nWrites, 2);
#endif

    /* Data integrity: blob should have valid data */
    ASSERT_NOT_NULL(mock_get_page_blob_data(co_ctx, "coalesce_test.db"));

    co_close_db(db);
}


/* ══════════════════════════════════════════════════════════════════════
** Test 4: test_coalesce_scattered
** Non-contiguous pages → multiple separate ranges
** ══════════════════════════════════════════════════════════════════════ */

TEST(coalesce_scattered) {
    co_setup();
    sqlite3 *db = co_open_db();
    ASSERT_NOT_NULL(db);

    /*
    ** Create multiple tables so btree roots are on different pages.
    ** This creates scattered dirty pages when we modify each table.
    */
    ASSERT_OK(co_exec(db, "CREATE TABLE t1(x); INSERT INTO t1 VALUES(1);"));
    ASSERT_OK(co_exec(db, "CREATE TABLE t2(x); INSERT INTO t2 VALUES(2);"));
    ASSERT_OK(co_exec(db, "CREATE TABLE t3(x); INSERT INTO t3 VALUES(3);"));

    /* Fill tables to push their b-trees apart in the file */
    ASSERT_OK(co_exec(db, "BEGIN;"));
    for (int i = 0; i < 50; i++) {
        char sql[128];
        snprintf(sql, sizeof(sql),
                 "INSERT INTO t1 VALUES(zeroblob(3800));");
        ASSERT_OK(co_exec(db, sql));
    }
    for (int i = 0; i < 50; i++) {
        char sql[128];
        snprintf(sql, sizeof(sql),
                 "INSERT INTO t2 VALUES(zeroblob(3800));");
        ASSERT_OK(co_exec(db, sql));
    }
    for (int i = 0; i < 50; i++) {
        char sql[128];
        snprintf(sql, sizeof(sql),
                 "INSERT INTO t3 VALUES(zeroblob(3800));");
        ASSERT_OK(co_exec(db, sql));
    }
    ASSERT_OK(co_exec(db, "COMMIT;"));

    /* Now sync is done. Clear and do scattered updates. */
    mock_clear_write_records(co_ctx);
    mock_reset_call_counts(co_ctx);

    /* Update one row in each table → dirtying pages in 3 different areas */
    ASSERT_OK(co_exec(db, "BEGIN;"));
    ASSERT_OK(co_exec(db, "UPDATE t1 SET x='modified1' WHERE rowid=1;"));
    ASSERT_OK(co_exec(db, "UPDATE t2 SET x='modified2' WHERE rowid=1;"));
    ASSERT_OK(co_exec(db, "UPDATE t3 SET x='modified3' WHERE rowid=1;"));
    ASSERT_OK(co_exec(db, "COMMIT;"));

    int nWrites = mock_get_write_record_count(co_ctx);

    /* Should have multiple writes (scattered dirty pages) */
    ASSERT_GT(nWrites, 1);

    /* All writes must be valid */
    assert_writes_aligned();
    assert_writes_within_4mb();

    co_close_db(db);
}


/* ══════════════════════════════════════════════════════════════════════
** Test 5: test_coalesce_4mb_split
** Run > 4 MiB (>1024 pages at 4096) splits into 2+ ranges
** ══════════════════════════════════════════════════════════════════════ */

TEST(coalesce_4mb_split) {
    co_setup();
    sqlite3 *db = co_open_db();
    ASSERT_NOT_NULL(db);

    ASSERT_OK(co_exec(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, data BLOB);"));

    /* Clear writes from CREATE TABLE */
    mock_clear_write_records(co_ctx);
    mock_reset_call_counts(co_ctx);

    /*
    ** Insert >4 MiB of data in one transaction.
    ** At 4096 bytes/page, 4 MiB = 1024 pages. Insert ~1200 pages worth.
    ** Each row is ~3900 bytes (fits in one page), so 1200 rows ≈ 1200 pages.
    */
    ASSERT_OK(co_exec(db, "BEGIN;"));
    for (int i = 0; i < 1200; i++) {
        char sql[256];
        snprintf(sql, sizeof(sql),
                 "INSERT INTO t VALUES(%d, zeroblob(3900));", i);
        int rc = co_exec(db, sql);
        if (rc != SQLITE_OK) break;
    }
    ASSERT_OK(co_exec(db, "COMMIT;"));

    int nWrites = mock_get_write_record_count(co_ctx);
    ASSERT_GT(nWrites, 0);

    /* Every write must respect the 4 MiB Azure PUT limit */
    assert_writes_within_4mb();
    assert_writes_aligned();

#ifdef ENABLE_COALESCE_TESTS
    /*
    ** With coalescing + 4 MiB split: ~1200 pages × 4096 = ~4.8 MiB.
    ** Should split into 2 coalesced ranges (4 MiB + 0.8 MiB).
    ** Allow some extra for b-tree overhead pages.
    */
    ASSERT_GE(nWrites, 2);   /* Must split (>4 MiB total) */
    ASSERT_LE(nWrites, 5);   /* But shouldn't be hundreds */
#endif

    co_close_db(db);
}


/* ══════════════════════════════════════════════════════════════════════
** Test 6: test_coalesce_every_other
** Every other page dirty → nDirty ranges (no coalescing possible)
** ══════════════════════════════════════════════════════════════════════ */

TEST(coalesce_every_other) {
    co_setup();
    sqlite3 *db = co_open_db();
    ASSERT_NOT_NULL(db);

    /*
    ** Strategy: create a large DB with indexed data, then do scattered
    ** updates that dirty non-adjacent pages. An INDEX on a TEXT column
    ** keeps index pages separate from data pages, creating gaps.
    */
    ASSERT_OK(co_exec(db,
        "CREATE TABLE t(id INTEGER PRIMARY KEY, key TEXT UNIQUE, val TEXT);"));

    /* Populate: 200 rows to spread across many pages */
    ASSERT_OK(co_exec(db, "BEGIN;"));
    for (int i = 0; i < 200; i++) {
        char sql[256];
        snprintf(sql, sizeof(sql),
                 "INSERT INTO t VALUES(%d, 'key%04d%s', zeroblob(1500));",
                 i, i, "paddingpaddingpaddingpadding");
        ASSERT_OK(co_exec(db, sql));
    }
    ASSERT_OK(co_exec(db, "COMMIT;"));

    /* Clear records, then do updates that scatter across pages */
    mock_clear_write_records(co_ctx);
    mock_reset_call_counts(co_ctx);

    ASSERT_OK(co_exec(db, "BEGIN;"));
    /* Update every 10th row — hits pages far apart in the file */
    for (int i = 0; i < 200; i += 10) {
        char sql[256];
        snprintf(sql, sizeof(sql),
                 "UPDATE t SET val='changed%d' WHERE id=%d;", i, i);
        ASSERT_OK(co_exec(db, sql));
    }
    ASSERT_OK(co_exec(db, "COMMIT;"));

    int nWrites = mock_get_write_record_count(co_ctx);

    /*
    ** Scattered updates should produce multiple writes.
    ** Without coalescing: one write per dirty page.
    ** With coalescing: still multiple writes (pages aren't contiguous).
    ** Key property: write count should be close to dirty page count.
    */
    ASSERT_GT(nWrites, 1);
    assert_writes_aligned();
    assert_writes_within_4mb();

    co_close_db(db);
}


/* ══════════════════════════════════════════════════════════════════════
** Test 7: test_coalesce_last_page_short
** Last page shorter than pageSize → range len 512-aligned
** ══════════════════════════════════════════════════════════════════════ */

TEST(coalesce_last_page_short) {
    co_setup();
    sqlite3 *db = co_open_db();
    ASSERT_NOT_NULL(db);

    /*
    ** Create a small DB. The file size may not be an exact multiple of
    ** page size. The xSync code must 512-align the last page write length.
    */
    ASSERT_OK(co_exec(db, "CREATE TABLE t(x TEXT);"));
    ASSERT_OK(co_exec(db, "INSERT INTO t VALUES('small');"));

    mock_clear_write_records(co_ctx);
    mock_reset_call_counts(co_ctx);

    /* Another insert to dirty a page */
    ASSERT_OK(co_exec(db, "INSERT INTO t VALUES('another');"));

    int nWrites = mock_get_write_record_count(co_ctx);
    ASSERT_GT(nWrites, 0);

    /* EVERY write must have 512-aligned length — especially the last page */
    for (int i = 0; i < nWrites; i++) {
        mock_write_record_t rec = mock_get_write_record(co_ctx, i);
        ASSERT_EQ(rec.len % 512, 0);
        ASSERT_EQ(rec.offset % 512, 0);
        ASSERT_GT(rec.len, 0);
    }

    /* Verify the blob data is valid and readable */
    const uint8_t *data = mock_get_page_blob_data(co_ctx, "coalesce_test.db");
    ASSERT_NOT_NULL(data);

    co_close_db(db);
}


/* ══════════════════════════════════════════════════════════════════════
** Test 8: test_coalesce_maxranges_overflow
** More runs than maxRanges → returns error
**
** This test requires direct access to coalesceDirtyRanges to set up
** an extreme number of non-contiguous runs. Gated until exposed.
** ══════════════════════════════════════════════════════════════════════ */

#ifdef ENABLE_COALESCE_TESTS

/*
** Expected function signature from sqlite_objs_vfs.c:
**
**   int coalesceDirtyRanges(
**       sqliteObjsFile *p,
**       azure_page_range_t *ranges,
**       int maxRanges
**   );
**
** Returns number of ranges, or -1 if maxRanges is exceeded.
*/
extern int sqlite_objs_test_coalesce_dirty_ranges(
    const unsigned char *aDirty, int nBitmapBytes,
    const unsigned char *aData, sqlite3_int64 nData,
    int pageSize,
    azure_page_range_t *ranges, int maxRanges
);

TEST(coalesce_maxranges_overflow) {
    /*
    ** Set up a dirty bitmap where every other page is dirty.
    ** With 256 pages, that's 128 non-contiguous dirty runs.
    ** Request maxRanges=4, which should cause overflow → return -1.
    */
    int pageSize = 4096;
    int nPages = 256;
    sqlite3_int64 nData = (sqlite3_int64)nPages * pageSize;
    int nBitmapBytes = (nPages + 7) / 8;  /* 32 bytes */

    unsigned char aDirty[32];
    memset(aDirty, 0, sizeof(aDirty));

    /* Mark every other page dirty: pages 0, 2, 4, ... */
    for (int i = 0; i < nPages; i += 2) {
        aDirty[i / 8] |= (1 << (i % 8));
    }

    /* Allocate a data buffer (content doesn't matter for this test) */
    unsigned char *aData = (unsigned char *)calloc(1, (size_t)nData);
    ASSERT_NOT_NULL(aData);

    /* Attempt coalescing with maxRanges=4 — should overflow */
    azure_page_range_t ranges[4];
    int nRanges = sqlite_objs_test_coalesce_dirty_ranges(
        aDirty, nBitmapBytes, aData, nData, pageSize,
        ranges, 4
    );

    ASSERT_EQ(nRanges, -1);  /* Overflow: 128 runs > 4 maxRanges */

    free(aData);
}

#endif /* ENABLE_COALESCE_TESTS */


/* ══════════════════════════════════════════════════════════════════════
** Test 9: test_sync_coalesced_sequential
** Full round-trip: write pages, sync with coalescing, verify mock
** received correct ranges and data integrity is maintained.
** ══════════════════════════════════════════════════════════════════════ */

TEST(sync_coalesced_sequential) {
    co_setup();
    sqlite3 *db = co_open_db();
    ASSERT_NOT_NULL(db);

    /* Create table and insert data */
    ASSERT_OK(co_exec(db,
        "CREATE TABLE roundtrip(id INTEGER PRIMARY KEY, val TEXT, data BLOB);"));

    ASSERT_OK(co_exec(db, "BEGIN;"));
    for (int i = 0; i < 50; i++) {
        char sql[256];
        snprintf(sql, sizeof(sql),
                 "INSERT INTO roundtrip VALUES(%d, 'value_%d', zeroblob(2000));",
                 i, i);
        ASSERT_OK(co_exec(db, sql));
    }
    ASSERT_OK(co_exec(db, "COMMIT;"));

    /* Verify blob has data (sync worked) */
    const uint8_t *blobData = mock_get_page_blob_data(co_ctx, "coalesce_test.db");
    ASSERT_NOT_NULL(blobData);

    int64_t blobSize = mock_get_page_blob_size(co_ctx, "coalesce_test.db");
    ASSERT_GT(blobSize, 0);

    /* Now close and reopen to verify the data survived the sync */
    co_close_db(db);

    db = NULL;
    sqlite_objs_vfs_register_with_ops(co_ops, co_ctx, 0);
    int rc = sqlite3_open_v2("coalesce_test.db", &db,
                              SQLITE_OPEN_READWRITE, "sqlite-objs");
    ASSERT_OK(rc);
    ASSERT_NOT_NULL(db);

    /* Read back the data — must match what we wrote */
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM roundtrip;", -1, &stmt, NULL);
    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_ROW);
    int count = sqlite3_column_int(stmt, 0);
    ASSERT_EQ(count, 50);
    sqlite3_finalize(stmt);

    /* Spot-check a specific row */
    sqlite3_prepare_v2(db, "SELECT val FROM roundtrip WHERE id=25;",
                        -1, &stmt, NULL);
    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0), "value_25");
    sqlite3_finalize(stmt);

    co_close_db(db);
}


/* ══════════════════════════════════════════════════════════════════════
** Mock page_blob_write_batch with configurable partial failure
**
** Used by batch tests to simulate a batch write that:
**   - Succeeds for all ranges (fail_at_range < 0)
**   - Fails on a specific range index (0-based fail_at_range)
**
** When succeeding, it delegates each range to mock_page_blob_write
** so the data actually lands in the mock blob store.
** ══════════════════════════════════════════════════════════════════════ */

static int mock_batch_fail_at_range = -1;  /* -1 = succeed all */
static int mock_batch_call_count    = 0;
static int mock_batch_nranges_seen  = 0;   /* nRanges from last call */

static azure_err_t mock_page_blob_write_batch(
    void *ctx, const char *name,
    const azure_page_range_t *ranges, int nRanges,
    const char *lease_id, azure_error_t *err)
{
    mock_batch_call_count++;
    mock_batch_nranges_seen = nRanges;
    azure_error_init(err);

    for (int i = 0; i < nRanges; i++) {
        if (i == mock_batch_fail_at_range) {
            err->code = AZURE_ERR_IO;
            snprintf(err->error_message, sizeof(err->error_message),
                     "Injected batch failure at range %d", i);
            return AZURE_ERR_IO;
        }
        /* Delegate to real mock page_blob_write for data integrity */
        azure_err_t arc = co_ops->page_blob_write(
            ctx, name, ranges[i].offset, ranges[i].data,
            ranges[i].len, lease_id, err);
        if (arc != AZURE_OK) return arc;
    }
    return AZURE_OK;
}

/* Wire up/tear down the batch function on the mock vtable */
static void co_enable_batch(int fail_at) {
    mock_batch_fail_at_range = fail_at;
    mock_batch_call_count = 0;
    mock_batch_nranges_seen = 0;
    co_ops->page_blob_write_batch = mock_page_blob_write_batch;
}

static void co_disable_batch(void) {
    co_ops->page_blob_write_batch = NULL;
    mock_batch_fail_at_range = -1;
    mock_batch_call_count = 0;
    mock_batch_nranges_seen = 0;
}


/* ══════════════════════════════════════════════════════════════════════
** Test 10: test_sync_batch_null_fallback
** page_blob_write_batch=NULL → sequential fallback works
**
** The mock's page_blob_write_batch is already NULL. This test verifies
** that xSync correctly falls back to sequential page_blob_write calls
** when batch is unavailable.
** ══════════════════════════════════════════════════════════════════════ */

TEST(sync_batch_null_fallback) {
    co_setup();

    /* Confirm batch is NULL on the mock ops */
    ASSERT_NULL(co_ops->page_blob_write_batch);

    sqlite3 *db = co_open_db();
    ASSERT_NOT_NULL(db);

    ASSERT_OK(co_exec(db, "CREATE TABLE fallback(id INTEGER PRIMARY KEY, v TEXT);"));

    mock_clear_write_records(co_ctx);
    mock_reset_call_counts(co_ctx);

    /* Insert data — this will trigger xSync with batch=NULL */
    ASSERT_OK(co_exec(db, "BEGIN;"));
    for (int i = 0; i < 20; i++) {
        char sql[128];
        snprintf(sql, sizeof(sql),
                 "INSERT INTO fallback VALUES(%d, 'fallback_%d');", i, i);
        ASSERT_OK(co_exec(db, sql));
    }
    ASSERT_OK(co_exec(db, "COMMIT;"));

    /* Verify sequential writes occurred (one per dirty page) */
    int nWrites = mock_get_write_record_count(co_ctx);
    ASSERT_GT(nWrites, 0);

    /* Verify sequential page_blob_write was used (not batch) */
    ASSERT_GT(mock_get_call_count(co_ctx, "page_blob_write"), 0);

    /* All writes must be valid */
    assert_writes_aligned();
    assert_writes_within_4mb();
    assert_writes_ordered_nonoverlapping();

    /* Verify data integrity via read-back */
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM fallback;", -1, &stmt, NULL);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), 20);
    sqlite3_finalize(stmt);

    co_close_db(db);
}

/* ══════════════════════════════════════════════════════════════════════
** Test 11: test_sync_batch_partial_failure
** Batch write that fails on the 2nd range → xSync returns error
**
** Wires up mock_page_blob_write_batch to fail at range index 1.
** Creates enough dirty pages to produce multiple coalesced ranges,
** then verifies xSync propagates the batch error as SQLITE_IOERR.
** ══════════════════════════════════════════════════════════════════════ */

TEST(sync_batch_partial_failure) {
    co_setup();

    /* Phase 1: Populate DB with batch DISABLED (sequential fallback) */
    ASSERT_NULL(co_ops->page_blob_write_batch);

    sqlite3 *db = co_open_db();
    ASSERT_NOT_NULL(db);

    ASSERT_OK(co_exec(db,
        "CREATE TABLE bf(id INTEGER PRIMARY KEY, key TEXT UNIQUE, val BLOB);"));
    ASSERT_OK(co_exec(db, "BEGIN;"));
    for (int i = 0; i < 200; i++) {
        char sql[256];
        snprintf(sql, sizeof(sql),
                 "INSERT INTO bf VALUES(%d, 'bfkey%04d_padding_extra', zeroblob(1500));",
                 i, i);
        ASSERT_OK(co_exec(db, sql));
    }
    ASSERT_OK(co_exec(db, "COMMIT;"));

    /* Phase 2: Enable batch with failure on 2nd range (index 1).
    ** Scattered updates will dirty non-contiguous pages → 2+ coalesced ranges.
    ** If somehow only 1 range is produced, fail_at_range=1 won't fire and we
    ** fall through. So also set a fallback: if batch call count > 0 and rc==OK,
    ** that means 1 range only — re-run with fail_at_range=0. */
    co_enable_batch(1);
    mock_clear_write_records(co_ctx);
    mock_reset_call_counts(co_ctx);

    ASSERT_OK(co_exec(db, "BEGIN;"));
    for (int i = 0; i < 200; i += 10) {
        char sql[256];
        snprintf(sql, sizeof(sql),
                 "UPDATE bf SET val=zeroblob(1500) WHERE id=%d;", i);
        ASSERT_OK(co_exec(db, sql));
    }
    int rc = co_exec(db, "COMMIT;");

    if (rc == SQLITE_OK && mock_batch_nranges_seen <= 1) {
        /* Only 1 range was produced — retry with fail on first range */
        co_disable_batch();
        co_enable_batch(0);

        ASSERT_OK(co_exec(db, "BEGIN;"));
        for (int i = 1; i < 200; i += 10) {
            char sql[256];
            snprintf(sql, sizeof(sql),
                     "UPDATE bf SET val=zeroblob(1400) WHERE id=%d;", i);
            ASSERT_OK(co_exec(db, sql));
        }
        rc = co_exec(db, "COMMIT;");
    }

    /* xSync should have returned an error from the failed batch write */
    ASSERT_NE(rc, SQLITE_OK);
    ASSERT_GT(mock_batch_call_count, 0);

    co_close_db(db);
    co_disable_batch();
}


/* ══════════════════════════════════════════════════════════════════════
** Test 12: test_sync_batch_all_succeed
** Batch write succeeds for all ranges → xSync returns SQLITE_OK,
** dirty pages are cleared, and data survives close/reopen.
** ══════════════════════════════════════════════════════════════════════ */

TEST(sync_batch_all_succeed) {
    co_setup();
    co_enable_batch(-1);  /* succeed on all ranges */

    sqlite3 *db = co_open_db();
    ASSERT_NOT_NULL(db);

    ASSERT_OK(co_exec(db,
        "CREATE TABLE bs(id INTEGER PRIMARY KEY, val TEXT, data BLOB);"));

    mock_clear_write_records(co_ctx);
    mock_reset_call_counts(co_ctx);
    mock_batch_call_count = 0;

    /* Insert data — triggers xSync via batch path */
    ASSERT_OK(co_exec(db, "BEGIN;"));
    for (int i = 0; i < 30; i++) {
        char sql[256];
        snprintf(sql, sizeof(sql),
                 "INSERT INTO bs VALUES(%d, 'batch_val_%d', zeroblob(2000));",
                 i, i);
        ASSERT_OK(co_exec(db, sql));
    }
    ASSERT_OK(co_exec(db, "COMMIT;"));

    /* Batch must have been invoked */
    ASSERT_GT(mock_batch_call_count, 0);
    ASSERT_GT(mock_batch_nranges_seen, 0);

    /* Verify data landed in the mock blob store */
    const uint8_t *blobData = mock_get_page_blob_data(co_ctx, "coalesce_test.db");
    ASSERT_NOT_NULL(blobData);
    int64_t blobSize = mock_get_page_blob_size(co_ctx, "coalesce_test.db");
    ASSERT_GT(blobSize, 0);

    /* Close and reopen to verify data integrity through batch path */
    co_close_db(db);

    db = NULL;
    sqlite_objs_vfs_register_with_ops(co_ops, co_ctx, 0);
    int rc = sqlite3_open_v2("coalesce_test.db", &db,
                              SQLITE_OPEN_READWRITE, "sqlite-objs");
    ASSERT_OK(rc);
    ASSERT_NOT_NULL(db);

    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM bs;", -1, &stmt, NULL);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), 30);
    sqlite3_finalize(stmt);

    /* Spot-check a row */
    sqlite3_prepare_v2(db, "SELECT val FROM bs WHERE id=15;", -1, &stmt, NULL);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0), "batch_val_15");
    sqlite3_finalize(stmt);

    co_close_db(db);
    co_disable_batch();
}

#endif /* ENABLE_VFS_INTEGRATION */


/* ══════════════════════════════════════════════════════════════════════
** Test Suite Runner
** ══════════════════════════════════════════════════════════════════════ */

void run_coalesce_tests(void) {
#ifdef ENABLE_VFS_INTEGRATION

    TEST_SUITE_BEGIN("Page Coalescing — Empty/Single");
    RUN_TEST(coalesce_empty);
    RUN_TEST(coalesce_single);
    TEST_SUITE_END();

    TEST_SUITE_BEGIN("Page Coalescing — Contiguous Runs");
    RUN_TEST(coalesce_contiguous);
    RUN_TEST(coalesce_4mb_split);
    TEST_SUITE_END();

    TEST_SUITE_BEGIN("Page Coalescing — Scattered Patterns");
    RUN_TEST(coalesce_scattered);
    RUN_TEST(coalesce_every_other);
    TEST_SUITE_END();

    TEST_SUITE_BEGIN("Page Coalescing — Edge Cases");
    RUN_TEST(coalesce_last_page_short);
#ifdef ENABLE_COALESCE_TESTS
    RUN_TEST(coalesce_maxranges_overflow);
#endif
    TEST_SUITE_END();

    TEST_SUITE_BEGIN("Page Coalescing — Integration");
    RUN_TEST(sync_coalesced_sequential);
    RUN_TEST(sync_batch_null_fallback);
    TEST_SUITE_END();

    TEST_SUITE_BEGIN("Page Coalescing — Batch");
    RUN_TEST(sync_batch_partial_failure);
    RUN_TEST(sync_batch_all_succeed);
    TEST_SUITE_END();

#endif /* ENABLE_VFS_INTEGRATION */
}
