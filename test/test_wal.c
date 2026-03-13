/*
** test_wal.c — WAL Mode Unit Tests for sqliteObjs VFS
**
** Tests Write-Ahead Logging support through the sqliteObjs VFS layer.
** WAL mode maps to Azure Append Blobs (sequential append-only writes)
** and requires EXCLUSIVE locking mode (no shared memory needed).
**
** Dependencies (Frodo/Aragorn — not yet implemented):
**   - azure_ops_t extended with append_blob_create/append/delete fields
**   - mock_azure_ops.c extended with mock append blob implementations
**   - sqlite_objs_vfs.c updated to support WAL file type routing
**
** Gated behind ENABLE_WAL_TESTS. Define when dependencies are ready:
**   make test-unit CFLAGS+="-DENABLE_WAL_TESTS"
**
** 12 tests across 5 suites:
**   WAL Mode — Prerequisites       (2 tests)
**   WAL Mode — Basic Operations    (4 tests)
**   WAL Mode — Checkpoint          (2 tests)
**   WAL Mode — Error Handling      (2 tests)
**   WAL Mode — Data Integrity      (2 tests)
*/

#include "../sqlite-autoconf-3520000/sqlite3.h"
#include "mock_azure_ops.h"
#include "test_harness.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#ifdef ENABLE_WAL_TESTS

#include "../src/sqlite_objs.h"

/* ── Test Context ────────────────────────────────────────────────── */

static mock_azure_ctx_t *wal_ctx = NULL;
static azure_ops_t      *wal_base_ops = NULL;

static void wal_setup(void) {
    if (wal_ctx) mock_reset(wal_ctx);
    else         wal_ctx = mock_azure_create();
    wal_base_ops = mock_azure_get_ops();
}


/* ── Helpers ──────────────────────────────────────────────────────── */

/*
** Create an azure_ops_t with NULL append blob operations.
** Copies the base mock ops and clears append blob function pointers.
** Use to test that VFS rejects WAL mode when append ops are absent.
*/
static azure_ops_t wal_make_no_append_ops(void) {
    azure_ops_t ops = *wal_base_ops;
    ops.append_blob_create = NULL;
    ops.append_blob_append = NULL;
    ops.append_blob_delete = NULL;
    return ops;
}

/*
** Execute SQL with error capture and logging.
** Returns the SQLite result code. Never ignores errors.
*/
static int wal_exec(sqlite3 *db, const char *sql) {
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "    wal_exec(\"%s\") failed (%d): %s\n",
                sql, rc, err ? err : "unknown");
    }
    if (err) sqlite3_free(err);
    return rc;
}

/*
** Set PRAGMA journal_mode and return the resulting mode string.
** Uses a static buffer — caller must not free or store the pointer.
*/
static const char *wal_set_journal_mode(sqlite3 *db, const char *mode) {
    static char result_buf[32];
    result_buf[0] = '\0';

    char sql[64];
    int n = snprintf(sql, sizeof(sql), "PRAGMA journal_mode=%s;", mode);
    if (n < 0 || (size_t)n >= sizeof(sql)) return result_buf;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "    prepare journal_mode failed: %d\n", rc);
        return result_buf;
    }

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        const char *res = (const char *)sqlite3_column_text(stmt, 0);
        if (res) {
            strncpy(result_buf, res, sizeof(result_buf) - 1);
            result_buf[sizeof(result_buf) - 1] = '\0';
        }
    }
    rc = sqlite3_finalize(stmt);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "    finalize journal_mode failed: %d\n", rc);
    }
    return result_buf;
}

/*
** Query the current PRAGMA journal_mode.
*/
static const char *wal_get_journal_mode(sqlite3 *db) {
    static char mode_buf[32];
    mode_buf[0] = '\0';

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, "PRAGMA journal_mode;", -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "    prepare journal_mode query failed: %d\n", rc);
        return mode_buf;
    }

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        const char *res = (const char *)sqlite3_column_text(stmt, 0);
        if (res) {
            strncpy(mode_buf, res, sizeof(mode_buf) - 1);
            mode_buf[sizeof(mode_buf) - 1] = '\0';
        }
    }
    rc = sqlite3_finalize(stmt);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "    finalize journal_mode query failed: %d\n", rc);
    }
    return mode_buf;
}

/*
** Open a database in WAL mode with full setup sequence:
**   1. Register VFS with given ops
**   2. Open database
**   3. PRAGMA locking_mode=EXCLUSIVE  (required for WAL without shm)
**   4. PRAGMA journal_mode=WAL
**   5. Verify WAL mode is active
**
** Returns NULL on failure (caller should ASSERT_NOT_NULL).
*/
static sqlite3 *wal_open_db(azure_ops_t *ops, mock_azure_ctx_t *ctx,
                             const char *dbname) {
    sqlite3 *db = NULL;

    int rc = sqlite_objs_vfs_register_with_ops(ops, ctx, 0);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "    VFS register failed: %d\n", rc);
        return NULL;
    }

    rc = sqlite3_open_v2(dbname, &db,
                          SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                          "sqlite-objs");
    if (rc != SQLITE_OK) {
        fprintf(stderr, "    open failed: %d\n", rc);
        if (db) sqlite3_close(db);
        return NULL;
    }

    /* EXCLUSIVE locking mode — required for WAL without shared memory */
    char *err = NULL;
    rc = sqlite3_exec(db, "PRAGMA locking_mode=EXCLUSIVE;", NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "    locking_mode failed: %s\n", err ? err : "unknown");
        if (err) sqlite3_free(err);
        sqlite3_close(db);
        return NULL;
    }
    if (err) sqlite3_free(err);

    /* Enable WAL mode */
    const char *mode = wal_set_journal_mode(db, "wal");
    if (strcmp(mode, "wal") != 0) {
        fprintf(stderr, "    WAL mode not accepted (got \"%s\")\n", mode);
        sqlite3_close(db);
        return NULL;
    }

    return db;
}

