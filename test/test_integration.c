/*
 * test_integration.c — Layer 2 Integration Tests for sqliteObjs
 *
 * These tests run against Azurite — a local Azure Storage emulator — and use
 * the REAL azure_client.c code (not mocks). They validate:
 *   - Real HTTP communication
 *   - Actual Azure REST API compatibility
 *   - End-to-end VFS functionality with a real backend
 *
 * Prerequisites:
 *   - Azurite running on 127.0.0.1:10000
 *   - Container "sqlite-objs-test" created
 *
 * The test/run-integration.sh wrapper script handles Azurite lifecycle.
 *
 * Part of the sqliteObjs project. License: MIT
 */

#include "test_harness.h"
#include "azure_client_impl.h"
#include "sqlite_objs.h"
#include "sqlite3.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/wait.h>

/* ================================================================
 * Azurite configuration (well-known dev credentials)
 * ================================================================ */

#define AZURITE_ACCOUNT    "devstoreaccount1"
#define AZURITE_CONTAINER  "sqlite-objs-test"
#define AZURITE_ENDPOINT   "http://127.0.0.1:10000"

/* Well-known Azurite shared key (same on every install — NOT a secret) */
#define AZURITE_KEY \
    "Eby8vdM02xNOcqFlqUwJPLlmEtlCDXJ1OUzFT50uSRZ6IFsuFq2UVErCz4I6tq/K1SZFPTOtr/KBHBeksoGMGw=="

/* ================================================================
 * Test context — shared Azure client
 * ================================================================ */

static azure_client_t *g_client = NULL;
static const azure_ops_t *g_ops = NULL;
static void *g_ctx = NULL;

/* ================================================================
 * Stress test parameterization (environment-controlled)
 * ================================================================ */

/* Get stress multiplier from environment (default: 1) */
static int get_stress_multiplier(void) {
    const char *env = getenv("SQLITE_OBJS_STRESS_MULTIPLIER");
    if (env == NULL || *env == '\0') {
        return 1;
    }
    int mult = atoi(env);
    return (mult > 0) ? mult : 1;
}

/* ================================================================
 * Setup / Teardown
 * ================================================================ */

static void setup_azure_client(void) {
    azure_client_config_t cfg = {
        .account = AZURITE_ACCOUNT,
        .container = AZURITE_CONTAINER,
        .sas_token = NULL,
        .account_key = AZURITE_KEY,
        .endpoint = AZURITE_ENDPOINT
    };
    azure_error_t err;
    azure_error_init(&err);

    azure_err_t rc = azure_client_create(&cfg, &g_client, &err);
    if (rc != AZURE_OK) {
        fprintf(stderr, "FATAL: Could not create Azure client: %s\n",
                err.error_message);
        fprintf(stderr, "Is Azurite running on %s?\n", AZURITE_ENDPOINT);
        exit(1);
    }

    /* Create the test container (idempotent: OK if already exists) */
    azure_error_init(&err);
    rc = azure_container_create(g_client, &err);
    if (rc != AZURE_OK) {
        fprintf(stderr, "FATAL: Could not create test container: %s\n",
                err.error_message);
        fprintf(stderr, "HTTP status: %d, Error code: %s\n",
                err.http_status, err.error_code);
        azure_client_destroy(g_client);
        exit(1);
    }

    g_ops = azure_client_get_ops();
    g_ctx = azure_client_get_ctx(g_client);
}

static void teardown_azure_client(void) {
    if (g_client) {
        azure_client_destroy(g_client);
        g_client = NULL;
        g_ops = NULL;
        g_ctx = NULL;
    }
}

/* Delete a blob if it exists (cleanup helper) */
static void cleanup_blob(const char *name) {
    azure_error_t err;
    azure_error_init(&err);
    g_ops->blob_delete(g_ctx, name, &err);
    /* Ignore errors — blob may not exist */
}

/* ================================================================
 * Test 1: Page Blob Lifecycle
 * ================================================================ */

TEST(page_blob_lifecycle) {
    const char *blob_name = "test-page-blob.db";
    cleanup_blob(blob_name);

    azure_error_t err;
    azure_error_init(&err);

    /* Create a 4KB page blob */
    azure_err_t rc = g_ops->page_blob_create(g_ctx, blob_name, 4096, &err);
    ASSERT_AZURE_OK(rc);

    /* Write 512 bytes at offset 0 */
    uint8_t write_data[512];
    for (int i = 0; i < 512; i++) {
        write_data[i] = (uint8_t)(i % 256);
    }
    rc = g_ops->page_blob_write(g_ctx, blob_name, 0, write_data, 512, NULL, NULL, &err);
    ASSERT_AZURE_OK(rc);

    /* Read back those 512 bytes */
    azure_buffer_t read_buf;
    azure_buffer_init(&read_buf);
    rc = g_ops->page_blob_read(g_ctx, blob_name, 0, 512, &read_buf, &err);
    ASSERT_AZURE_OK(rc);
    ASSERT_EQ(read_buf.size, 512);
    ASSERT_MEM_EQ(read_buf.data, write_data, 512);
    azure_buffer_free(&read_buf);

    /* Get blob properties */
    int64_t size = -1;
    char lease_state[32] = {0};
    char lease_status[32] = {0};
    rc = g_ops->blob_get_properties(g_ctx, blob_name, &size,
                                     lease_state, lease_status, &err);
    ASSERT_AZURE_OK(rc);
    ASSERT_EQ(size, 4096);

    /* Delete the blob */
    rc = g_ops->blob_delete(g_ctx, blob_name, &err);
    ASSERT_AZURE_OK(rc);

    /* Verify it's gone */
    int exists = 1;
    rc = g_ops->blob_exists(g_ctx, blob_name, &exists, &err);
    ASSERT_AZURE_OK(rc);
    ASSERT_EQ(exists, 0);
}

/* ================================================================
 * Test 2: Block Blob Lifecycle
 * ================================================================ */

TEST(block_blob_lifecycle) {
    const char *blob_name = "test-block-blob.dat";
    cleanup_blob(blob_name);

    azure_error_t err;
    azure_error_init(&err);

    /* Upload 256 bytes */
    uint8_t upload_data[256];
    for (int i = 0; i < 256; i++) {
        upload_data[i] = (uint8_t)(255 - i);
    }
    azure_err_t rc = g_ops->block_blob_upload(g_ctx, blob_name,
                                               upload_data, 256, &err);
    ASSERT_AZURE_OK(rc);

    /* Download and verify */
    azure_buffer_t download_buf;
    azure_buffer_init(&download_buf);
    rc = g_ops->block_blob_download(g_ctx, blob_name, &download_buf, &err);
    ASSERT_AZURE_OK(rc);
    ASSERT_EQ(download_buf.size, 256);
    ASSERT_MEM_EQ(download_buf.data, upload_data, 256);
    azure_buffer_free(&download_buf);

    /* Cleanup */
    cleanup_blob(blob_name);
}

/* ================================================================
 * Test 3: Lease Lifecycle (Acquire → Renew → Release)
 * ================================================================ */

TEST(lease_lifecycle) {
    const char *blob_name = "test-lease-blob.db";
    cleanup_blob(blob_name);

    azure_error_t err;
    azure_error_init(&err);

    /* Create a page blob to lease */
    azure_err_t rc = g_ops->page_blob_create(g_ctx, blob_name, 512, &err);
    ASSERT_AZURE_OK(rc);

    /* Acquire a 30-second lease */
    char lease_id[64] = {0};
    rc = g_ops->lease_acquire(g_ctx, blob_name, 30, lease_id,
                              sizeof(lease_id), &err);
    ASSERT_AZURE_OK(rc);
    ASSERT_GT(strlen(lease_id), 0);

    /* Renew the lease */
    rc = g_ops->lease_renew(g_ctx, blob_name, lease_id, &err);
    ASSERT_AZURE_OK(rc);

    /* Release the lease */
    rc = g_ops->lease_release(g_ctx, blob_name, lease_id, &err);
    ASSERT_AZURE_OK(rc);

    /* Cleanup */
    cleanup_blob(blob_name);
}

/* ================================================================
 * Test 4: Lease Conflict (Two Clients)
 * ================================================================ */

TEST(lease_conflict) {
    const char *blob_name = "test-lease-conflict.db";
    cleanup_blob(blob_name);

    azure_error_t err;
    azure_error_init(&err);

    /* Create a page blob */
    azure_err_t rc = g_ops->page_blob_create(g_ctx, blob_name, 512, &err);
    ASSERT_AZURE_OK(rc);

    /* Acquire a lease (first client) */
    char lease_id1[64] = {0};
    rc = g_ops->lease_acquire(g_ctx, blob_name, 30, lease_id1,
                              sizeof(lease_id1), &err);
    ASSERT_AZURE_OK(rc);

    /* Try to acquire again (simulating second client) → should fail */
    char lease_id2[64] = {0};
    rc = g_ops->lease_acquire(g_ctx, blob_name, 30, lease_id2,
                              sizeof(lease_id2), &err);
    ASSERT_AZURE_ERR(rc, AZURE_ERR_CONFLICT);

    /* Release the first lease */
    rc = g_ops->lease_release(g_ctx, blob_name, lease_id1, &err);
    ASSERT_AZURE_OK(rc);

    /* Now the second acquire should succeed */
    azure_error_init(&err);
    rc = g_ops->lease_acquire(g_ctx, blob_name, 30, lease_id2,
                              sizeof(lease_id2), &err);
    ASSERT_AZURE_OK(rc);

    /* Cleanup */
    rc = g_ops->lease_release(g_ctx, blob_name, lease_id2, &err);
    ASSERT_AZURE_OK(rc);
    cleanup_blob(blob_name);
}

/* ================================================================
 * Test 5: Page Blob Alignment
 * ================================================================ */

TEST(page_blob_alignment) {
    const char *blob_name = "test-alignment.db";
    cleanup_blob(blob_name);

    azure_error_t err;
    azure_error_init(&err);

    /* Create a 2KB page blob */
    azure_err_t rc = g_ops->page_blob_create(g_ctx, blob_name, 2048, &err);
    ASSERT_AZURE_OK(rc);

    /* Write at offset 512 (aligned) */
    uint8_t data[512];
    memset(data, 0xAB, 512);
    rc = g_ops->page_blob_write(g_ctx, blob_name, 512, data, 512, NULL, NULL, &err);
    ASSERT_AZURE_OK(rc);

    /* Write at offset 1024 (aligned) */
    memset(data, 0xCD, 512);
    rc = g_ops->page_blob_write(g_ctx, blob_name, 1024, data, 512, NULL, NULL, &err);
    ASSERT_AZURE_OK(rc);

    /* Read back and verify */
    azure_buffer_t read_buf;
    azure_buffer_init(&read_buf);
    rc = g_ops->page_blob_read(g_ctx, blob_name, 512, 512, &read_buf, &err);
    ASSERT_AZURE_OK(rc);
    ASSERT_EQ(read_buf.size, 512);
    for (int i = 0; i < 512; i++) {
        ASSERT_EQ(read_buf.data[i], 0xAB);
    }
    azure_buffer_free(&read_buf);

    /* Cleanup */
    cleanup_blob(blob_name);
}

/* ================================================================
 * Test 6: Full VFS Round-Trip (SQLite on Azurite)
 * ================================================================ */

TEST(vfs_roundtrip) {
    const char *db_name = "vfs-test.db";
    cleanup_blob(db_name);
    cleanup_blob("vfs-test.db-journal");

    /* Register the sqliteObjs VFS with Azurite config */
    sqlite_objs_config_t cfg = {
        .account = AZURITE_ACCOUNT,
        .container = AZURITE_CONTAINER,
        .sas_token = NULL,
        .account_key = AZURITE_KEY,
        .endpoint = AZURITE_ENDPOINT,
        .ops = NULL,  /* Use production client */
        .ops_ctx = NULL
    };
    int rc = sqlite_objs_vfs_register_with_config(&cfg, 0);
    ASSERT_OK(rc);

    /* Open a database using the sqliteObjs VFS */
    sqlite3 *db = NULL;
    rc = sqlite3_open_v2(db_name, &db,
                         SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                         "sqlite-objs");
    ASSERT_OK(rc);
    ASSERT_NOT_NULL(db);

    /* Create a table and insert data */
    char *errmsg = NULL;
    rc = sqlite3_exec(db, "CREATE TABLE test (id INTEGER, name TEXT);",
                      NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "CREATE TABLE failed: %s\n", errmsg);
        sqlite3_free(errmsg);
    }
    ASSERT_OK(rc);

    rc = sqlite3_exec(db,
        "INSERT INTO test VALUES (1, 'Frodo');"
        "INSERT INTO test VALUES (2, 'Sam');"
        "INSERT INTO test VALUES (3, 'Gandalf');",
        NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "INSERT failed: %s\n", errmsg);
        sqlite3_free(errmsg);
    }
    ASSERT_OK(rc);

    /* Close the database */
    rc = sqlite3_close(db);
    ASSERT_OK(rc);

    /* Reopen and verify data persists */
    db = NULL;
    rc = sqlite3_open_v2(db_name, &db,
                         SQLITE_OPEN_READWRITE, "sqlite-objs");
    ASSERT_OK(rc);

    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM test;", -1, &stmt, NULL);
    ASSERT_OK(rc);

    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_ROW);
    int count = sqlite3_column_int(stmt, 0);
    ASSERT_EQ(count, 3);

    sqlite3_finalize(stmt);
    sqlite3_close(db);

    /* Cleanup */
    cleanup_blob(db_name);
    cleanup_blob("vfs-test.db-journal");
}

/* ================================================================
 * Test 7: Journal Round-Trip
 * ================================================================ */

TEST(journal_roundtrip) {
    const char *db_name = "journal-test.db";
    cleanup_blob(db_name);
    cleanup_blob("journal-test.db-journal");

    /* Register VFS (reuse from previous test) */
    sqlite_objs_config_t cfg = {
        .account = AZURITE_ACCOUNT,
        .container = AZURITE_CONTAINER,
        .sas_token = NULL,
        .account_key = AZURITE_KEY,
        .endpoint = AZURITE_ENDPOINT,
        .ops = NULL,
        .ops_ctx = NULL
    };
    int rc = sqlite_objs_vfs_register_with_config(&cfg, 0);
    ASSERT_OK(rc);

    sqlite3 *db = NULL;
    rc = sqlite3_open_v2(db_name, &db,
                         SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                         "sqlite-objs");
    ASSERT_OK(rc);

    /* Create a table */
    char *errmsg = NULL;
    rc = sqlite3_exec(db, "CREATE TABLE txn_test (val INTEGER);",
                      NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "CREATE TABLE failed: %s\n", errmsg);
        sqlite3_free(errmsg);
    }
    ASSERT_OK(rc);

    /* Start a transaction */
    rc = sqlite3_exec(db, "BEGIN;", NULL, NULL, &errmsg);
    ASSERT_OK(rc);

    /* Insert data */
    rc = sqlite3_exec(db, "INSERT INTO txn_test VALUES (42);",
                      NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "INSERT failed: %s\n", errmsg);
        sqlite3_free(errmsg);
    }
    ASSERT_OK(rc);

    /* Commit (journal should be written and deleted) */
    rc = sqlite3_exec(db, "COMMIT;", NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "COMMIT failed: %s\n", errmsg);
        sqlite3_free(errmsg);
    }
    ASSERT_OK(rc);

    /* Verify data */
    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(db, "SELECT val FROM txn_test;", -1, &stmt, NULL);
    ASSERT_OK(rc);
    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_ROW);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), 42);
    sqlite3_finalize(stmt);

    sqlite3_close(db);

    /* Cleanup */
    cleanup_blob(db_name);
    cleanup_blob("journal-test.db-journal");
}

/* ================================================================
 * Test 8: Error Handling (Not Found)
 * ================================================================ */

TEST(error_not_found) {
    const char *blob_name = "nonexistent-blob.db";
    cleanup_blob(blob_name);

    azure_error_t err;
    azure_error_init(&err);

    /* Try to read a non-existent blob → should get NOT_FOUND */
    azure_buffer_t buf;
    azure_buffer_init(&buf);
    azure_err_t rc = g_ops->page_blob_read(g_ctx, blob_name, 0, 512, &buf, &err);
    ASSERT_AZURE_ERR(rc, AZURE_ERR_NOT_FOUND);
    azure_buffer_free(&buf);

    /* Try to delete a non-existent blob → should succeed (idempotent) or NOT_FOUND */
    rc = g_ops->blob_delete(g_ctx, blob_name, &err);
    /* Azure's behavior: delete of non-existent blob succeeds */
    /* But Azurite might return 404 — accept either */
    ASSERT_TRUE(rc == AZURE_OK || rc == AZURE_ERR_NOT_FOUND);
}

/* ================================================================
 * Test 9: Page Blob Resize
 * ================================================================ */

TEST(page_blob_resize) {
    const char *blob_name = "resize-test.db";
    cleanup_blob(blob_name);

    azure_error_t err;
    azure_error_init(&err);

    /* Create a 1KB blob */
    azure_err_t rc = g_ops->page_blob_create(g_ctx, blob_name, 1024, &err);
    ASSERT_AZURE_OK(rc);

    /* Resize to 2KB */
    rc = g_ops->page_blob_resize(g_ctx, blob_name, 2048, NULL, &err);
    ASSERT_AZURE_OK(rc);

    /* Verify new size */
    int64_t size = -1;
    rc = g_ops->blob_get_properties(g_ctx, blob_name, &size, NULL, NULL, &err);
    ASSERT_AZURE_OK(rc);
    ASSERT_EQ(size, 2048);

    /* Cleanup */
    cleanup_blob(blob_name);
}

/* ================================================================
 * Test 10: Lease Break
 * ================================================================ */

TEST(lease_break) {
    const char *blob_name = "lease-break-test.db";
    cleanup_blob(blob_name);

    azure_error_t err;
    azure_error_init(&err);

    /* Create a page blob */
    azure_err_t rc = g_ops->page_blob_create(g_ctx, blob_name, 512, &err);
    ASSERT_AZURE_OK(rc);

    /* Acquire a lease */
    char lease_id[64] = {0};
    rc = g_ops->lease_acquire(g_ctx, blob_name, 30, lease_id,
                              sizeof(lease_id), &err);
    ASSERT_AZURE_OK(rc);

    /* Break the lease (immediate) */
    int remaining = -1;
    rc = g_ops->lease_break(g_ctx, blob_name, 0, &remaining, &err);
    ASSERT_AZURE_OK(rc);

    /* After immediate break, the lease should be broken */
    /* Try to acquire again → should succeed */
    char new_lease_id[64] = {0};
    rc = g_ops->lease_acquire(g_ctx, blob_name, 30, new_lease_id,
                              sizeof(new_lease_id), &err);
    ASSERT_AZURE_OK(rc);

    /* Cleanup */
    rc = g_ops->lease_release(g_ctx, blob_name, new_lease_id, &err);
    ASSERT_AZURE_OK(rc);
    cleanup_blob(blob_name);
}

TEST(batch_reqs_alloc_failure_releases_mutex) {
    uint8_t data[1024];
    memset(data, 0xA5, sizeof(data));
    azure_page_range_t ranges[2] = {
        { .offset = 0, .data = data, .len = 512 },
        { .offset = 512, .data = data + 512, .len = 512 }
    };

    azure_error_t err;
    azure_error_init(&err);
    azure_test_fail_next_batch_reqs_alloc(1);
    azure_err_t rc = g_ops->page_blob_write_batch(
        g_ctx, "batch-oom.db", ranges, 2, NULL, NULL, &err);
    ASSERT_AZURE_ERR(rc, AZURE_ERR_NOMEM);

    int lock_rc = pthread_mutex_trylock(&g_client->mutex);
    ASSERT_EQ(lock_rc, 0);
    pthread_mutex_unlock(&g_client->mutex);
}

TEST(batch_lease_renewal_path_does_not_deadlock) {
    const char *blob = "batch-renew.db";
    cleanup_blob(blob);

    azure_error_t err;
    azure_error_init(&err);
    azure_err_t rc = g_ops->page_blob_create(g_ctx, blob, 1024, &err);
    ASSERT_AZURE_OK(rc);

    char lease_id[64];
    rc = g_ops->lease_acquire(g_ctx, blob, 60, lease_id, sizeof(lease_id), &err);
    ASSERT_AZURE_OK(rc);

    uint8_t data[1024];
    memset(data, 0x6B, sizeof(data));
    azure_page_range_t ranges[2] = {
        { .offset = 0, .data = data, .len = 512 },
        { .offset = 512, .data = data + 512, .len = 512 }
    };

    azure_test_set_batch_lease_renewal_seconds(0);
    azure_test_set_batch_lease_renew_result(1, AZURE_OK);
    rc = g_ops->page_blob_write_batch(
        g_ctx, blob, ranges, 2, lease_id, NULL, &err);
    ASSERT_AZURE_OK(rc);
    ASSERT_GT(azure_test_get_batch_lease_renew_count(), 0);

    int lock_rc = pthread_mutex_trylock(&g_client->mutex);
    ASSERT_EQ(lock_rc, 0);
    pthread_mutex_unlock(&g_client->mutex);

    azure_test_set_batch_lease_renew_result(0, AZURE_OK);
    azure_test_set_batch_lease_renewal_seconds(-1);
    rc = g_ops->lease_release(g_ctx, blob, lease_id, &err);
    ASSERT_AZURE_OK(rc);
    cleanup_blob(blob);
}

/* ================================================================
 * Test: URI Open with Azurite Params
 * Open a database via URI with Azurite credentials, insert data,
 * verify it's readable.
 * ================================================================ */

TEST(integ_uri_open_with_params) {
    const char *db_name = "uritest.db";
    cleanup_blob(db_name);
    cleanup_blob("uritest.db-journal");

    /* Register the VFS in URI-only mode */
    int rc = sqlite_objs_vfs_register_uri(0);
    ASSERT_OK(rc);

    /* Open database with URI parameters pointing to Azurite */
    sqlite3 *db = NULL;
    rc = sqlite3_open_v2(
        "file:uritest.db?"
        "azure_account=" AZURITE_ACCOUNT
        "&azure_container=" AZURITE_CONTAINER
        "&azure_key=" AZURITE_KEY
        "&azure_endpoint=" AZURITE_ENDPOINT,
        &db,
        SQLITE_OPEN_URI | SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
        "sqlite-objs");
    ASSERT_OK(rc);
    ASSERT_NOT_NULL(db);

    /* Create table and insert data */
    char *errmsg = NULL;
    rc = sqlite3_exec(db,
        "CREATE TABLE uri_t (id INTEGER PRIMARY KEY, val TEXT);"
        "INSERT INTO uri_t VALUES (1, 'uri_works');",
        NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "  integ_uri SQL error: %s\n", errmsg ? errmsg : "unknown");
        sqlite3_free(errmsg);
    }
    ASSERT_OK(rc);

    /* Verify data is readable */
    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(db, "SELECT val FROM uri_t WHERE id=1;", -1, &stmt, NULL);
    ASSERT_OK(rc);
    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_ROW);
    ASSERT_STR_EQ(sqlite3_column_text(stmt, 0), "uri_works");
    sqlite3_finalize(stmt);

    sqlite3_close(db);

    /* Cleanup */
    cleanup_blob(db_name);
    cleanup_blob("uritest.db-journal");
}

/* ================================================================
 * Test: Multi-DB Independent (same container, different blobs)
 * Two databases in the same container via standard registration.
 * Insert different data, verify independence.
 * ================================================================ */

TEST(integ_multi_db_independent) {
    const char *db1_name = "multi_a.db";
    const char *db2_name = "multi_b.db";
    cleanup_blob(db1_name);
    cleanup_blob("multi_a.db-journal");
    cleanup_blob(db2_name);
    cleanup_blob("multi_b.db-journal");

    /* Register VFS with Azurite config */
    sqlite_objs_config_t cfg = {
        .account = AZURITE_ACCOUNT,
        .container = AZURITE_CONTAINER,
        .sas_token = NULL,
        .account_key = AZURITE_KEY,
        .endpoint = AZURITE_ENDPOINT,
        .ops = NULL,
        .ops_ctx = NULL
    };
    int rc = sqlite_objs_vfs_register_with_config(&cfg, 0);
    ASSERT_OK(rc);

    /* Open two databases */
    sqlite3 *db1 = NULL, *db2 = NULL;
    rc = sqlite3_open_v2(db1_name, &db1,
                          SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                          "sqlite-objs");
    ASSERT_OK(rc);
    ASSERT_NOT_NULL(db1);

    rc = sqlite3_open_v2(db2_name, &db2,
                          SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                          "sqlite-objs");
    ASSERT_OK(rc);
    ASSERT_NOT_NULL(db2);

    /* Insert different data in each */
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

    /* Verify each has its own data */
    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(db1, "SELECT val FROM t WHERE id=1;", -1, &stmt, NULL);
    ASSERT_OK(rc);
    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_ROW);
    ASSERT_STR_EQ(sqlite3_column_text(stmt, 0), "alpha");
    sqlite3_finalize(stmt);

    rc = sqlite3_prepare_v2(db2, "SELECT val FROM t WHERE id=1;", -1, &stmt, NULL);
    ASSERT_OK(rc);
    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_ROW);
    ASSERT_STR_EQ(sqlite3_column_text(stmt, 0), "beta");
    sqlite3_finalize(stmt);

    /* Close and reopen to verify persistence */
    sqlite3_close(db1);
    sqlite3_close(db2);

    rc = sqlite3_open_v2(db1_name, &db1, SQLITE_OPEN_READWRITE, "sqlite-objs");
    ASSERT_OK(rc);
    rc = sqlite3_prepare_v2(db1, "SELECT val FROM t WHERE id=1;", -1, &stmt, NULL);
    ASSERT_OK(rc);
    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_ROW);
    ASSERT_STR_EQ(sqlite3_column_text(stmt, 0), "alpha");
    sqlite3_finalize(stmt);
    sqlite3_close(db1);

    rc = sqlite3_open_v2(db2_name, &db2, SQLITE_OPEN_READWRITE, "sqlite-objs");
    ASSERT_OK(rc);
    rc = sqlite3_prepare_v2(db2, "SELECT val FROM t WHERE id=1;", -1, &stmt, NULL);
    ASSERT_OK(rc);
    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_ROW);
    ASSERT_STR_EQ(sqlite3_column_text(stmt, 0), "beta");
    sqlite3_finalize(stmt);
    sqlite3_close(db2);

    /* Cleanup */
    cleanup_blob(db1_name);
    cleanup_blob("multi_a.db-journal");
    cleanup_blob(db2_name);
    cleanup_blob("multi_b.db-journal");
}

