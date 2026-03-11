/*
** test_wal.c — WAL Mode Unit Tests for azqlite VFS
**
** Tests Write-Ahead Logging support through the azqlite VFS layer.
** WAL mode maps to Azure Append Blobs (sequential append-only writes)
** and requires EXCLUSIVE locking mode (no shared memory needed).
**
** Dependencies (Frodo/Aragorn — not yet implemented):
**   - azure_ops_t extended with append_blob_create/append/delete fields
**   - mock_azure_ops.c extended with mock append blob implementations
**   - azqlite_vfs.c updated to support WAL file type routing
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

#include "../src/azqlite.h"

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

    int rc = azqlite_vfs_register_with_ops(ops, ctx, 0);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "    VFS register failed: %d\n", rc);
        return NULL;
    }

    rc = sqlite3_open_v2(dbname, &db,
                          SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                          "azqlite");
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
    int rc = azqlite_vfs_register_with_ops(&no_append_ops, wal_ctx, 0);
    ASSERT_OK(rc);

    sqlite3 *db = NULL;
    rc = sqlite3_open_v2("wal_prereq.db", &db,
                          SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                          "azqlite");
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

    int rc = azqlite_vfs_register_with_ops(wal_base_ops, wal_ctx, 0);
    ASSERT_OK(rc);

    sqlite3 *db = NULL;
    rc = sqlite3_open_v2("walfail_create.db", &db,
                          SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                          "azqlite");
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
    rc = azqlite_vfs_register_with_ops(wal_base_ops, wal_ctx, 0);
    ASSERT_OK(rc);

    rc = sqlite3_open_v2("walround.db", &db,
                          SQLITE_OPEN_READWRITE, "azqlite");
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

    /* Cleanup */
    if (wal_ctx) {
        mock_azure_destroy(wal_ctx);
        wal_ctx = NULL;
    }

#endif /* ENABLE_WAL_TESTS */
}