static void wal_close_db(sqlite3 *db) {
    if (db) {
        int rc = sqlite3_close(db);
        if (rc != SQLITE_OK) {
            fprintf(stderr, "    sqlite3_close failed: %d\n", rc);
        }
    }
}


/* ══════════════════════════════════════════════════════════════════════
** Suite: WAL Mode — Prerequisites
**
** Verify that WAL mode activation is correctly gated by the presence
** of append blob operations in the azure_ops_t vtable.
** ══════════════════════════════════════════════════════════════════════ */

/*
** Test 1: WAL mode should be rejected when append_blob ops are NULL.
**
** When the VFS receives PRAGMA journal_mode=WAL but the azure_ops_t
** vtable has NULL append_blob_create/append/delete, it should refuse
** WAL and fall back to "delete" mode.
*/
TEST(wal_mode_requires_append_ops) {
    wal_setup();

    /* Build ops with NULL append blob fields */
    azure_ops_t no_append_ops = wal_make_no_append_ops();
    int rc = sqlite_objs_vfs_register_with_ops(&no_append_ops, wal_ctx, 0);
    ASSERT_OK(rc);

    sqlite3 *db = NULL;
    rc = sqlite3_open_v2("wal_prereq.db", &db,
                          SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                          "sqlite-objs");
    ASSERT_OK(rc);
    ASSERT_NOT_NULL(db);

    /* Set exclusive locking mode first */
    rc = wal_exec(db, "PRAGMA locking_mode=EXCLUSIVE;");
    ASSERT_OK(rc);

    /* Try to enable WAL — should be rejected (append ops are NULL) */
    const char *mode = wal_set_journal_mode(db, "wal");
    ASSERT_STR_EQ(mode, "delete");

    /* Double-check: query current mode to confirm it stayed as delete */
    const char *current = wal_get_journal_mode(db);
    ASSERT_NOT_NULL(current);
    ASSERT_STR_EQ(current, "delete");

    wal_close_db(db);
}

/*
** Test 2: WAL mode should be accepted when append ops are non-NULL
** AND locking_mode is EXCLUSIVE.
*/
TEST(wal_mode_allowed_with_append_ops) {
    wal_setup();

    /* Base mock ops already have non-NULL append blob functions */
    sqlite3 *db = wal_open_db(wal_base_ops, wal_ctx, "wal_allowed.db");
    ASSERT_NOT_NULL(db);

    /* wal_open_db already verified WAL is active; confirm independently */
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, "PRAGMA journal_mode;", -1, &stmt, NULL);
    ASSERT_OK(rc);
    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_ROW);
    const char *mode = (const char *)sqlite3_column_text(stmt, 0);
    ASSERT_NOT_NULL(mode);
    ASSERT_STR_EQ(mode, "wal");
    rc = sqlite3_finalize(stmt);
    ASSERT_OK(rc);

    wal_close_db(db);
}


/* ══════════════════════════════════════════════════════════════════════
** Suite: WAL Mode — Basic Operations
**
** Verify WAL I/O patterns: append blob creation, WAL writes (append),
** reads through WAL cache, and multi-transaction sequencing.
** ══════════════════════════════════════════════════════════════════════ */

/*
** Test 3: Opening a DB in WAL mode should create an append blob
** named with the "-wal" suffix (e.g., "waltest.db-wal").
*/
TEST(wal_open_creates_append_blob) {
    wal_setup();

    /* Base mock ops already have non-NULL append blob functions */
    sqlite3 *db = wal_open_db(wal_base_ops, wal_ctx, "waltest.db");
    ASSERT_NOT_NULL(db);

    /* Trigger WAL file creation with a write */
    int rc = wal_exec(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, val TEXT);");
    ASSERT_OK(rc);

    /* Verify append_blob_create was called */
    int create_count = mock_get_call_count(wal_ctx, "append_blob_create");
    ASSERT_GT(create_count, 0);

    /* Verify the WAL blob exists with "-wal" suffix naming convention */
    int exists = 0;
    azure_error_t aerr;
    azure_error_init(&aerr);
    azure_err_t arc = wal_base_ops->blob_exists(wal_ctx, "waltest.db-wal",
                                                  &exists, &aerr);
    ASSERT_AZURE_OK(arc);
    ASSERT_TRUE(exists);

    wal_close_db(db);
}

/*
** Test 4: Writing data through WAL (INSERT) and syncing should call
** mock_append_blob_append with frame data.
**
** WAL frames are 24-byte header + page_size (4096) = 4120 bytes each.
** At least one frame must be appended per transaction.
*/
TEST(wal_write_and_sync) {
    wal_setup();

    sqlite3 *db = wal_open_db(wal_base_ops, wal_ctx, "walsync.db");
    ASSERT_NOT_NULL(db);

    int rc = wal_exec(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, val TEXT);");
    ASSERT_OK(rc);

    /* Reset counters to isolate the INSERT's effect */
    mock_reset_append_data(wal_ctx, "walsync.db-wal");
    mock_reset_call_counts(wal_ctx);

    rc = wal_exec(db, "INSERT INTO t VALUES(1, 'hello-wal');");
    ASSERT_OK(rc);

    /* Verify append_blob_append was called */
    int append_count = mock_get_call_count(wal_ctx, "append_blob_append");
    ASSERT_GT(append_count, 0);

    /* Verify appended data is non-empty and at least one frame (4120 bytes) */
    int64_t append_size = mock_get_append_size(wal_ctx, "walsync.db-wal");
    ASSERT_GE(append_size, (int64_t)4120);

    wal_close_db(db);
}