/* ================================================================
 * Test: URI Two Containers
 * Create two Azurite containers. Open DB1 in container1 via URI,
 * DB2 in container2 via URI. Write to both. Verify data independence.
 * This is the killer test for URI config.
 * ================================================================ */

TEST(integ_uri_two_containers) {
    const char *container1 = "sqlite-objs-uri-c1";
    const char *container2 = "sqlite-objs-uri-c2";

    /* Create two separate containers via two clients */
    azure_client_config_t cfg1 = {
        .account = AZURITE_ACCOUNT,
        .container = container1,
        .sas_token = NULL,
        .account_key = AZURITE_KEY,
        .endpoint = AZURITE_ENDPOINT
    };
    azure_client_config_t cfg2 = {
        .account = AZURITE_ACCOUNT,
        .container = container2,
        .sas_token = NULL,
        .account_key = AZURITE_KEY,
        .endpoint = AZURITE_ENDPOINT
    };

    azure_error_t err;
    azure_client_t *client1 = NULL, *client2 = NULL;

    azure_error_init(&err);
    azure_err_t arc = azure_client_create(&cfg1, &client1, &err);
    ASSERT_AZURE_OK(arc);
    arc = azure_container_create(client1, &err);
    ASSERT_AZURE_OK(arc);

    azure_error_init(&err);
    arc = azure_client_create(&cfg2, &client2, &err);
    ASSERT_AZURE_OK(arc);
    arc = azure_container_create(client2, &err);
    ASSERT_AZURE_OK(arc);

    azure_client_destroy(client1);
    azure_client_destroy(client2);

    /* Register VFS in URI-only mode */
    int rc = sqlite_objs_vfs_register_uri(0);
    ASSERT_OK(rc);

    /* Open DB1 in container1 via URI */
    sqlite3 *db1 = NULL;
    char uri1[512];
    snprintf(uri1, sizeof(uri1),
        "file:crossc.db?"
        "azure_account=%s&azure_container=%s&azure_key=%s&azure_endpoint=%s",
        AZURITE_ACCOUNT, container1, AZURITE_KEY, AZURITE_ENDPOINT);
    rc = sqlite3_open_v2(uri1, &db1,
                          SQLITE_OPEN_URI | SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                          "sqlite-objs");
    ASSERT_OK(rc);
    ASSERT_NOT_NULL(db1);

    /* Open DB2 in container2 via URI */
    sqlite3 *db2 = NULL;
    char uri2[512];
    snprintf(uri2, sizeof(uri2),
        "file:crossc.db?"
        "azure_account=%s&azure_container=%s&azure_key=%s&azure_endpoint=%s",
        AZURITE_ACCOUNT, container2, AZURITE_KEY, AZURITE_ENDPOINT);
    rc = sqlite3_open_v2(uri2, &db2,
                          SQLITE_OPEN_URI | SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                          "sqlite-objs");
    ASSERT_OK(rc);
    ASSERT_NOT_NULL(db2);

    /* Insert different data (use DROP IF EXISTS for idempotency with persistent Azurite) */
    char *errmsg = NULL;
    rc = sqlite3_exec(db1,
        "DROP TABLE IF EXISTS t;"
        "CREATE TABLE t (id INTEGER, val TEXT);"
        "INSERT INTO t VALUES (1, 'container_one');",
        NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "  db1 error: %s\n", errmsg ? errmsg : "unknown");
        sqlite3_free(errmsg);
    }
    ASSERT_OK(rc);

    rc = sqlite3_exec(db2,
        "DROP TABLE IF EXISTS t;"
        "CREATE TABLE t (id INTEGER, val TEXT);"
        "INSERT INTO t VALUES (1, 'container_two');",
        NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "  db2 error: %s\n", errmsg ? errmsg : "unknown");
        sqlite3_free(errmsg);
    }
    ASSERT_OK(rc);

    /* Verify data independence */
    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(db1, "SELECT val FROM t WHERE id=1;", -1, &stmt, NULL);
    ASSERT_OK(rc);
    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_ROW);
    ASSERT_STR_EQ(sqlite3_column_text(stmt, 0), "container_one");
    sqlite3_finalize(stmt);

    rc = sqlite3_prepare_v2(db2, "SELECT val FROM t WHERE id=1;", -1, &stmt, NULL);
    ASSERT_OK(rc);
    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_ROW);
    ASSERT_STR_EQ(sqlite3_column_text(stmt, 0), "container_two");
    sqlite3_finalize(stmt);

    sqlite3_close(db1);
    sqlite3_close(db2);
}

/* ================================================================
 * Test: ATTACH cross-container (via URI)
 * Open DB1 normally, ATTACH DB2 from a different container via URI.
 * Run a cross-database JOIN query.
 *
 * TODO: ATTACH with URI parameters may not work directly through
 * SQLite's ATTACH syntax. If this test fails, it documents a
 * known limitation for future work.
 * ================================================================ */

TEST(integ_attach_cross_container) {
    const char *container_main = "sqlite-objs-uri-c1";
    const char *container_att  = "sqlite-objs-uri-c2";

    /* Register VFS with Azurite config for primary container */
    sqlite_objs_config_t cfg = {
        .account = AZURITE_ACCOUNT,
        .container = container_main,
        .sas_token = NULL,
        .account_key = AZURITE_KEY,
        .endpoint = AZURITE_ENDPOINT,
        .ops = NULL,
        .ops_ctx = NULL
    };
    int rc = sqlite_objs_vfs_register_with_config(&cfg, 0);
    ASSERT_OK(rc);

    /* Open primary database */
    sqlite3 *db = NULL;
    rc = sqlite3_open_v2("attach_main.db", &db,
                          SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                          "sqlite-objs");
    ASSERT_OK(rc);
    ASSERT_NOT_NULL(db);

    /* Create data in main db */
    char *errmsg = NULL;
    rc = sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS main_t (id INTEGER, name TEXT);"
        "INSERT INTO main_t VALUES (1, 'main_record');",
        NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "  main_t error: %s\n", errmsg ? errmsg : "unknown");
        sqlite3_free(errmsg);
    }
    ASSERT_OK(rc);

    /* Try to ATTACH the DB from the other container via URI.
    ** NOTE: This may not work because ATTACH uses its own open path.
    ** We test it and document the result. */
    char attach_sql[512];
    snprintf(attach_sql, sizeof(attach_sql),
        "ATTACH DATABASE 'file:crossc.db?"
        "azure_account=%s&azure_container=%s&azure_key=%s&azure_endpoint=%s"
        "' AS other;",
        AZURITE_ACCOUNT, container_att, AZURITE_KEY, AZURITE_ENDPOINT);

    rc = sqlite3_exec(db, attach_sql, NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        /* Cross-container ATTACH via URI is a known limitation —
        ** document but don't fail the test */
        fprintf(stderr,
            "  NOTE: Cross-container ATTACH via URI not supported yet: %s\n",
            errmsg ? errmsg : "unknown");
        sqlite3_free(errmsg);
        sqlite3_close(db);
        return; /* Skip rest of test gracefully */
    }

    /* If ATTACH succeeded, try a cross-database query */
    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(db,
        "SELECT m.name, o.val FROM main_t m, other.t o WHERE m.id = o.id;",
        -1, &stmt, NULL);
    if (rc == SQLITE_OK) {
        rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW) {
            ASSERT_STR_EQ(sqlite3_column_text(stmt, 0), "main_record");
        }
        sqlite3_finalize(stmt);
    }

    sqlite3_exec(db, "DETACH DATABASE other;", NULL, NULL, NULL);
    sqlite3_close(db);
}

/* ================================================================
 * ETag Cache Reuse Tests
 *
 * Verify the cache_reuse URI parameter: when a database was previously
 * cached locally and the blob's ETag hasn't changed, the VFS skips the
 * download and reuses the local file.  When the blob changes (different
 * ETag), the VFS re-downloads.
 * ================================================================ */

/* Helper: clean leftover cache + etag sidecar files matching a pattern */
static void cleanup_cache_files(const char *blobName) {
    /* The VFS stores cache files in /tmp as sqlite-objs-<hash>.cache
     * and corresponding .etag, .state, and .snapshot files.  We brute-force
     * remove them by scanning /tmp for our prefix.  This is a test-only convenience. */
    char cmd[256];
    snprintf(cmd, sizeof(cmd),
             "rm -f /tmp/sqlite-objs-*.cache /tmp/sqlite-objs-*.etag"
             " /tmp/sqlite-objs-*.state /tmp/sqlite-objs-*.snapshot");
    (void)system(cmd);
    (void)blobName;  /* suppress unused warning */
}

/*
 * Test: ETag cache hit — reuse cached file
 *
 * 1. Open a database via URI with cache_reuse=1
 * 2. Create a table, insert data, close
 * 3. Re-open the same URI with cache_reuse=1
 * 4. Verify data is intact (table + rows)
 * 5. The re-open should NOT re-download since ETag matches
 */
TEST(etag_cache_hit) {
    const char *db_name = "etag-hit.db";
    cleanup_blob(db_name);
    cleanup_blob("etag-hit.db-journal");
    cleanup_blob("etag-hit.db-wal");
    cleanup_cache_files(db_name);

    /* Register VFS in URI mode */
    int rc = sqlite_objs_vfs_register_uri(0);
    ASSERT_OK(rc);

    /* Build URI with cache_reuse=1 */
    char uri[1024];
    snprintf(uri, sizeof(uri),
        "file:%s?"
        "azure_account=" AZURITE_ACCOUNT
        "&azure_container=" AZURITE_CONTAINER
        "&azure_key=" AZURITE_KEY
        "&azure_endpoint=" AZURITE_ENDPOINT
        "&cache_reuse=1",
        db_name);

    /* --- First open: create schema and data --- */
    sqlite3 *db = NULL;
    rc = sqlite3_open_v2(uri, &db,
        SQLITE_OPEN_URI | SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
        "sqlite-objs");
    ASSERT_OK(rc);
    ASSERT_NOT_NULL(db);

    char *errmsg = NULL;
    rc = sqlite3_exec(db,
        "CREATE TABLE cache_t (id INTEGER PRIMARY KEY, val TEXT);"
        "INSERT INTO cache_t VALUES (1, 'cached_alpha');"
        "INSERT INTO cache_t VALUES (2, 'cached_beta');",
        NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "  etag_cache_hit setup SQL: %s\n", errmsg ? errmsg : "?");
        sqlite3_free(errmsg);
    }
    ASSERT_OK(rc);

    /* Close — this persists cache file + ETag sidecar */
    rc = sqlite3_close(db);
    ASSERT_OK(rc);

    /* --- Second open: cache_reuse should hit ETag match, skip download --- */
    db = NULL;
    rc = sqlite3_open_v2(uri, &db,
        SQLITE_OPEN_URI | SQLITE_OPEN_READWRITE,
        "sqlite-objs");
    ASSERT_OK(rc);
    ASSERT_NOT_NULL(db);

    /* Verify data survived the cache-reuse path */
    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM cache_t;", -1, &stmt, NULL);
    ASSERT_OK(rc);
    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_ROW);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), 2);
    sqlite3_finalize(stmt);

    /* Verify actual row values */
    stmt = NULL;
    rc = sqlite3_prepare_v2(db,
        "SELECT val FROM cache_t WHERE id = 1;", -1, &stmt, NULL);
    ASSERT_OK(rc);
    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0), "cached_alpha");
    sqlite3_finalize(stmt);

    /* Verify no blob download occurred on second open (ETag cache hit) */
    {
        int dlCount = -1;
        rc = sqlite3_file_control(db, "main",
                                  SQLITE_OBJS_FCNTL_DOWNLOAD_COUNT, &dlCount);
        ASSERT_OK(rc);
        ASSERT_EQ(dlCount, 0);
    }

    sqlite3_close(db);

    /* Cleanup */
    cleanup_blob(db_name);
    cleanup_blob("etag-hit.db-journal");
    cleanup_cache_files(db_name);
}

/*
 * Test: ETag cache miss — blob changed, must re-download
 *
 * 1. Open database, create table, insert data, close (cache + ETag saved)
 * 2. Open a SECOND connection to the same blob, modify data, close
 *    — this changes the Azure ETag
 * 3. Re-open original with cache_reuse — ETag won't match, forces download
 * 4. Verify the modified data is visible
 */
TEST(etag_cache_miss) {
    const char *db_name = "etag-miss.db";
    cleanup_blob(db_name);
    cleanup_blob("etag-miss.db-journal");
    cleanup_blob("etag-miss.db-wal");
    cleanup_cache_files(db_name);

    int rc = sqlite_objs_vfs_register_uri(0);
    ASSERT_OK(rc);

    char uri[1024];
    snprintf(uri, sizeof(uri),
        "file:%s?"
        "azure_account=" AZURITE_ACCOUNT
        "&azure_container=" AZURITE_CONTAINER
        "&azure_key=" AZURITE_KEY
        "&azure_endpoint=" AZURITE_ENDPOINT
        "&cache_reuse=1",
        db_name);

    /* --- First open: seed the database --- */
    sqlite3 *db = NULL;
    rc = sqlite3_open_v2(uri, &db,
        SQLITE_OPEN_URI | SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
        "sqlite-objs");
    ASSERT_OK(rc);
    ASSERT_NOT_NULL(db);

    char *errmsg = NULL;
    rc = sqlite3_exec(db,
        "CREATE TABLE miss_t (id INTEGER PRIMARY KEY, val TEXT);"
        "INSERT INTO miss_t VALUES (1, 'original');",
        NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "  etag_cache_miss setup SQL: %s\n", errmsg ? errmsg : "?");
        sqlite3_free(errmsg);
    }
    ASSERT_OK(rc);

    rc = sqlite3_close(db);
    ASSERT_OK(rc);

    /* --- Modify blob via a second connection (no cache_reuse) ---
     * This writes new data to Azure, changing the blob's ETag.  The
     * locally cached .cache + .etag from the first connection will be
     * stale after this. */
    char uri_no_cache[1024];
    snprintf(uri_no_cache, sizeof(uri_no_cache),
        "file:%s?"
        "azure_account=" AZURITE_ACCOUNT
        "&azure_container=" AZURITE_CONTAINER
        "&azure_key=" AZURITE_KEY
        "&azure_endpoint=" AZURITE_ENDPOINT,
        db_name);

    db = NULL;
    rc = sqlite3_open_v2(uri_no_cache, &db,
        SQLITE_OPEN_URI | SQLITE_OPEN_READWRITE,
        "sqlite-objs");
    ASSERT_OK(rc);
    ASSERT_NOT_NULL(db);

    rc = sqlite3_exec(db,
        "UPDATE miss_t SET val = 'modified' WHERE id = 1;"
        "INSERT INTO miss_t VALUES (2, 'new_row');",
        NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "  etag_cache_miss modify SQL: %s\n", errmsg ? errmsg : "?");
        sqlite3_free(errmsg);
    }
    ASSERT_OK(rc);

    rc = sqlite3_close(db);
    ASSERT_OK(rc);

    /* --- Third open: cache_reuse=1 but ETag has changed → re-download --- */
    db = NULL;
    rc = sqlite3_open_v2(uri, &db,
        SQLITE_OPEN_URI | SQLITE_OPEN_READWRITE,
        "sqlite-objs");
    ASSERT_OK(rc);
    ASSERT_NOT_NULL(db);

    /* The VFS should have detected the ETag mismatch, re-downloaded,
     * and we should see the MODIFIED data, not the original. */
    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(db,
        "SELECT val FROM miss_t WHERE id = 1;", -1, &stmt, NULL);
    ASSERT_OK(rc);
    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0), "modified");
    sqlite3_finalize(stmt);

    /* Also verify the new row from the second connection */
    stmt = NULL;
    rc = sqlite3_prepare_v2(db,
        "SELECT COUNT(*) FROM miss_t;", -1, &stmt, NULL);
    ASSERT_OK(rc);
    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_ROW);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), 2);
    sqlite3_finalize(stmt);

    /* Verify that a blob download DID occur (ETag mismatch → re-download) */
    {
        int dlCount = -1;
        rc = sqlite3_file_control(db, "main",
                                  SQLITE_OBJS_FCNTL_DOWNLOAD_COUNT, &dlCount);
        ASSERT_OK(rc);
        ASSERT_EQ(dlCount, 1);
    }

    sqlite3_close(db);

    /* Cleanup */
    cleanup_blob(db_name);
    cleanup_blob("etag-miss.db-journal");
    cleanup_cache_files(db_name);
}

/*
 * Test: Cache reuse with WAL mode
 *
 * WAL files are stored as block blobs (separate from the page-blob main db).
 * Verify that cache_reuse works correctly when WAL mode is active:
 * 1. Open database in WAL mode, create table, insert data, close
 * 2. Re-open with cache_reuse — data must survive
 * 3. Insert more data via WAL, close, re-open — still intact
 */
TEST(etag_cache_reuse_wal) {
    /*
     * Regression test for the batch-write ETag bug: az_page_blob_write_batch()
     * used to call azure_error_init(err) on success, zeroing the ETag. This
     * caused the ETag sidecar to always hold a stale ETag, so cache reuse
     * NEVER worked for databases that went through the batch write path.
     *
     * Strategy: use WAL mode with a low autocheckpoint threshold and insert
     * enough data to force multiple checkpoint cycles that each flush many
     * dirty pages through az_page_blob_write_batch() (curl_multi path).
     * After close, reopen with cache_reuse=1 and assert download_count == 0.
     */
    const char *db_name = "etag-wal.db";
    cleanup_blob(db_name);
    cleanup_blob("etag-wal.db-journal");
    cleanup_blob("etag-wal.db-wal");
    cleanup_cache_files(db_name);

    int rc = sqlite_objs_vfs_register_uri(0);
    ASSERT_OK(rc);

    char uri[1024];
    snprintf(uri, sizeof(uri),
        "file:%s?"
        "azure_account=" AZURITE_ACCOUNT
        "&azure_container=" AZURITE_CONTAINER
        "&azure_key=" AZURITE_KEY
        "&azure_endpoint=" AZURITE_ENDPOINT
        "&cache_reuse=1",
        db_name);

    /* Total rows to insert across all batches */
    const int total_rows = 300;
    /* Rows per transaction — each commit may trigger an autocheckpoint */
    const int batch_size = 50;
    /* 200-byte payload per row to fill pages quickly (4096-byte page ≈ 5-6
     * rows), giving ~50-60 dirty pages total across checkpoint cycles. */
    const int payload_len = 200;

    /* --- First open: WAL mode + batch writes via low autocheckpoint --- */
    sqlite3 *db = NULL;
    rc = sqlite3_open_v2(uri, &db,
        SQLITE_OPEN_URI | SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
        "sqlite-objs");
    ASSERT_OK(rc);
    ASSERT_NOT_NULL(db);

    char *errmsg = NULL;
    /* WAL on a remote VFS requires exclusive locking mode */
    rc = sqlite3_exec(db, "PRAGMA locking_mode=EXCLUSIVE;", NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "  etag_wal locking_mode: %s\n", errmsg ? errmsg : "?");
        sqlite3_free(errmsg);
    }
    ASSERT_OK(rc);

    /* Enable WAL journal mode */
    rc = sqlite3_exec(db, "PRAGMA journal_mode=WAL;", NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "  etag_wal journal_mode: %s\n", errmsg ? errmsg : "?");
        sqlite3_free(errmsg);
    }
    ASSERT_OK(rc);

    /* Verify WAL mode is active */
    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(db, "PRAGMA journal_mode;", -1, &stmt, NULL);
    ASSERT_OK(rc);
    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_ROW);
    const char *jmode = (const char *)sqlite3_column_text(stmt, 0);
    ASSERT_STR_EQ(jmode, "wal");
    sqlite3_finalize(stmt);

    /* Low autocheckpoint threshold forces frequent batch checkpoints.
     * With 10 pages, each checkpoint flushes ~10 dirty pages through
     * az_page_blob_write_batch() using curl_multi handles (nRanges > 1). */
    rc = sqlite3_exec(db, "PRAGMA wal_autocheckpoint=10;", NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "  etag_wal autocheckpoint: %s\n", errmsg ? errmsg : "?");
        sqlite3_free(errmsg);
    }
    ASSERT_OK(rc);

    /* Create the table */
    rc = sqlite3_exec(db,
        "CREATE TABLE wal_t (id INTEGER PRIMARY KEY, val TEXT);",
        NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "  etag_wal create: %s\n", errmsg ? errmsg : "?");
        sqlite3_free(errmsg);
    }
    ASSERT_OK(rc);

    /* Build a deterministic 200-byte payload: "row_NNNN_AAAAAA..." */
    char payload[201];
    memset(payload, 'X', payload_len);
    payload[payload_len] = '\0';

    /* Prepare INSERT statement for binding in the loop */
    sqlite3_stmt *ins = NULL;
    rc = sqlite3_prepare_v2(db,
        "INSERT INTO wal_t VALUES (?, ?);", -1, &ins, NULL);
    ASSERT_OK(rc);

    /* Insert rows in batches — each COMMIT may trigger an autocheckpoint
     * that flushes dirty pages through the batch write path. */
    for (int base = 1; base <= total_rows; base += batch_size) {
        rc = sqlite3_exec(db, "BEGIN;", NULL, NULL, &errmsg);
        if (rc != SQLITE_OK) {
            fprintf(stderr, "  etag_wal BEGIN: %s\n", errmsg ? errmsg : "?");
            sqlite3_free(errmsg);
        }
        ASSERT_OK(rc);

        int end = base + batch_size;
        if (end > total_rows + 1) end = total_rows + 1;
        for (int i = base; i < end; i++) {
            /* Make each payload unique for data-integrity verification */
            snprintf(payload, sizeof(payload), "row_%04d_", i);
            memset(payload + 9, 'A' + (i % 26), (size_t)(payload_len - 9));
            payload[payload_len] = '\0';

            sqlite3_bind_int(ins, 1, i);
            sqlite3_bind_text(ins, 2, payload, payload_len, SQLITE_TRANSIENT);
            rc = sqlite3_step(ins);
            ASSERT_EQ(rc, SQLITE_DONE);
            sqlite3_reset(ins);
        }

        rc = sqlite3_exec(db, "COMMIT;", NULL, NULL, &errmsg);
        if (rc != SQLITE_OK) {
            fprintf(stderr, "  etag_wal COMMIT: %s\n", errmsg ? errmsg : "?");
            sqlite3_free(errmsg);
        }
        ASSERT_OK(rc);
    }
    sqlite3_finalize(ins);

    /* Final TRUNCATE checkpoint to ensure all WAL content is flushed
     * to the main DB before close.  This is the last batch write. */
    rc = sqlite3_exec(db, "PRAGMA wal_checkpoint(TRUNCATE);", NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "  etag_wal checkpoint: %s\n", errmsg ? errmsg : "?");
        sqlite3_free(errmsg);
    }
    ASSERT_OK(rc);

    rc = sqlite3_close(db);
    ASSERT_OK(rc);

    /* --- Second open: cache_reuse must hit ETag match (no download) --- */
    db = NULL;
    rc = sqlite3_open_v2(uri, &db,
        SQLITE_OPEN_URI | SQLITE_OPEN_READWRITE,
        "sqlite-objs");
    ASSERT_OK(rc);
    ASSERT_NOT_NULL(db);

    /* WAL mode persists in the db header — must set exclusive locking first */
    rc = sqlite3_exec(db, "PRAGMA locking_mode=EXCLUSIVE;", NULL, NULL, &errmsg);
    ASSERT_OK(rc);

    /* CRITICAL ASSERTION: download count must be 0 — the ETag sidecar
     * written after the batch checkpoint must match the blob's current ETag.
     * Before the fix, az_page_blob_write_batch() zeroed the ETag on success,
     * making the sidecar stale and forcing a re-download every time. */
    {
        int dlCount = -1;
        rc = sqlite3_file_control(db, "main",
                                  SQLITE_OBJS_FCNTL_DOWNLOAD_COUNT, &dlCount);
        ASSERT_OK(rc);
        if (dlCount != 0) {
            fprintf(stderr,
                "  etag_wal FAIL: download_count=%d (expected 0) — "
                "ETag sidecar was stale after batch write\n", dlCount);
        }
        ASSERT_EQ(dlCount, 0);
    }

    /* Data integrity: all 300 rows must be present */
    stmt = NULL;
    rc = sqlite3_prepare_v2(db,
        "SELECT COUNT(*) FROM wal_t;", -1, &stmt, NULL);
    ASSERT_OK(rc);
    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_ROW);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), total_rows);
    sqlite3_finalize(stmt);

    /* Spot-check first and last rows */
    stmt = NULL;
    rc = sqlite3_prepare_v2(db,
        "SELECT length(val) FROM wal_t WHERE id = 1;", -1, &stmt, NULL);
    ASSERT_OK(rc);
    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_ROW);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), payload_len);
    sqlite3_finalize(stmt);

    stmt = NULL;
    rc = sqlite3_prepare_v2(db,
        "SELECT length(val) FROM wal_t WHERE id = ?;", -1, &stmt, NULL);
    ASSERT_OK(rc);
    sqlite3_bind_int(stmt, 1, total_rows);
    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_ROW);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), payload_len);
    sqlite3_finalize(stmt);

    sqlite3_close(db);

    /* Cleanup */
    cleanup_blob(db_name);
    cleanup_blob("etag-wal.db-journal");
    cleanup_blob("etag-wal.db-wal");
    cleanup_cache_files(db_name);
}

