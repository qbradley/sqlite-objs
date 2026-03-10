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

    /* Cleanup */
    teardown_azure_client();

    return test_harness_summary();
}