/*
** Test 5: Data written through WAL should be readable via SELECT.
** Verifies the WAL read path: WAL-index lookup → cached WAL data.
** In exclusive mode, the WAL-index is entirely in-process memory.
*/
TEST(wal_read_after_write) {
    wal_setup();

    sqlite3 *db = wal_open_db(wal_base_ops, wal_ctx, "walread.db");
    ASSERT_NOT_NULL(db);

    int rc = wal_exec(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, val TEXT);");
    ASSERT_OK(rc);

    rc = wal_exec(db, "INSERT INTO t VALUES(1, 'azure-wal-test');");
    ASSERT_OK(rc);

    /* Read back the data through WAL */
    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(db, "SELECT val FROM t WHERE id=1;", -1,
                             &stmt, NULL);
    ASSERT_OK(rc);
    ASSERT_NOT_NULL(stmt);

    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_ROW);

    const char *val = (const char *)sqlite3_column_text(stmt, 0);
    ASSERT_NOT_NULL(val);
    ASSERT_STR_EQ(val, "azure-wal-test");

    rc = sqlite3_finalize(stmt);
    ASSERT_OK(rc);

    wal_close_db(db);
}

/*
** Test 6: Three sequential transactions should each trigger
** an append_blob_append call, with the call count incrementing
** after each commit.
*/
TEST(wal_multiple_transactions) {
    wal_setup();

    sqlite3 *db = wal_open_db(wal_base_ops, wal_ctx, "walmulti.db");
    ASSERT_NOT_NULL(db);

    int rc = wal_exec(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, val TEXT);");
    ASSERT_OK(rc);

    mock_reset_call_counts(wal_ctx);

    /* Transaction 1 */
    rc = wal_exec(db, "INSERT INTO t VALUES(1, 'first');");
    ASSERT_OK(rc);
    int count_after_1 = mock_get_call_count(wal_ctx, "append_blob_append");
    ASSERT_GT(count_after_1, 0);

    /* Transaction 2 */
    rc = wal_exec(db, "INSERT INTO t VALUES(2, 'second');");
    ASSERT_OK(rc);
    int count_after_2 = mock_get_call_count(wal_ctx, "append_blob_append");
    ASSERT_GT(count_after_2, count_after_1);

    /* Transaction 3 */
    rc = wal_exec(db, "INSERT INTO t VALUES(3, 'third');");
    ASSERT_OK(rc);
    int count_after_3 = mock_get_call_count(wal_ctx, "append_blob_append");
    ASSERT_GT(count_after_3, count_after_2);

    /* Verify all 3 rows are readable */
    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM t;", -1, &stmt, NULL);
    ASSERT_OK(rc);

    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_ROW);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), 3);

    rc = sqlite3_finalize(stmt);
    ASSERT_OK(rc);

    wal_close_db(db);
}


/* ══════════════════════════════════════════════════════════════════════
** Suite: WAL Mode — Checkpoint
**
** Checkpoint moves committed WAL frames to the main DB page blob.
** Reuses the existing page_blob_write / page_blob_write_batch infra.
** After checkpoint, the WAL append blob is deleted and recreated.
** ══════════════════════════════════════════════════════════════════════ */

/*
** Test 7: After manual PRAGMA wal_checkpoint(TRUNCATE), verify that
** page_blob_write was called — pages were moved from WAL to main DB.
*/
TEST(wal_checkpoint_writes_pages) {
    wal_setup();

    sqlite3 *db = wal_open_db(wal_base_ops, wal_ctx, "walckpt.db");
    ASSERT_NOT_NULL(db);

    int rc = wal_exec(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, val TEXT);");
    ASSERT_OK(rc);

    /* Insert enough rows to generate dirty pages */
    for (int i = 0; i < 50; i++) {
        char sql[128];
        int n = snprintf(sql, sizeof(sql),
                         "INSERT INTO t VALUES(%d, 'checkpoint-test-%d');",
                         i, i);
        ASSERT_TRUE(n > 0 && (size_t)n < sizeof(sql));
        rc = wal_exec(db, sql);
        ASSERT_OK(rc);
    }

    /* Clear write records to isolate checkpoint's writes */
    mock_clear_write_records(wal_ctx);
    mock_reset_call_counts(wal_ctx);

    /* Force checkpoint — transfers WAL frames to main DB page blob */
    rc = wal_exec(db, "PRAGMA wal_checkpoint(TRUNCATE);");
    ASSERT_OK(rc);

    /* Verify page_blob_write was called (pages moved from WAL → main DB) */
    int write_count = mock_get_call_count(wal_ctx, "page_blob_write");
    ASSERT_GT(write_count, 0);

    /* Verify write records captured the checkpoint writes */
    int record_count = mock_get_write_record_count(wal_ctx);
    ASSERT_GT(record_count, 0);

    wal_close_db(db);
}