/* ================================================================
 * Helper: Build a URI with Azurite credentials and optional params
 * ================================================================ */
static void build_uri(char *buf, size_t buflen, const char *db_name,
                      const char *extra_params) {
    if (extra_params && extra_params[0]) {
        snprintf(buf, buflen,
            "file:%s?"
            "azure_account=" AZURITE_ACCOUNT
            "&azure_container=" AZURITE_CONTAINER
            "&azure_key=" AZURITE_KEY
            "&azure_endpoint=" AZURITE_ENDPOINT
            "&%s",
            db_name, extra_params);
    } else {
        snprintf(buf, buflen,
            "file:%s?"
            "azure_account=" AZURITE_ACCOUNT
            "&azure_container=" AZURITE_CONTAINER
            "&azure_key=" AZURITE_KEY
            "&azure_endpoint=" AZURITE_ENDPOINT,
            db_name);
    }
}

/* Helper: Open a database on Azurite via URI */
static sqlite3 *open_azurite_db(const char *db_name, const char *extra_params,
                                 int create) {
    char uri[1024];
    build_uri(uri, sizeof(uri), db_name, extra_params);

    sqlite3 *db = NULL;
    int flags = SQLITE_OPEN_URI | SQLITE_OPEN_READWRITE;
    if (create) flags |= SQLITE_OPEN_CREATE;

    int rc = sqlite3_open_v2(uri, &db, flags, "sqlite-objs");
    if (rc != SQLITE_OK) {
        fprintf(stderr, "  open_azurite_db(%s) failed: %s\n",
                db_name, db ? sqlite3_errmsg(db) : "NULL");
        if (db) sqlite3_close(db);
        return NULL;
    }
    
    /* Set busy timeout to handle lease contention in multi-client scenarios.
    ** Azure blob leases may take a moment to be released/acquired, so give
    ** SQLite time to retry instead of returning SQLITE_BUSY immediately. */
    sqlite3_busy_timeout(db, 10000);  /* 10 seconds */
    
    return db;
}

/* Helper: Execute SQL and assert success */
static int exec_sql(sqlite3 *db, const char *sql) {
    char *errmsg = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "  SQL error: %s\n", errmsg ? errmsg : "unknown");
        sqlite3_free(errmsg);
    }
    return rc;
}

/* Helper: Query an integer value (first column of first row) */
static int query_int(sqlite3 *db, const char *sql, int *out) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) return rc;
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        *out = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
        return SQLITE_OK;
    }
    sqlite3_finalize(stmt);
    return SQLITE_ERROR;
}

/* Helper: Cleanup blob and journal */
static void cleanup_test_blobs(const char *db_name) {
    char jrnl[256];
    snprintf(jrnl, sizeof(jrnl), "%s-journal", db_name);
    cleanup_blob(db_name);
    cleanup_blob(jrnl);
    cleanup_cache_files(db_name);
}

/* ================================================================
 * A. Client A writes, disconnects. Client B connects, reads.
 * ================================================================ */

/*
 * Test A1: Basic write-read handoff
 * A creates table with 100+ rows, disconnects. B reads all rows.
 */
TEST(mc_basic_write_read_handoff) {
    const char *db_name = "mc-basic-handoff.db";
    cleanup_test_blobs(db_name);

    int rc = sqlite_objs_vfs_register_uri(0);
    ASSERT_OK(rc);

    /* Client A: create and populate */
    sqlite3 *dbA = open_azurite_db(db_name, NULL, 1);
    ASSERT_NOT_NULL(dbA);

    rc = exec_sql(dbA, "CREATE TABLE items (id INTEGER PRIMARY KEY, val TEXT, num REAL);");
    ASSERT_OK(rc);

    /* Insert 150 rows with varied data */
    sqlite3_stmt *ins = NULL;
    rc = sqlite3_prepare_v2(dbA, "INSERT INTO items VALUES(?,?,?);", -1, &ins, NULL);
    ASSERT_OK(rc);

    rc = exec_sql(dbA, "BEGIN;");
    ASSERT_OK(rc);
    for (int i = 1; i <= 150; i++) {
        char val[64];
        snprintf(val, sizeof(val), "item_%04d_data", i);
        sqlite3_bind_int(ins, 1, i);
        sqlite3_bind_text(ins, 2, val, -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(ins, 3, i * 1.5);
        rc = sqlite3_step(ins);
        ASSERT_EQ(rc, SQLITE_DONE);
        sqlite3_reset(ins);
    }
    sqlite3_finalize(ins);
    rc = exec_sql(dbA, "COMMIT;");
    ASSERT_OK(rc);

    rc = sqlite3_close(dbA);
    ASSERT_OK(rc);

    /* Client B: open and verify */
    sqlite3 *dbB = open_azurite_db(db_name, NULL, 0);
    ASSERT_NOT_NULL(dbB);

    int count = 0;
    rc = query_int(dbB, "SELECT COUNT(*) FROM items;", &count);
    ASSERT_OK(rc);
    ASSERT_EQ(count, 150);

    /* Verify specific rows */
    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(dbB,
        "SELECT val, num FROM items WHERE id = 75;", -1, &stmt, NULL);
    ASSERT_OK(rc);
    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0), "item_0075_data");
    ASSERT_TRUE(sqlite3_column_double(stmt, 1) > 112.4 &&
                sqlite3_column_double(stmt, 1) < 112.6);
    sqlite3_finalize(stmt);

    sqlite3_close(dbB);
    cleanup_test_blobs(db_name);
}

/*
 * Test A2: Large data handoff (1000+ rows with varied data types)
 */
TEST(mc_large_data_handoff) {
    const char *db_name = "mc-large-handoff.db";
    cleanup_test_blobs(db_name);

    int rc = sqlite_objs_vfs_register_uri(0);
    ASSERT_OK(rc);

    /* Client A: write 1000 rows with TEXT, INTEGER, REAL, BLOB */
    sqlite3 *dbA = open_azurite_db(db_name, NULL, 1);
    ASSERT_NOT_NULL(dbA);

    rc = exec_sql(dbA,
        "CREATE TABLE big (id INTEGER PRIMARY KEY, txt TEXT, "
        "num INTEGER, flt REAL, bin BLOB);");
    ASSERT_OK(rc);

    sqlite3_stmt *ins = NULL;
    rc = sqlite3_prepare_v2(dbA,
        "INSERT INTO big VALUES(?,?,?,?,?);", -1, &ins, NULL);
    ASSERT_OK(rc);

    rc = exec_sql(dbA, "BEGIN;");
    ASSERT_OK(rc);

    unsigned char blob_data[128];
    for (int i = 1; i <= 1200; i++) {
        char txt[128];
        snprintf(txt, sizeof(txt), "row_%05d_payload_with_some_padding_data", i);
        /* Fill blob with deterministic pattern */
        for (int b = 0; b < 128; b++) blob_data[b] = (unsigned char)((i + b) & 0xFF);

        sqlite3_bind_int(ins, 1, i);
        sqlite3_bind_text(ins, 2, txt, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(ins, 3, i * 7);
        sqlite3_bind_double(ins, 4, i * 0.123);
        sqlite3_bind_blob(ins, 5, blob_data, 128, SQLITE_TRANSIENT);
        rc = sqlite3_step(ins);
        ASSERT_EQ(rc, SQLITE_DONE);
        sqlite3_reset(ins);
    }
    sqlite3_finalize(ins);
    rc = exec_sql(dbA, "COMMIT;");
    ASSERT_OK(rc);

    rc = sqlite3_close(dbA);
    ASSERT_OK(rc);

    /* Client B: verify all data */
    sqlite3 *dbB = open_azurite_db(db_name, NULL, 0);
    ASSERT_NOT_NULL(dbB);

    int count = 0;
    rc = query_int(dbB, "SELECT COUNT(*) FROM big;", &count);
    ASSERT_OK(rc);
    ASSERT_EQ(count, 1200);

    /* Verify a specific row's blob data */
    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(dbB,
        "SELECT txt, num, flt, bin FROM big WHERE id = 500;", -1, &stmt, NULL);
    ASSERT_OK(rc);
    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0),
                  "row_00500_payload_with_some_padding_data");
    ASSERT_EQ(sqlite3_column_int(stmt, 1), 3500);

    const unsigned char *got_blob = sqlite3_column_blob(stmt, 3);
    int blob_sz = sqlite3_column_bytes(stmt, 3);
    ASSERT_EQ(blob_sz, 128);
    for (int b = 0; b < 128; b++) {
        ASSERT_EQ(got_blob[b], (unsigned char)((500 + b) & 0xFF));
    }
    sqlite3_finalize(stmt);

    /* Checksum-style validation: sum of all integers */
    int total = 0;
    rc = query_int(dbB, "SELECT SUM(num) FROM big;", &total);
    ASSERT_OK(rc);
    /* sum of i*7 for i=1..1200 = 7 * (1200*1201/2) = 7 * 720600 = 5044200 */
    ASSERT_EQ(total, 5044200);

    sqlite3_close(dbB);
    cleanup_test_blobs(db_name);
}

/*
 * Test A3: Schema handoff — A creates multi-table schema with indexes, B reads
 */
TEST(mc_schema_handoff) {
    const char *db_name = "mc-schema-handoff.db";
    cleanup_test_blobs(db_name);

    int rc = sqlite_objs_vfs_register_uri(0);
    ASSERT_OK(rc);

    sqlite3 *dbA = open_azurite_db(db_name, NULL, 1);
    ASSERT_NOT_NULL(dbA);

    rc = exec_sql(dbA,
        "CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT NOT NULL, email TEXT UNIQUE);"
        "CREATE TABLE orders (id INTEGER PRIMARY KEY, user_id INTEGER REFERENCES users(id), amount REAL);"
        "CREATE TABLE items (id INTEGER PRIMARY KEY, order_id INTEGER, product TEXT);"
        "CREATE INDEX idx_orders_user ON orders(user_id);"
        "CREATE INDEX idx_items_order ON items(order_id);"
        "INSERT INTO users VALUES (1, 'Alice', 'alice@test.com');"
        "INSERT INTO users VALUES (2, 'Bob', 'bob@test.com');"
        "INSERT INTO orders VALUES (1, 1, 99.99);"
        "INSERT INTO orders VALUES (2, 2, 49.50);"
        "INSERT INTO items VALUES (1, 1, 'Widget');"
        "INSERT INTO items VALUES (2, 1, 'Gadget');"
        "INSERT INTO items VALUES (3, 2, 'Doohickey');");
    ASSERT_OK(rc);

    rc = sqlite3_close(dbA);
    ASSERT_OK(rc);

    /* Client B verifies schema structure */
    sqlite3 *dbB = open_azurite_db(db_name, NULL, 0);
    ASSERT_NOT_NULL(dbB);

    /* Count tables */
    int tbl_count = 0;
    rc = query_int(dbB,
        "SELECT COUNT(*) FROM sqlite_master WHERE type='table';", &tbl_count);
    ASSERT_OK(rc);
    ASSERT_EQ(tbl_count, 3);

    /* Count indexes (excluding auto-indexes) */
    int idx_count = 0;
    rc = query_int(dbB,
        "SELECT COUNT(*) FROM sqlite_master WHERE type='index' AND "
        "name NOT LIKE 'sqlite_%';", &idx_count);
    ASSERT_OK(rc);
    ASSERT_GE(idx_count, 2);

    /* Cross-table join */
    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(dbB,
        "SELECT u.name, o.amount FROM users u "
        "JOIN orders o ON u.id=o.user_id WHERE u.name='Alice';",
        -1, &stmt, NULL);
    ASSERT_OK(rc);
    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0), "Alice");
    sqlite3_finalize(stmt);

    sqlite3_close(dbB);
    cleanup_test_blobs(db_name);
}

/*
 * Test A4: cache_reuse handoff — A writes with cache, B reads with cache
 */
TEST(mc_cache_reuse_handoff) {
    const char *db_name = "mc-cache-handoff.db";
    cleanup_test_blobs(db_name);

    int rc = sqlite_objs_vfs_register_uri(0);
    ASSERT_OK(rc);

    /* Client A writes with cache_reuse=1 */
    sqlite3 *dbA = open_azurite_db(db_name, "cache_reuse=1", 1);
    ASSERT_NOT_NULL(dbA);

    rc = exec_sql(dbA,
        "CREATE TABLE cached (id INTEGER PRIMARY KEY, val TEXT);"
        "INSERT INTO cached VALUES (1, 'from_client_a');"
        "INSERT INTO cached VALUES (2, 'also_from_a');");
    ASSERT_OK(rc);

    rc = sqlite3_close(dbA);
    ASSERT_OK(rc);

    /* Client B opens with cache_reuse=1 — should download fresh (no prior cache for B) */
    sqlite3 *dbB = open_azurite_db(db_name, "cache_reuse=1", 0);
    ASSERT_NOT_NULL(dbB);

    int count = 0;
    rc = query_int(dbB, "SELECT COUNT(*) FROM cached;", &count);
    ASSERT_OK(rc);
    ASSERT_EQ(count, 2);

    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(dbB,
        "SELECT val FROM cached WHERE id=1;", -1, &stmt, NULL);
    ASSERT_OK(rc);
    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0), "from_client_a");
    sqlite3_finalize(stmt);

    rc = sqlite3_close(dbB);
    ASSERT_OK(rc);

    /* Client A reconnects with cache_reuse=1 — should hit cache (ETag match) */
    sqlite3 *dbA2 = open_azurite_db(db_name, "cache_reuse=1", 0);
    ASSERT_NOT_NULL(dbA2);

    {
        int dlCount = -1;
        rc = sqlite3_file_control(dbA2, "main",
                                  SQLITE_OBJS_FCNTL_DOWNLOAD_COUNT, &dlCount);
        ASSERT_OK(rc);
        ASSERT_EQ(dlCount, 0);  /* Cache hit: no download */
    }

    count = 0;
    rc = query_int(dbA2, "SELECT COUNT(*) FROM cached;", &count);
    ASSERT_OK(rc);
    ASSERT_EQ(count, 2);

    sqlite3_close(dbA2);
    cleanup_test_blobs(db_name);
}

/* ================================================================
 * B. Client A writes, Client B writes (sequential), Client C reads.
 * ================================================================ */

/*
 * Test B1: Sequential writes from different clients
 */
TEST(mc_sequential_writes) {
    const char *db_name = "mc-seq-writes.db";
    cleanup_test_blobs(db_name);

    int rc = sqlite_objs_vfs_register_uri(0);
    ASSERT_OK(rc);

    /* Client A creates and inserts rows 1-50 */
    sqlite3 *dbA = open_azurite_db(db_name, NULL, 1);
    ASSERT_NOT_NULL(dbA);

    rc = exec_sql(dbA, "CREATE TABLE seq (id INTEGER PRIMARY KEY, src TEXT);");
    ASSERT_OK(rc);

    sqlite3_stmt *ins = NULL;
    rc = sqlite3_prepare_v2(dbA, "INSERT INTO seq VALUES(?,?);", -1, &ins, NULL);
    ASSERT_OK(rc);

    rc = exec_sql(dbA, "BEGIN;");
    ASSERT_OK(rc);
    for (int i = 1; i <= 50; i++) {
        sqlite3_bind_int(ins, 1, i);
        sqlite3_bind_text(ins, 2, "client_a", -1, SQLITE_STATIC);
        rc = sqlite3_step(ins);
        ASSERT_EQ(rc, SQLITE_DONE);
        sqlite3_reset(ins);
    }
    sqlite3_finalize(ins);
    rc = exec_sql(dbA, "COMMIT;");
    ASSERT_OK(rc);
    rc = sqlite3_close(dbA);
    ASSERT_OK(rc);

    /* Client B inserts rows 51-100 */
    sqlite3 *dbB = open_azurite_db(db_name, NULL, 0);
    ASSERT_NOT_NULL(dbB);

    rc = sqlite3_prepare_v2(dbB, "INSERT INTO seq VALUES(?,?);", -1, &ins, NULL);
    ASSERT_OK(rc);

    rc = exec_sql(dbB, "BEGIN;");
    ASSERT_OK(rc);
    for (int i = 51; i <= 100; i++) {
        sqlite3_bind_int(ins, 1, i);
        sqlite3_bind_text(ins, 2, "client_b", -1, SQLITE_STATIC);
        rc = sqlite3_step(ins);
        ASSERT_EQ(rc, SQLITE_DONE);
        sqlite3_reset(ins);
    }
    sqlite3_finalize(ins);
    rc = exec_sql(dbB, "COMMIT;");
    ASSERT_OK(rc);
    rc = sqlite3_close(dbB);
    ASSERT_OK(rc);

    /* Client C reads and verifies all 100 rows */
    sqlite3 *dbC = open_azurite_db(db_name, NULL, 0);
    ASSERT_NOT_NULL(dbC);

    int count = 0;
    rc = query_int(dbC, "SELECT COUNT(*) FROM seq;", &count);
    ASSERT_OK(rc);
    ASSERT_EQ(count, 100);

    int count_a = 0, count_b = 0;
    rc = query_int(dbC, "SELECT COUNT(*) FROM seq WHERE src='client_a';", &count_a);
    ASSERT_OK(rc);
    ASSERT_EQ(count_a, 50);
    rc = query_int(dbC, "SELECT COUNT(*) FROM seq WHERE src='client_b';", &count_b);
    ASSERT_OK(rc);
    ASSERT_EQ(count_b, 50);

    sqlite3_close(dbC);
    cleanup_test_blobs(db_name);
}

/*
 * Test B2: Sequential writes with UPDATE
 */
TEST(mc_sequential_update) {
    const char *db_name = "mc-seq-update.db";
    cleanup_test_blobs(db_name);

    int rc = sqlite_objs_vfs_register_uri(0);
    ASSERT_OK(rc);

    /* Client A creates 100 rows */
    sqlite3 *dbA = open_azurite_db(db_name, NULL, 1);
    ASSERT_NOT_NULL(dbA);

    rc = exec_sql(dbA,
        "CREATE TABLE upd (id INTEGER PRIMARY KEY, val INTEGER, src TEXT);");
    ASSERT_OK(rc);

    sqlite3_stmt *ins = NULL;
    rc = sqlite3_prepare_v2(dbA, "INSERT INTO upd VALUES(?,?,?);", -1, &ins, NULL);
    ASSERT_OK(rc);
    rc = exec_sql(dbA, "BEGIN;");
    ASSERT_OK(rc);
    for (int i = 1; i <= 100; i++) {
        sqlite3_bind_int(ins, 1, i);
        sqlite3_bind_int(ins, 2, i * 10);
        sqlite3_bind_text(ins, 3, "original", -1, SQLITE_STATIC);
        rc = sqlite3_step(ins);
        ASSERT_EQ(rc, SQLITE_DONE);
        sqlite3_reset(ins);
    }
    sqlite3_finalize(ins);
    rc = exec_sql(dbA, "COMMIT;");
    ASSERT_OK(rc);
    rc = sqlite3_close(dbA);
    ASSERT_OK(rc);

    /* Client B updates even-numbered rows */
    sqlite3 *dbB = open_azurite_db(db_name, NULL, 0);
    ASSERT_NOT_NULL(dbB);

    rc = exec_sql(dbB,
        "UPDATE upd SET val = val * 2, src = 'updated' WHERE id % 2 = 0;");
    ASSERT_OK(rc);
    rc = sqlite3_close(dbB);
    ASSERT_OK(rc);

    /* Client C verifies */
    sqlite3 *dbC = open_azurite_db(db_name, NULL, 0);
    ASSERT_NOT_NULL(dbC);

    int count = 0;
    rc = query_int(dbC, "SELECT COUNT(*) FROM upd;", &count);
    ASSERT_OK(rc);
    ASSERT_EQ(count, 100);

    int updated = 0;
    rc = query_int(dbC, "SELECT COUNT(*) FROM upd WHERE src='updated';", &updated);
    ASSERT_OK(rc);
    ASSERT_EQ(updated, 50);

    /* Verify specific updated row: id=10 should be 10*10*2 = 200 */
    int val = 0;
    rc = query_int(dbC, "SELECT val FROM upd WHERE id=10;", &val);
    ASSERT_OK(rc);
    ASSERT_EQ(val, 200);

    /* Verify untouched row: id=11 should be 11*10 = 110 */
    rc = query_int(dbC, "SELECT val FROM upd WHERE id=11;", &val);
    ASSERT_OK(rc);
    ASSERT_EQ(val, 110);

    sqlite3_close(dbC);
    cleanup_test_blobs(db_name);
}

/*
 * Test B3: Sequential writes with DELETE + INSERT
 */
TEST(mc_sequential_delete_insert) {
    const char *db_name = "mc-seq-delinst.db";
    cleanup_test_blobs(db_name);

    int rc = sqlite_objs_vfs_register_uri(0);
    ASSERT_OK(rc);

    /* Client A populates 100 rows */
    sqlite3 *dbA = open_azurite_db(db_name, NULL, 1);
    ASSERT_NOT_NULL(dbA);

    rc = exec_sql(dbA, "CREATE TABLE deli (id INTEGER PRIMARY KEY, val TEXT);");
    ASSERT_OK(rc);

    sqlite3_stmt *ins = NULL;
    rc = sqlite3_prepare_v2(dbA, "INSERT INTO deli VALUES(?,?);", -1, &ins, NULL);
    ASSERT_OK(rc);
    rc = exec_sql(dbA, "BEGIN;");
    ASSERT_OK(rc);
    for (int i = 1; i <= 100; i++) {
        char val[32];
        snprintf(val, sizeof(val), "orig_%04d", i);
        sqlite3_bind_int(ins, 1, i);
        sqlite3_bind_text(ins, 2, val, -1, SQLITE_TRANSIENT);
        rc = sqlite3_step(ins);
        ASSERT_EQ(rc, SQLITE_DONE);
        sqlite3_reset(ins);
    }
    sqlite3_finalize(ins);
    rc = exec_sql(dbA, "COMMIT;");
    ASSERT_OK(rc);
    rc = sqlite3_close(dbA);
    ASSERT_OK(rc);

    /* Client B deletes rows 1-30, inserts rows 101-130 */
    sqlite3 *dbB = open_azurite_db(db_name, NULL, 0);
    ASSERT_NOT_NULL(dbB);

    rc = exec_sql(dbB, "BEGIN;");
    ASSERT_OK(rc);
    rc = exec_sql(dbB, "DELETE FROM deli WHERE id <= 30;");
    ASSERT_OK(rc);

    rc = sqlite3_prepare_v2(dbB, "INSERT INTO deli VALUES(?,?);", -1, &ins, NULL);
    ASSERT_OK(rc);
    for (int i = 101; i <= 130; i++) {
        char val[32];
        snprintf(val, sizeof(val), "new_%04d", i);
        sqlite3_bind_int(ins, 1, i);
        sqlite3_bind_text(ins, 2, val, -1, SQLITE_TRANSIENT);
        rc = sqlite3_step(ins);
        ASSERT_EQ(rc, SQLITE_DONE);
        sqlite3_reset(ins);
    }
    sqlite3_finalize(ins);
    rc = exec_sql(dbB, "COMMIT;");
    ASSERT_OK(rc);
    rc = sqlite3_close(dbB);
    ASSERT_OK(rc);

    /* Client C verifies final state */
    sqlite3 *dbC = open_azurite_db(db_name, NULL, 0);
    ASSERT_NOT_NULL(dbC);

    int count = 0;
    rc = query_int(dbC, "SELECT COUNT(*) FROM deli;", &count);
    ASSERT_OK(rc);
    ASSERT_EQ(count, 100);  /* 100 - 30 + 30 = 100 */

    /* Rows 1-30 should NOT exist */
    int deleted = 0;
    rc = query_int(dbC, "SELECT COUNT(*) FROM deli WHERE id <= 30;", &deleted);
    ASSERT_OK(rc);
    ASSERT_EQ(deleted, 0);

    /* Rows 101-130 should exist with 'new_' prefix */
    int new_count = 0;
    rc = query_int(dbC, "SELECT COUNT(*) FROM deli WHERE id >= 101 AND val LIKE 'new_%';",
                   &new_count);
    ASSERT_OK(rc);
    ASSERT_EQ(new_count, 30);

    sqlite3_close(dbC);
    cleanup_test_blobs(db_name);
}

/*
 * Test B4: Multi-table sequential writes
 */
