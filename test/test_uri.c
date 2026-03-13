/*
** test_uri.c — Unit tests for URI-based per-file Azure client configuration
**
** Tests the Phase 6 feature: each database can specify its own Azure
** credentials via URI parameters (azure_account, azure_container,
** azure_sas, azure_key, azure_endpoint).
**
** Test categories:
**   1. URI-only VFS registration (no global client)
**   2. Fallback to global mock ops when no URI params
**   3. Per-blob journal cache isolation
**   4. Error paths for missing credentials
*/

#include "../sqlite-autoconf-3520000/sqlite3.h"
#include "mock_azure_ops.h"
#include "test_harness.h"
#include "../src/sqlite_objs.h"
#include <string.h>
#include <stdlib.h>

/* ── Helpers ──────────────────────────────────────────────────────── */

static mock_azure_ctx_t *uri_ctx = NULL;
static azure_ops_t      *uri_ops = NULL;

static void uri_setup(void) {
    if (uri_ctx) mock_reset(uri_ctx);
    else         uri_ctx = mock_azure_create();
    uri_ops = mock_azure_get_ops();
}

/* ══════════════════════════════════════════════════════════════════════
** Test 1: Register URI-only VFS, open without URI params → CANTOPEN
** ══════════════════════════════════════════════════════════════════════ */

TEST(uri_register_uri_no_global_client) {
    uri_setup();

    /* Register VFS with no global ops (URI-only mode) */
    int rc = sqlite_objs_vfs_register_uri(0);
    ASSERT_OK(rc);

    /* Open a database WITHOUT URI params — should fail */
    sqlite3 *db = NULL;
    rc = sqlite3_open_v2("test_nouri.db", &db,
                          SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                          "sqlite-objs");
    ASSERT_EQ(rc, SQLITE_CANTOPEN);
    if (db) sqlite3_close(db);
}

/* ══════════════════════════════════════════════════════════════════════
** Test 2: Register with global mock ops, open WITH URI params.
** URI params trigger azure_client_create() which requires a real Azure
** endpoint — in unit tests this should fail with SQLITE_CANTOPEN
** because the stub client can't actually connect.
** ══════════════════════════════════════════════════════════════════════ */

TEST(uri_parse_with_mock_fallback) {
    uri_setup();

    /* Register VFS with global mock ops */
    int rc = sqlite_objs_vfs_register_with_ops(uri_ops, uri_ctx, 0);
    ASSERT_OK(rc);

    /* Open a database WITH URI params — will attempt azure_client_create()
    ** which fails in unit tests since there's no real Azure. That's expected. */
    sqlite3 *db = NULL;
    rc = sqlite3_open_v2(
        "file:uri_test.db?azure_account=acct&azure_container=cont&azure_sas=tok",
        &db,
        SQLITE_OPEN_URI | SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
        "sqlite-objs");

    /* azure_client_create from stub returns error → SQLITE_CANTOPEN */
    ASSERT_EQ(rc, SQLITE_CANTOPEN);
    if (db) sqlite3_close(db);
}

/* ══════════════════════════════════════════════════════════════════════
** Test 3: Register with global mock ops, open WITHOUT URI params.
** Falls back to global mock ops → should succeed (backward compat).
** ══════════════════════════════════════════════════════════════════════ */