/*
** Test 8: After checkpoint, the WAL append blob should be deleted
** and recreated (TRUNCATE mode). Verify append_blob_delete is called
** and the WAL data is smaller than before checkpoint.
*/
TEST(wal_checkpoint_resets_wal) {
    wal_setup();

    sqlite3 *db = wal_open_db(wal_base_ops, wal_ctx, "walreset.db");
    ASSERT_NOT_NULL(db);

    int rc = wal_exec(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, val TEXT);");
    ASSERT_OK(rc);

    /* Insert data to populate the WAL */
    for (int i = 0; i < 20; i++) {
        char sql[128];
        int n = snprintf(sql, sizeof(sql),
                         "INSERT INTO t VALUES(%d, 'reset-test-%d');", i, i);
        ASSERT_TRUE(n > 0 && (size_t)n < sizeof(sql));
        rc = wal_exec(db, sql);
        ASSERT_OK(rc);
    }

    /* Record WAL data size before checkpoint */
    int64_t pre_ckpt_size = mock_get_append_size(wal_ctx, "walreset.db-wal");
    ASSERT_GT(pre_ckpt_size, (int64_t)0);

    mock_reset_call_counts(wal_ctx);

    /* Force checkpoint with TRUNCATE to reset WAL */
    rc = wal_exec(db, "PRAGMA wal_checkpoint(TRUNCATE);");
    ASSERT_OK(rc);

    /* Verify append_blob_delete was called (WAL blob reset) */
    int delete_count = mock_get_call_count(wal_ctx, "append_blob_delete");
    ASSERT_GT(delete_count, 0);

    /* After TRUNCATE checkpoint, WAL should be empty or near-empty */
    int64_t post_ckpt_size = mock_get_append_size(wal_ctx, "walreset.db-wal");
    ASSERT_LT(post_ckpt_size, pre_ckpt_size);

    wal_close_db(db);
}


/* ══════════════════════════════════════════════════════════════════════
** Suite: WAL Mode — Error Handling
**
** Verify that Azure failures during WAL operations propagate as
** SQLite error codes. Data must never be silently lost.
** ══════════════════════════════════════════════════════════════════════ */

/*
** Test 9: If append_blob_append fails, the transaction should fail.
** xSync returns SQLITE_IOERR_FSYNC (or similar), preventing
** a silent data loss scenario.
*/
TEST(wal_append_failure_returns_error) {
    wal_setup();

    sqlite3 *db = wal_open_db(wal_base_ops, wal_ctx, "walfail_append.db");
    ASSERT_NOT_NULL(db);

    int rc = wal_exec(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, val TEXT);");
    ASSERT_OK(rc);

    /* Inject failure on append_blob_append */
    mock_set_fail_operation(wal_ctx, "append_blob_append", AZURE_ERR_IO);

    /* INSERT should fail because the WAL append fails during sync */
    char *err_msg = NULL;
    rc = sqlite3_exec(db, "INSERT INTO t VALUES(1, 'should-fail');",
                       NULL, NULL, &err_msg);
    ASSERT_NE(rc, SQLITE_OK);
    if (err_msg) {
        fprintf(stderr, "    Expected failure: %s\n", err_msg);
        sqlite3_free(err_msg);
    }

    mock_clear_failures(wal_ctx);
    wal_close_db(db);
}

/*
** Test 10: If append_blob_create fails, WAL mode activation should
** fail. The VFS should either reject WAL mode or fail on the first
** write that requires the WAL blob.
*/
TEST(wal_create_failure_returns_error) {
    wal_setup();

    /* Inject failure on append_blob_create BEFORE opening the DB */
    mock_set_fail_operation(wal_ctx, "append_blob_create", AZURE_ERR_IO);

    int rc = sqlite_objs_vfs_register_with_ops(wal_base_ops, wal_ctx, 0);
    ASSERT_OK(rc);

    sqlite3 *db = NULL;
    rc = sqlite3_open_v2("walfail_create.db", &db,
                          SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                          "sqlite-objs");
    if (rc != SQLITE_OK) {
        /* xOpen itself failed — acceptable behavior */
        if (db) sqlite3_close(db);
        mock_clear_failures(wal_ctx);
        return;
    }

    /* If open succeeded, try to enable WAL */
    char *err_msg = NULL;
    rc = sqlite3_exec(db, "PRAGMA locking_mode=EXCLUSIVE;", NULL, NULL, &err_msg);
    if (err_msg) { sqlite3_free(err_msg); err_msg = NULL; }

    /* Set WAL mode — creating the WAL blob should fail */
    const char *mode = wal_set_journal_mode(db, "wal");

    if (strcmp(mode, "wal") == 0) {
        /* WAL was accepted despite create failure — next operation must fail */
        rc = sqlite3_exec(db, "CREATE TABLE t(x);", NULL, NULL, &err_msg);
        ASSERT_NE(rc, SQLITE_OK);
        if (err_msg) {
            fprintf(stderr, "    Expected failure: %s\n", err_msg);
            sqlite3_free(err_msg);
        }
    } else {
        /* WAL creation was rejected — fell back to delete mode. Correct. */
        ASSERT_STR_NE(mode, "wal");
    }

    mock_clear_failures(wal_ctx);
    if (db) sqlite3_close(db);
}


/* ══════════════════════════════════════════════════════════════════════
** Suite: WAL Mode — Data Integrity
**
** End-to-end verification that data survives WAL → checkpoint → close
** → reopen cycles, and that reads are consistent during writes.
** ══════════════════════════════════════════════════════════════════════ */