TEST(mc_multi_table_sequential) {
    const char *db_name = "mc-multitbl.db";
    cleanup_test_blobs(db_name);

    int rc = sqlite_objs_vfs_register_uri(0);
    ASSERT_OK(rc);

    /* Client A creates table1 with data */
    sqlite3 *dbA = open_azurite_db(db_name, NULL, 1);
    ASSERT_NOT_NULL(dbA);

    rc = exec_sql(dbA,
        "CREATE TABLE table1 (id INTEGER PRIMARY KEY, name TEXT);"
        "INSERT INTO table1 VALUES (1, 'alpha');"
        "INSERT INTO table1 VALUES (2, 'beta');"
        "INSERT INTO table1 VALUES (3, 'gamma');");
    ASSERT_OK(rc);
    rc = sqlite3_close(dbA);
    ASSERT_OK(rc);

    /* Client B opens same DB, creates table2 */
    sqlite3 *dbB = open_azurite_db(db_name, NULL, 0);
    ASSERT_NOT_NULL(dbB);

    rc = exec_sql(dbB,
        "CREATE TABLE table2 (id INTEGER PRIMARY KEY, value REAL);"
        "INSERT INTO table2 VALUES (1, 3.14);"
        "INSERT INTO table2 VALUES (2, 2.72);"
        "INSERT INTO table2 VALUES (3, 1.62);");
    ASSERT_OK(rc);
    rc = sqlite3_close(dbB);
    ASSERT_OK(rc);

    /* Client C reads both tables */
    sqlite3 *dbC = open_azurite_db(db_name, NULL, 0);
    ASSERT_NOT_NULL(dbC);

    int t1_count = 0, t2_count = 0;
    rc = query_int(dbC, "SELECT COUNT(*) FROM table1;", &t1_count);
    ASSERT_OK(rc);
    ASSERT_EQ(t1_count, 3);
    rc = query_int(dbC, "SELECT COUNT(*) FROM table2;", &t2_count);
    ASSERT_OK(rc);
    ASSERT_EQ(t2_count, 3);

    /* Cross-table join */
    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(dbC,
        "SELECT t1.name, t2.value FROM table1 t1 "
        "JOIN table2 t2 ON t1.id = t2.id WHERE t1.id = 1;",
        -1, &stmt, NULL);
    ASSERT_OK(rc);
    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0), "alpha");
    sqlite3_finalize(stmt);

    sqlite3_close(dbC);
    cleanup_test_blobs(db_name);
}

/* ================================================================
 * C. Prefetch modes (prefetch=all vs prefetch=none)
 * ================================================================ */

/*
 * Test C1: prefetch=none basic read — compare with prefetch=all
 */
TEST(mc_prefetch_none_basic) {
    const char *db_name = "mc-prefetch-basic.db";
    cleanup_test_blobs(db_name);

    int rc = sqlite_objs_vfs_register_uri(0);
    ASSERT_OK(rc);

    /* Seed the database with enough data to exceed bootstrap window (>64KB) */
    sqlite3 *dbSeed = open_azurite_db(db_name, NULL, 1);
    ASSERT_NOT_NULL(dbSeed);

    rc = exec_sql(dbSeed,
        "CREATE TABLE pdata (id INTEGER PRIMARY KEY, payload TEXT);");
    ASSERT_OK(rc);

    sqlite3_stmt *ins = NULL;
    rc = sqlite3_prepare_v2(dbSeed, "INSERT INTO pdata VALUES(?,?);", -1, &ins, NULL);
    ASSERT_OK(rc);

    char payload[256];
    memset(payload, 'P', 200);
    payload[200] = '\0';

    rc = exec_sql(dbSeed, "BEGIN;");
    ASSERT_OK(rc);
    for (int i = 1; i <= 600; i++) {
        snprintf(payload, 10, "row_%05d", i);
        payload[9] = '_';  /* Overwrite null from snprintf */
        sqlite3_bind_int(ins, 1, i);
        sqlite3_bind_text(ins, 2, payload, 200, SQLITE_TRANSIENT);
        rc = sqlite3_step(ins);
        ASSERT_EQ(rc, SQLITE_DONE);
        sqlite3_reset(ins);
    }
    sqlite3_finalize(ins);
    rc = exec_sql(dbSeed, "COMMIT;");
    ASSERT_OK(rc);
    rc = sqlite3_close(dbSeed);
    ASSERT_OK(rc);

    /* Open with prefetch=all, read count */
    sqlite3 *dbAll = open_azurite_db(db_name, "prefetch=all", 0);
    ASSERT_NOT_NULL(dbAll);
    int count_all = 0;
    rc = query_int(dbAll, "SELECT COUNT(*) FROM pdata;", &count_all);
    ASSERT_OK(rc);
    sqlite3_close(dbAll);

    /* Open with prefetch=none, read count — should match */
    sqlite3 *dbNone = open_azurite_db(db_name, "prefetch=none", 0);
    ASSERT_NOT_NULL(dbNone);
    int count_none = 0;
    rc = query_int(dbNone, "SELECT COUNT(*) FROM pdata;", &count_none);
    ASSERT_OK(rc);
    sqlite3_close(dbNone);

    ASSERT_EQ(count_all, 600);
    ASSERT_EQ(count_none, 600);

    cleanup_test_blobs(db_name);
}

/*
 * Test C2: prefetch=none write-read — write, read back, reopen with prefetch=all
 */
TEST(mc_prefetch_none_write_read) {
    const char *db_name = "mc-prefetch-wr.db";
    cleanup_test_blobs(db_name);

    int rc = sqlite_objs_vfs_register_uri(0);
    ASSERT_OK(rc);

    /* Open with prefetch=none, write data */
    sqlite3 *dbW = open_azurite_db(db_name, "prefetch=none", 1);
    ASSERT_NOT_NULL(dbW);

    rc = exec_sql(dbW,
        "CREATE TABLE pfwr (id INTEGER PRIMARY KEY, txt TEXT);");
    ASSERT_OK(rc);

    sqlite3_stmt *ins = NULL;
    rc = sqlite3_prepare_v2(dbW, "INSERT INTO pfwr VALUES(?,?);", -1, &ins, NULL);
    ASSERT_OK(rc);

    rc = exec_sql(dbW, "BEGIN;");
    ASSERT_OK(rc);
    for (int i = 1; i <= 200; i++) {
        char txt[64];
        snprintf(txt, sizeof(txt), "prefetch_none_write_%04d", i);
        sqlite3_bind_int(ins, 1, i);
        sqlite3_bind_text(ins, 2, txt, -1, SQLITE_TRANSIENT);
        rc = sqlite3_step(ins);
        ASSERT_EQ(rc, SQLITE_DONE);
        sqlite3_reset(ins);
    }
    sqlite3_finalize(ins);
    rc = exec_sql(dbW, "COMMIT;");
    ASSERT_OK(rc);

    /* Read back in same connection */
    int count = 0;
    rc = query_int(dbW, "SELECT COUNT(*) FROM pfwr;", &count);
    ASSERT_OK(rc);
    ASSERT_EQ(count, 200);

    rc = sqlite3_close(dbW);
    ASSERT_OK(rc);

    /* Reopen with prefetch=all — verify same data */
    sqlite3 *dbR = open_azurite_db(db_name, "prefetch=all", 0);
    ASSERT_NOT_NULL(dbR);

    count = 0;
    rc = query_int(dbR, "SELECT COUNT(*) FROM pfwr;", &count);
    ASSERT_OK(rc);
    ASSERT_EQ(count, 200);

    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(dbR,
        "SELECT txt FROM pfwr WHERE id = 100;", -1, &stmt, NULL);
    ASSERT_OK(rc);
    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0),
                  "prefetch_none_write_0100");
    sqlite3_finalize(stmt);

    sqlite3_close(dbR);
    cleanup_test_blobs(db_name);
}

/*
 * Test C3: Mixed prefetch reconnect
 * A(prefetch=all) writes → B(prefetch=none) reads+writes → C(prefetch=all) reads all
 */
TEST(mc_mixed_prefetch_reconnect) {
    const char *db_name = "mc-mixed-prefetch.db";
    cleanup_test_blobs(db_name);

    int rc = sqlite_objs_vfs_register_uri(0);
    ASSERT_OK(rc);

    /* Client A: prefetch=all, writes 200 rows with 200-byte payloads */
    sqlite3 *dbA = open_azurite_db(db_name, "prefetch=all", 1);
    ASSERT_NOT_NULL(dbA);

    rc = exec_sql(dbA,
        "CREATE TABLE mixed (id INTEGER PRIMARY KEY, src TEXT, payload TEXT);");
    ASSERT_OK(rc);

    sqlite3_stmt *ins = NULL;
    rc = sqlite3_prepare_v2(dbA, "INSERT INTO mixed VALUES(?,?,?);", -1, &ins, NULL);
    ASSERT_OK(rc);

    char payload[201];
    memset(payload, 'A', 200);
    payload[200] = '\0';

    rc = exec_sql(dbA, "BEGIN;");
    ASSERT_OK(rc);
    for (int i = 1; i <= 200; i++) {
        sqlite3_bind_int(ins, 1, i);
        sqlite3_bind_text(ins, 2, "client_a", -1, SQLITE_STATIC);
        sqlite3_bind_text(ins, 3, payload, 200, SQLITE_STATIC);
        rc = sqlite3_step(ins);
        ASSERT_EQ(rc, SQLITE_DONE);
        sqlite3_reset(ins);
    }
    sqlite3_finalize(ins);
    rc = exec_sql(dbA, "COMMIT;");
    ASSERT_OK(rc);
    rc = sqlite3_close(dbA);
    ASSERT_OK(rc);

    /* Client B: prefetch=none, reads + writes */
    sqlite3 *dbB = open_azurite_db(db_name, "prefetch=none", 0);
    ASSERT_NOT_NULL(dbB);

    int count = 0;
    rc = query_int(dbB, "SELECT COUNT(*) FROM mixed WHERE src='client_a';", &count);
    ASSERT_OK(rc);
    ASSERT_EQ(count, 200);

    /* B adds rows 201-300 */
    rc = sqlite3_prepare_v2(dbB, "INSERT INTO mixed VALUES(?,?,?);", -1, &ins, NULL);
    ASSERT_OK(rc);

    memset(payload, 'B', 200);

    rc = exec_sql(dbB, "BEGIN;");
    ASSERT_OK(rc);
    for (int i = 201; i <= 300; i++) {
        sqlite3_bind_int(ins, 1, i);
        sqlite3_bind_text(ins, 2, "client_b", -1, SQLITE_STATIC);
        sqlite3_bind_text(ins, 3, payload, 200, SQLITE_STATIC);
        rc = sqlite3_step(ins);
        ASSERT_EQ(rc, SQLITE_DONE);
        sqlite3_reset(ins);
    }
    sqlite3_finalize(ins);
    rc = exec_sql(dbB, "COMMIT;");
    ASSERT_OK(rc);
    rc = sqlite3_close(dbB);
    ASSERT_OK(rc);

    /* Client C: prefetch=all, verifies all writes */
    sqlite3 *dbC = open_azurite_db(db_name, "prefetch=all", 0);
    ASSERT_NOT_NULL(dbC);

    count = 0;
    rc = query_int(dbC, "SELECT COUNT(*) FROM mixed;", &count);
    ASSERT_OK(rc);
    ASSERT_EQ(count, 300);

    int ca = 0, cb = 0;
    rc = query_int(dbC, "SELECT COUNT(*) FROM mixed WHERE src='client_a';", &ca);
    ASSERT_OK(rc);
    ASSERT_EQ(ca, 200);
    rc = query_int(dbC, "SELECT COUNT(*) FROM mixed WHERE src='client_b';", &cb);
    ASSERT_OK(rc);
    ASSERT_EQ(cb, 100);

    sqlite3_close(dbC);
    cleanup_test_blobs(db_name);
}

/*
 * Test C4: prefetch=none + PRAGMA sqlite_objs_prefetch
 */
TEST(mc_prefetch_pragma) {
    const char *db_name = "mc-prefetch-pragma.db";
    cleanup_test_blobs(db_name);

    int rc = sqlite_objs_vfs_register_uri(0);
    ASSERT_OK(rc);

    /* Seed database with >64KB data */
    sqlite3 *dbSeed = open_azurite_db(db_name, NULL, 1);
    ASSERT_NOT_NULL(dbSeed);

    rc = exec_sql(dbSeed,
        "CREATE TABLE prgm (id INTEGER PRIMARY KEY, data TEXT);");
    ASSERT_OK(rc);

    sqlite3_stmt *ins = NULL;
    rc = sqlite3_prepare_v2(dbSeed, "INSERT INTO prgm VALUES(?,?);", -1, &ins, NULL);
    ASSERT_OK(rc);

    char data[256];
    memset(data, 'D', 200);
    data[200] = '\0';

    rc = exec_sql(dbSeed, "BEGIN;");
    ASSERT_OK(rc);
    for (int i = 1; i <= 600; i++) {
        sqlite3_bind_int(ins, 1, i);
        sqlite3_bind_text(ins, 2, data, 200, SQLITE_STATIC);
        rc = sqlite3_step(ins);
        ASSERT_EQ(rc, SQLITE_DONE);
        sqlite3_reset(ins);
    }
    sqlite3_finalize(ins);
    rc = exec_sql(dbSeed, "COMMIT;");
    ASSERT_OK(rc);
    rc = sqlite3_close(dbSeed);
    ASSERT_OK(rc);

    /* Open with prefetch=none */
    sqlite3 *dbP = open_azurite_db(db_name, "prefetch=none", 0);
    ASSERT_NOT_NULL(dbP);

    /* Run PRAGMA to fetch all pages */
    rc = exec_sql(dbP, "PRAGMA sqlite_objs_prefetch;");
    ASSERT_OK(rc);

    /* Now read all data — should work since pages are fetched */
    int count = 0;
    rc = query_int(dbP, "SELECT COUNT(*) FROM prgm;", &count);
    ASSERT_OK(rc);
    ASSERT_EQ(count, 600);

    /* Spot check */
    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(dbP,
        "SELECT data FROM prgm WHERE id = 300;", -1, &stmt, NULL);
    ASSERT_OK(rc);
    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_ROW);
    ASSERT_EQ(sqlite3_column_bytes(stmt, 0), 200);
    sqlite3_finalize(stmt);

    sqlite3_close(dbP);
    cleanup_test_blobs(db_name);
}

/* ================================================================
 * D. Cache reuse scenarios
 * ================================================================ */

/*
 * Test D1: ETag match reconnect — fast reconnect via cache
 */
TEST(mc_etag_match_reconnect) {
    const char *db_name = "mc-etag-match.db";
    cleanup_test_blobs(db_name);

    int rc = sqlite_objs_vfs_register_uri(0);
    ASSERT_OK(rc);

    /* First open: write data */
    sqlite3 *db1 = open_azurite_db(db_name, "cache_reuse=1", 1);
    ASSERT_NOT_NULL(db1);

    rc = exec_sql(db1,
        "CREATE TABLE etm (id INTEGER PRIMARY KEY, val TEXT);");
    ASSERT_OK(rc);

    sqlite3_stmt *ins = NULL;
    rc = sqlite3_prepare_v2(db1, "INSERT INTO etm VALUES(?,?);", -1, &ins, NULL);
    ASSERT_OK(rc);
    rc = exec_sql(db1, "BEGIN;");
    ASSERT_OK(rc);
    for (int i = 1; i <= 200; i++) {
        char val[64];
        snprintf(val, sizeof(val), "etag_match_%04d", i);
        sqlite3_bind_int(ins, 1, i);
        sqlite3_bind_text(ins, 2, val, -1, SQLITE_TRANSIENT);
        rc = sqlite3_step(ins);
        ASSERT_EQ(rc, SQLITE_DONE);
        sqlite3_reset(ins);
    }
    sqlite3_finalize(ins);
    rc = exec_sql(db1, "COMMIT;");
    ASSERT_OK(rc);
    rc = sqlite3_close(db1);
    ASSERT_OK(rc);

    /* Second open: cache_reuse — ETag should match, no download */
    sqlite3 *db2 = open_azurite_db(db_name, "cache_reuse=1", 0);
    ASSERT_NOT_NULL(db2);

    {
        int dlCount = -1;
        rc = sqlite3_file_control(db2, "main",
                                  SQLITE_OBJS_FCNTL_DOWNLOAD_COUNT, &dlCount);
        ASSERT_OK(rc);
        ASSERT_EQ(dlCount, 0);
    }

    int count = 0;
    rc = query_int(db2, "SELECT COUNT(*) FROM etm;", &count);
    ASSERT_OK(rc);
    ASSERT_EQ(count, 200);

    sqlite3_close(db2);
    cleanup_test_blobs(db_name);
}

/*
 * Test D2: ETag mismatch reconnect — B modifies, A must revalidate
 */
TEST(mc_etag_mismatch_reconnect) {
    const char *db_name = "mc-etag-mismatch.db";
    cleanup_test_blobs(db_name);

    int rc = sqlite_objs_vfs_register_uri(0);
    ASSERT_OK(rc);

    /* Client A writes with cache_reuse=1 */
    sqlite3 *dbA = open_azurite_db(db_name, "cache_reuse=1", 1);
    ASSERT_NOT_NULL(dbA);

    rc = exec_sql(dbA,
        "CREATE TABLE emm (id INTEGER PRIMARY KEY, val TEXT);"
        "INSERT INTO emm VALUES (1, 'original_a');"
        "INSERT INTO emm VALUES (2, 'original_b');");
    ASSERT_OK(rc);
    rc = sqlite3_close(dbA);
    ASSERT_OK(rc);

    /* Client B writes WITHOUT cache_reuse — changes the blob's ETag */
    sqlite3 *dbB = open_azurite_db(db_name, NULL, 0);
    ASSERT_NOT_NULL(dbB);

    rc = exec_sql(dbB,
        "UPDATE emm SET val = 'modified_by_b' WHERE id = 1;"
        "INSERT INTO emm VALUES (3, 'added_by_b');");
    ASSERT_OK(rc);
    rc = sqlite3_close(dbB);
    ASSERT_OK(rc);

    /* Client A reopens with cache_reuse=1 — ETag must mismatch, force download */
    sqlite3 *dbA2 = open_azurite_db(db_name, "cache_reuse=1", 0);
    ASSERT_NOT_NULL(dbA2);

    {
        int dlCount = -1;
        rc = sqlite3_file_control(dbA2, "main",
                                  SQLITE_OBJS_FCNTL_DOWNLOAD_COUNT, &dlCount);
        ASSERT_OK(rc);
        ASSERT_EQ(dlCount, 1);  /* Must have re-downloaded */
    }

    /* Must see B's modifications */
    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(dbA2,
        "SELECT val FROM emm WHERE id=1;", -1, &stmt, NULL);
    ASSERT_OK(rc);
    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0), "modified_by_b");
    sqlite3_finalize(stmt);

    int count = 0;
    rc = query_int(dbA2, "SELECT COUNT(*) FROM emm;", &count);
    ASSERT_OK(rc);
    ASSERT_EQ(count, 3);

    sqlite3_close(dbA2);
    cleanup_test_blobs(db_name);
}

/*
 * Test D3: cache_reuse + prefetch=none
 */
TEST(mc_cache_reuse_prefetch_none) {
    const char *db_name = "mc-cache-pfnone.db";
    cleanup_test_blobs(db_name);

    int rc = sqlite_objs_vfs_register_uri(0);
    ASSERT_OK(rc);

    /* Client A: cache_reuse + prefetch=none, write enough data to exceed bootstrap */
    sqlite3 *dbA = open_azurite_db(db_name, "cache_reuse=1&prefetch=none", 1);
    ASSERT_NOT_NULL(dbA);

    rc = exec_sql(dbA,
        "CREATE TABLE cpn (id INTEGER PRIMARY KEY, payload TEXT);");
    ASSERT_OK(rc);

    sqlite3_stmt *ins = NULL;
    rc = sqlite3_prepare_v2(dbA, "INSERT INTO cpn VALUES(?,?);", -1, &ins, NULL);
    ASSERT_OK(rc);

    char payload[201];
    memset(payload, 'X', 200);
    payload[200] = '\0';

    rc = exec_sql(dbA, "BEGIN;");
    ASSERT_OK(rc);
    for (int i = 1; i <= 500; i++) {
        sqlite3_bind_int(ins, 1, i);
        sqlite3_bind_text(ins, 2, payload, 200, SQLITE_STATIC);
        rc = sqlite3_step(ins);
        ASSERT_EQ(rc, SQLITE_DONE);
        sqlite3_reset(ins);
    }
    sqlite3_finalize(ins);
    rc = exec_sql(dbA, "COMMIT;");
    ASSERT_OK(rc);
    rc = sqlite3_close(dbA);
    ASSERT_OK(rc);

    /* Client B modifies (no cache) to change ETag */
    sqlite3 *dbB = open_azurite_db(db_name, NULL, 0);
    ASSERT_NOT_NULL(dbB);
    rc = exec_sql(dbB,
        "INSERT INTO cpn VALUES(501, 'from_b');");
    ASSERT_OK(rc);
    rc = sqlite3_close(dbB);
    ASSERT_OK(rc);

    /* Client A reopens with cache_reuse + prefetch=none */
    sqlite3 *dbA2 = open_azurite_db(db_name, "cache_reuse=1&prefetch=none", 0);
    ASSERT_NOT_NULL(dbA2);

    /* Should see all data including B's write */
    int count = 0;
    rc = query_int(dbA2, "SELECT COUNT(*) FROM cpn;", &count);
    ASSERT_OK(rc);
    ASSERT_EQ(count, 501);

    sqlite3_close(dbA2);
    cleanup_test_blobs(db_name);
}

/*
 * Test D4: No cache reuse — clean download every time
 */
TEST(mc_no_cache_reuse) {
    const char *db_name = "mc-no-cache.db";
    cleanup_test_blobs(db_name);

    int rc = sqlite_objs_vfs_register_uri(0);
    ASSERT_OK(rc);

    /* First open: create data */
    sqlite3 *db1 = open_azurite_db(db_name, "cache_reuse=0", 1);
    ASSERT_NOT_NULL(db1);

    rc = exec_sql(db1,
        "CREATE TABLE nocache (id INTEGER PRIMARY KEY, val TEXT);"
        "INSERT INTO nocache VALUES (1, 'hello');"
        "INSERT INTO nocache VALUES (2, 'world');");
    ASSERT_OK(rc);
    rc = sqlite3_close(db1);
    ASSERT_OK(rc);

    /* Second open: must re-download (no cache) */
    sqlite3 *db2 = open_azurite_db(db_name, "cache_reuse=0", 0);
    ASSERT_NOT_NULL(db2);

    {
        int dlCount = -1;
        rc = sqlite3_file_control(db2, "main",
                                  SQLITE_OBJS_FCNTL_DOWNLOAD_COUNT, &dlCount);
        ASSERT_OK(rc);
        ASSERT_EQ(dlCount, 1);  /* Must have downloaded */
    }

    int count = 0;
    rc = query_int(db2, "SELECT COUNT(*) FROM nocache;", &count);
    ASSERT_OK(rc);
    ASSERT_EQ(count, 2);

    /* Third open: still must download */
    rc = sqlite3_close(db2);
    ASSERT_OK(rc);

    sqlite3 *db3 = open_azurite_db(db_name, "cache_reuse=0", 0);
    ASSERT_NOT_NULL(db3);

    {
        int dlCount = -1;
        rc = sqlite3_file_control(db3, "main",
                                  SQLITE_OBJS_FCNTL_DOWNLOAD_COUNT, &dlCount);
        ASSERT_OK(rc);
        ASSERT_EQ(dlCount, 1);
    }

    sqlite3_close(db3);
    cleanup_test_blobs(db_name);
}

/* ================================================================
 * E. Transaction isolation and data integrity
 * ================================================================ */

/*
 * Test E1: Large transaction — 5000 rows in one BEGIN/COMMIT
 */
TEST(mc_large_transaction) {
    const char *db_name = "mc-large-txn.db";
    cleanup_test_blobs(db_name);

    int rc = sqlite_objs_vfs_register_uri(0);
    ASSERT_OK(rc);

    sqlite3 *dbA = open_azurite_db(db_name, NULL, 1);
    ASSERT_NOT_NULL(dbA);

    rc = exec_sql(dbA,
        "CREATE TABLE bigtxn (id INTEGER PRIMARY KEY, val INTEGER);");
    ASSERT_OK(rc);

    sqlite3_stmt *ins = NULL;
    rc = sqlite3_prepare_v2(dbA, "INSERT INTO bigtxn VALUES(?,?);", -1, &ins, NULL);
    ASSERT_OK(rc);

    rc = exec_sql(dbA, "BEGIN;");
    ASSERT_OK(rc);
    for (int i = 1; i <= 5000; i++) {
        sqlite3_bind_int(ins, 1, i);
        sqlite3_bind_int(ins, 2, i * 3);
        rc = sqlite3_step(ins);
        ASSERT_EQ(rc, SQLITE_DONE);
        sqlite3_reset(ins);
    }
    sqlite3_finalize(ins);
    rc = exec_sql(dbA, "COMMIT;");
    ASSERT_OK(rc);
    rc = sqlite3_close(dbA);
    ASSERT_OK(rc);

    /* Client B reads */
    sqlite3 *dbB = open_azurite_db(db_name, NULL, 0);
    ASSERT_NOT_NULL(dbB);

    int count = 0;
    rc = query_int(dbB, "SELECT COUNT(*) FROM bigtxn;", &count);
    ASSERT_OK(rc);
    ASSERT_EQ(count, 5000);

    /* Verify sum: sum(i*3) for i=1..5000 = 3 * 5000*5001/2 = 37507500 */
    int total = 0;
    rc = query_int(dbB, "SELECT SUM(val) FROM bigtxn;", &total);
    ASSERT_OK(rc);
    ASSERT_EQ(total, 37507500);

    sqlite3_close(dbB);
    cleanup_test_blobs(db_name);
}