TEST(uri_fallback_to_global) {
    uri_setup();

    /* Register VFS with global mock ops */
    int rc = sqlite_objs_vfs_register_with_ops(uri_ops, uri_ctx, 0);
    ASSERT_OK(rc);

    /* Open WITHOUT URI params → uses global mock ops → should succeed */
    sqlite3 *db = NULL;
    rc = sqlite3_open_v2("fallback_test.db", &db,
                          SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                          "sqlite-objs");
    ASSERT_OK(rc);
    ASSERT_NOT_NULL(db);

    /* Verify we can actually create a table and insert data */
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

/* ══════════════════════════════════════════════════════════════════════
** Test 4: Journal cache isolation — two databases share the VFS but
** each gets its own journal cache entry in the per-blob cache array.
** ══════════════════════════════════════════════════════════════════════ */

TEST(uri_journal_cache_isolation) {
    uri_setup();

    int rc = sqlite_objs_vfs_register_with_ops(uri_ops, uri_ctx, 0);
    ASSERT_OK(rc);

    /* Open two separate databases */
    sqlite3 *db1 = NULL, *db2 = NULL;
    rc = sqlite3_open_v2("cache_iso_a.db", &db1,
                          SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                          "sqlite-objs");
    ASSERT_OK(rc);
    ASSERT_NOT_NULL(db1);

    rc = sqlite3_open_v2("cache_iso_b.db", &db2,
                          SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                          "sqlite-objs");
    ASSERT_OK(rc);
    ASSERT_NOT_NULL(db2);

    /* Create tables and insert data in both — exercises journal path */
    char *errmsg = NULL;
    rc = sqlite3_exec(db1,
        "CREATE TABLE t1 (id INTEGER PRIMARY KEY, val TEXT);"
        "INSERT INTO t1 VALUES (1, 'db1_value');",
        NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "  db1 SQL error: %s\n", errmsg ? errmsg : "unknown");
        sqlite3_free(errmsg);
    }
    ASSERT_OK(rc);

    rc = sqlite3_exec(db2,
        "CREATE TABLE t2 (id INTEGER PRIMARY KEY, val TEXT);"
        "INSERT INTO t2 VALUES (1, 'db2_value');",
        NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "  db2 SQL error: %s\n", errmsg ? errmsg : "unknown");
        sqlite3_free(errmsg);
    }
    ASSERT_OK(rc);

    /* Verify each database has its own independent data */
    sqlite3_stmt *stmt = NULL;

    rc = sqlite3_prepare_v2(db1, "SELECT val FROM t1 WHERE id=1;", -1, &stmt, NULL);
    ASSERT_OK(rc);
    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_ROW);
    ASSERT_STR_EQ(sqlite3_column_text(stmt, 0), "db1_value");
    sqlite3_finalize(stmt);

    rc = sqlite3_prepare_v2(db2, "SELECT val FROM t2 WHERE id=1;", -1, &stmt, NULL);
    ASSERT_OK(rc);
    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_ROW);
    ASSERT_STR_EQ(sqlite3_column_text(stmt, 0), "db2_value");
    sqlite3_finalize(stmt);

    /* Close both and reopen to verify journal cache didn't cross-contaminate */
    sqlite3_close(db1);
    sqlite3_close(db2);

    rc = sqlite3_open_v2("cache_iso_a.db", &db1,
                          SQLITE_OPEN_READWRITE, "sqlite-objs");
    ASSERT_OK(rc);
    rc = sqlite3_prepare_v2(db1, "SELECT val FROM t1 WHERE id=1;", -1, &stmt, NULL);
    ASSERT_OK(rc);
    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_ROW);
    ASSERT_STR_EQ(sqlite3_column_text(stmt, 0), "db1_value");
    sqlite3_finalize(stmt);

    rc = sqlite3_open_v2("cache_iso_b.db", &db2,
                          SQLITE_OPEN_READWRITE, "sqlite-objs");
    ASSERT_OK(rc);
    rc = sqlite3_prepare_v2(db2, "SELECT val FROM t2 WHERE id=1;", -1, &stmt, NULL);
    ASSERT_OK(rc);
    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_ROW);
    ASSERT_STR_EQ(sqlite3_column_text(stmt, 0), "db2_value");
    sqlite3_finalize(stmt);

    sqlite3_close(db1);
    sqlite3_close(db2);
}

/* ══════════════════════════════════════════════════════════════════════
** Test 5: URI-only VFS with no URI params → CANTOPEN (same as test 1
** but verifies the error path more thoroughly)
** ══════════════════════════════════════════════════════════════════════ */

TEST(uri_cantopen_no_ops_no_uri) {
    uri_setup();

    /* Register URI-only VFS (NULL global ops) */
    int rc = sqlite_objs_vfs_register_uri(0);
    ASSERT_OK(rc);

    /* Try to open without URI params using sqlite3_open_v2 with URI flag */
    sqlite3 *db = NULL;
    rc = sqlite3_open_v2("file:no_uri_params.db", &db,
                          SQLITE_OPEN_URI | SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                          "sqlite-objs");
    ASSERT_EQ(rc, SQLITE_CANTOPEN);
    if (db) sqlite3_close(db);

    /* Also try without the URI flag — plain filename */
    db = NULL;
    rc = sqlite3_open_v2("plain_no_uri.db", &db,
                          SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                          "sqlite-objs");
    ASSERT_EQ(rc, SQLITE_CANTOPEN);
    if (db) sqlite3_close(db);
}

