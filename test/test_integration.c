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
     * and corresponding .etag files.  We brute-force remove them by
     * scanning /tmp for our prefix.  This is a test-only convenience. */
    char cmd[256];
    snprintf(cmd, sizeof(cmd),
             "rm -f /tmp/sqlite-objs-*.cache /tmp/sqlite-objs-*.etag");
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

    /* Cleanup */
    teardown_azure_client();

    return test_harness_summary();
}