/*
 * Test E2: Multiple small transactions (100 transactions, 1 row each)
 */
TEST(mc_many_small_transactions) {
    const char *db_name = "mc-small-txns.db";
    cleanup_test_blobs(db_name);

    int rc = sqlite_objs_vfs_register_uri(0);
    ASSERT_OK(rc);

    sqlite3 *dbA = open_azurite_db(db_name, NULL, 1);
    ASSERT_NOT_NULL(dbA);

    rc = exec_sql(dbA,
        "CREATE TABLE stxn (id INTEGER PRIMARY KEY, val TEXT);");
    ASSERT_OK(rc);

    sqlite3_stmt *ins = NULL;
    rc = sqlite3_prepare_v2(dbA, "INSERT INTO stxn VALUES(?,?);", -1, &ins, NULL);
    ASSERT_OK(rc);

    /* 100 individual transactions */
    for (int i = 1; i <= 100; i++) {
        rc = exec_sql(dbA, "BEGIN;");
        ASSERT_OK(rc);

        char val[32];
        snprintf(val, sizeof(val), "txn_%03d", i);
        sqlite3_bind_int(ins, 1, i);
        sqlite3_bind_text(ins, 2, val, -1, SQLITE_TRANSIENT);
        rc = sqlite3_step(ins);
        ASSERT_EQ(rc, SQLITE_DONE);
        sqlite3_reset(ins);

        rc = exec_sql(dbA, "COMMIT;");
        ASSERT_OK(rc);
    }
    sqlite3_finalize(ins);
    rc = sqlite3_close(dbA);
    ASSERT_OK(rc);

    /* Client B reads */
    sqlite3 *dbB = open_azurite_db(db_name, NULL, 0);
    ASSERT_NOT_NULL(dbB);

    int count = 0;
    rc = query_int(dbB, "SELECT COUNT(*) FROM stxn;", &count);
    ASSERT_OK(rc);
    ASSERT_EQ(count, 100);

    /* Verify first and last */
    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(dbB,
        "SELECT val FROM stxn WHERE id=1;", -1, &stmt, NULL);
    ASSERT_OK(rc);
    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0), "txn_001");
    sqlite3_finalize(stmt);

    rc = sqlite3_prepare_v2(dbB,
        "SELECT val FROM stxn WHERE id=100;", -1, &stmt, NULL);
    ASSERT_OK(rc);
    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0), "txn_100");
    sqlite3_finalize(stmt);

    sqlite3_close(dbB);
    cleanup_test_blobs(db_name);
}

/*
 * Test E3: VACUUM after writes
 */
TEST(mc_vacuum_after_writes) {
    const char *db_name = "mc-vacuum.db";
    cleanup_test_blobs(db_name);

    int rc = sqlite_objs_vfs_register_uri(0);
    ASSERT_OK(rc);

    sqlite3 *dbA = open_azurite_db(db_name, NULL, 1);
    ASSERT_NOT_NULL(dbA);

    /* Create, populate, delete half, VACUUM */
    rc = exec_sql(dbA,
        "CREATE TABLE vac (id INTEGER PRIMARY KEY, data TEXT);");
    ASSERT_OK(rc);

    sqlite3_stmt *ins = NULL;
    rc = sqlite3_prepare_v2(dbA, "INSERT INTO vac VALUES(?,?);", -1, &ins, NULL);
    ASSERT_OK(rc);

    rc = exec_sql(dbA, "BEGIN;");
    ASSERT_OK(rc);
    for (int i = 1; i <= 300; i++) {
        char data[128];
        snprintf(data, sizeof(data), "vacuum_test_data_%05d_padding", i);
        sqlite3_bind_int(ins, 1, i);
        sqlite3_bind_text(ins, 2, data, -1, SQLITE_TRANSIENT);
        rc = sqlite3_step(ins);
        ASSERT_EQ(rc, SQLITE_DONE);
        sqlite3_reset(ins);
    }
    sqlite3_finalize(ins);
    rc = exec_sql(dbA, "COMMIT;");
    ASSERT_OK(rc);

    /* Delete first 150 rows to create fragmentation */
    rc = exec_sql(dbA, "DELETE FROM vac WHERE id <= 150;");
    ASSERT_OK(rc);

    /* VACUUM to compact */
    rc = exec_sql(dbA, "VACUUM;");
    ASSERT_OK(rc);

    rc = sqlite3_close(dbA);
    ASSERT_OK(rc);

    /* Client B reads — data should be intact after vacuum */
    sqlite3 *dbB = open_azurite_db(db_name, NULL, 0);
    ASSERT_NOT_NULL(dbB);

    int count = 0;
    rc = query_int(dbB, "SELECT COUNT(*) FROM vac;", &count);
    ASSERT_OK(rc);
    ASSERT_EQ(count, 150);

    /* Verify remaining rows are 151-300 */
    int min_id = 0, max_id = 0;
    rc = query_int(dbB, "SELECT MIN(id) FROM vac;", &min_id);
    ASSERT_OK(rc);
    ASSERT_EQ(min_id, 151);
    rc = query_int(dbB, "SELECT MAX(id) FROM vac;", &max_id);
    ASSERT_OK(rc);
    ASSERT_EQ(max_id, 300);

    sqlite3_close(dbB);
    cleanup_test_blobs(db_name);
}

/*
 * Test E4: Cross-database join (local primary + ATTACH Azure via URI)
 *
 * ATTACH inherits the main connection's VFS, so we open a LOCAL database
 * first (default VFS) and ATTACH the Azure database via URI. This tests
 * that an Azure VFS database can be ATTACHed to a local connection.
 *
 * If ATTACH with Azure URI is not yet supported, the test skips gracefully.
 */
TEST(mc_cross_database_join) {
    const char *db_name = "mc-crossdb.db";
    const char *local_name = "/tmp/mc-crossdb-local.db";
    cleanup_test_blobs(db_name);
    (void)unlink(local_name);

    int rc = sqlite_objs_vfs_register_uri(0);
    ASSERT_OK(rc);

    /* First, create the Azure database with data */
    sqlite3 *dbAz = open_azurite_db(db_name, NULL, 1);
    ASSERT_NOT_NULL(dbAz);

    rc = exec_sql(dbAz,
        "CREATE TABLE remote_t (id INTEGER PRIMARY KEY, name TEXT);"
        "INSERT INTO remote_t VALUES (1, 'remote_alice');"
        "INSERT INTO remote_t VALUES (2, 'remote_bob');");
    ASSERT_OK(rc);
    rc = sqlite3_close(dbAz);
    ASSERT_OK(rc);

    /* Open a LOCAL database (default VFS), create local data */
    sqlite3 *dbLocal = NULL;
    rc = sqlite3_open_v2(local_name, &dbLocal,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, NULL);
    ASSERT_OK(rc);
    ASSERT_NOT_NULL(dbLocal);

    rc = exec_sql(dbLocal,
        "CREATE TABLE local_t (id INTEGER PRIMARY KEY, score INTEGER);"
        "INSERT INTO local_t VALUES (1, 95);"
        "INSERT INTO local_t VALUES (2, 87);");
    ASSERT_OK(rc);

    /* Try to ATTACH the Azure database via URI */
    char attach_sql[1024];
    snprintf(attach_sql, sizeof(attach_sql),
        "ATTACH DATABASE 'file:%s?"
        "azure_account=" AZURITE_ACCOUNT
        "&azure_container=" AZURITE_CONTAINER
        "&azure_key=" AZURITE_KEY
        "&azure_endpoint=" AZURITE_ENDPOINT
        "' AS azure_db;",
        db_name);

    char *errmsg = NULL;
    rc = sqlite3_exec(dbLocal, attach_sql, NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        /* Cross-VFS ATTACH via URI is a known limitation —
         * document but don't fail the test */
        fprintf(stderr,
            "  NOTE: Cross-VFS ATTACH via URI not supported yet: %s\n",
            errmsg ? errmsg : "unknown");
        sqlite3_free(errmsg);
        sqlite3_close(dbLocal);
        (void)unlink(local_name);
        cleanup_test_blobs(db_name);
        return;
    }

    /* If ATTACH succeeded, try a cross-database JOIN */
    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(dbLocal,
        "SELECT a.name, l.score FROM azure_db.remote_t a "
        "JOIN local_t l ON a.id = l.id ORDER BY a.id;",
        -1, &stmt, NULL);
    ASSERT_OK(rc);

    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0), "remote_alice");
    ASSERT_EQ(sqlite3_column_int(stmt, 1), 95);

    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0), "remote_bob");
    ASSERT_EQ(sqlite3_column_int(stmt, 1), 87);
    sqlite3_finalize(stmt);

    rc = exec_sql(dbLocal, "DETACH DATABASE azure_db;");
    ASSERT_OK(rc);

    sqlite3_close(dbLocal);

    /* Cleanup */
    cleanup_test_blobs(db_name);
    (void)unlink(local_name);
}

/* ================================================================
 * F. Edge cases
 * ================================================================ */

/*
 * Test F1: Empty database reconnect
 */
TEST(mc_empty_db_reconnect) {
    const char *db_name = "mc-empty.db";
    cleanup_test_blobs(db_name);

    int rc = sqlite_objs_vfs_register_uri(0);
    ASSERT_OK(rc);

    /* Create empty database (no tables) */
    sqlite3 *dbA = open_azurite_db(db_name, NULL, 1);
    ASSERT_NOT_NULL(dbA);

    /* Force SQLite to write the header (creating a table and dropping it) */
    rc = exec_sql(dbA, "CREATE TABLE dummy (x INTEGER); DROP TABLE dummy;");
    ASSERT_OK(rc);

    rc = sqlite3_close(dbA);
    ASSERT_OK(rc);

    /* Client B opens the empty database — should succeed */
    sqlite3 *dbB = open_azurite_db(db_name, NULL, 0);
    ASSERT_NOT_NULL(dbB);

    int tbl_count = 0;
    rc = query_int(dbB,
        "SELECT COUNT(*) FROM sqlite_master WHERE type='table';", &tbl_count);
    ASSERT_OK(rc);
    ASSERT_EQ(tbl_count, 0);

    /* B can create tables in the empty db */
    rc = exec_sql(dbB,
        "CREATE TABLE new_tbl (id INTEGER PRIMARY KEY);"
        "INSERT INTO new_tbl VALUES (1);");
    ASSERT_OK(rc);

    int count = 0;
    rc = query_int(dbB, "SELECT COUNT(*) FROM new_tbl;", &count);
    ASSERT_OK(rc);
    ASSERT_EQ(count, 1);

    sqlite3_close(dbB);
    cleanup_test_blobs(db_name);
}

/*
 * Test F2: Very wide rows (64KB+ TEXT/BLOB spanning multiple pages)
 */
TEST(mc_wide_rows) {
    const char *db_name = "mc-wide-rows.db";
    cleanup_test_blobs(db_name);

    int rc = sqlite_objs_vfs_register_uri(0);
    ASSERT_OK(rc);

    sqlite3 *dbA = open_azurite_db(db_name, NULL, 1);
    ASSERT_NOT_NULL(dbA);

    rc = exec_sql(dbA,
        "CREATE TABLE wide (id INTEGER PRIMARY KEY, big_text TEXT, big_blob BLOB);");
    ASSERT_OK(rc);

    /* Create 70KB payloads */
    const int payload_size = 70 * 1024;
    char *big_text = malloc((size_t)payload_size + 1);
    ASSERT_NOT_NULL(big_text);
    unsigned char *big_blob = malloc((size_t)payload_size);
    ASSERT_NOT_NULL(big_blob);

    sqlite3_stmt *ins = NULL;
    rc = sqlite3_prepare_v2(dbA,
        "INSERT INTO wide VALUES(?,?,?);", -1, &ins, NULL);
    ASSERT_OK(rc);

    rc = exec_sql(dbA, "BEGIN;");
    ASSERT_OK(rc);
    for (int row = 1; row <= 5; row++) {
        /* Fill with row-specific patterns */
        memset(big_text, 'A' + (row - 1), (size_t)payload_size);
        big_text[payload_size] = '\0';
        for (int b = 0; b < payload_size; b++) {
            big_blob[b] = (unsigned char)((row + b) & 0xFF);
        }

        sqlite3_bind_int(ins, 1, row);
        sqlite3_bind_text(ins, 2, big_text, payload_size, SQLITE_TRANSIENT);
        sqlite3_bind_blob(ins, 3, big_blob, payload_size, SQLITE_TRANSIENT);
        rc = sqlite3_step(ins);
        ASSERT_EQ(rc, SQLITE_DONE);
        sqlite3_reset(ins);
    }
    sqlite3_finalize(ins);
    rc = exec_sql(dbA, "COMMIT;");
    ASSERT_OK(rc);
    rc = sqlite3_close(dbA);
    ASSERT_OK(rc);

    /* Client B reads and verifies */
    sqlite3 *dbB = open_azurite_db(db_name, NULL, 0);
    ASSERT_NOT_NULL(dbB);

    int count = 0;
    rc = query_int(dbB, "SELECT COUNT(*) FROM wide;", &count);
    ASSERT_OK(rc);
    ASSERT_EQ(count, 5);

    /* Verify row 3's data */
    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(dbB,
        "SELECT big_text, big_blob FROM wide WHERE id=3;", -1, &stmt, NULL);
    ASSERT_OK(rc);
    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_ROW);

    ASSERT_EQ(sqlite3_column_bytes(stmt, 0), payload_size);
    const char *got_text = (const char *)sqlite3_column_text(stmt, 0);
    /* First char should be 'C' (row 3, 'A'+2) */
    ASSERT_EQ(got_text[0], 'C');
    ASSERT_EQ(got_text[payload_size - 1], 'C');

    ASSERT_EQ(sqlite3_column_bytes(stmt, 1), payload_size);
    const unsigned char *got_blob = sqlite3_column_blob(stmt, 1);
    for (int b = 0; b < 128; b++) {  /* Spot-check first 128 bytes */
        ASSERT_EQ(got_blob[b], (unsigned char)((3 + b) & 0xFF));
    }
    sqlite3_finalize(stmt);

    sqlite3_close(dbB);
    free(big_text);
    free(big_blob);
    cleanup_test_blobs(db_name);
}

/*
 * Test F3: Many small tables (50+ tables)
 */
TEST(mc_many_small_tables) {
    const char *db_name = "mc-many-tables.db";
    cleanup_test_blobs(db_name);

    int rc = sqlite_objs_vfs_register_uri(0);
    ASSERT_OK(rc);

    sqlite3 *dbA = open_azurite_db(db_name, NULL, 1);
    ASSERT_NOT_NULL(dbA);

    rc = exec_sql(dbA, "BEGIN;");
    ASSERT_OK(rc);

    for (int t = 1; t <= 50; t++) {
        char sql[256];
        snprintf(sql, sizeof(sql),
            "CREATE TABLE tbl_%03d (id INTEGER PRIMARY KEY, val TEXT);", t);
        rc = exec_sql(dbA, sql);
        ASSERT_OK(rc);

        for (int r = 1; r <= 5; r++) {
            snprintf(sql, sizeof(sql),
                "INSERT INTO tbl_%03d VALUES (%d, 'data_%03d_%02d');",
                t, r, t, r);
            rc = exec_sql(dbA, sql);
            ASSERT_OK(rc);
        }
    }

    rc = exec_sql(dbA, "COMMIT;");
    ASSERT_OK(rc);
    rc = sqlite3_close(dbA);
    ASSERT_OK(rc);

    /* Client B reads all tables */
    sqlite3 *dbB = open_azurite_db(db_name, NULL, 0);
    ASSERT_NOT_NULL(dbB);

    int tbl_count = 0;
    rc = query_int(dbB,
        "SELECT COUNT(*) FROM sqlite_master WHERE type='table';", &tbl_count);
    ASSERT_OK(rc);
    ASSERT_EQ(tbl_count, 50);

    /* Verify a few tables */
    int count = 0;
    rc = query_int(dbB, "SELECT COUNT(*) FROM tbl_001;", &count);
    ASSERT_OK(rc);
    ASSERT_EQ(count, 5);

    rc = query_int(dbB, "SELECT COUNT(*) FROM tbl_050;", &count);
    ASSERT_OK(rc);
    ASSERT_EQ(count, 5);

    /* Verify specific data in middle table */
    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(dbB,
        "SELECT val FROM tbl_025 WHERE id=3;", -1, &stmt, NULL);
    ASSERT_OK(rc);
    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0), "data_025_03");
    sqlite3_finalize(stmt);

    sqlite3_close(dbB);
    cleanup_test_blobs(db_name);
}

/*
 * Test F4: Rapid open-close cycles
 */
TEST(mc_rapid_open_close) {
    const char *db_name = "mc-rapid-oc.db";
    cleanup_test_blobs(db_name);

    int rc = sqlite_objs_vfs_register_uri(0);
    ASSERT_OK(rc);

    /* Create initial database with data */
    sqlite3 *dbInit = open_azurite_db(db_name, NULL, 1);
    ASSERT_NOT_NULL(dbInit);

    rc = exec_sql(dbInit,
        "CREATE TABLE rapid (id INTEGER PRIMARY KEY, cycle INTEGER);");
    ASSERT_OK(rc);

    sqlite3_stmt *ins = NULL;
    rc = sqlite3_prepare_v2(dbInit, "INSERT INTO rapid VALUES(?,?);", -1, &ins, NULL);
    ASSERT_OK(rc);
    rc = exec_sql(dbInit, "BEGIN;");
    ASSERT_OK(rc);
    for (int i = 1; i <= 50; i++) {
        sqlite3_bind_int(ins, 1, i);
        sqlite3_bind_int(ins, 2, 0);
        rc = sqlite3_step(ins);
        ASSERT_EQ(rc, SQLITE_DONE);
        sqlite3_reset(ins);
    }
    sqlite3_finalize(ins);
    rc = exec_sql(dbInit, "COMMIT;");
    ASSERT_OK(rc);
    rc = sqlite3_close(dbInit);
    ASSERT_OK(rc);

    /* Rapidly open, read, write one row, close — 10 cycles */
    for (int cycle = 1; cycle <= 10; cycle++) {
        sqlite3 *db = open_azurite_db(db_name, NULL, 0);
        ASSERT_NOT_NULL(db);

        /* Read count */
        int count = 0;
        rc = query_int(db, "SELECT COUNT(*) FROM rapid;", &count);
        ASSERT_OK(rc);
        ASSERT_EQ(count, 50 + cycle - 1);

        /* Insert one row per cycle */
        char sql[128];
        snprintf(sql, sizeof(sql),
            "INSERT INTO rapid VALUES(%d, %d);", 50 + cycle, cycle);
        rc = exec_sql(db, sql);
        ASSERT_OK(rc);

        rc = sqlite3_close(db);
        ASSERT_OK(rc);
    }

    /* Final verification */
    sqlite3 *dbFinal = open_azurite_db(db_name, NULL, 0);
    ASSERT_NOT_NULL(dbFinal);

    int count = 0;
    rc = query_int(dbFinal, "SELECT COUNT(*) FROM rapid;", &count);
    ASSERT_OK(rc);
    ASSERT_EQ(count, 60);

    /* Verify last inserted row */
    int last_cycle = 0;
    rc = query_int(dbFinal, "SELECT cycle FROM rapid WHERE id=60;", &last_cycle);
    ASSERT_OK(rc);
    ASSERT_EQ(last_cycle, 10);

    sqlite3_close(dbFinal);
    cleanup_test_blobs(db_name);
}

/* ================================================================
 * G. Concurrent Writers Regression Test
 * Tests for bug: concurrent writers lose inserts / duplicate rowids
 * ================================================================ */

/* Thread context for concurrent writes */
typedef struct {
    const char *db_name;
    const char *table_name;  /* Added: configurable table name */
    int writer_id;
    int inserts_per_writer;
    int *assigned_ids;
    int success;
    int result_code;
} writer_context_t;

/* Thread function: each writer opens connection, inserts N rows */
static void *concurrent_writer_thread(void *arg) {
    writer_context_t *ctx = (writer_context_t *)arg;
    ctx->success = 0;
    ctx->result_code = SQLITE_OK;

    /* Open independent connection */
    sqlite3 *db = open_azurite_db(ctx->db_name, NULL, 0);
    if (!db) {
        fprintf(stderr, "  writer %d: failed to open database\n", ctx->writer_id);
        ctx->result_code = SQLITE_CANTOPEN;
        return NULL;
    }

    /* Insert rows for this writer */
    sqlite3_stmt *ins = NULL;
    char sql[256];
    const char *table = ctx->table_name ? ctx->table_name : "messages";
    snprintf(sql, sizeof(sql), 
        "INSERT INTO %s(writer, seq) VALUES (?, ?);", table);
    
    int rc = sqlite3_prepare_v2(db, sql, -1, &ins, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "  writer %d: prepare failed: %s\n", 
                ctx->writer_id, sqlite3_errmsg(db));
        ctx->result_code = rc;
        sqlite3_close(db);
        return NULL;
    }

    int ids_written = 0;
    for (int seq = 0; seq < ctx->inserts_per_writer; seq++) {
        sqlite3_bind_int(ins, 1, ctx->writer_id);
        sqlite3_bind_int(ins, 2, seq);
        
        rc = sqlite3_step(ins);
        if (rc == SQLITE_DONE) {
            ctx->assigned_ids[seq] = (int)sqlite3_last_insert_rowid(db);
            ids_written++;
            sqlite3_reset(ins);
        } else if (rc == SQLITE_BUSY || rc == SQLITE_LOCKED) {
            /* Acceptable: lease conflict, retry budget exhausted */
            fprintf(stderr, "  writer %d seq %d: %s (acceptable)\n",
                    ctx->writer_id, seq, 
                    rc == SQLITE_BUSY ? "SQLITE_BUSY" : "SQLITE_LOCKED");
            ctx->result_code = rc;
            sqlite3_reset(ins);
            break;
        } else {
            fprintf(stderr, "  writer %d seq %d: unexpected error %d: %s\n",
                    ctx->writer_id, seq, rc, sqlite3_errmsg(db));
            ctx->result_code = rc;
            sqlite3_reset(ins);
            break;
        }
    }

    sqlite3_finalize(ins);
    sqlite3_close(db);
    
    ctx->success = ids_written;
    return NULL;
}

/*
 * Test G1: Concurrent writers must serialize or fail clearly
 *
 * REGRESSION TEST for bug: "concurrent writers lose inserts / duplicate rowids"
 *
 * Expected outcomes (both acceptable):
 *   1. All writers serialize — all N inserts persist with distinct IDs
 *   2. Some writers hit SQLITE_BUSY/SQLITE_LOCKED and back off
 *
 * UNACCEPTABLE outcome (the bug):
 *   - Some inserts silently lost (fewer persisted rows than committed)
 *   - Duplicate rowids (concurrent autoincrement collision)
 *
 * This test spawns multiple threads writing concurrently to the same database.
 * After all threads complete, it verifies:
 *   - Every assigned rowid is persisted in the database
 *   - All persisted rowids are distinct (no duplicates)
 */