/* ══════════════════════════════════════════════════════════════════════
** Test 6: Multiple DBs with journal writes — exercises per-blob cache
** eviction when more than SQLITE_OBJS_MAX_JOURNAL_CACHE entries exist
** ══════════════════════════════════════════════════════════════════════ */

TEST(uri_journal_cache_multiple_dbs) {
    uri_setup();

    int rc = sqlite_objs_vfs_register_with_ops(uri_ops, uri_ctx, 0);
    ASSERT_OK(rc);

    /* Open 4 databases and write to each */
    sqlite3 *dbs[4];
    const char *names[] = {
        "jcache_a.db", "jcache_b.db", "jcache_c.db", "jcache_d.db"
    };

    for (int i = 0; i < 4; i++) {
        rc = sqlite3_open_v2(names[i], &dbs[i],
                              SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                              "sqlite-objs");
        ASSERT_OK(rc);
        ASSERT_NOT_NULL(dbs[i]);

        char sql[256];
        snprintf(sql, sizeof(sql),
            "CREATE TABLE t (id INTEGER, val TEXT);"
            "INSERT INTO t VALUES (%d, 'val_%d');",
            i, i);
        char *errmsg = NULL;
        rc = sqlite3_exec(dbs[i], sql, NULL, NULL, &errmsg);
        if (rc != SQLITE_OK) {
            fprintf(stderr, "  db[%d] error: %s\n", i, errmsg ? errmsg : "unknown");
            sqlite3_free(errmsg);
        }
        ASSERT_OK(rc);
    }

    /* Verify each has its own data */
    for (int i = 0; i < 4; i++) {
        sqlite3_stmt *stmt = NULL;
        rc = sqlite3_prepare_v2(dbs[i], "SELECT val FROM t WHERE id=?;",
                                -1, &stmt, NULL);
        ASSERT_OK(rc);
        sqlite3_bind_int(stmt, 1, i);
        rc = sqlite3_step(stmt);
        ASSERT_EQ(rc, SQLITE_ROW);

        char expected[32];
        snprintf(expected, sizeof(expected), "val_%d", i);
        ASSERT_STR_EQ(sqlite3_column_text(stmt, 0), expected);
        sqlite3_finalize(stmt);
    }

    for (int i = 0; i < 4; i++) {
        sqlite3_close(dbs[i]);
    }
}

/* ══════════════════════════════════════════════════════════════════════
** Test 7: URI registration returns SQLITE_OK
** ══════════════════════════════════════════════════════════════════════ */

TEST(uri_register_returns_ok) {
    uri_setup();
    int rc = sqlite_objs_vfs_register_uri(0);
    ASSERT_OK(rc);

    /* Verify the VFS is findable */
    sqlite3_vfs *vfs = sqlite3_vfs_find("sqlite-objs");
    ASSERT_NOT_NULL(vfs);
}

/* ══════════════════════════════════════════════════════════════════════
** Test 8: Global ops backward compat — re-register with ops after URI
** ══════════════════════════════════════════════════════════════════════ */

TEST(uri_reregister_with_ops) {
    uri_setup();

    /* First register as URI-only */
    int rc = sqlite_objs_vfs_register_uri(0);
    ASSERT_OK(rc);

    /* Now re-register with global mock ops */
    rc = sqlite_objs_vfs_register_with_ops(uri_ops, uri_ctx, 0);
    ASSERT_OK(rc);

    /* Should be able to open without URI params now */
    sqlite3 *db = NULL;
    rc = sqlite3_open_v2("reregister_test.db", &db,
                          SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                          "sqlite-objs");
    ASSERT_OK(rc);
    ASSERT_NOT_NULL(db);
    sqlite3_close(db);
}

/* ══════════════════════════════════════════════════════════════════════
** Runner
** ══════════════════════════════════════════════════════════════════════ */

static void run_uri_tests(void) {
    TEST_SUITE_BEGIN("URI-Based Per-File Config");
    RUN_TEST(uri_register_uri_no_global_client);
    RUN_TEST(uri_parse_with_mock_fallback);
    RUN_TEST(uri_fallback_to_global);
    RUN_TEST(uri_journal_cache_isolation);
    RUN_TEST(uri_cantopen_no_ops_no_uri);
    RUN_TEST(uri_journal_cache_multiple_dbs);
    RUN_TEST(uri_register_returns_ok);
    RUN_TEST(uri_reregister_with_ops);
    TEST_SUITE_END();
}