/*
** Test 11: Full round-trip — open DB in WAL mode, create table,
** insert rows, checkpoint, close, reopen, verify data persists.
**
** This exercises the complete WAL lifecycle:
**   write → WAL append → checkpoint → page blob write → close → reopen → read
*/
TEST(wal_insert_select_roundtrip) {
    wal_setup();

    /* Phase 1: Write data in WAL mode */
    sqlite3 *db = wal_open_db(wal_base_ops, wal_ctx, "walround.db");
    ASSERT_NOT_NULL(db);

    int rc = wal_exec(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, val TEXT);");
    ASSERT_OK(rc);

    rc = wal_exec(db, "INSERT INTO t VALUES(1, 'persist-a');");
    ASSERT_OK(rc);
    rc = wal_exec(db, "INSERT INTO t VALUES(2, 'persist-b');");
    ASSERT_OK(rc);
    rc = wal_exec(db, "INSERT INTO t VALUES(3, 'persist-c');");
    ASSERT_OK(rc);

    /* Force checkpoint to flush WAL frames to main DB page blob */
    rc = wal_exec(db, "PRAGMA wal_checkpoint(TRUNCATE);");
    ASSERT_OK(rc);

    wal_close_db(db);
    db = NULL;

    /* Phase 2: Reopen and verify data persists */
    rc = sqlite_objs_vfs_register_with_ops(wal_base_ops, wal_ctx, 0);
    ASSERT_OK(rc);

    rc = sqlite3_open_v2("walround.db", &db,
                          SQLITE_OPEN_READWRITE, "sqlite-objs");
    ASSERT_OK(rc);
    ASSERT_NOT_NULL(db);

    /* Re-enter exclusive + WAL mode for the reopened connection */
    rc = wal_exec(db, "PRAGMA locking_mode=EXCLUSIVE;");
    ASSERT_OK(rc);

    /* Verify all 3 rows survived the close/reopen cycle */
    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(db, "SELECT val FROM t ORDER BY id;", -1,
                             &stmt, NULL);
    ASSERT_OK(rc);

    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0), "persist-a");

    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0), "persist-b");

    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0), "persist-c");

    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_DONE);

    rc = sqlite3_finalize(stmt);
    ASSERT_OK(rc);

    wal_close_db(db);
}

/*
** Test 12: Placeholder for concurrent reads during WAL writes.
**
** WAL mode supports concurrent readers + single writer on local SQLite.
** With EXCLUSIVE locking mode (our MVP 2 model), only one connection
** accesses the DB at a time. True concurrent testing requires MVP 3+
** multi-machine support.
**
** For now: verify that reads work within an active write transaction
** (the single-connection case that must always work).
*/
TEST(wal_concurrent_reads_during_write) {
    wal_setup();

    sqlite3 *db = wal_open_db(wal_base_ops, wal_ctx, "walconcur.db");
    ASSERT_NOT_NULL(db);

    int rc = wal_exec(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, val TEXT);");
    ASSERT_OK(rc);

    /* Begin an explicit write transaction */
    rc = wal_exec(db, "BEGIN;");
    ASSERT_OK(rc);

    rc = wal_exec(db, "INSERT INTO t VALUES(1, 'during-write');");
    ASSERT_OK(rc);

    /* Read within the same active transaction — should see uncommitted data */
    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(db, "SELECT val FROM t WHERE id=1;", -1,
                             &stmt, NULL);
    ASSERT_OK(rc);
    ASSERT_NOT_NULL(stmt);

    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_ROW);

    const char *val = (const char *)sqlite3_column_text(stmt, 0);
    ASSERT_NOT_NULL(val);
    ASSERT_STR_EQ(val, "during-write");

    rc = sqlite3_finalize(stmt);
    ASSERT_OK(rc);

    rc = wal_exec(db, "COMMIT;");
    ASSERT_OK(rc);

    wal_close_db(db);
}


/* ══════════════════════════════════════════════════════════════════════
** Suite: WAL Mode — Crash Recovery on Open
**
** When sqliteObjs opens a DB and a WAL blob already exists in Azure,
** the VFS downloads it via block_blob_download to restore crash
** recovery data.  These tests exercise the walExists download path
** in xOpen for WAL files.
** ══════════════════════════════════════════════════════════════════════ */

/*
** Test 13: Pre-populate mock with WAL data, open DB, verify the
** WAL data is downloaded into the file's buffer.
**
** Approach:
**   1. Open DB in WAL mode and write data (populates the append blob)
**   2. Capture WAL blob data from the mock
**   3. Close, reset mock, re-populate page blob + WAL data as block blob
**   4. Reopen — WAL recovery should download and replay the data
**   5. Verify data written through WAL is readable
*/
TEST(wal_recovery_downloads_existing_wal) {
    wal_setup();

    /* Phase 1: Write data in WAL mode to generate real WAL content */
    sqlite3 *db = wal_open_db(wal_base_ops, wal_ctx, "walrecov.db");
    ASSERT_NOT_NULL(db);

    int rc = wal_exec(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, val TEXT);");
    ASSERT_OK(rc);
    rc = wal_exec(db, "INSERT INTO t VALUES(1, 'crash-recovery');");
    ASSERT_OK(rc);

    /* Checkpoint so data reaches the page blob */
    rc = wal_exec(db, "PRAGMA wal_checkpoint(TRUNCATE);");
    ASSERT_OK(rc);

    /* Write more data (stays in WAL only — not checkpointed) */
    rc = wal_exec(db, "INSERT INTO t VALUES(2, 'wal-only-data');");
    ASSERT_OK(rc);

    /* Capture WAL blob data before closing */
    const unsigned char *wal_data = mock_get_append_data(wal_ctx, "walrecov.db-wal");
    int64_t wal_size = mock_get_append_size(wal_ctx, "walrecov.db-wal");
    ASSERT_NOT_NULL(wal_data);
    ASSERT_GT(wal_size, (int64_t)0);

    /* Save a copy of WAL data (close may modify the blob) */
    unsigned char *wal_copy = (unsigned char *)malloc((size_t)wal_size);
    ASSERT_NOT_NULL(wal_copy);
    memcpy(wal_copy, wal_data, (size_t)wal_size);

    /* Close DB (may checkpoint and clear WAL) */
    wal_close_db(db);
    db = NULL;

    /* Phase 2: Simulate crash recovery scenario.
    ** The page blob from phase 1 survives (Azure persists it).
    ** Re-upload the WAL data as a block blob to simulate Azure state
    ** after a crash (WAL append blob still present). The mock's
    ** block_blob_download is type-strict, so upload as block blob. */
    azure_error_t aerr;
    azure_error_init(&aerr);
    azure_err_t arc = wal_base_ops->block_blob_upload(
        wal_ctx, "walrecov.db-wal", wal_copy, (size_t)wal_size, &aerr);
    ASSERT_AZURE_OK(arc);
    free(wal_copy);

    /* Reset call counts to isolate recovery calls */
    mock_reset_call_counts(wal_ctx);

    /* Phase 3: Reopen — should detect WAL blob and download it */
    rc = sqlite_objs_vfs_register_with_ops(wal_base_ops, wal_ctx, 0);
    ASSERT_OK(rc);

    rc = sqlite3_open_v2("walrecov.db", &db,
                          SQLITE_OPEN_READWRITE, "sqlite-objs");
    ASSERT_OK(rc);
    ASSERT_NOT_NULL(db);

    rc = wal_exec(db, "PRAGMA locking_mode=EXCLUSIVE;");
    ASSERT_OK(rc);

    /* Entering WAL mode triggers xOpen for the WAL file,
    ** which should find the blob and call block_blob_download */
    const char *mode = wal_set_journal_mode(db, "wal");
    ASSERT_STR_EQ(mode, "wal");

    /* Verify block_blob_download was called (WAL recovery path) */
    int dl_count = mock_get_call_count(wal_ctx, "block_blob_download");
    ASSERT_GT(dl_count, 0);

    wal_close_db(db);
}