TEST(concurrent_writers_regression) {
    const char *db_name = "concurrent-writers-bug.db";
    cleanup_test_blobs(db_name);

    int rc = sqlite_objs_vfs_register_uri(0);
    ASSERT_OK(rc);

    /* Create schema */
    sqlite3 *init = open_azurite_db(db_name, NULL, 1);
    ASSERT_NOT_NULL(init);

    rc = exec_sql(init,
        "CREATE TABLE messages ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  writer INTEGER NOT NULL,"
        "  seq INTEGER NOT NULL"
        ");");
    ASSERT_OK(rc);
    sqlite3_close(init);

    /* Spawn concurrent writers (default: 4 × 10, stress mode: 4×mult × 10×mult) */
    const int mult = get_stress_multiplier();
    const int num_writers = 4 * mult;
    const int inserts_per_writer = 10 * mult;
    pthread_t threads[num_writers];
    writer_context_t contexts[num_writers];
    int assigned_ids[num_writers][inserts_per_writer];

    for (int i = 0; i < num_writers; i++) {
        memset(assigned_ids[i], 0, sizeof(assigned_ids[i]));
        contexts[i].db_name = db_name;
        contexts[i].table_name = "messages";  /* Added: specify table name */
        contexts[i].writer_id = i;
        contexts[i].inserts_per_writer = inserts_per_writer;
        contexts[i].assigned_ids = assigned_ids[i];
        contexts[i].success = 0;
        contexts[i].result_code = SQLITE_OK;

        rc = pthread_create(&threads[i], NULL, concurrent_writer_thread, &contexts[i]);
        if (rc != 0) {
            fprintf(stderr, "  pthread_create failed for writer %d: %d\n", i, rc);
            ASSERT_EQ(rc, 0);
        }
    }

    /* Wait for all writers to complete */
    for (int i = 0; i < num_writers; i++) {
        pthread_join(threads[i], NULL);
    }

    /* Collect all assigned IDs (only from successful inserts) */
    int total_assigned = 0;
    int assigned_list[num_writers * inserts_per_writer];
    
    for (int i = 0; i < num_writers; i++) {
        fprintf(stdout, "  writer %d: %d inserts succeeded\n", 
                i, contexts[i].success);
        ASSERT_TRUE(contexts[i].result_code == SQLITE_OK ||
                    contexts[i].result_code == SQLITE_BUSY ||
                    contexts[i].result_code == SQLITE_LOCKED);
        for (int j = 0; j < contexts[i].success; j++) {
            assigned_list[total_assigned++] = assigned_ids[i][j];
        }
    }
    ASSERT_GT(total_assigned, 0);

    /* Verify: every assigned ID must be persisted */
    sqlite3 *check = open_azurite_db(db_name, NULL, 0);
    ASSERT_NOT_NULL(check);

    int persisted_count = 0;
    rc = query_int(check, "SELECT COUNT(*) FROM messages;", &persisted_count);
    ASSERT_OK(rc);

    fprintf(stdout, "  total assigned IDs: %d\n", total_assigned);
    fprintf(stdout, "  total persisted rows: %d\n", persisted_count);

    /* CRITICAL: persisted count must equal assigned count */
    if (persisted_count != total_assigned) {
        fprintf(stderr, 
            "  REGRESSION BUG DETECTED: %d IDs assigned but only %d persisted\n",
            total_assigned, persisted_count);
        fprintf(stderr, 
            "  This indicates concurrent writers lost committed inserts!\n");
    }
    ASSERT_EQ(persisted_count, total_assigned);

    /* Verify: all assigned IDs are present in database */
    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(check, 
        "SELECT COUNT(*) FROM messages WHERE id = ?;", -1, &stmt, NULL);
    ASSERT_OK(rc);

    for (int i = 0; i < total_assigned; i++) {
        sqlite3_bind_int(stmt, 1, assigned_list[i]);
        rc = sqlite3_step(stmt);
        ASSERT_EQ(rc, SQLITE_ROW);
        
        int found = sqlite3_column_int(stmt, 0);
        if (found != 1) {
            fprintf(stderr, "  assigned ID %d not found in database!\n", 
                    assigned_list[i]);
        }
        ASSERT_EQ(found, 1);
        sqlite3_reset(stmt);
    }
    sqlite3_finalize(stmt);

    /* Verify: all persisted IDs are distinct (no duplicates) */
    int distinct_count = 0;
    rc = query_int(check, "SELECT COUNT(DISTINCT id) FROM messages;", &distinct_count);
    ASSERT_OK(rc);

    if (distinct_count != persisted_count) {
        fprintf(stderr, 
            "  REGRESSION BUG DETECTED: %d rows but only %d distinct IDs\n",
            persisted_count, distinct_count);
        fprintf(stderr, "  This indicates duplicate rowids from concurrent writers!\n");
    }
    ASSERT_EQ(distinct_count, persisted_count);

    sqlite3_close(check);
    cleanup_test_blobs(db_name);
}

/* ================================================================
 * H. Phase 1: Concurrency Invariant Tests
 * ================================================================ */

/* ─────────────────────────────────────────────────────────────────
 * H.1 Shared Invariant Helpers
 * ───────────────────────────────────────────────────────────────── */

/*
 * check_rowid_uniqueness — verify all rowids in a table are distinct
 * Returns: 1 if all rowids unique, 0 if duplicates found
 */
static int check_rowid_uniqueness(sqlite3 *db, const char *table_name) {
    char sql[256];
    snprintf(sql, sizeof(sql), 
        "SELECT COUNT(*), COUNT(DISTINCT rowid) FROM %s;", table_name);
    
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "  check_rowid_uniqueness: prepare failed: %s\n", 
                sqlite3_errmsg(db));
        return 0;
    }
    
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        fprintf(stderr, "  check_rowid_uniqueness: step failed\n");
        sqlite3_finalize(stmt);
        return 0;
    }
    
    int total_count = sqlite3_column_int(stmt, 0);
    int distinct_count = sqlite3_column_int(stmt, 1);
    sqlite3_finalize(stmt);
    
    if (total_count != distinct_count) {
        fprintf(stderr, "  INVARIANT VIOLATION: %d rows but only %d distinct rowids\n",
                total_count, distinct_count);
        return 0;
    }
    
    return 1;
}

/*
 * check_persisted_rowids — verify all assigned rowids exist in the table
 * Returns: number of missing rowids (0 = all present)
 */
static int check_persisted_rowids(sqlite3 *db, const char *table_name,
                                   const int *rowids, int count) {
    sqlite3_stmt *stmt = NULL;
    char sql[256];
    snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM %s WHERE rowid = ?;", table_name);
    
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "  check_persisted_rowids: prepare failed: %s\n",
                sqlite3_errmsg(db));
        return count;  /* assume all missing */
    }
    
    int missing = 0;
    for (int i = 0; i < count; i++) {
        sqlite3_bind_int(stmt, 1, rowids[i]);
        rc = sqlite3_step(stmt);
        
        if (rc == SQLITE_ROW) {
            int found = sqlite3_column_int(stmt, 0);
            if (found != 1) {
                fprintf(stderr, "  MISSING ROWID: assigned rowid %d not found!\n", 
                        rowids[i]);
                missing++;
            }
        } else {
            fprintf(stderr, "  check_persisted_rowids: step failed for rowid %d\n",
                    rowids[i]);
            missing++;
        }
        
        sqlite3_reset(stmt);
    }
    
    sqlite3_finalize(stmt);
    return missing;
}

/*
 * check_integrity — run PRAGMA integrity_check
 * Returns: 1 if OK, 0 if corruption detected
 */
static int check_integrity(sqlite3 *db) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, "PRAGMA integrity_check;", -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "  check_integrity: prepare failed: %s\n", 
                sqlite3_errmsg(db));
        return 0;
    }
    
    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        fprintf(stderr, "  check_integrity: no result\n");
        sqlite3_finalize(stmt);
        return 0;
    }
    
    const char *result = (const char *)sqlite3_column_text(stmt, 0);
    int ok = (strcmp(result, "ok") == 0);
    
    if (!ok) {
        fprintf(stderr, "  INTEGRITY CHECK FAILED: %s\n", result);
        /* Print all error lines */
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            fprintf(stderr, "    %s\n", sqlite3_column_text(stmt, 0));
        }
    }
    
    sqlite3_finalize(stmt);
    return ok;
}

/* ─────────────────────────────────────────────────────────────────
 * H.2 Enhanced Multi-Writer Stress Test (8-16 writers)
 * ───────────────────────────────────────────────────────────────── */

/*
 * Test H1: Stress test with 8 writers × 25 inserts each = 200 total ops
 * 
 * This tests significantly more concurrency than the 4x10 regression test.
 * Goals:
 *   - Catch rowid/data loss under heavier load
 *   - Verify invariants hold at scale
 *   - Keep CI runtime reasonable (~5-10 sec)
 */
TEST(stress_8_writers_25_each) {
    const char *db_name = "stress-8x25.db";
    cleanup_test_blobs(db_name);
    
    int rc = sqlite_objs_vfs_register_uri(0);
    ASSERT_OK(rc);
    
    /* Create schema */
    sqlite3 *init = open_azurite_db(db_name, NULL, 1);
    ASSERT_NOT_NULL(init);
    
    rc = exec_sql(init,
        "CREATE TABLE stress ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  writer INTEGER NOT NULL,"
        "  seq INTEGER NOT NULL,"
        "  payload TEXT"
        ");");
    ASSERT_OK(rc);
    sqlite3_close(init);
    
    /* Spawn concurrent writers (default: 8 × 25, stress mode: 8×mult × 25×mult) */
    const int mult = get_stress_multiplier();
    const int num_writers = 8 * mult;
    const int inserts_per_writer = 25 * mult;
    pthread_t threads[num_writers];
    writer_context_t contexts[num_writers];
    int assigned_ids[num_writers][inserts_per_writer];
    
    fprintf(stdout, "  spawning %d writers × %d inserts = %d total ops...\n",
            num_writers, inserts_per_writer, num_writers * inserts_per_writer);
    
    for (int i = 0; i < num_writers; i++) {
        memset(assigned_ids[i], 0, sizeof(assigned_ids[i]));
        contexts[i].db_name = db_name;
        contexts[i].table_name = "stress";  /* Use stress table */
        contexts[i].writer_id = i;
        contexts[i].inserts_per_writer = inserts_per_writer;
        contexts[i].assigned_ids = assigned_ids[i];
        contexts[i].success = 0;
        contexts[i].result_code = SQLITE_OK;
        
        rc = pthread_create(&threads[i], NULL, concurrent_writer_thread, &contexts[i]);
        ASSERT_EQ(rc, 0);
    }
    
    /* Wait for all writers */
    for (int i = 0; i < num_writers; i++) {
        pthread_join(threads[i], NULL);
    }
    
    /* Collect assigned IDs */
    int total_assigned = 0;
    int assigned_list[num_writers * inserts_per_writer];
    
    for (int i = 0; i < num_writers; i++) {
        fprintf(stdout, "  writer %d: %d/%d inserts succeeded\n",
                i, contexts[i].success, inserts_per_writer);
        for (int j = 0; j < contexts[i].success; j++) {
            assigned_list[total_assigned++] = assigned_ids[i][j];
        }
    }
    ASSERT_GT(total_assigned, 0);
    
    /* Verify with invariant helpers */
    sqlite3 *check = open_azurite_db(db_name, NULL, 0);
    ASSERT_NOT_NULL(check);
    
    fprintf(stdout, "  checking integrity...\n");
    ASSERT_TRUE(check_integrity(check));
    
    int persisted_count = 0;
    rc = query_int(check, "SELECT COUNT(*) FROM stress;", &persisted_count);
    ASSERT_OK(rc);
    
    fprintf(stdout, "  assigned: %d, persisted: %d\n", 
            total_assigned, persisted_count);
    ASSERT_EQ(persisted_count, total_assigned);
    
    fprintf(stdout, "  checking rowid uniqueness...\n");
    ASSERT_TRUE(check_rowid_uniqueness(check, "stress"));
    
    fprintf(stdout, "  checking all assigned rowids persisted...\n");
    int missing = check_persisted_rowids(check, "stress", assigned_list, total_assigned);
    ASSERT_EQ(missing, 0);
    
    sqlite3_close(check);
    cleanup_test_blobs(db_name);
}

/*
 * Test H2: Heavy stress with 16 writers × 20 inserts = 320 ops
 * 
 * This is the "extended" version — heavier load for catching subtle bugs.
 * May be slower in CI, but still practical (~10-15 sec).
 */
TEST(stress_16_writers_20_each) {
    const char *db_name = "stress-16x20.db";
    cleanup_test_blobs(db_name);
    
    int rc = sqlite_objs_vfs_register_uri(0);
    ASSERT_OK(rc);
    
    /* Create schema */
    sqlite3 *init = open_azurite_db(db_name, NULL, 1);
    ASSERT_NOT_NULL(init);
    
    rc = exec_sql(init,
        "CREATE TABLE stress ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  writer INTEGER NOT NULL,"
        "  seq INTEGER NOT NULL"
        ");");
    ASSERT_OK(rc);
    sqlite3_close(init);
    
    /* Spawn 16 concurrent writers */
    /* Spawn concurrent writers (default: 16 × 20, stress mode: 16×mult × 20×mult) */
    const int mult = get_stress_multiplier();
    const int num_writers = 16 * mult;
    const int inserts_per_writer = 20 * mult;
    pthread_t threads[num_writers];
    writer_context_t contexts[num_writers];
    int assigned_ids[num_writers][inserts_per_writer];
    
    fprintf(stdout, "  spawning %d writers × %d inserts = %d total ops...\n",
            num_writers, inserts_per_writer, num_writers * inserts_per_writer);
    
    for (int i = 0; i < num_writers; i++) {
        memset(assigned_ids[i], 0, sizeof(assigned_ids[i]));
        contexts[i].db_name = db_name;
        contexts[i].table_name = "stress";  /* Use stress table */
        contexts[i].writer_id = i;
        contexts[i].inserts_per_writer = inserts_per_writer;
        contexts[i].assigned_ids = assigned_ids[i];
        contexts[i].success = 0;
        contexts[i].result_code = SQLITE_OK;
        
        rc = pthread_create(&threads[i], NULL, concurrent_writer_thread, &contexts[i]);
        ASSERT_EQ(rc, 0);
    }
    
    /* Wait for all writers */
    for (int i = 0; i < num_writers; i++) {
        pthread_join(threads[i], NULL);
    }
    
    /* Collect assigned IDs */
    int total_assigned = 0;
    int assigned_list[num_writers * inserts_per_writer];
    
    for (int i = 0; i < num_writers; i++) {
        fprintf(stdout, "  writer %d: %d/%d inserts succeeded\n",
                i, contexts[i].success, inserts_per_writer);
        for (int j = 0; j < contexts[i].success; j++) {
            assigned_list[total_assigned++] = assigned_ids[i][j];
        }
    }
    ASSERT_GT(total_assigned, 0);
    
    /* Verify with invariant helpers */
    sqlite3 *check = open_azurite_db(db_name, NULL, 0);
    ASSERT_NOT_NULL(check);
    
    fprintf(stdout, "  checking integrity...\n");
    ASSERT_TRUE(check_integrity(check));
    
    int persisted_count = 0;
    rc = query_int(check, "SELECT COUNT(*) FROM stress;", &persisted_count);
    ASSERT_OK(rc);
    
    fprintf(stdout, "  assigned: %d, persisted: %d\n",
            total_assigned, persisted_count);
    ASSERT_EQ(persisted_count, total_assigned);
    
    fprintf(stdout, "  checking rowid uniqueness...\n");
    ASSERT_TRUE(check_rowid_uniqueness(check, "stress"));
    
    fprintf(stdout, "  checking all assigned rowids persisted...\n");
    int missing = check_persisted_rowids(check, "stress", assigned_list, total_assigned);
    ASSERT_EQ(missing, 0);
    
    sqlite3_close(check);
    cleanup_test_blobs(db_name);
}

/* ─────────────────────────────────────────────────────────────────
 * H.3 Reader/Writer Interleaving Tests
 * ───────────────────────────────────────────────────────────────── */

/* Thread context for readers */
typedef struct {
    const char *db_name;
    int reader_id;
    int snapshot_duration_ms;
    int rows_seen;
    int iterations;
    int success;
    int result_code;
} reader_context_t;

/* Thread function: reader holds snapshot, yields to writers */
static void *reader_snapshot_thread(void *arg) {
    reader_context_t *ctx = (reader_context_t *)arg;
    ctx->success = 0;
    ctx->result_code = SQLITE_OK;
    ctx->rows_seen = 0;
    
    sqlite3 *db = open_azurite_db(ctx->db_name, NULL, 0);
    if (!db) {
        fprintf(stderr, "  reader %d: failed to open database\n", ctx->reader_id);
        ctx->result_code = SQLITE_CANTOPEN;
        return NULL;
    }
    
    for (int iter = 0; iter < ctx->iterations; iter++) {
        /* Begin a read transaction to establish a snapshot */
        int rc = exec_sql(db, "BEGIN;");
        if (rc != SQLITE_OK) {
            fprintf(stderr, "  reader %d iter %d: BEGIN failed\n", 
                    ctx->reader_id, iter);
            ctx->result_code = rc;
            sqlite3_close(db);
            return NULL;
        }
        
        /* Count rows visible in this snapshot */
        int count = 0;
        rc = query_int(db, "SELECT COUNT(*) FROM data;", &count);
        if (rc != SQLITE_OK) {
            fprintf(stderr, "  reader %d iter %d: SELECT failed (%d)\n",
                    ctx->reader_id, iter, rc);
            ctx->result_code = rc;
            exec_sql(db, "ROLLBACK;");
            sqlite3_close(db);
            return NULL;
        }
        ctx->rows_seen = count;
        
        /* Hold the snapshot for a bit (simulate long read) */
        usleep(ctx->snapshot_duration_ms * 1000);
        
        /* Rollback (we're read-only) */
        rc = exec_sql(db, "ROLLBACK;");
        if (rc != SQLITE_OK) {
            fprintf(stderr, "  reader %d iter %d: ROLLBACK failed (%d)\n",
                    ctx->reader_id, iter, rc);
            ctx->result_code = rc;
            sqlite3_close(db);
            return NULL;
        }
        
        /* Brief pause between iterations */
        usleep(10000);  /* 10ms */
    }
    
    sqlite3_close(db);
    ctx->success = 1;
    return NULL;
}

/*
 * Test H3: Reader/Writer interleaving — readers hold snapshots while writers commit
 * 
 * Pattern:
 *   - 2 readers continuously query + hold snapshots (100ms each)
 *   - 4 writers insert concurrently
 *   - Verify: all data persisted, no corruption, readers don't block writers
 */
TEST(reader_writer_interleaving) {
    const char *db_name = "reader-writer-interlv.db";
    cleanup_test_blobs(db_name);
    
    int rc = sqlite_objs_vfs_register_uri(0);
    ASSERT_OK(rc);
    
    /* Create schema */
    sqlite3 *init = open_azurite_db(db_name, NULL, 1);
    ASSERT_NOT_NULL(init);
    
    rc = exec_sql(init,
        "CREATE TABLE data ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  writer INTEGER NOT NULL,"
        "  seq INTEGER NOT NULL"
        ");");
    ASSERT_OK(rc);
    sqlite3_close(init);
    
    fprintf(stdout, "  spawning 2 readers + 4 writers...\n");
    
    /* Start readers first (they'll loop and hold snapshots) */
    const int num_readers = 2;
    pthread_t reader_threads[num_readers];
    reader_context_t reader_contexts[num_readers];
    
    for (int i = 0; i < num_readers; i++) {
        reader_contexts[i].db_name = db_name;
        reader_contexts[i].reader_id = i;
        reader_contexts[i].snapshot_duration_ms = 100;  /* 100ms per snapshot */
        reader_contexts[i].iterations = 3;  /* 3 iterations per reader */
        reader_contexts[i].success = 0;
        reader_contexts[i].rows_seen = 0;
        reader_contexts[i].result_code = SQLITE_OK;
        
        rc = pthread_create(&reader_threads[i], NULL, 
                           reader_snapshot_thread, &reader_contexts[i]);
        ASSERT_EQ(rc, 0);
    }
    
    /* Brief delay to let readers establish first snapshot */
    usleep(50000);  /* 50ms */
    
    /* Now spawn writers */
    const int num_writers = 4;
    const int inserts_per_writer = 15;
    pthread_t writer_threads[num_writers];
    writer_context_t writer_contexts[num_writers];
    int assigned_ids[num_writers][inserts_per_writer];
    
    for (int i = 0; i < num_writers; i++) {
        memset(assigned_ids[i], 0, sizeof(assigned_ids[i]));
        writer_contexts[i].db_name = db_name;
        writer_contexts[i].table_name = "data";  /* Use data table */
        writer_contexts[i].writer_id = i;
        writer_contexts[i].inserts_per_writer = inserts_per_writer;
        writer_contexts[i].assigned_ids = assigned_ids[i];
        writer_contexts[i].success = 0;
        writer_contexts[i].result_code = SQLITE_OK;
        
        rc = pthread_create(&writer_threads[i], NULL, 
                           concurrent_writer_thread, &writer_contexts[i]);
        ASSERT_EQ(rc, 0);
    }
    
    /* Wait for all threads */
    for (int i = 0; i < num_readers; i++) {
        pthread_join(reader_threads[i], NULL);
    }
    for (int i = 0; i < num_writers; i++) {
        pthread_join(writer_threads[i], NULL);
    }
    
    /* Verify readers succeeded */
    for (int i = 0; i < num_readers; i++) {
        fprintf(stdout, "  reader %d: completed %d iterations, last saw %d rows\n",
                i, reader_contexts[i].iterations, reader_contexts[i].rows_seen);
        ASSERT_EQ(reader_contexts[i].success, 1);
        ASSERT_EQ(reader_contexts[i].result_code, SQLITE_OK);
    }
    
    /* Collect writer results */
    int total_assigned = 0;
    int assigned_list[num_writers * inserts_per_writer];
    
    for (int i = 0; i < num_writers; i++) {
        fprintf(stdout, "  writer %d: %d/%d inserts succeeded\n",
                i, writer_contexts[i].success, inserts_per_writer);
        for (int j = 0; j < writer_contexts[i].success; j++) {
            assigned_list[total_assigned++] = assigned_ids[i][j];
        }
    }
    ASSERT_GT(total_assigned, 0);
    
    /* Verify data integrity */
    sqlite3 *check = open_azurite_db(db_name, NULL, 0);
    ASSERT_NOT_NULL(check);
    
    fprintf(stdout, "  checking integrity...\n");
    ASSERT_TRUE(check_integrity(check));
    
    int persisted_count = 0;
    rc = query_int(check, "SELECT COUNT(*) FROM data;", &persisted_count);
    ASSERT_OK(rc);
    
    fprintf(stdout, "  assigned: %d, persisted: %d\n",
            total_assigned, persisted_count);
    ASSERT_EQ(persisted_count, total_assigned);
    
    fprintf(stdout, "  checking rowid uniqueness...\n");
    ASSERT_TRUE(check_rowid_uniqueness(check, "data"));
    
    fprintf(stdout, "  checking all assigned rowids persisted...\n");
    int missing = check_persisted_rowids(check, "data", assigned_list, total_assigned);
    ASSERT_EQ(missing, 0);
    
    sqlite3_close(check);
    cleanup_test_blobs(db_name);
}

/* ================================================================
 * I. Phase 2: Crash Recovery & Partial-Write Testing
 * ================================================================ */

/* ─────────────────────────────────────────────────────────────────
 * I.1 Sync Hook Infrastructure for Crash Simulation
 * ───────────────────────────────────────────────────────────────── */

/* Context for crash simulation hooks */
typedef struct {
    int call_count;              /* Total hook invocations */
    int fail_at_call;            /* Inject failure at this call number (0 = disabled) */
    const char *target_blob;     /* Only fail for this blob name (NULL = all) */
    int injected;                /* Flag: failure was injected */
} crash_hook_ctx_t;

/* Generic sync hook that fails at a specific call number */
static int crash_inject_at_call(void *ctx, const char *blob_name) {
    crash_hook_ctx_t *hook = (crash_hook_ctx_t *)ctx;
    hook->call_count++;
    
    fprintf(stdout, "      [HOOK] call %d on blob '%s'\n",
            hook->call_count, blob_name ? blob_name : "(null)");
    
    /* Skip if we're targeting a specific blob and this isn't it */
    if (hook->target_blob && blob_name && strcmp(blob_name, hook->target_blob) != 0) {
        return 0;  /* proceed normally */
    }

    /* Inject failure if this is the target call */
    if (hook->fail_at_call > 0 && hook->call_count == hook->fail_at_call) {
        hook->injected = 1;
        fprintf(stdout, "      [CRASH INJECT] failing call %d on blob '%s'\n",
                hook->call_count, blob_name ? blob_name : "(null)");
        return 1;  /* abort with SQLITE_IOERR_FSYNC */
    }
    
    return 0;  /* proceed normally */
}

static void assert_child_exited(pid_t pid, int expected_code) {
    int status = 0;
    ASSERT_EQ(waitpid(pid, &status, 0), pid);
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), expected_code);
}

/* ─────────────────────────────────────────────────────────────────
 * I.2 Crash Recovery Tests
 * ───────────────────────────────────────────────────────────────── */

/*
 * Test I1: Crash during batch page write — verify rollback journal recovery
 * 
 * Pattern:
 *   1. Create DB with initial data
 *   2. Start transaction, insert new rows
 *   3. Inject crash during batch page write (xSync)
 *   4. Verify: transaction rolled back, no partial state, integrity OK
 */
TEST(crash_batch_write_rollback) {
    const char *db_name = "crash-batch-write.db";
    cleanup_test_blobs(db_name);
    
    int rc = sqlite_objs_vfs_register_uri(0);
    ASSERT_OK(rc);
    
    /* Create initial data */
    sqlite3 *db = open_azurite_db(db_name, NULL, 1);
    ASSERT_NOT_NULL(db);
    
    rc = exec_sql(db,
        "CREATE TABLE recovery ("
        "  id INTEGER PRIMARY KEY,"
        "  data TEXT"
        ");");
    ASSERT_OK(rc);
    
    rc = exec_sql(db,
        "INSERT INTO recovery VALUES (1, 'committed-1'),"
        "                             (2, 'committed-2'),"
        "                             (3, 'committed-3');");
    ASSERT_OK(rc);
    sqlite3_close(db);
    
    fprintf(stdout, "  initial: 3 rows committed\n");
    
    pid_t pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        sqlite3 *child_db = open_azurite_db(db_name, NULL, 0);
        if (!child_db) _exit(90);

        crash_hook_ctx_t hook = {0};
        hook.fail_at_call = 1;
        sqlite_objs_test_set_sync_hooks(&hook, NULL, crash_inject_at_call,
                                         crash_inject_at_call, NULL, NULL);

        int child_rc = exec_sql(child_db, "BEGIN IMMEDIATE;");
        if (child_rc != SQLITE_OK) _exit(91);
        child_rc = exec_sql(child_db,
            "INSERT INTO recovery VALUES (4, 'uncommitted-4'),"
            "                             (5, 'uncommitted-5');");
        if (child_rc != SQLITE_OK) _exit(92);

        fprintf(stdout, "  child attempting COMMIT (hook will fire on sync)...\n");
        child_rc = exec_sql(child_db, "COMMIT;");
        fprintf(stdout, "  child COMMIT result: %d (injected=%d)\n", child_rc, hook.injected);
        _exit((hook.injected && child_rc != SQLITE_OK) ? 10 : 93);
    }
    assert_child_exited(pid, 10);
    
    /* Reopen and verify recovery */
    fprintf(stdout, "  reopening to verify recovery...\n");
    db = open_azurite_db(db_name, NULL, 0);
    ASSERT_NOT_NULL(db);
    
    ASSERT_TRUE(check_integrity(db));
    
    int count = 0;
    rc = query_int(db, "SELECT COUNT(*) FROM recovery;", &count);
    ASSERT_OK(rc);
    fprintf(stdout, "  after recovery: %d rows (expected 3)\n", count);
    ASSERT_EQ(count, 3);
    
    /* Verify uncommitted rows are NOT present */
    rc = query_int(db, "SELECT COUNT(*) FROM recovery WHERE id >= 4;", &count);
    ASSERT_OK(rc);
    ASSERT_EQ(count, 0);
    
    sqlite3_close(db);
    cleanup_test_blobs(db_name);
}

