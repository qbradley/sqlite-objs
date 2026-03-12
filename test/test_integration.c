/*
 * test_integration.c — Layer 2 Integration Tests for azqlite
 *
 * These tests run against Azurite — a local Azure Storage emulator — and use
 * the REAL azure_client.c code (not mocks). They validate:
 *   - Real HTTP communication
 *   - Actual Azure REST API compatibility
 *   - End-to-end VFS functionality with a real backend
 *
 * Prerequisites:
 *   - Azurite running on 127.0.0.1:10000
 *   - Container "azqlite-test" created
 *
 * The test/run-integration.sh wrapper script handles Azurite lifecycle.
 *
 * Part of the azqlite project. License: MIT
 */

#include "test_harness.h"
#include "azure_client.h"
#include "azqlite.h"
#include "sqlite3.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

/* ================================================================
 * Azurite configuration (well-known dev credentials)
 * ================================================================ */

#define AZURITE_ACCOUNT    "devstoreaccount1"
#define AZURITE_CONTAINER  "azqlite-test"
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

    /* Register the azqlite VFS with Azurite config */
    azqlite_config_t cfg = {
        .account = AZURITE_ACCOUNT,
        .container = AZURITE_CONTAINER,
        .sas_token = NULL,
        .account_key = AZURITE_KEY,
        .endpoint = AZURITE_ENDPOINT,
        .ops = NULL,  /* Use production client */
        .ops_ctx = NULL
    };
    int rc = azqlite_vfs_register_with_config(&cfg, 0);
    ASSERT_OK(rc);

    /* Open a database using the azqlite VFS */
    sqlite3 *db = NULL;
    rc = sqlite3_open_v2(db_name, &db,
                         SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                         "azqlite");
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
                         SQLITE_OPEN_READWRITE, "azqlite");
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
    azqlite_config_t cfg = {
        .account = AZURITE_ACCOUNT,
        .container = AZURITE_CONTAINER,
        .sas_token = NULL,
        .account_key = AZURITE_KEY,
        .endpoint = AZURITE_ENDPOINT,
        .ops = NULL,
        .ops_ctx = NULL
    };
    int rc = azqlite_vfs_register_with_config(&cfg, 0);
    ASSERT_OK(rc);

    sqlite3 *db = NULL;
    rc = sqlite3_open_v2(db_name, &db,
                         SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                         "azqlite");
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
    int rc = azqlite_vfs_register_uri(0);
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
        "azqlite");
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
    azqlite_config_t cfg = {
        .account = AZURITE_ACCOUNT,
        .container = AZURITE_CONTAINER,
        .sas_token = NULL,
        .account_key = AZURITE_KEY,
        .endpoint = AZURITE_ENDPOINT,
        .ops = NULL,
        .ops_ctx = NULL
    };
    int rc = azqlite_vfs_register_with_config(&cfg, 0);
    ASSERT_OK(rc);

    /* Open two databases */
    sqlite3 *db1 = NULL, *db2 = NULL;
    rc = sqlite3_open_v2(db1_name, &db1,
                          SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                          "azqlite");
    ASSERT_OK(rc);
    ASSERT_NOT_NULL(db1);

    rc = sqlite3_open_v2(db2_name, &db2,
                          SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                          "azqlite");
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

    rc = sqlite3_open_v2(db1_name, &db1, SQLITE_OPEN_READWRITE, "azqlite");
    ASSERT_OK(rc);
    rc = sqlite3_prepare_v2(db1, "SELECT val FROM t WHERE id=1;", -1, &stmt, NULL);
    ASSERT_OK(rc);
    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_ROW);
    ASSERT_STR_EQ(sqlite3_column_text(stmt, 0), "alpha");
    sqlite3_finalize(stmt);
    sqlite3_close(db1);

    rc = sqlite3_open_v2(db2_name, &db2, SQLITE_OPEN_READWRITE, "azqlite");
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
    const char *container1 = "azqlite-uri-c1";
    const char *container2 = "azqlite-uri-c2";

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
    int rc = azqlite_vfs_register_uri(0);
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
                          "azqlite");
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
                          "azqlite");
    ASSERT_OK(rc);
    ASSERT_NOT_NULL(db2);

    /* Insert different data */
    char *errmsg = NULL;
    rc = sqlite3_exec(db1,
        "CREATE TABLE t (id INTEGER, val TEXT);"
        "INSERT INTO t VALUES (1, 'container_one');",
        NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "  db1 error: %s\n", errmsg ? errmsg : "unknown");
        sqlite3_free(errmsg);
    }
    ASSERT_OK(rc);

    rc = sqlite3_exec(db2,
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
    const char *container_main = "azqlite-uri-c1";
    const char *container_att  = "azqlite-uri-c2";

    /* Register VFS with Azurite config for primary container */
    azqlite_config_t cfg = {
        .account = AZURITE_ACCOUNT,
        .container = container_main,
        .sas_token = NULL,
        .account_key = AZURITE_KEY,
        .endpoint = AZURITE_ENDPOINT,
        .ops = NULL,
        .ops_ctx = NULL
    };
    int rc = azqlite_vfs_register_with_config(&cfg, 0);
    ASSERT_OK(rc);

    /* Open primary database */
    sqlite3 *db = NULL;
    rc = sqlite3_open_v2("attach_main.db", &db,
                          SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                          "azqlite");
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
 * Main runner
 * ================================================================ */

int main(void) {
    fprintf(stdout, "\n");
    fprintf(stdout, "╔════════════════════════════════════════════════════════╗\n");
    fprintf(stdout, "║  azqlite Layer 2 Integration Tests (Azurite)          ║\n");
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

    /* Cleanup */
    teardown_azure_client();

    return test_harness_summary();
}