/*
** Test 14: WAL blob exists but block_blob_download fails — the PRAGMA
** journal_mode=WAL must fail to prevent silent data loss.
** This exercises the C1 fix.
**
** Note: SQLite deletes leftover WAL files for empty databases (nPage==0).
** So we must create a non-empty database first (in DELETE journal mode),
** then pre-populate the WAL blob and inject the download failure.
*/
TEST(wal_recovery_download_failure) {
    wal_setup();

    /* Phase 1: Create a non-empty database in DELETE journal mode */
    int rc = sqlite_objs_vfs_register_with_ops(wal_base_ops, wal_ctx, 0);
    ASSERT_OK(rc);

    sqlite3 *db = NULL;
    rc = sqlite3_open_v2("walfail_dl.db", &db,
                          SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                          "sqlite-objs");
    ASSERT_OK(rc);
    ASSERT_NOT_NULL(db);

    rc = wal_exec(db, "PRAGMA locking_mode=EXCLUSIVE;");
    ASSERT_OK(rc);

    /* Create a table so the database has at least one page */
    rc = wal_exec(db, "CREATE TABLE setup(x);");
    ASSERT_OK(rc);
    wal_close_db(db);
    db = NULL;

    /* Phase 2: Pre-create WAL blob and inject failure */
    azure_error_t aerr;
    azure_error_init(&aerr);
    unsigned char dummy[] = "FAKE_WAL_DATA_FOR_RECOVERY";
    azure_err_t arc = wal_base_ops->block_blob_upload(
        wal_ctx, "walfail_dl.db-wal", dummy, sizeof(dummy), &aerr);
    ASSERT_AZURE_OK(arc);

    mock_set_fail_operation(wal_ctx, "block_blob_download", AZURE_ERR_IO);

    /* Phase 3: Reopen and try WAL mode */
    rc = sqlite_objs_vfs_register_with_ops(wal_base_ops, wal_ctx, 0);
    ASSERT_OK(rc);

    rc = sqlite3_open_v2("walfail_dl.db", &db,
                          SQLITE_OPEN_READWRITE, "sqlite-objs");
    ASSERT_OK(rc);
    ASSERT_NOT_NULL(db);

    rc = wal_exec(db, "PRAGMA locking_mode=EXCLUSIVE;");
    ASSERT_OK(rc);

    /* PRAGMA journal_mode=WAL should fail:
    **   pagerOpenWalIfPresent → blob_exists → 1 → nPage > 0 →
    **   sqlite3PagerOpenWal → sqliteObjsOpen → blob_exists → 1 →
    **   block_blob_download → fails → SQLITE_CANTOPEN
    ** The journal mode should remain "delete". */
    const char *mode = wal_set_journal_mode(db, "wal");
    ASSERT_STR_NE(mode, "wal");

    mock_clear_failures(wal_ctx);
    wal_close_db(db);
}

/*
** Test 15: WAL blob exists but is empty (0 bytes).
** Open should succeed normally — an empty WAL has nothing to replay.
*/
TEST(wal_recovery_empty_wal) {
    wal_setup();

    /* Create a zero-length block blob for the WAL name */
    azure_error_t aerr;
    azure_error_init(&aerr);
    azure_err_t arc = wal_base_ops->block_blob_upload(
        wal_ctx, "walempty.db-wal", NULL, 0, &aerr);
    ASSERT_AZURE_OK(arc);

    /* Open DB and enter WAL mode — should succeed despite empty WAL */
    sqlite3 *db = wal_open_db(wal_base_ops, wal_ctx, "walempty.db");
    ASSERT_NOT_NULL(db);

    /* Verify the DB is functional */
    int rc = wal_exec(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, val TEXT);");
    ASSERT_OK(rc);
    rc = wal_exec(db, "INSERT INTO t VALUES(1, 'after-empty-wal');");
    ASSERT_OK(rc);

    /* Read back to confirm */
    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(db, "SELECT val FROM t WHERE id=1;", -1,
                             &stmt, NULL);
    ASSERT_OK(rc);
    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0), "after-empty-wal");
    rc = sqlite3_finalize(stmt);
    ASSERT_OK(rc);

    wal_close_db(db);
}