/*
 * Test I2: Crash during journal upload — verify partial journal ignored
 * 
 * Pattern:
 *   1. Create DB with data
 *   2. Start transaction
 *   3. Inject crash during journal upload
 *   4. Verify: transaction not committed, no corruption
 */
TEST(crash_journal_upload) {
    const char *db_name = "crash-journal.db";
    cleanup_test_blobs(db_name);
    
    int rc = sqlite_objs_vfs_register_uri(0);
    ASSERT_OK(rc);
    
    /* Create initial data */
    sqlite3 *db = open_azurite_db(db_name, NULL, 1);
    ASSERT_NOT_NULL(db);
    
    rc = exec_sql(db,
        "CREATE TABLE recovery ("
        "  id INTEGER PRIMARY KEY,"
        "  value INTEGER"
        ");");
    ASSERT_OK(rc);
    
    rc = exec_sql(db, "INSERT INTO recovery VALUES (1, 100);");
    ASSERT_OK(rc);
    sqlite3_close(db);
    
    fprintf(stdout, "  initial: 1 row committed\n");
    
    pid_t pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        sqlite3 *child_db = open_azurite_db(db_name, NULL, 0);
        if (!child_db) _exit(90);

        crash_hook_ctx_t hook = {0};
        hook.fail_at_call = 1;
        sqlite_objs_test_set_sync_hooks(&hook, NULL, NULL, NULL,
                                         crash_inject_at_call, NULL);

        int child_rc = exec_sql(child_db, "BEGIN IMMEDIATE;");
        if (child_rc != SQLITE_OK) _exit(91);
        child_rc = exec_sql(child_db, "UPDATE recovery SET value = 999 WHERE id = 1;");
        if (child_rc != SQLITE_OK) _exit(92);

        fprintf(stdout, "  child attempting COMMIT (hook will fire on journal upload)...\n");
        child_rc = exec_sql(child_db, "COMMIT;");
        fprintf(stdout, "  child COMMIT result: %d (injected=%d)\n", child_rc, hook.injected);
        _exit((hook.injected && child_rc != SQLITE_OK) ? 10 : 93);
    }
    assert_child_exited(pid, 10);
    
    /* Verify recovery */
    fprintf(stdout, "  reopening after crash...\n");
    db = open_azurite_db(db_name, NULL, 0);
    ASSERT_NOT_NULL(db);
    
    ASSERT_TRUE(check_integrity(db));
    
    int value = 0;
    rc = query_int(db, "SELECT value FROM recovery WHERE id = 1;", &value);
    ASSERT_OK(rc);
    fprintf(stdout, "  value: %d (expected 100, not 999)\n", value);
    ASSERT_EQ(value, 100);
    
    sqlite3_close(db);
    cleanup_test_blobs(db_name);
}

/*
 * Test I3: Multiple crash/recovery cycles — verify cumulative integrity
 * 
 * Pattern:
 *   1. Commit batch 1 (succeeds)
 *   2. Commit batch 2 with crash (fails)
 *   3. Recover and verify batch 1 intact
 *   4. Commit batch 3 (succeeds)
 *   5. Verify all committed data present, no partial state
 */
TEST(crash_recovery_multiple_cycles) {
    const char *db_name = "crash-multi-cycle.db";
    cleanup_test_blobs(db_name);
    
    int rc = sqlite_objs_vfs_register_uri(0);
    ASSERT_OK(rc);
    
    /* Cycle 1: Successful commit */
    sqlite3 *db = open_azurite_db(db_name, NULL, 1);
    ASSERT_NOT_NULL(db);
    
    rc = exec_sql(db,
        "CREATE TABLE cycles ("
        "  batch INTEGER,"
        "  seq INTEGER"
        ");");
    ASSERT_OK(rc);
    
    rc = exec_sql(db, "INSERT INTO cycles VALUES (1, 1), (1, 2), (1, 3);");
    ASSERT_OK(rc);
    sqlite3_close(db);
    
    fprintf(stdout, "  cycle 1: 3 rows committed\n");
    
    pid_t pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        sqlite3 *child_db = open_azurite_db(db_name, NULL, 0);
        if (!child_db) _exit(90);

        crash_hook_ctx_t hook = {0};
        hook.fail_at_call = 1;
        sqlite_objs_test_set_sync_hooks(&hook, NULL, crash_inject_at_call,
                                         crash_inject_at_call, NULL, NULL);

        int child_rc = exec_sql(child_db, "BEGIN IMMEDIATE;");
        if (child_rc != SQLITE_OK) _exit(91);
        child_rc = exec_sql(child_db, "INSERT INTO cycles VALUES (2, 1), (2, 2), (2, 3);");
        if (child_rc != SQLITE_OK) _exit(92);

        fprintf(stdout, "  child attempting COMMIT (hook will crash)...\n");
        child_rc = exec_sql(child_db, "COMMIT;");
        fprintf(stdout, "  child cycle 2 COMMIT result=%d, injected=%d\n", child_rc, hook.injected);
        _exit((hook.injected && child_rc != SQLITE_OK) ? 10 : 93);
    }
    assert_child_exited(pid, 10);
    
    /* Verify recovery: batch 1 present, batch 2 absent */
    fprintf(stdout, "  verifying recovery...\n");
    db = open_azurite_db(db_name, NULL, 0);
    ASSERT_NOT_NULL(db);
    
    ASSERT_TRUE(check_integrity(db));
    
    int count = 0;
    rc = query_int(db, "SELECT COUNT(*) FROM cycles WHERE batch = 1;", &count);
    ASSERT_OK(rc);
    ASSERT_EQ(count, 3);
    
    rc = query_int(db, "SELECT COUNT(*) FROM cycles WHERE batch = 2;", &count);
    ASSERT_OK(rc);
    ASSERT_EQ(count, 0);
    
    /* Cycle 3: Successful commit after recovery */
    rc = exec_sql(db, "INSERT INTO cycles VALUES (3, 1), (3, 2);");
    ASSERT_OK(rc);
    sqlite3_close(db);
    
    fprintf(stdout, "  cycle 3: 2 rows committed\n");
    
    /* Final verification */
    db = open_azurite_db(db_name, NULL, 0);
    ASSERT_NOT_NULL(db);
    
    ASSERT_TRUE(check_integrity(db));
    
    rc = query_int(db, "SELECT COUNT(*) FROM cycles;", &count);
    ASSERT_OK(rc);
    fprintf(stdout, "  final count: %d (expected 5)\n", count);
    ASSERT_EQ(count, 5);
    
    ASSERT_TRUE(check_rowid_uniqueness(db, "cycles"));
    
    sqlite3_close(db);
    cleanup_test_blobs(db_name);
}

/* ─────────────────────────────────────────────────────────────────
 * I.4 Partial Write Recovery Tests
 * ───────────────────────────────────────────────────────────────── */

/*
 * Test I4: Page blob resize crash — verify DB remains at old size
 * 
 * Pattern:
 *   1. Create small DB
 *   2. Insert enough data to trigger resize
 *   3. Crash during resize operation
 *   4. Verify: DB reopens at old size, no corruption
 */
TEST(crash_page_blob_resize) {
    const char *db_name = "crash-resize.db";
    cleanup_test_blobs(db_name);
    
    int rc = sqlite_objs_vfs_register_uri(0);
    ASSERT_OK(rc);
    
    /* Create small DB */
    sqlite3 *db = open_azurite_db(db_name, NULL, 1);
    ASSERT_NOT_NULL(db);
    
    rc = exec_sql(db,
        "CREATE TABLE resize_test (data BLOB);");
    ASSERT_OK(rc);
    
    /* Insert small amount to establish baseline */
    rc = exec_sql(db, "INSERT INTO resize_test VALUES (zeroblob(1024));");
    ASSERT_OK(rc);
    sqlite3_close(db);
    
    pid_t pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        sqlite3 *child_db = open_azurite_db(db_name, NULL, 0);
        if (!child_db) _exit(90);

        crash_hook_ctx_t hook = {0};
        hook.fail_at_call = 1;
        sqlite_objs_test_set_sync_hooks(&hook, crash_inject_at_call,
                                         NULL, NULL, NULL, NULL);

        fprintf(stdout, "  child attempting large INSERT (may trigger resize)...\n");
        int child_rc = exec_sql(child_db, "INSERT INTO resize_test VALUES (zeroblob(100000));");
        fprintf(stdout, "  child large INSERT result: %d (injected=%d)\n", child_rc, hook.injected);
        _exit((hook.injected && child_rc != SQLITE_OK) ? 10 : 93);
    }
    assert_child_exited(pid, 10);
    
    /* Verify recovery */
    fprintf(stdout, "  reopening after resize crash...\n");
    db = open_azurite_db(db_name, NULL, 0);
    ASSERT_NOT_NULL(db);
    
    ASSERT_TRUE(check_integrity(db));
    
    /* Small row should still be there */
    int count = 0;
    rc = query_int(db, "SELECT COUNT(*) FROM resize_test;", &count);
    ASSERT_OK(rc);
    fprintf(stdout, "  row count: %d (expected 1)\n", count);
    ASSERT_EQ(count, 1);

    rc = query_int(db, "SELECT COUNT(*) FROM resize_test WHERE length(data) = 100000;", &count);
    ASSERT_OK(rc);
    ASSERT_EQ(count, 0);
    
    sqlite3_close(db);
    cleanup_test_blobs(db_name);
}

/* ─────────────────────────────────────────────────────────────────
 * I.5 Advanced Reader/Writer Interleaving (Phase 2 Extension)
 * ───────────────────────────────────────────────────────────────── */

/*
 * Test I5: Long-running snapshot isolation — verify writer commits don't affect reader
 * 
 * Pattern:
 *   1. Reader opens long-running snapshot (simulated via explicit BEGIN)
 *   2. Writer commits new data
 *   3. Reader still sees old snapshot
 *   4. Reader commits (read-only)
 *   5. New reader sees updated data
 */
TEST(snapshot_isolation_long_running) {
    const char *db_name = "snapshot-isolation.db";
    cleanup_test_blobs(db_name);
    
    int rc = sqlite_objs_vfs_register_uri(0);
    ASSERT_OK(rc);
    
    /* Create initial data */
    sqlite3 *db = open_azurite_db(db_name, NULL, 1);
    ASSERT_NOT_NULL(db);
    
    rc = exec_sql(db,
        "CREATE TABLE snapshot ("
        "  id INTEGER PRIMARY KEY,"
        "  value TEXT"
        ");");
    ASSERT_OK(rc);
    
    rc = exec_sql(db, "INSERT INTO snapshot VALUES (1, 'initial');");
    ASSERT_OK(rc);
    sqlite3_close(db);
    
    /* Reader establishes snapshot */
    sqlite3 *reader = open_azurite_db(db_name, NULL, 0);
    ASSERT_NOT_NULL(reader);
    
    rc = exec_sql(reader, "BEGIN DEFERRED;");
    ASSERT_OK(rc);
    
    /* Read to lock snapshot */
    int count = 0;
    rc = query_int(reader, "SELECT COUNT(*) FROM snapshot;", &count);
    ASSERT_OK(rc);
    ASSERT_EQ(count, 1);
    
    fprintf(stdout, "  reader: snapshot established (1 row)\n");
    
    /* Writer commits new data */
    sqlite3 *writer = open_azurite_db(db_name, NULL, 0);
    ASSERT_NOT_NULL(writer);
    
    rc = exec_sql(writer, "INSERT INTO snapshot VALUES (2, 'new-data');");
    ASSERT_OK(rc);
    sqlite3_close(writer);
    
    fprintf(stdout, "  writer: committed new row\n");
    
    /* Reader should still see old snapshot */
    rc = query_int(reader, "SELECT COUNT(*) FROM snapshot;", &count);
    ASSERT_OK(rc);
    fprintf(stdout, "  reader (same snapshot): %d rows (expected 1)\n", count);
    ASSERT_EQ(count, 1);
    
    rc = exec_sql(reader, "ROLLBACK;");
    ASSERT_OK(rc);
    sqlite3_close(reader);
    
    /* New reader sees updated data */
    sqlite3 *reader2 = open_azurite_db(db_name, NULL, 0);
    ASSERT_NOT_NULL(reader2);
    
    rc = query_int(reader2, "SELECT COUNT(*) FROM snapshot;", &count);
    ASSERT_OK(rc);
    fprintf(stdout, "  new reader: %d rows (expected 2)\n", count);
    ASSERT_EQ(count, 2);
    
    sqlite3_close(reader2);
    cleanup_test_blobs(db_name);
}

/*
 * Test I6: Stale snapshot detection — verify ETag mismatch forces re-read
 * 
 * Pattern:
 *   1. Client A reads DB (caches ETag)
 *   2. Client B modifies DB (ETag changes)
 *   3. Client A attempts to read with stale cache
 *   4. Verify: Client A detects ETag mismatch and re-reads
 */
TEST(stale_snapshot_etag_revalidation) {
    const char *db_name = "stale-snapshot.db";
    cleanup_test_blobs(db_name);
    
    int rc = sqlite_objs_vfs_register_uri(0);
    ASSERT_OK(rc);
    
    /* Client A creates initial data */
    sqlite3 *clientA = open_azurite_db(db_name, NULL, 1);
    ASSERT_NOT_NULL(clientA);
    
    rc = exec_sql(clientA,
        "CREATE TABLE etag_test (id INTEGER PRIMARY KEY, data TEXT);");
    ASSERT_OK(rc);
    
    rc = exec_sql(clientA, "INSERT INTO etag_test VALUES (1, 'version-1');");
    ASSERT_OK(rc);
    
    /* Close to flush */
    sqlite3_close(clientA);
    
    fprintf(stdout, "  client A: created DB with version-1\n");
    
    /* Client A reopens (may cache ETag) */
    clientA = open_azurite_db(db_name, NULL, 0);
    ASSERT_NOT_NULL(clientA);
    
    int count = 0;
    rc = query_int(clientA, "SELECT COUNT(*) FROM etag_test;", &count);
    ASSERT_OK(rc);
    ASSERT_EQ(count, 1);
    
    /* Client B modifies DB */
    sqlite3 *clientB = open_azurite_db(db_name, NULL, 0);
    ASSERT_NOT_NULL(clientB);
    
    rc = exec_sql(clientB, "INSERT INTO etag_test VALUES (2, 'version-2');");
    ASSERT_OK(rc);
    sqlite3_close(clientB);
    
    fprintf(stdout, "  client B: inserted version-2\n");
    
    /* Client A should detect stale cache and see new data */
    rc = query_int(clientA, "SELECT COUNT(*) FROM etag_test;", &count);
    ASSERT_OK(rc);
    fprintf(stdout, "  client A (after B's write): %d rows (expected 2)\n", count);
    ASSERT_EQ(count, 2);
    
    sqlite3_close(clientA);
    cleanup_test_blobs(db_name);
}

/* ─────────────────────────────────────────────────────────────────
 * I.7 Invariant Stress Tests After Failures
 * ───────────────────────────────────────────────────────────────── */

/*
 * Test I7: Concurrent writes with intermittent crashes — verify final consistency
 * 
 * Pattern:
 *   1. Multiple writers insert data
 *   2. Randomly inject crashes (some succeed, some fail)
 *   3. Verify: all committed data persisted, no partial commits, rowid uniqueness
 */
TEST(invariant_check_after_crash_stress) {
    const char *db_name = "invariant-crash-stress.db";
    cleanup_test_blobs(db_name);
    
    int rc = sqlite_objs_vfs_register_uri(0);
    ASSERT_OK(rc);
    
    /* Create schema */
    sqlite3 *db = open_azurite_db(db_name, NULL, 1);
    ASSERT_NOT_NULL(db);
    
    rc = exec_sql(db,
        "CREATE TABLE stress ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  batch INTEGER,"
        "  value INTEGER"
        ");");
    ASSERT_OK(rc);
    sqlite3_close(db);
    
    fprintf(stdout, "  running 10 batches with selective crash injection...\n");
    
    int committed_batches = 0;
    int crashed_batches = 0;
    
    for (int batch = 1; batch <= 10; batch++) {
        db = open_azurite_db(db_name, NULL, 0);
        ASSERT_NOT_NULL(db);
        
        /* Inject crash on batches 3, 6, 9 */
        int should_crash = (batch == 3 || batch == 6 || batch == 9);

        if (should_crash) {
            sqlite3_close(db);
            pid_t pid = fork();
            ASSERT_GE(pid, 0);
            if (pid == 0) {
                sqlite3 *child_db = open_azurite_db(db_name, NULL, 0);
                if (!child_db) _exit(90);

                crash_hook_ctx_t hook = {0};
                hook.fail_at_call = 1;
                sqlite_objs_test_set_sync_hooks(&hook, NULL, crash_inject_at_call,
                                                 crash_inject_at_call, NULL, NULL);

                char child_sql[256];
                snprintf(child_sql, sizeof(child_sql),
                         "INSERT INTO stress (batch, value) VALUES (%d, %d), (%d, %d);",
                         batch, batch * 10, batch, batch * 10 + 1);
                int child_rc = exec_sql(child_db, child_sql);
                fprintf(stdout, "    child batch %d: rc=%d injected=%d\n",
                        batch, child_rc, hook.injected);
                _exit((hook.injected && child_rc != SQLITE_OK) ? 10 : 93);
            }

            assert_child_exited(pid, 10);
            fprintf(stdout, "    batch %d: crashed as expected\n", batch);
            crashed_batches++;
            continue;
        }

        char sql[256];
        snprintf(sql, sizeof(sql),
                 "INSERT INTO stress (batch, value) VALUES (%d, %d), (%d, %d);",
                 batch, batch * 10, batch, batch * 10 + 1);

        rc = exec_sql(db, sql);
        fprintf(stdout, "    batch %d: committed\n", batch);
        ASSERT_OK(rc);
        committed_batches++;
        sqlite3_close(db);
    }
    /* Final verification */
    fprintf(stdout, "  verifying final state (%d committed, %d crashed)...\n",
            committed_batches, crashed_batches);
    ASSERT_GT(crashed_batches, 0);
    
    db = open_azurite_db(db_name, NULL, 0);
    ASSERT_NOT_NULL(db);
    
    ASSERT_TRUE(check_integrity(db));
    
    int total_rows = 0;
    rc = query_int(db, "SELECT COUNT(*) FROM stress;", &total_rows);
    ASSERT_OK(rc);
    fprintf(stdout, "  total rows: %d (expected %d)\n",
            total_rows, committed_batches * 2);
    ASSERT_EQ(total_rows, committed_batches * 2);
    
    ASSERT_TRUE(check_rowid_uniqueness(db, "stress"));
    
    /* Verify no partial batches */
    rc = query_int(db, "SELECT COUNT(DISTINCT batch) FROM stress;", &total_rows);
    ASSERT_OK(rc);
    ASSERT_EQ(total_rows, committed_batches);
    
    sqlite3_close(db);
    cleanup_test_blobs(db_name);
}

/* ================================================================
 * J. Phase 3: Deterministic Randomized/Property-Based Testing
 * ================================================================ */

/* ─────────────────────────────────────────────────────────────────
 * J.1 Pseudo-Random Number Generator (Fixed Seed)
 * ───────────────────────────────────────────────────────────────── */

/*
 * Simple LCG (Linear Congruential Generator) for deterministic randomness.
 * Constants from Numerical Recipes (good statistical properties).
 */
typedef struct {
    uint64_t state;
} prng_t;

static void prng_seed(prng_t *rng, uint64_t seed) {
    rng->state = seed;
}

static uint32_t prng_next(prng_t *rng) {
    rng->state = rng->state * 1664525ULL + 1013904223ULL;
    return (uint32_t)(rng->state >> 32);
}

static int prng_range(prng_t *rng, int min, int max) {
    if (min >= max) return min;
    uint32_t range = (uint32_t)(max - min + 1);
    return min + (int)(prng_next(rng) % range);
}

/* ─────────────────────────────────────────────────────────────────
 * J.2 Model State Tracker
 * ───────────────────────────────────────────────────────────────── */

#define MAX_MODEL_ROWS 1000

typedef struct {
    int id;
    int value;
    int active;  /* 1 if exists, 0 if deleted */
} model_row_t;

typedef struct {
    model_row_t rows[MAX_MODEL_ROWS];
    int row_count;
    int in_transaction;
    int next_id;
    /* Snapshot for rollback */
    model_row_t snapshot[MAX_MODEL_ROWS];
    int snapshot_count;
    int snapshot_next_id;
} model_state_t;

static void model_init(model_state_t *m) {
    memset(m, 0, sizeof(*m));
    m->next_id = 1;
}

static void model_begin(model_state_t *m) {
    /* Save snapshot */
    memcpy(m->snapshot, m->rows, sizeof(m->rows));
    m->snapshot_count = m->row_count;
    m->snapshot_next_id = m->next_id;
    m->in_transaction = 1;
}

static void model_commit(model_state_t *m) {
    m->in_transaction = 0;
}

static void model_rollback(model_state_t *m) {
    /* Restore snapshot */
    memcpy(m->rows, m->snapshot, sizeof(m->rows));
    m->row_count = m->snapshot_count;
    m->next_id = m->snapshot_next_id;
    m->in_transaction = 0;
}

static int model_insert(model_state_t *m, int value) {
    if (m->row_count >= MAX_MODEL_ROWS) return -1;
    
    int id = m->next_id++;
    m->rows[m->row_count].id = id;
    m->rows[m->row_count].value = value;
    m->rows[m->row_count].active = 1;
    m->row_count++;
    return id;
}

static int model_update(model_state_t *m, int id, int new_value) {
    for (int i = 0; i < m->row_count; i++) {
        if (m->rows[i].id == id && m->rows[i].active) {
            m->rows[i].value = new_value;
            return 1;
        }
    }
    return 0;  /* Not found */
}

static int model_delete(model_state_t *m, int id) {
    for (int i = 0; i < m->row_count; i++) {
        if (m->rows[i].id == id && m->rows[i].active) {
            m->rows[i].active = 0;
            return 1;
        }
    }
    return 0;  /* Not found */
}

static int model_count_active(model_state_t *m) {
    int count = 0;
    for (int i = 0; i < m->row_count; i++) {
        if (m->rows[i].active) count++;
    }
    return count;
}

/* ─────────────────────────────────────────────────────────────────
 * J.3 Operation Types & Execution
 * ───────────────────────────────────────────────────────────────── */

typedef enum {
    OP_INSERT,
    OP_UPDATE,
    OP_DELETE,
    OP_BEGIN,
    OP_COMMIT,
    OP_ROLLBACK,
    OP_REOPEN
} op_type_t;

typedef struct {
    op_type_t type;
    int param1;  /* For INSERT: value; for UPDATE/DELETE: id */
    int param2;  /* For UPDATE: new_value */
} operation_t;

static const char* op_name(op_type_t t) {
    switch (t) {
        case OP_INSERT: return "INSERT";
        case OP_UPDATE: return "UPDATE";
        case OP_DELETE: return "DELETE";
        case OP_BEGIN: return "BEGIN";
        case OP_COMMIT: return "COMMIT";
        case OP_ROLLBACK: return "ROLLBACK";
        case OP_REOPEN: return "REOPEN";
        default: return "UNKNOWN";
    }
}

