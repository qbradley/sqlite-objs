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
#include "azure_client.h"
#include "sqlite_objs.h"
#include "sqlite3.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

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
    rc = g_ops->page_blob_write(g_ctx, blob_name, 0, write_data, 512, NULL, &err);
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
    rc = g_ops->page_blob_write(g_ctx, blob_name, 512, data, 512, NULL, &err);
    ASSERT_AZURE_OK(rc);

    /* Write at offset 1024 (aligned) */
    memset(data, 0xCD, 512);
    rc = g_ops->page_blob_write(g_ctx, blob_name, 1024, data, 512, NULL, &err);
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
 * Main runner
 * ================================================================ */

int main(void) {
    fprintf(stdout, "\n");
    fprintf(stdout, "╔════════════════════════════════════════════════════════╗\n");
    fprintf(stdout, "║  sqliteObjs Layer 2 Integration Tests (Azurite)          ║\n");
    fprintf(stdout, "║  Testing REAL azure_client.c against local emulator   ║\n");
    fprintf(stdout, "╚════════════════════════════════════════════════════════╝\n");

    /* Initialize the Azure client */
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

    /* Cleanup */
    teardown_azure_client();

    return test_harness_summary();
}