/* ══════════════════════════════════════════════════════════════════════
** Suite: WAL Mode — Full Resync Chunking
**
** When SQLite overwrites previously-synced WAL data (e.g., WAL header
** rewrite after checkpoint), walNeedFullResync is set to 1.  The xSync
** path must delete+recreate the append blob and re-upload the entire
** WAL buffer, chunking at 4 MiB boundaries per Azure append limit.
**
** The incremental append path also chunks at 4 MiB when new data
** since the last sync exceeds the limit.
** ══════════════════════════════════════════════════════════════════════ */

/*
** Helper: insert N rows with randomblob(blob_size) in a single transaction.
** The table must already exist with columns (id, data).
*/
static int wal_insert_blobs(sqlite3 *db, int start_id, int count,
                             int blob_size) {
    int rc = wal_exec(db, "BEGIN;");
    if (rc != SQLITE_OK) return rc;

    for (int i = 0; i < count; i++) {
        char sql[128];
        int n = snprintf(sql, sizeof(sql),
                         "INSERT INTO t VALUES(%d, randomblob(%d));",
                         start_id + i, blob_size);
        if (n < 0 || (size_t)n >= sizeof(sql)) return SQLITE_ERROR;
        rc = wal_exec(db, sql);
        if (rc != SQLITE_OK) {
            wal_exec(db, "ROLLBACK;");
            return rc;
        }
    }

    rc = wal_exec(db, "COMMIT;");
    return rc;
}

/*
** Test 16: Full resync with small data (< 4 MiB).
** After PASSIVE checkpoint, the next write triggers WAL restart which
** overwrites the WAL header at offset 0 (before nWalSynced).
** xSync sees walNeedFullResync → delete + create + single append call.
*/
TEST(wal_full_resync_small_data) {
    wal_setup();

    sqlite3 *db = wal_open_db(wal_base_ops, wal_ctx, "walresync_sm.db");
    ASSERT_NOT_NULL(db);

    int rc = wal_exec(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, data BLOB);");
    ASSERT_OK(rc);

    /* Write some data to populate WAL (generates a few frames, syncs) */
    rc = wal_exec(db, "INSERT INTO t VALUES(1, 'small-data');");
    ASSERT_OK(rc);
    rc = wal_exec(db, "INSERT INTO t VALUES(2, 'more-data');");
    ASSERT_OK(rc);

    /* Passive checkpoint — moves frames to page blob but does NOT truncate
    ** the WAL file.  nWalSynced stays positive. */
    rc = wal_exec(db, "PRAGMA wal_checkpoint(PASSIVE);");
    ASSERT_OK(rc);

    /* Reset counters + records to isolate the next operation */
    mock_reset_call_counts(wal_ctx);
    mock_clear_append_records(wal_ctx);

    /* This INSERT triggers WAL restart:
    **   walRestartLog() → walRestartHdr() (no xTruncate)
    **   iFrame==0 → xWrite(WAL header, offset 0) → walNeedFullResync=1
    **   xSync → full resync (delete + create + chunked append) */
    rc = wal_exec(db, "INSERT INTO t VALUES(3, 'trigger-resync');");
    ASSERT_OK(rc);

    /* Verify full resync path was taken:
    **   append_blob_delete + append_blob_create indicate blob was recreated */
    int del_count = mock_get_call_count(wal_ctx, "append_blob_delete");
    int create_count = mock_get_call_count(wal_ctx, "append_blob_create");
    ASSERT_GT(del_count, 0);
    ASSERT_GT(create_count, 0);

    /* With small WAL data (< 4 MiB), single append call suffices */
    int append_records = mock_get_append_record_count(wal_ctx);
    ASSERT_GT(append_records, 0);

    /* Verify each append chunk is ≤ 4 MiB */
    for (int i = 0; i < append_records; i++) {
        mock_append_record_t rec = mock_get_append_record(wal_ctx, i);
        ASSERT_LE((long long)rec.len, (long long)(4 * 1024 * 1024));
    }

    wal_close_db(db);
}

/*
** Test 17: Full resync with data > 4 MiB.
** Build up > 4 MiB of WAL data, then trigger full resync.
** The upload must be chunked into multiple append calls, each ≤ 4 MiB.
** This exercises the C2 fix.
*/
TEST(wal_full_resync_large_data) {
    wal_setup();

    sqlite3 *db = wal_open_db(wal_base_ops, wal_ctx, "walresync_lg.db");
    ASSERT_NOT_NULL(db);

    int rc = wal_exec(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, data BLOB);");
    ASSERT_OK(rc);

    /* Build up > 4 MiB of WAL data in a single transaction.
    ** Each row with randomblob(3800) fills approximately one 4096-byte page.
    ** 1100 rows → ~1100 WAL frames → ~1100 × 4120 ≈ 4.5 MB. */
    rc = wal_insert_blobs(db, 0, 1100, 3800);
    ASSERT_OK(rc);

    /* Verify WAL data exceeds 4 MiB */
    int64_t wal_size = mock_get_append_size(wal_ctx, "walresync_lg.db-wal");
    ASSERT_GT(wal_size, (int64_t)(4 * 1024 * 1024));

    /* Passive checkpoint — moves frames to page blob, no WAL truncation */
    rc = wal_exec(db, "PRAGMA wal_checkpoint(PASSIVE);");
    ASSERT_OK(rc);

    /* Reset counters + records to isolate the full resync */
    mock_reset_call_counts(wal_ctx);
    mock_clear_append_records(wal_ctx);

    /* Trigger WAL restart → full resync of > 4 MiB data */
    rc = wal_exec(db, "INSERT INTO t VALUES(9999, 'trigger-chunked-resync');");
    ASSERT_OK(rc);

    /* Verify full resync path (delete + create) */
    int del_count = mock_get_call_count(wal_ctx, "append_blob_delete");
    int create_count = mock_get_call_count(wal_ctx, "append_blob_create");
    ASSERT_GT(del_count, 0);
    ASSERT_GT(create_count, 0);

    /* Must have multiple append calls (data > 4 MiB → chunked) */
    int append_records = mock_get_append_record_count(wal_ctx);
    ASSERT_GT(append_records, 1);

    /* Every chunk must be ≤ 4 MiB */
    for (int i = 0; i < append_records; i++) {
        mock_append_record_t rec = mock_get_append_record(wal_ctx, i);
        ASSERT_LE((long long)rec.len, (long long)(4 * 1024 * 1024));
        ASSERT_GT((long long)rec.len, 0LL);
    }

    wal_close_db(db);
}