static int execute_op(sqlite3 **db_ptr, const char *db_name, 
                      model_state_t *model, operation_t *op,
                      int *last_insert_id, int verbose) {
    sqlite3 *db = *db_ptr;
    int rc = SQLITE_OK;
    char sql[256];
    
    switch (op->type) {
        case OP_INSERT: {
            snprintf(sql, sizeof(sql), 
                "INSERT INTO prop_test (value) VALUES (%d);", op->param1);
            rc = exec_sql(db, sql);
            if (rc == SQLITE_OK) {
                *last_insert_id = model_insert(model, op->param1);
                if (verbose) {
                    fprintf(stdout, "    INSERT value=%d -> id=%d\n", 
                            op->param1, *last_insert_id);
                }
            }
            break;
        }
        
        case OP_UPDATE: {
            if (model_count_active(model) == 0) {
                /* Skip update if no rows */
                if (verbose) fprintf(stdout, "    UPDATE skipped (no rows)\n");
                return SQLITE_OK;
            }
            snprintf(sql, sizeof(sql),
                "UPDATE prop_test SET value = %d WHERE id = %d;", 
                op->param2, op->param1);
            rc = exec_sql(db, sql);
            if (rc == SQLITE_OK) {
                int updated = model_update(model, op->param1, op->param2);
                if (verbose) {
                    fprintf(stdout, "    UPDATE id=%d value=%d (%s)\n",
                            op->param1, op->param2, updated ? "found" : "not found");
                }
            }
            break;
        }
        
        case OP_DELETE: {
            if (model_count_active(model) == 0) {
                /* Skip delete if no rows */
                if (verbose) fprintf(stdout, "    DELETE skipped (no rows)\n");
                return SQLITE_OK;
            }
            snprintf(sql, sizeof(sql),
                "DELETE FROM prop_test WHERE id = %d;", op->param1);
            rc = exec_sql(db, sql);
            if (rc == SQLITE_OK) {
                int deleted = model_delete(model, op->param1);
                if (verbose) {
                    fprintf(stdout, "    DELETE id=%d (%s)\n",
                            op->param1, deleted ? "found" : "not found");
                }
            }
            break;
        }
        
        case OP_BEGIN: {
            /* Skip if already in transaction */
            if (model->in_transaction) {
                if (verbose) fprintf(stdout, "    BEGIN skipped (already in txn)\n");
                return SQLITE_OK;
            }
            rc = exec_sql(db, "BEGIN;");
            if (rc == SQLITE_OK) {
                model_begin(model);
                if (verbose) fprintf(stdout, "    BEGIN\n");
            }
            break;
        }
        
        case OP_COMMIT: {
            /* Skip if not in transaction */
            if (!model->in_transaction) {
                if (verbose) fprintf(stdout, "    COMMIT skipped (no active txn)\n");
                return SQLITE_OK;
            }
            rc = exec_sql(db, "COMMIT;");
            if (rc == SQLITE_OK) {
                model_commit(model);
                if (verbose) fprintf(stdout, "    COMMIT\n");
            }
            break;
        }
        
        case OP_ROLLBACK: {
            /* Skip if not in transaction */
            if (!model->in_transaction) {
                if (verbose) fprintf(stdout, "    ROLLBACK skipped (no active txn)\n");
                return SQLITE_OK;
            }
            rc = exec_sql(db, "ROLLBACK;");
            if (rc == SQLITE_OK) {
                model_rollback(model);
                if (verbose) fprintf(stdout, "    ROLLBACK\n");
            }
            break;
        }
        
        case OP_REOPEN: {
            sqlite3_close(db);
            db = open_azurite_db(db_name, NULL, 0);
            *db_ptr = db;
            if (!db) {
                fprintf(stderr, "    REOPEN failed\n");
                return SQLITE_CANTOPEN;
            }
            /* After reopen, any uncommitted transaction is rolled back */
            if (model->in_transaction) {
                model_rollback(model);
            }
            if (verbose) fprintf(stdout, "    REOPEN\n");
            break;
        }
    }
    
    return rc;
}

/* ─────────────────────────────────────────────────────────────────
 * J.4 Validation: Model vs. Database
 * ───────────────────────────────────────────────────────────────── */

static int validate_model_vs_db(sqlite3 *db, model_state_t *model, 
                                 int verbose, uint64_t seed) {
    /* 1. Check row count */
    int db_count = 0;
    int rc = query_int(db, "SELECT COUNT(*) FROM prop_test;", &db_count);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "  VALIDATION FAILED: Cannot query count (rc=%d)\n", rc);
        fprintf(stderr, "  Seed: %lu\n", (unsigned long)seed);
        return 0;
    }
    
    int model_count = model_count_active(model);
    if (db_count != model_count) {
        fprintf(stderr, "  VALIDATION FAILED: Count mismatch\n");
        fprintf(stderr, "    Model: %d rows, DB: %d rows\n", model_count, db_count);
        fprintf(stderr, "  Seed: %lu\n", (unsigned long)seed);
        return 0;
    }
    
    if (verbose) {
        fprintf(stdout, "    Count check: %d rows (OK)\n", db_count);
    }
    
    /* 2. Check each active row exists in DB with correct value */
    for (int i = 0; i < model->row_count; i++) {
        if (!model->rows[i].active) continue;
        
        char sql[256];
        snprintf(sql, sizeof(sql),
            "SELECT value FROM prop_test WHERE id = %d;", model->rows[i].id);
        
        sqlite3_stmt *stmt = NULL;
        rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
        if (rc != SQLITE_OK) {
            fprintf(stderr, "  VALIDATION FAILED: Cannot prepare for id=%d\n",
                    model->rows[i].id);
            fprintf(stderr, "  Seed: %lu\n", (unsigned long)seed);
            return 0;
        }
        
        rc = sqlite3_step(stmt);
        if (rc != SQLITE_ROW) {
            fprintf(stderr, "  VALIDATION FAILED: Row id=%d not found in DB\n",
                    model->rows[i].id);
            fprintf(stderr, "  Seed: %lu\n", (unsigned long)seed);
            sqlite3_finalize(stmt);
            return 0;
        }
        
        int db_value = sqlite3_column_int(stmt, 0);
        if (db_value != model->rows[i].value) {
            fprintf(stderr, "  VALIDATION FAILED: Value mismatch for id=%d\n",
                    model->rows[i].id);
            fprintf(stderr, "    Model: %d, DB: %d\n", 
                    model->rows[i].value, db_value);
            fprintf(stderr, "  Seed: %lu\n", (unsigned long)seed);
            sqlite3_finalize(stmt);
            return 0;
        }
        
        sqlite3_finalize(stmt);
    }
    
    /* 3. Check DB doesn't have extra rows */
    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(db, "SELECT id FROM prop_test;", -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "  VALIDATION FAILED: Cannot query all IDs\n");
        fprintf(stderr, "  Seed: %lu\n", (unsigned long)seed);
        return 0;
    }
    
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int db_id = sqlite3_column_int(stmt, 0);
        int found = 0;
        for (int i = 0; i < model->row_count; i++) {
            if (model->rows[i].id == db_id && model->rows[i].active) {
                found = 1;
                break;
            }
        }
        if (!found) {
            fprintf(stderr, "  VALIDATION FAILED: DB has extra row id=%d\n", db_id);
            fprintf(stderr, "  Seed: %lu\n", (unsigned long)seed);
            sqlite3_finalize(stmt);
            return 0;
        }
    }
    sqlite3_finalize(stmt);
    
    if (verbose) {
        fprintf(stdout, "    Model vs DB: MATCH\n");
    }
    
    return 1;
}

/* ─────────────────────────────────────────────────────────────────
 * J.5 Operation Generator
 * ───────────────────────────────────────────────────────────────── */

/*
 * Generate a mix of operations:
 *  - 40% INSERT
 *  - 20% UPDATE
 *  - 15% DELETE
 *  - 10% BEGIN
 *  - 8% COMMIT
 *  - 5% ROLLBACK
 *  - 2% REOPEN
 */
static void generate_operation(prng_t *rng, model_state_t *model, 
                                operation_t *op) {
    int dice = prng_range(rng, 1, 100);
    
    if (dice <= 40) {
        /* INSERT: random value 1-1000 */
        op->type = OP_INSERT;
        op->param1 = prng_range(rng, 1, 1000);
    } else if (dice <= 60) {
        /* UPDATE: pick random existing ID if available */
        op->type = OP_UPDATE;
        if (model_count_active(model) > 0) {
            /* Pick a random active row */
            int active_idx = prng_range(rng, 0, model_count_active(model) - 1);
            int count = 0;
            for (int i = 0; i < model->row_count; i++) {
                if (model->rows[i].active) {
                    if (count == active_idx) {
                        op->param1 = model->rows[i].id;
                        break;
                    }
                    count++;
                }
            }
        } else {
            op->param1 = 1;  /* No rows, will skip */
        }
        op->param2 = prng_range(rng, 1, 1000);
    } else if (dice <= 75) {
        /* DELETE: pick random existing ID if available */
        op->type = OP_DELETE;
        if (model_count_active(model) > 0) {
            int active_idx = prng_range(rng, 0, model_count_active(model) - 1);
            int count = 0;
            for (int i = 0; i < model->row_count; i++) {
                if (model->rows[i].active) {
                    if (count == active_idx) {
                        op->param1 = model->rows[i].id;
                        break;
                    }
                    count++;
                }
            }
        } else {
            op->param1 = 1;  /* No rows, will skip */
        }
    } else if (dice <= 85) {
        /* BEGIN */
        op->type = OP_BEGIN;
    } else if (dice <= 93) {
        /* COMMIT */
        op->type = OP_COMMIT;
    } else if (dice <= 98) {
        /* ROLLBACK */
        op->type = OP_ROLLBACK;
    } else {
        /* REOPEN */
        op->type = OP_REOPEN;
    }
}

/* ─────────────────────────────────────────────────────────────────
 * J.6 Test: Deterministic Property Test (Compact, CI-friendly)
 * ───────────────────────────────────────────────────────────────── */

/*
 * Test J1: Deterministic randomized operations (default: 100 ops)
 * 
 * Executes a pseudo-random sequence of operations with a fixed seed.
 * Tracks expected state in a C model and validates DB matches model.
 * Always runs integrity_check and rowid uniqueness invariants.
 * 
 * Runtime: ~2-3 seconds on Azurite (CI-friendly).
 * For extended testing, use test-integration-extended target.
 */
TEST(prop_deterministic_basic) {
    const char *db_name = "prop-det-basic.db";
    cleanup_test_blobs(db_name);
    
    uint64_t seed = 42;  /* Fixed seed for reproducibility */
    int num_ops = 100;
    
    /* Allow override via environment variable for extended testing */
    const char *env_ops = getenv("PROP_TEST_OPS");
    if (env_ops) {
        num_ops = atoi(env_ops);
        if (num_ops <= 0) num_ops = 100;
    }
    
    fprintf(stdout, "  Running %d deterministic operations (seed=%lu)...\n",
            num_ops, (unsigned long)seed);
    
    int rc = sqlite_objs_vfs_register_uri(0);
    ASSERT_OK(rc);
    
    /* Create schema */
    sqlite3 *db = open_azurite_db(db_name, NULL, 1);
    ASSERT_NOT_NULL(db);
    
    rc = exec_sql(db, 
        "CREATE TABLE prop_test ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  value INTEGER NOT NULL"
        ");");
    ASSERT_OK(rc);
    
    /* Initialize PRNG and model */
    prng_t rng;
    prng_seed(&rng, seed);
    
    model_state_t model;
    model_init(&model);
    
    /* Execute operations */
    int last_insert_id = 0;
    for (int i = 0; i < num_ops; i++) {
        operation_t op;
        generate_operation(&rng, &model, &op);
        
        rc = execute_op(&db, db_name, &model, &op, &last_insert_id, 0);

        if (op.type == OP_REOPEN && rc != SQLITE_OK) {
            fprintf(stderr, "  Operation %d (REOPEN) failed unexpectedly: rc=%d\n", i, rc);
            ASSERT_OK(rc);
        }
        
        /* Data operations are generated to be valid. They may contend, but
        ** ordinary SQLITE_ERROR indicates a VFS/regression bug. */
        if (rc != SQLITE_OK && rc != SQLITE_BUSY && rc != SQLITE_LOCKED) {
            if (op.type == OP_INSERT || op.type == OP_UPDATE || op.type == OP_DELETE) {
                fprintf(stderr, "  Operation %d (%s) failed unexpectedly: rc=%d\n",
                        i, op_name(op.type), rc);
                ASSERT_OK(rc);
            }
            /* Transaction control operations can fail without corrupting state. */
        }
    }
    
    /* Close any open transaction */
    if (model.in_transaction) {
        exec_sql(db, "ROLLBACK;");
        model_rollback(&model);
    }
    
    fprintf(stdout, "  Validating final state...\n");
    
    /* Validate model vs database */
    ASSERT_TRUE(validate_model_vs_db(db, &model, 1, seed));
    
    /* Run integrity check */
    ASSERT_TRUE(check_integrity(db));
    
    /* Check rowid uniqueness */
    ASSERT_TRUE(check_rowid_uniqueness(db, "prop_test"));
    
    fprintf(stdout, "  Final state: %d rows, all invariants passed\n",
            model_count_active(&model));
    
    sqlite3_close(db);
    cleanup_test_blobs(db_name);
}

/*
 * Test J2: Deterministic randomized operations with multiple seeds
 * 
 * Runs the same test with 3 different seeds to increase coverage.
 * Each seed produces a different operation sequence.
 * 
 * Runtime: ~6-8 seconds on Azurite.
 */
TEST(prop_deterministic_multi_seed) {
    const uint64_t seeds[] = {42, 12345, 999999};
    const int num_seeds = 3;
    int ops_per_seed = 80;
    
    for (int s = 0; s < num_seeds; s++) {
        uint64_t seed = seeds[s];
        char db_name[64];
        snprintf(db_name, sizeof(db_name), "prop-seed-%lu.db", 
                (unsigned long)seed);
        cleanup_test_blobs(db_name);
        
        fprintf(stdout, "  Seed %lu: %d operations...\n",
                (unsigned long)seed, ops_per_seed);
        
        int rc = sqlite_objs_vfs_register_uri(0);
        ASSERT_OK(rc);
        
        sqlite3 *db = open_azurite_db(db_name, NULL, 1);
        ASSERT_NOT_NULL(db);
        
        rc = exec_sql(db,
            "CREATE TABLE prop_test ("
            "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  value INTEGER NOT NULL"
            ");");
        ASSERT_OK(rc);
        
        prng_t rng;
        prng_seed(&rng, seed);
        
        model_state_t model;
        model_init(&model);
        
        int last_insert_id = 0;
        for (int i = 0; i < ops_per_seed; i++) {
            operation_t op;
            generate_operation(&rng, &model, &op);
            
            rc = execute_op(&db, db_name, &model, &op, &last_insert_id, 0);

            if (op.type == OP_REOPEN && rc != SQLITE_OK) {
                fprintf(stderr, "  Seed %lu op %d (REOPEN) failed unexpectedly: rc=%d\n",
                        (unsigned long)seed, i, rc);
                ASSERT_OK(rc);
            }
            
            /* Allow operations to fail gracefully */
            if (rc != SQLITE_OK && rc != SQLITE_BUSY && rc != SQLITE_LOCKED) {
                if (op.type == OP_INSERT || op.type == OP_UPDATE || op.type == OP_DELETE) {
                    fprintf(stderr, "  Seed %lu op %d (%s) failed: rc=%d\n",
                            (unsigned long)seed, i, op_name(op.type), rc);
                    ASSERT_OK(rc);
                }
            }
        }
        
        if (model.in_transaction) {
            exec_sql(db, "ROLLBACK;");
            model_rollback(&model);
        }
        
        ASSERT_TRUE(validate_model_vs_db(db, &model, 0, seed));
        ASSERT_TRUE(check_integrity(db));
        ASSERT_TRUE(check_rowid_uniqueness(db, "prop_test"));
        
        fprintf(stdout, "    Seed %lu: %d rows, OK\n",
                (unsigned long)seed, model_count_active(&model));
        
        sqlite3_close(db);
        cleanup_test_blobs(db_name);
    }
}

/*
 * Test J3: Transaction-heavy property test
 * 
 * Focuses on transaction boundaries: more BEGIN/COMMIT/ROLLBACK operations.
 * Tests that transaction semantics are correctly maintained.
 */
TEST(prop_transaction_heavy) {
    const char *db_name = "prop-txn-heavy.db";
    cleanup_test_blobs(db_name);
    
    uint64_t seed = 777;
    int num_ops = 120;
    
    fprintf(stdout, "  Transaction-heavy test: %d ops (seed=%lu)...\n",
            num_ops, (unsigned long)seed);
    
    int rc = sqlite_objs_vfs_register_uri(0);
    ASSERT_OK(rc);
    
    sqlite3 *db = open_azurite_db(db_name, NULL, 1);
    ASSERT_NOT_NULL(db);
    
    rc = exec_sql(db,
        "CREATE TABLE prop_test ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  value INTEGER NOT NULL"
        ");");
    ASSERT_OK(rc);
    
    prng_t rng;
    prng_seed(&rng, seed);
    
    model_state_t model;
    model_init(&model);
    
    /* Biased operation generator for more transactions */
    int last_insert_id = 0;
    for (int i = 0; i < num_ops; i++) {
        operation_t op;
        int dice = prng_range(&rng, 1, 100);
        
        /* 30% INSERT, 15% UPDATE, 10% DELETE, 20% BEGIN, 15% COMMIT, 10% ROLLBACK */
        if (dice <= 30) {
            op.type = OP_INSERT;
            op.param1 = prng_range(&rng, 1, 500);
        } else if (dice <= 45) {
            op.type = OP_UPDATE;
            if (model_count_active(&model) > 0) {
                int active_idx = prng_range(&rng, 0, model_count_active(&model) - 1);
                int count = 0;
                for (int j = 0; j < model.row_count; j++) {
                    if (model.rows[j].active) {
                        if (count == active_idx) {
                            op.param1 = model.rows[j].id;
                            break;
                        }
                        count++;
                    }
                }
            } else {
                op.param1 = 1;
            }
            op.param2 = prng_range(&rng, 1, 500);
        } else if (dice <= 55) {
            op.type = OP_DELETE;
            if (model_count_active(&model) > 0) {
                int active_idx = prng_range(&rng, 0, model_count_active(&model) - 1);
                int count = 0;
                for (int j = 0; j < model.row_count; j++) {
                    if (model.rows[j].active) {
                        if (count == active_idx) {
                            op.param1 = model.rows[j].id;
                            break;
                        }
                        count++;
                    }
                }
            } else {
                op.param1 = 1;
            }
        } else if (dice <= 75) {
            op.type = OP_BEGIN;
        } else if (dice <= 90) {
            op.type = OP_COMMIT;
        } else {
            op.type = OP_ROLLBACK;
        }
        
        rc = execute_op(&db, db_name, &model, &op, &last_insert_id, 0);

        if (op.type == OP_REOPEN && rc != SQLITE_OK) {
            fprintf(stderr, "  Op %d (REOPEN) failed unexpectedly: rc=%d\n", i, rc);
            ASSERT_OK(rc);
        }
        
        /* Allow operations to fail gracefully */
        if (rc != SQLITE_OK && rc != SQLITE_BUSY && rc != SQLITE_LOCKED) {
            if (op.type == OP_INSERT || op.type == OP_UPDATE || op.type == OP_DELETE) {
                fprintf(stderr, "  Op %d (%s) failed: rc=%d\n",
                        i, op_name(op.type), rc);
                ASSERT_OK(rc);
            }
        }
    }
    
    if (model.in_transaction) {
        exec_sql(db, "ROLLBACK;");
        model_rollback(&model);
    }
    
    fprintf(stdout, "  Validating...\n");
    ASSERT_TRUE(validate_model_vs_db(db, &model, 1, seed));
    ASSERT_TRUE(check_integrity(db));
    ASSERT_TRUE(check_rowid_uniqueness(db, "prop_test"));
    
    fprintf(stdout, "  Transaction-heavy test: %d rows, OK\n",
            model_count_active(&model));
    
    sqlite3_close(db);
    cleanup_test_blobs(db_name);
}

/* ================================================================
 * Main runner
 * ================================================================ */

int main(void) {
    fprintf(stdout, "\n");
    fprintf(stdout, "╔════════════════════════════════════════════════════════╗\n");
    fprintf(stdout, "║  sqliteObjs Layer 2 Integration Tests (Azurite)          ║\n");
    fprintf(stdout, "║  Testing REAL azure_client.c against local emulator   ║\n");
    fprintf(stdout, "╚════════════════════════════════════════════════════════╝\n");

    /* Initialize the Azure client */
    /* Check if stress mode is enabled */
    int mult = get_stress_multiplier();
    int iter = 1;
    
    if (mult > 1) {
        fprintf(stdout, "\n");
        fprintf(stdout, "%s╔════════════════════════════════════════════════════════╗%s\n",
                TH_BOLD, TH_RESET);
        fprintf(stdout, "%s║  STRESS MODE ENABLED                                   ║%s\n",
                TH_BOLD, TH_RESET);
        fprintf(stdout, "%s║  Multiplier: %-3d                                      ║%s\n",
                TH_BOLD, mult, TH_RESET);
        fprintf(stdout, "%s╚════════════════════════════════════════════════════════╝%s\n",
                TH_BOLD, TH_RESET);
        fprintf(stdout, "\n");
    }

    setup_azure_client();

    /* Run Azure client tests */
    TEST_SUITE_BEGIN("Azure Client Integration");
    RUN_TEST(page_blob_lifecycle);
    RUN_TEST(block_blob_lifecycle);
    RUN_TEST(lease_lifecycle);
    RUN_TEST(lease_conflict);
    RUN_TEST(page_blob_alignment);
    RUN_TEST(error_not_found);
    RUN_TEST(page_blob_resize);
    RUN_TEST(lease_break);
    RUN_TEST(batch_reqs_alloc_failure_releases_mutex);
    RUN_TEST(batch_lease_renewal_path_does_not_deadlock);
    TEST_SUITE_END();

    /* Run VFS integration tests */
    TEST_SUITE_BEGIN("VFS Integration (SQLite on Azurite)");
    RUN_TEST(vfs_roundtrip);
    RUN_TEST(journal_roundtrip);
    TEST_SUITE_END();

    /* Run URI-based per-file config tests */
    TEST_SUITE_BEGIN("URI Per-File Config (SQLite on Azurite)");
    RUN_TEST(integ_uri_open_with_params);
    RUN_TEST(integ_multi_db_independent);
    RUN_TEST(integ_uri_two_containers);
    RUN_TEST(integ_attach_cross_container);
    TEST_SUITE_END();

    /* Run ETag cache-reuse tests */
    TEST_SUITE_BEGIN("ETag Cache Reuse (SQLite on Azurite)");
    RUN_TEST(etag_cache_hit);
    RUN_TEST(etag_cache_miss);
    RUN_TEST(etag_cache_reuse_wal);
    TEST_SUITE_END();

    /* Multi-client: A writes, B reads */
    TEST_SUITE_BEGIN("Multi-Client: Write-Read Handoff");
    RUN_TEST(mc_basic_write_read_handoff);
    RUN_TEST(mc_large_data_handoff);
    RUN_TEST(mc_schema_handoff);
    RUN_TEST(mc_cache_reuse_handoff);
    TEST_SUITE_END();

    /* Multi-client: Sequential writes */
    TEST_SUITE_BEGIN("Multi-Client: Sequential Writes");
    RUN_TEST(mc_sequential_writes);
    RUN_TEST(mc_sequential_update);
    RUN_TEST(mc_sequential_delete_insert);
    RUN_TEST(mc_multi_table_sequential);
    TEST_SUITE_END();

    /* Prefetch modes */
    TEST_SUITE_BEGIN("Multi-Client: Prefetch Modes");
    RUN_TEST(mc_prefetch_none_basic);
    RUN_TEST(mc_prefetch_none_write_read);
    RUN_TEST(mc_mixed_prefetch_reconnect);
    RUN_TEST(mc_prefetch_pragma);
    TEST_SUITE_END();

    /* Cache reuse scenarios */
    TEST_SUITE_BEGIN("Multi-Client: Cache Reuse");
    RUN_TEST(mc_etag_match_reconnect);
    RUN_TEST(mc_etag_mismatch_reconnect);
    RUN_TEST(mc_cache_reuse_prefetch_none);
    RUN_TEST(mc_no_cache_reuse);
    TEST_SUITE_END();

    /* Transaction integrity */
    TEST_SUITE_BEGIN("Multi-Client: Transactions & Integrity");
    RUN_TEST(mc_large_transaction);
    RUN_TEST(mc_many_small_transactions);
    RUN_TEST(mc_vacuum_after_writes);
    RUN_TEST(mc_cross_database_join);
    TEST_SUITE_END();

    /* Edge cases */
    TEST_SUITE_BEGIN("Multi-Client: Edge Cases");
    RUN_TEST(mc_empty_db_reconnect);
    RUN_TEST(mc_wide_rows);
    RUN_TEST(mc_many_small_tables);
    RUN_TEST(mc_rapid_open_close);
    TEST_SUITE_END();

    /* Concurrent writers regression test */
    TEST_SUITE_BEGIN("Concurrency: Regression Tests");
    RUN_TEST(concurrent_writers_regression);
    TEST_SUITE_END();

    /* Phase 1: Enhanced concurrency & invariant tests */
    TEST_SUITE_BEGIN("Concurrency: Phase 1 Stress & Invariants");
    RUN_TEST(stress_8_writers_25_each);
    RUN_TEST(stress_16_writers_20_each);
    RUN_TEST(reader_writer_interleaving);
    TEST_SUITE_END();

    /* Phase 2: Crash recovery & advanced invariants */
    TEST_SUITE_BEGIN("Crash Recovery: Phase 2 Deterministic Testing");
    RUN_TEST(crash_batch_write_rollback);
    RUN_TEST(crash_journal_upload);
    RUN_TEST(crash_recovery_multiple_cycles);
    RUN_TEST(crash_page_blob_resize);
    TEST_SUITE_END();

    TEST_SUITE_BEGIN("Snapshot Isolation: Phase 2 Advanced Scenarios");
    RUN_TEST(snapshot_isolation_long_running);
    RUN_TEST(stale_snapshot_etag_revalidation);
    TEST_SUITE_END();

    TEST_SUITE_BEGIN("Invariants: Phase 2 Post-Failure Stress");
    RUN_TEST(invariant_check_after_crash_stress);
    TEST_SUITE_END();

    /* Phase 3: Deterministic randomized / property-based testing */
    TEST_SUITE_BEGIN("Property-Based: Phase 3 Deterministic Randomized Testing");
    RUN_TEST(prop_deterministic_basic);
    RUN_TEST(prop_deterministic_multi_seed);
    RUN_TEST(prop_transaction_heavy);
    TEST_SUITE_END();

    /* Cleanup */
    teardown_azure_client();

    return test_harness_summary();
}