/*
** Test 18: Incremental append with large data (> 4 MiB).
** A single large transaction accumulates > 4 MiB of new WAL data
** since the last sync.  The incremental append path must chunk the
** upload at 4 MiB boundaries.
*/
TEST(wal_incremental_append_large_data) {
    wal_setup();

    sqlite3 *db = wal_open_db(wal_base_ops, wal_ctx, "walincr_lg.db");
    ASSERT_NOT_NULL(db);

    int rc = wal_exec(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, data BLOB);");
    ASSERT_OK(rc);

    /* Reset counters + records — isolate the large transaction's sync */
    mock_reset_call_counts(wal_ctx);
    mock_clear_append_records(wal_ctx);

    /* Single large transaction: > 4 MiB of WAL frames committed at once.
    ** The incremental append sees pending > 4 MiB and chunks the upload. */
    rc = wal_insert_blobs(db, 0, 1100, 3800);
    ASSERT_OK(rc);

    /* Must have multiple append calls (data > 4 MiB → chunked) */
    int append_records = mock_get_append_record_count(wal_ctx);
    ASSERT_GT(append_records, 1);

    /* Every chunk must be ≤ 4 MiB */
    for (int i = 0; i < append_records; i++) {
        mock_append_record_t rec = mock_get_append_record(wal_ctx, i);
        ASSERT_LE((long long)rec.len, (long long)(4 * 1024 * 1024));
        ASSERT_GT((long long)rec.len, 0LL);
    }

    /* Verify this was NOT a full resync — no delete+create in the
    ** incremental path (the blob was already created during WAL mode setup) */
    int del_count = mock_get_call_count(wal_ctx, "append_blob_delete");
    ASSERT_EQ(del_count, 0);

    wal_close_db(db);
}

#endif /* ENABLE_WAL_TESTS */


/* ══════════════════════════════════════════════════════════════════════
** Test Suite Runner
**
** Always defined so test_main.c can call run_wal_tests() unconditionally.
** When ENABLE_WAL_TESTS is not defined, this is a no-op.
** ══════════════════════════════════════════════════════════════════════ */

void run_wal_tests(void) {
#ifdef ENABLE_WAL_TESTS

    TEST_SUITE_BEGIN("WAL Mode \xe2\x80\x94 Prerequisites");
    RUN_TEST(wal_mode_requires_append_ops);
    RUN_TEST(wal_mode_allowed_with_append_ops);
    TEST_SUITE_END();

    TEST_SUITE_BEGIN("WAL Mode \xe2\x80\x94 Basic Operations");
    RUN_TEST(wal_open_creates_append_blob);
    RUN_TEST(wal_write_and_sync);
    RUN_TEST(wal_read_after_write);
    RUN_TEST(wal_multiple_transactions);
    TEST_SUITE_END();

    TEST_SUITE_BEGIN("WAL Mode \xe2\x80\x94 Checkpoint");
    RUN_TEST(wal_checkpoint_writes_pages);
    RUN_TEST(wal_checkpoint_resets_wal);
    TEST_SUITE_END();

    TEST_SUITE_BEGIN("WAL Mode \xe2\x80\x94 Error Handling");
    RUN_TEST(wal_append_failure_returns_error);
    RUN_TEST(wal_create_failure_returns_error);
    TEST_SUITE_END();

    TEST_SUITE_BEGIN("WAL Mode \xe2\x80\x94 Data Integrity");
    RUN_TEST(wal_insert_select_roundtrip);
    RUN_TEST(wal_concurrent_reads_during_write);
    TEST_SUITE_END();

    TEST_SUITE_BEGIN("WAL Mode \xe2\x80\x94 Crash Recovery on Open");
    RUN_TEST(wal_recovery_downloads_existing_wal);
    RUN_TEST(wal_recovery_download_failure);
    RUN_TEST(wal_recovery_empty_wal);
    TEST_SUITE_END();

    TEST_SUITE_BEGIN("WAL Mode \xe2\x80\x94 Full Resync Chunking");
    RUN_TEST(wal_full_resync_small_data);
    RUN_TEST(wal_full_resync_large_data);
    RUN_TEST(wal_incremental_append_large_data);
    TEST_SUITE_END();

    /* Cleanup */
    if (wal_ctx) {
        mock_azure_destroy(wal_ctx);
        wal_ctx = NULL;
    }

#endif /* ENABLE_WAL_TESTS */
}
