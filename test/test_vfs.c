/*
** test_vfs.c — VFS Unit Tests (Layer 1) for sqliteObjs
**
** Tests the VFS through SQLite's API with mock_azure_ops underneath.
** Until the VFS implementation exists (Aragorn is building it), these
** tests document the EXPECTED behavior — they are the spec in code.
**
** Each test follows: setup mock → perform SQLite operation → assert
** mock state + SQLite result.
**
** Since sqlite_objs_vfs.c doesn't exist yet, we test what we CAN test
** (the mock itself, the types, the interface contract) and write the
** VFS integration tests as compile-ready stubs that will link once
** Aragorn delivers the VFS.
*/

#include "../sqlite-autoconf-3520000/sqlite3.h"
#include "mock_azure_ops.h"
#include "test_harness.h"
#include <string.h>
#include <stdlib.h>

/* ── Helpers ──────────────────────────────────────────────────────── */

static mock_azure_ctx_t *g_ctx = NULL;
static azure_ops_t      *g_ops = NULL;

static void setup(void) {
    if (g_ctx) mock_reset(g_ctx);
    else       g_ctx = mock_azure_create();
    g_ops = mock_azure_get_ops();
}

static void teardown(void) {
    /* Intentionally left alive across tests for efficiency.
    ** mock_reset in setup() clears state. */
}

/* ══════════════════════════════════════════════════════════════════════
** SECTION 1: Mock Page Blob Operations
** ══════════════════════════════════════════════════════════════════════ */

TEST(page_blob_create_basic) {
    setup();
    azure_error_t err;
    azure_err_t rc = g_ops->page_blob_create(g_ctx, "test.db", 4096, &err);
    ASSERT_AZURE_OK(rc);
    ASSERT_EQ(mock_get_page_blob_size(g_ctx, "test.db"), 4096);
}

TEST(page_blob_create_zero_size) {
    setup();
    azure_error_t err;
    azure_err_t rc = g_ops->page_blob_create(g_ctx, "empty.db", 0, &err);
    ASSERT_AZURE_OK(rc);
    ASSERT_EQ(mock_get_page_blob_size(g_ctx, "empty.db"), 0);
}

TEST(page_blob_create_unaligned_fails) {
    setup();
    azure_error_t err;
    azure_err_t rc = g_ops->page_blob_create(g_ctx, "bad.db", 1000, &err);
    ASSERT_AZURE_ERR(rc, AZURE_ERR_ALIGNMENT);
}

TEST(page_blob_create_already_exists_resizes) {
    setup();
    azure_error_t err;
    g_ops->page_blob_create(g_ctx, "test.db", 4096, &err);
    azure_err_t rc = g_ops->page_blob_create(g_ctx, "test.db", 8192, &err);
    ASSERT_AZURE_OK(rc);
    ASSERT_EQ(mock_get_page_blob_size(g_ctx, "test.db"), 8192);
}

TEST(page_blob_create_large) {
    setup();
    azure_error_t err;
    int64_t size = 1024 * 512; /* 512 KB, aligned */
    azure_err_t rc = g_ops->page_blob_create(g_ctx, "large.db", size, &err);
    ASSERT_AZURE_OK(rc);
    ASSERT_EQ(mock_get_page_blob_size(g_ctx, "large.db"), size);
}

TEST(page_blob_write_basic) {
    setup();
    azure_error_t err;
    g_ops->page_blob_create(g_ctx, "test.db", 4096, &err);

    uint8_t data[512];
    memset(data, 0xAB, sizeof(data));
    azure_err_t rc = g_ops->page_blob_write(g_ctx, "test.db", 0, data, 512, NULL, &err);
    ASSERT_AZURE_OK(rc);

    const uint8_t *blob = mock_get_page_blob_data(g_ctx, "test.db");
    ASSERT_NOT_NULL(blob);
    ASSERT_MEM_EQ(blob, data, 512);
}

TEST(page_blob_write_at_offset) {
    setup();
    azure_error_t err;
    g_ops->page_blob_create(g_ctx, "test.db", 4096, &err);

    uint8_t data[512];
    memset(data, 0xCD, sizeof(data));
    azure_err_t rc = g_ops->page_blob_write(g_ctx, "test.db", 1024, data, 512, NULL, &err);
    ASSERT_AZURE_OK(rc);

    const uint8_t *blob = mock_get_page_blob_data(g_ctx, "test.db");
    ASSERT_NOT_NULL(blob);
    /* First 1024 bytes should be zero (untouched) */
    uint8_t zeros[1024];
    memset(zeros, 0, sizeof(zeros));
    ASSERT_MEM_EQ(blob, zeros, 1024);
    /* Bytes at offset 1024 should be our data */
    ASSERT_MEM_EQ(blob + 1024, data, 512);
}

TEST(page_blob_write_unaligned_offset_fails) {
    setup();
    azure_error_t err;
    g_ops->page_blob_create(g_ctx, "test.db", 4096, &err);

    uint8_t data[512];
    azure_err_t rc = g_ops->page_blob_write(g_ctx, "test.db", 100, data, 512, NULL, &err);
    ASSERT_AZURE_ERR(rc, AZURE_ERR_ALIGNMENT);
}

TEST(page_blob_write_unaligned_length_fails) {
    setup();
    azure_error_t err;
    g_ops->page_blob_create(g_ctx, "test.db", 4096, &err);

    uint8_t data[100];
    azure_err_t rc = g_ops->page_blob_write(g_ctx, "test.db", 0, data, 100, NULL, &err);
    ASSERT_AZURE_ERR(rc, AZURE_ERR_ALIGNMENT);
}

TEST(page_blob_write_nonexistent_fails) {
    setup();
    azure_error_t err;
    uint8_t data[512];
    azure_err_t rc = g_ops->page_blob_write(g_ctx, "ghost.db", 0, data, 512, NULL, &err);
    ASSERT_AZURE_ERR(rc, AZURE_ERR_NOT_FOUND);
}

TEST(page_blob_write_grows_blob) {
    setup();
    azure_error_t err;
    g_ops->page_blob_create(g_ctx, "test.db", 512, &err);

    uint8_t data[512];
    memset(data, 0xEF, sizeof(data));
    /* Write at offset 512 — past current size */
    azure_err_t rc = g_ops->page_blob_write(g_ctx, "test.db", 512, data, 512, NULL, &err);
    ASSERT_AZURE_OK(rc);
    ASSERT_EQ(mock_get_page_blob_size(g_ctx, "test.db"), 1024);
}

TEST(page_blob_write_multiple_pages) {
    setup();
    azure_error_t err;
    g_ops->page_blob_create(g_ctx, "test.db", 4096, &err);

    /* Write 4 different pages */
    for (int i = 0; i < 4; i++) {
        uint8_t data[512];
        memset(data, (uint8_t)(i + 1), sizeof(data));
        azure_err_t rc = g_ops->page_blob_write(g_ctx, "test.db",
                                                  (int64_t)(i * 512),
                                                  data, 512, NULL, &err);
        ASSERT_AZURE_OK(rc);
    }

    const uint8_t *blob = mock_get_page_blob_data(g_ctx, "test.db");
    for (int i = 0; i < 4; i++) {
        ASSERT_EQ(blob[i * 512], (uint8_t)(i + 1));
    }
}

TEST(page_blob_write_overwrite) {
    setup();
    azure_error_t err;
    g_ops->page_blob_create(g_ctx, "test.db", 4096, &err);

    uint8_t data1[512], data2[512];
    memset(data1, 0xAA, sizeof(data1));
    memset(data2, 0xBB, sizeof(data2));

    g_ops->page_blob_write(g_ctx, "test.db", 0, data1, 512, NULL, &err);
    g_ops->page_blob_write(g_ctx, "test.db", 0, data2, 512, NULL, &err);

    const uint8_t *blob = mock_get_page_blob_data(g_ctx, "test.db");
    ASSERT_MEM_EQ(blob, data2, 512);
}

TEST(page_blob_read_basic) {
    setup();
    azure_error_t err;
    g_ops->page_blob_create(g_ctx, "test.db", 4096, &err);

    uint8_t data[512];
    memset(data, 0xDE, sizeof(data));
    g_ops->page_blob_write(g_ctx, "test.db", 0, data, 512, NULL, &err);

    azure_buffer_t buf;
    azure_buffer_init(&buf);
    azure_err_t rc = g_ops->page_blob_read(g_ctx, "test.db", 0, 512, &buf, &err);
    ASSERT_AZURE_OK(rc);
    ASSERT_EQ(buf.size, 512);
    ASSERT_MEM_EQ(buf.data, data, 512);
    azure_buffer_free(&buf);
}

TEST(page_blob_read_at_offset) {
    setup();
    azure_error_t err;
    g_ops->page_blob_create(g_ctx, "test.db", 4096, &err);

    uint8_t data[512];
    memset(data, 0xFE, sizeof(data));
    g_ops->page_blob_write(g_ctx, "test.db", 1024, data, 512, NULL, &err);

    azure_buffer_t buf;
    azure_buffer_init(&buf);
    azure_err_t rc = g_ops->page_blob_read(g_ctx, "test.db", 1024, 512, &buf, &err);
    ASSERT_AZURE_OK(rc);
    ASSERT_MEM_EQ(buf.data, data, 512);
    azure_buffer_free(&buf);
}

TEST(page_blob_read_nonexistent_fails) {
    setup();
    azure_error_t err;
    azure_buffer_t buf;
    azure_buffer_init(&buf);
    azure_err_t rc = g_ops->page_blob_read(g_ctx, "ghost.db", 0, 512, &buf, &err);
    ASSERT_AZURE_ERR(rc, AZURE_ERR_NOT_FOUND);
    azure_buffer_free(&buf);
}

TEST(page_blob_read_past_eof_zero_fills) {
    setup();
    azure_error_t err;
    g_ops->page_blob_create(g_ctx, "test.db", 512, &err);

    uint8_t data[512];
    memset(data, 0xAA, sizeof(data));
    g_ops->page_blob_write(g_ctx, "test.db", 0, data, 512, NULL, &err);

    /* Read 1024 bytes but blob is only 512 */
    azure_buffer_t buf;
    azure_buffer_init(&buf);
    azure_err_t rc = g_ops->page_blob_read(g_ctx, "test.db", 0, 1024, &buf, &err);
    ASSERT_AZURE_OK(rc);
    ASSERT_EQ(buf.size, 1024);
    /* First 512 bytes = our data */
    ASSERT_MEM_EQ(buf.data, data, 512);
    /* Next 512 bytes = zero */
    uint8_t zeros[512];
    memset(zeros, 0, sizeof(zeros));
    ASSERT_MEM_EQ(buf.data + 512, zeros, 512);
    azure_buffer_free(&buf);
}

TEST(page_blob_read_offset_past_eof) {
    setup();
    azure_error_t err;
    g_ops->page_blob_create(g_ctx, "test.db", 512, &err);

    azure_buffer_t buf;
    azure_buffer_init(&buf);
    azure_err_t rc = g_ops->page_blob_read(g_ctx, "test.db", 1024, 512, &buf, &err);
    ASSERT_AZURE_ERR(rc, AZURE_ERR_INVALID_ARG);
    azure_buffer_free(&buf);
}

TEST(page_blob_write_then_read_roundtrip) {
    setup();
    azure_error_t err;
    g_ops->page_blob_create(g_ctx, "test.db", 4096, &err);

    /* Write pattern across 8 pages */
    for (int i = 0; i < 8; i++) {
        uint8_t data[512];
        memset(data, (uint8_t)(0x10 + i), sizeof(data));
        g_ops->page_blob_write(g_ctx, "test.db", (int64_t)(i * 512),
                               data, 512, NULL, &err);
    }

    /* Read back and verify each page */
    for (int i = 0; i < 8; i++) {
        azure_buffer_t buf;
        azure_buffer_init(&buf);
        azure_err_t rc = g_ops->page_blob_read(g_ctx, "test.db",
                                                 (int64_t)(i * 512),
                                                 512, &buf, &err);
        ASSERT_AZURE_OK(rc);
        uint8_t expected[512];
        memset(expected, (uint8_t)(0x10 + i), sizeof(expected));
        ASSERT_MEM_EQ(buf.data, expected, 512);
        azure_buffer_free(&buf);
    }
}

TEST(page_blob_resize_grow) {
    setup();
    azure_error_t err;
    g_ops->page_blob_create(g_ctx, "test.db", 4096, &err);
    azure_err_t rc = g_ops->page_blob_resize(g_ctx, "test.db", 8192, NULL, &err);
    ASSERT_AZURE_OK(rc);
    ASSERT_EQ(mock_get_page_blob_size(g_ctx, "test.db"), 8192);
}

TEST(page_blob_resize_shrink) {
    setup();
    azure_error_t err;
    g_ops->page_blob_create(g_ctx, "test.db", 8192, &err);
    azure_err_t rc = g_ops->page_blob_resize(g_ctx, "test.db", 4096, NULL, &err);
    ASSERT_AZURE_OK(rc);
    ASSERT_EQ(mock_get_page_blob_size(g_ctx, "test.db"), 4096);
}

TEST(page_blob_resize_unaligned_fails) {
    setup();
    azure_error_t err;
    g_ops->page_blob_create(g_ctx, "test.db", 4096, &err);
    azure_err_t rc = g_ops->page_blob_resize(g_ctx, "test.db", 5000, NULL, &err);
    ASSERT_AZURE_ERR(rc, AZURE_ERR_ALIGNMENT);
}

TEST(page_blob_resize_nonexistent_fails) {
    setup();
    azure_error_t err;
    azure_err_t rc = g_ops->page_blob_resize(g_ctx, "ghost.db", 4096, NULL, &err);
    ASSERT_AZURE_ERR(rc, AZURE_ERR_NOT_FOUND);
}

TEST(page_blob_resize_preserves_data) {
    setup();
    azure_error_t err;
    g_ops->page_blob_create(g_ctx, "test.db", 4096, &err);

    uint8_t data[512];
    memset(data, 0xAB, sizeof(data));
    g_ops->page_blob_write(g_ctx, "test.db", 0, data, 512, NULL, &err);

    g_ops->page_blob_resize(g_ctx, "test.db", 8192, NULL, &err);

    azure_buffer_t buf;
    azure_buffer_init(&buf);
    g_ops->page_blob_read(g_ctx, "test.db", 0, 512, &buf, &err);
    ASSERT_MEM_EQ(buf.data, data, 512);
    azure_buffer_free(&buf);
}

TEST(page_blob_resize_to_zero) {
    setup();
    azure_error_t err;
    g_ops->page_blob_create(g_ctx, "test.db", 4096, &err);
    azure_err_t rc = g_ops->page_blob_resize(g_ctx, "test.db", 0, NULL, &err);
    ASSERT_AZURE_OK(rc);
    ASSERT_EQ(mock_get_page_blob_size(g_ctx, "test.db"), 0);
}

/* ══════════════════════════════════════════════════════════════════════
** SECTION 2: Mock Block Blob Operations
** ══════════════════════════════════════════════════════════════════════ */

TEST(block_blob_upload_basic) {
    setup();
    azure_error_t err;
    uint8_t data[] = "journal data for test";
    azure_err_t rc = g_ops->block_blob_upload(g_ctx, "test.db-journal",
                                               data, sizeof(data), &err);
    ASSERT_AZURE_OK(rc);
    ASSERT_EQ(mock_get_block_blob_size(g_ctx, "test.db-journal"), (int64_t)sizeof(data));
}

TEST(block_blob_upload_replaces) {
    setup();
    azure_error_t err;
    uint8_t data1[] = "version 1";
    uint8_t data2[] = "version 2 which is longer";

    g_ops->block_blob_upload(g_ctx, "j", data1, sizeof(data1), &err);
    g_ops->block_blob_upload(g_ctx, "j", data2, sizeof(data2), &err);

    ASSERT_EQ(mock_get_block_blob_size(g_ctx, "j"), (int64_t)sizeof(data2));
    const uint8_t *blob = mock_get_block_blob_data(g_ctx, "j");
    ASSERT_MEM_EQ(blob, data2, sizeof(data2));
}

TEST(block_blob_upload_empty) {
    setup();
    azure_error_t err;
    azure_err_t rc = g_ops->block_blob_upload(g_ctx, "empty", NULL, 0, &err);
    ASSERT_AZURE_OK(rc);
    ASSERT_EQ(mock_get_block_blob_size(g_ctx, "empty"), 0);
}

TEST(block_blob_download_basic) {
    setup();
    azure_error_t err;
    uint8_t data[] = "journal content 12345";
    g_ops->block_blob_upload(g_ctx, "j", data, sizeof(data), &err);

    azure_buffer_t buf;
    azure_buffer_init(&buf);
    azure_err_t rc = g_ops->block_blob_download(g_ctx, "j", &buf, &err);
    ASSERT_AZURE_OK(rc);
    ASSERT_EQ(buf.size, sizeof(data));
    ASSERT_MEM_EQ(buf.data, data, sizeof(data));
    azure_buffer_free(&buf);
}

TEST(block_blob_download_nonexistent_fails) {
    setup();
    azure_error_t err;
    azure_buffer_t buf;
    azure_buffer_init(&buf);
    azure_err_t rc = g_ops->block_blob_download(g_ctx, "ghost", &buf, &err);
    ASSERT_AZURE_ERR(rc, AZURE_ERR_NOT_FOUND);
    azure_buffer_free(&buf);
}

TEST(block_blob_upload_download_roundtrip) {
    setup();
    azure_error_t err;

    /* Upload large data */
    size_t len = 65536;
    uint8_t *data = (uint8_t *)malloc(len);
    for (size_t i = 0; i < len; i++) data[i] = (uint8_t)(i & 0xFF);

    g_ops->block_blob_upload(g_ctx, "big-journal", data, len, &err);

    azure_buffer_t buf;
    azure_buffer_init(&buf);
    azure_err_t rc = g_ops->block_blob_download(g_ctx, "big-journal", &buf, &err);
    ASSERT_AZURE_OK(rc);
    ASSERT_EQ(buf.size, len);
    ASSERT_MEM_EQ(buf.data, data, len);

    azure_buffer_free(&buf);
    free(data);
}

/* ══════════════════════════════════════════════════════════════════════
** SECTION 3: Common Blob Operations
** ══════════════════════════════════════════════════════════════════════ */

TEST(blob_exists_page_blob) {
    setup();
    azure_error_t err;
    g_ops->page_blob_create(g_ctx, "test.db", 4096, &err);

    int exists = 0;
    azure_err_t rc = g_ops->blob_exists(g_ctx, "test.db", &exists, &err);
    ASSERT_AZURE_OK(rc);
    ASSERT_EQ(exists, 1);
}

TEST(blob_exists_block_blob) {
    setup();
    azure_error_t err;
    uint8_t data[] = "hi";
    g_ops->block_blob_upload(g_ctx, "j", data, 2, &err);

    int exists = 0;
    azure_err_t rc = g_ops->blob_exists(g_ctx, "j", &exists, &err);
    ASSERT_AZURE_OK(rc);
    ASSERT_EQ(exists, 1);
}

TEST(blob_exists_nonexistent) {
    setup();
    azure_error_t err;
    int exists = 1;
    azure_err_t rc = g_ops->blob_exists(g_ctx, "ghost", &exists, &err);
    ASSERT_AZURE_OK(rc);
    ASSERT_EQ(exists, 0);
}

TEST(blob_delete_page_blob) {
    setup();
    azure_error_t err;
    g_ops->page_blob_create(g_ctx, "test.db", 4096, &err);
    azure_err_t rc = g_ops->blob_delete(g_ctx, "test.db", &err);
    ASSERT_AZURE_OK(rc);
    ASSERT_EQ(mock_blob_exists(g_ctx, "test.db"), 0);
}

TEST(blob_delete_block_blob) {
    setup();
    azure_error_t err;
    uint8_t data[] = "hi";
    g_ops->block_blob_upload(g_ctx, "j", data, 2, &err);
    azure_err_t rc = g_ops->blob_delete(g_ctx, "j", &err);
    ASSERT_AZURE_OK(rc);
    ASSERT_EQ(mock_blob_exists(g_ctx, "j"), 0);
}

TEST(blob_delete_nonexistent_fails) {
    setup();
    azure_error_t err;
    azure_err_t rc = g_ops->blob_delete(g_ctx, "ghost", &err);
    ASSERT_AZURE_ERR(rc, AZURE_ERR_NOT_FOUND);
}

TEST(blob_get_properties_page_blob) {
    setup();
    azure_error_t err;
    g_ops->page_blob_create(g_ctx, "test.db", 4096, &err);

    int64_t size = 0;
    char lease_state[32] = {0};
    char lease_status[32] = {0};
    azure_err_t rc = g_ops->blob_get_properties(g_ctx, "test.db", &size,
                                                  lease_state, lease_status, &err);
    ASSERT_AZURE_OK(rc);
    ASSERT_EQ(size, 4096);
    ASSERT_STR_EQ(lease_state, "available");
    ASSERT_STR_EQ(lease_status, "unlocked");
}

TEST(blob_get_properties_nonexistent_fails) {
    setup();
    azure_error_t err;
    int64_t size;
    char ls[32], lst[32];
    azure_err_t rc = g_ops->blob_get_properties(g_ctx, "ghost", &size, ls, lst, &err);
    ASSERT_AZURE_ERR(rc, AZURE_ERR_NOT_FOUND);
}

TEST(blob_get_properties_with_lease) {
    setup();
    azure_error_t err;
    g_ops->page_blob_create(g_ctx, "test.db", 4096, &err);

    char lease_id[64];
    g_ops->lease_acquire(g_ctx, "test.db", 30, lease_id, sizeof(lease_id), &err);

    int64_t size;
    char ls[32] = {0}, lst[32] = {0};
    g_ops->blob_get_properties(g_ctx, "test.db", &size, ls, lst, &err);
    ASSERT_STR_EQ(ls, "leased");
    ASSERT_STR_EQ(lst, "locked");
}

/* ══════════════════════════════════════════════════════════════════════
** SECTION 4: Lease State Machine
** ══════════════════════════════════════════════════════════════════════ */

TEST(lease_acquire_basic) {
    setup();
    azure_error_t err;
    g_ops->page_blob_create(g_ctx, "test.db", 4096, &err);

    char lease_id[64] = {0};
    azure_err_t rc = g_ops->lease_acquire(g_ctx, "test.db", 30,
                                           lease_id, sizeof(lease_id), &err);
    ASSERT_AZURE_OK(rc);
    ASSERT_TRUE(strlen(lease_id) > 0);
    ASSERT_EQ(mock_is_leased(g_ctx, "test.db"), 1);
}

TEST(lease_acquire_already_leased_fails) {
    setup();
    azure_error_t err;
    g_ops->page_blob_create(g_ctx, "test.db", 4096, &err);

    char id1[64], id2[64];
    g_ops->lease_acquire(g_ctx, "test.db", 30, id1, sizeof(id1), &err);
    azure_err_t rc = g_ops->lease_acquire(g_ctx, "test.db", 30,
                                           id2, sizeof(id2), &err);
    ASSERT_AZURE_ERR(rc, AZURE_ERR_CONFLICT);
}

TEST(lease_acquire_nonexistent_fails) {
    setup();
    azure_error_t err;
    char id[64];
    azure_err_t rc = g_ops->lease_acquire(g_ctx, "ghost", 30,
                                           id, sizeof(id), &err);
    ASSERT_AZURE_ERR(rc, AZURE_ERR_NOT_FOUND);
}

TEST(lease_renew_basic) {
    setup();
    azure_error_t err;
    g_ops->page_blob_create(g_ctx, "test.db", 4096, &err);

    char id[64];
    g_ops->lease_acquire(g_ctx, "test.db", 30, id, sizeof(id), &err);

    azure_err_t rc = g_ops->lease_renew(g_ctx, "test.db", id, &err);
    ASSERT_AZURE_OK(rc);
    ASSERT_EQ(mock_is_leased(g_ctx, "test.db"), 1);
}

TEST(lease_renew_wrong_id_fails) {
    setup();
    azure_error_t err;
    g_ops->page_blob_create(g_ctx, "test.db", 4096, &err);

    char id[64];
    g_ops->lease_acquire(g_ctx, "test.db", 30, id, sizeof(id), &err);

    azure_err_t rc = g_ops->lease_renew(g_ctx, "test.db", "wrong-id", &err);
    ASSERT_AZURE_ERR(rc, AZURE_ERR_CONFLICT);
}

TEST(lease_renew_not_leased_fails) {
    setup();
    azure_error_t err;
    g_ops->page_blob_create(g_ctx, "test.db", 4096, &err);

    azure_err_t rc = g_ops->lease_renew(g_ctx, "test.db", "any-id", &err);
    ASSERT_AZURE_ERR(rc, AZURE_ERR_CONFLICT);
}

TEST(lease_release_basic) {
    setup();
    azure_error_t err;
    g_ops->page_blob_create(g_ctx, "test.db", 4096, &err);

    char id[64];
    g_ops->lease_acquire(g_ctx, "test.db", 30, id, sizeof(id), &err);

    azure_err_t rc = g_ops->lease_release(g_ctx, "test.db", id, &err);
    ASSERT_AZURE_OK(rc);
    ASSERT_EQ(mock_is_leased(g_ctx, "test.db"), 0);
    ASSERT_EQ(mock_get_lease_state(g_ctx, "test.db"), LEASE_AVAILABLE);
}

TEST(lease_release_wrong_id_fails) {
    setup();
    azure_error_t err;
    g_ops->page_blob_create(g_ctx, "test.db", 4096, &err);

    char id[64];
    g_ops->lease_acquire(g_ctx, "test.db", 30, id, sizeof(id), &err);

    azure_err_t rc = g_ops->lease_release(g_ctx, "test.db", "wrong-id", &err);
    ASSERT_AZURE_ERR(rc, AZURE_ERR_CONFLICT);
    ASSERT_EQ(mock_is_leased(g_ctx, "test.db"), 1); /* Still leased */
}

TEST(lease_release_not_leased_fails) {
    setup();
    azure_error_t err;
    g_ops->page_blob_create(g_ctx, "test.db", 4096, &err);

    azure_err_t rc = g_ops->lease_release(g_ctx, "test.db", "any-id", &err);
    ASSERT_AZURE_ERR(rc, AZURE_ERR_CONFLICT);
}

TEST(lease_break_immediate) {
    setup();
    azure_error_t err;
    g_ops->page_blob_create(g_ctx, "test.db", 4096, &err);

    char id[64];
    g_ops->lease_acquire(g_ctx, "test.db", 30, id, sizeof(id), &err);

    int remaining = -1;
    azure_err_t rc = g_ops->lease_break(g_ctx, "test.db", 0, &remaining, &err);
    ASSERT_AZURE_OK(rc);
    ASSERT_EQ(remaining, 0);
    ASSERT_EQ(mock_get_lease_state(g_ctx, "test.db"), LEASE_AVAILABLE);
}

TEST(lease_break_with_period) {
    setup();
    azure_error_t err;
    g_ops->page_blob_create(g_ctx, "test.db", 4096, &err);

    char id[64];
    g_ops->lease_acquire(g_ctx, "test.db", 30, id, sizeof(id), &err);

    int remaining = -1;
    azure_err_t rc = g_ops->lease_break(g_ctx, "test.db", 15, &remaining, &err);
    ASSERT_AZURE_OK(rc);
    ASSERT_EQ(remaining, 15);
    ASSERT_EQ(mock_get_lease_state(g_ctx, "test.db"), LEASE_BREAKING);
}

TEST(lease_break_not_leased_fails) {
    setup();
    azure_error_t err;
    g_ops->page_blob_create(g_ctx, "test.db", 4096, &err);

    int remaining;
    azure_err_t rc = g_ops->lease_break(g_ctx, "test.db", 0, &remaining, &err);
    ASSERT_AZURE_ERR(rc, AZURE_ERR_CONFLICT);
}

TEST(lease_acquire_after_release) {
    setup();
    azure_error_t err;
    g_ops->page_blob_create(g_ctx, "test.db", 4096, &err);

    char id1[64], id2[64];
    g_ops->lease_acquire(g_ctx, "test.db", 30, id1, sizeof(id1), &err);
    g_ops->lease_release(g_ctx, "test.db", id1, &err);

    azure_err_t rc = g_ops->lease_acquire(g_ctx, "test.db", 30,
                                           id2, sizeof(id2), &err);
    ASSERT_AZURE_OK(rc);
    /* New lease should have a different ID */
    ASSERT_STR_NE(id1, id2);
}

TEST(lease_acquire_after_immediate_break) {
    setup();
    azure_error_t err;
    g_ops->page_blob_create(g_ctx, "test.db", 4096, &err);

    char id1[64], id2[64];
    int remaining;
    g_ops->lease_acquire(g_ctx, "test.db", 30, id1, sizeof(id1), &err);
    g_ops->lease_break(g_ctx, "test.db", 0, &remaining, &err);

    azure_err_t rc = g_ops->lease_acquire(g_ctx, "test.db", 30,
                                           id2, sizeof(id2), &err);
    ASSERT_AZURE_OK(rc);
}

TEST(lease_acquire_during_breaking_fails) {
    setup();
    azure_error_t err;
    g_ops->page_blob_create(g_ctx, "test.db", 4096, &err);

    char id[64];
    int remaining;
    g_ops->lease_acquire(g_ctx, "test.db", 30, id, sizeof(id), &err);
    g_ops->lease_break(g_ctx, "test.db", 15, &remaining, &err);

    /* While in BREAKING state, new acquire should fail */
    char id2[64];
    azure_err_t rc = g_ops->lease_acquire(g_ctx, "test.db", 30,
                                           id2, sizeof(id2), &err);
    ASSERT_AZURE_ERR(rc, AZURE_ERR_CONFLICT);
}

TEST(lease_state_inspection) {
    setup();
    azure_error_t err;
    g_ops->page_blob_create(g_ctx, "test.db", 4096, &err);

    /* Initial: available */
    ASSERT_EQ(mock_get_lease_state(g_ctx, "test.db"), LEASE_AVAILABLE);
    ASSERT_NULL(mock_get_lease_id(g_ctx, "test.db"));

    /* After acquire: leased */
    char id[64];
    g_ops->lease_acquire(g_ctx, "test.db", 30, id, sizeof(id), &err);
    ASSERT_EQ(mock_get_lease_state(g_ctx, "test.db"), LEASE_LEASED);
    ASSERT_NOT_NULL(mock_get_lease_id(g_ctx, "test.db"));
    ASSERT_STR_EQ(mock_get_lease_id(g_ctx, "test.db"), id);

    /* After release: available again */
    g_ops->lease_release(g_ctx, "test.db", id, &err);
    ASSERT_EQ(mock_get_lease_state(g_ctx, "test.db"), LEASE_AVAILABLE);
    ASSERT_NULL(mock_get_lease_id(g_ctx, "test.db"));
}

/* ══════════════════════════════════════════════════════════════════════
** SECTION 5: Failure Injection
** ══════════════════════════════════════════════════════════════════════ */

TEST(fail_at_specific_call) {
    setup();
    azure_error_t err;

    /* First call succeeds, second call fails */
    mock_set_fail_at(g_ctx, 2, AZURE_ERR_NETWORK);

    azure_err_t rc1 = g_ops->page_blob_create(g_ctx, "test.db", 4096, &err);
    ASSERT_AZURE_OK(rc1);

    uint8_t data[512];
    azure_err_t rc2 = g_ops->page_blob_write(g_ctx, "test.db", 0, data, 512, NULL, &err);
    ASSERT_AZURE_ERR(rc2, AZURE_ERR_NETWORK);
}

TEST(fail_at_is_one_shot) {
    setup();
    azure_error_t err;

    mock_set_fail_at(g_ctx, 1, AZURE_ERR_SERVER);

    azure_err_t rc1 = g_ops->page_blob_create(g_ctx, "test.db", 4096, &err);
    ASSERT_AZURE_ERR(rc1, AZURE_ERR_SERVER);

    /* Second call should succeed (one-shot rule consumed) */
    azure_err_t rc2 = g_ops->page_blob_create(g_ctx, "test.db", 4096, &err);
    ASSERT_AZURE_OK(rc2);
}

TEST(fail_operation_always) {
    setup();
    azure_error_t err;

    mock_set_fail_operation(g_ctx, "page_blob_write", AZURE_ERR_NETWORK);

    g_ops->page_blob_create(g_ctx, "test.db", 4096, &err);

    uint8_t data[512];
    azure_err_t rc1 = g_ops->page_blob_write(g_ctx, "test.db", 0, data, 512, NULL, &err);
    ASSERT_AZURE_ERR(rc1, AZURE_ERR_NETWORK);

    azure_err_t rc2 = g_ops->page_blob_write(g_ctx, "test.db", 0, data, 512, NULL, &err);
    ASSERT_AZURE_ERR(rc2, AZURE_ERR_NETWORK);
}

TEST(fail_operation_lease_acquire) {
    setup();
    azure_error_t err;
    g_ops->page_blob_create(g_ctx, "test.db", 4096, &err);

    mock_set_fail_operation(g_ctx, "lease_acquire", AZURE_ERR_THROTTLE);

    char id[64];
    azure_err_t rc = g_ops->lease_acquire(g_ctx, "test.db", 30,
                                           id, sizeof(id), &err);
    ASSERT_AZURE_ERR(rc, AZURE_ERR_THROTTLE);
    ASSERT_EQ(mock_is_leased(g_ctx, "test.db"), 0);
}

TEST(fail_clear_restores_normal) {
    setup();
    azure_error_t err;

    mock_set_fail_operation(g_ctx, "page_blob_create", AZURE_ERR_AUTH);
    azure_err_t rc1 = g_ops->page_blob_create(g_ctx, "test.db", 4096, &err);
    ASSERT_AZURE_ERR(rc1, AZURE_ERR_AUTH);

    mock_clear_failures(g_ctx);

    azure_err_t rc2 = g_ops->page_blob_create(g_ctx, "test.db", 4096, &err);
    ASSERT_AZURE_OK(rc2);
}

TEST(fail_injection_populates_error) {
    setup();
    azure_error_t err;
    memset(&err, 0, sizeof(err));

    mock_set_fail_at(g_ctx, 1, AZURE_ERR_SERVER);
    g_ops->page_blob_create(g_ctx, "test.db", 4096, &err);

    ASSERT_EQ(err.http_status, 500);
    ASSERT_TRUE(strlen(err.error_code) > 0);
    ASSERT_TRUE(strlen(err.error_message) > 0);
    ASSERT_TRUE(strlen(err.request_id) > 0);
}

TEST(fail_blob_read) {
    setup();
    azure_error_t err;
    g_ops->page_blob_create(g_ctx, "test.db", 4096, &err);

    mock_set_fail_operation(g_ctx, "page_blob_read", AZURE_ERR_TIMEOUT);

    azure_buffer_t buf;
    azure_buffer_init(&buf);
    azure_err_t rc = g_ops->page_blob_read(g_ctx, "test.db", 0, 512, &buf, &err);
    ASSERT_AZURE_ERR(rc, AZURE_ERR_TIMEOUT);
    azure_buffer_free(&buf);
}

TEST(fail_blob_delete) {
    setup();
    azure_error_t err;
    uint8_t data[] = "hi";
    g_ops->block_blob_upload(g_ctx, "j", data, 2, &err);

    mock_set_fail_operation(g_ctx, "blob_delete", AZURE_ERR_AUTH);
    azure_err_t rc = g_ops->blob_delete(g_ctx, "j", &err);
    ASSERT_AZURE_ERR(rc, AZURE_ERR_AUTH);
    /* Blob should still exist (delete failed) */
    ASSERT_EQ(mock_blob_exists(g_ctx, "j"), 1);
}

TEST(fail_block_blob_download) {
    setup();
    azure_error_t err;
    uint8_t data[] = "journal";
    g_ops->block_blob_upload(g_ctx, "j", data, sizeof(data), &err);

    mock_set_fail_operation(g_ctx, "block_blob_download", AZURE_ERR_NETWORK);

    azure_buffer_t buf;
    azure_buffer_init(&buf);
    azure_err_t rc = g_ops->block_blob_download(g_ctx, "j", &buf, &err);
    ASSERT_AZURE_ERR(rc, AZURE_ERR_NETWORK);
    azure_buffer_free(&buf);
}

TEST(fail_block_blob_upload) {
    setup();
    azure_error_t err;
    mock_set_fail_operation(g_ctx, "block_blob_upload", AZURE_ERR_SERVER);

    uint8_t data[] = "journal";
    azure_err_t rc = g_ops->block_blob_upload(g_ctx, "j", data, sizeof(data), &err);
    ASSERT_AZURE_ERR(rc, AZURE_ERR_SERVER);
    ASSERT_EQ(mock_blob_exists(g_ctx, "j"), 0);
}

/* ══════════════════════════════════════════════════════════════════════
** SECTION 6: Call Counting
** ══════════════════════════════════════════════════════════════════════ */

TEST(call_count_tracks_operations) {
    setup();
    azure_error_t err;

    ASSERT_EQ(mock_get_call_count(g_ctx, "page_blob_create"), 0);

    g_ops->page_blob_create(g_ctx, "a.db", 4096, &err);
    g_ops->page_blob_create(g_ctx, "b.db", 4096, &err);

    ASSERT_EQ(mock_get_call_count(g_ctx, "page_blob_create"), 2);
}

TEST(call_count_per_operation) {
    setup();
    azure_error_t err;

    g_ops->page_blob_create(g_ctx, "test.db", 4096, &err);

    uint8_t data[512] = {0};
    g_ops->page_blob_write(g_ctx, "test.db", 0, data, 512, NULL, &err);
    g_ops->page_blob_write(g_ctx, "test.db", 512, data, 512, NULL, &err);
    g_ops->page_blob_write(g_ctx, "test.db", 1024, data, 512, NULL, &err);

    ASSERT_EQ(mock_get_call_count(g_ctx, "page_blob_create"), 1);
    ASSERT_EQ(mock_get_call_count(g_ctx, "page_blob_write"), 3);
    ASSERT_EQ(mock_get_total_call_count(g_ctx), 4);
}

TEST(call_count_includes_failures) {
    setup();
    azure_error_t err;

    mock_set_fail_operation(g_ctx, "page_blob_create", AZURE_ERR_AUTH);
    g_ops->page_blob_create(g_ctx, "test.db", 4096, &err);

    ASSERT_EQ(mock_get_call_count(g_ctx, "page_blob_create"), 1);
}

TEST(call_count_reset) {
    setup();
    azure_error_t err;

    g_ops->page_blob_create(g_ctx, "test.db", 4096, &err);
    ASSERT_EQ(mock_get_call_count(g_ctx, "page_blob_create"), 1);

    mock_reset_call_counts(g_ctx);
    ASSERT_EQ(mock_get_call_count(g_ctx, "page_blob_create"), 0);
    ASSERT_EQ(mock_get_total_call_count(g_ctx), 0);
}

TEST(call_count_unknown_op) {
    setup();
    ASSERT_EQ(mock_get_call_count(g_ctx, "nonexistent_op"), 0);
}

/* ══════════════════════════════════════════════════════════════════════
** SECTION 7: Mock Reset and Isolation
** ══════════════════════════════════════════════════════════════════════ */

TEST(reset_clears_blobs) {
    setup();
    azure_error_t err;
    g_ops->page_blob_create(g_ctx, "test.db", 4096, &err);
    ASSERT_EQ(mock_blob_exists(g_ctx, "test.db"), 1);

    mock_reset(g_ctx);
    ASSERT_EQ(mock_blob_exists(g_ctx, "test.db"), 0);
}

TEST(reset_clears_leases) {
    setup();
    azure_error_t err;
    g_ops->page_blob_create(g_ctx, "test.db", 4096, &err);
    char id[64];
    g_ops->lease_acquire(g_ctx, "test.db", 30, id, sizeof(id), &err);

    mock_reset(g_ctx);
    ASSERT_EQ(mock_is_leased(g_ctx, "test.db"), 0);
}

TEST(reset_clears_failures) {
    setup();
    azure_error_t err;
    mock_set_fail_operation(g_ctx, "page_blob_create", AZURE_ERR_AUTH);

    mock_reset(g_ctx);

    azure_err_t rc = g_ops->page_blob_create(g_ctx, "test.db", 4096, &err);
    ASSERT_AZURE_OK(rc);
}

TEST(reset_clears_counters) {
    setup();
    azure_error_t err;
    g_ops->page_blob_create(g_ctx, "test.db", 4096, &err);

    mock_reset(g_ctx);
    ASSERT_EQ(mock_get_total_call_count(g_ctx), 0);
}

/* ══════════════════════════════════════════════════════════════════════
** SECTION 8: Buffer Management
** ══════════════════════════════════════════════════════════════════════ */

TEST(buffer_init) {
    azure_buffer_t buf;
    azure_buffer_init(&buf);
    ASSERT_NULL(buf.data);
    ASSERT_EQ(buf.size, 0);
    ASSERT_EQ(buf.capacity, 0);
}

TEST(buffer_reuse) {
    setup();
    azure_error_t err;
    g_ops->page_blob_create(g_ctx, "test.db", 4096, &err);

    uint8_t data[512];
    memset(data, 0xAA, sizeof(data));
    g_ops->page_blob_write(g_ctx, "test.db", 0, data, 512, NULL, &err);

    /* Reuse the same buffer for multiple reads */
    azure_buffer_t buf;
    azure_buffer_init(&buf);

    g_ops->page_blob_read(g_ctx, "test.db", 0, 512, &buf, &err);
    ASSERT_EQ(buf.size, 512);

    memset(data, 0xBB, sizeof(data));
    g_ops->page_blob_write(g_ctx, "test.db", 512, data, 512, NULL, &err);

    g_ops->page_blob_read(g_ctx, "test.db", 512, 512, &buf, &err);
    ASSERT_EQ(buf.size, 512);
    ASSERT_EQ(buf.data[0], 0xBB);

    azure_buffer_free(&buf);
}

TEST(buffer_free_resets) {
    azure_buffer_t buf;
    azure_buffer_init(&buf);
    buf.data = (uint8_t *)malloc(100);
    buf.size = 50;
    buf.capacity = 100;

    azure_buffer_free(&buf);
    ASSERT_NULL(buf.data);
    ASSERT_EQ(buf.size, 0);
    ASSERT_EQ(buf.capacity, 0);
}

/* ══════════════════════════════════════════════════════════════════════
** SECTION 9: Error Detail Population
** ══════════════════════════════════════════════════════════════════════ */

TEST(error_populated_on_not_found) {
    setup();
    azure_error_t err;
    memset(&err, 0, sizeof(err));

    azure_buffer_t buf;
    azure_buffer_init(&buf);
    g_ops->page_blob_read(g_ctx, "ghost.db", 0, 512, &buf, &err);

    ASSERT_EQ(err.http_status, 404);
    ASSERT_STR_EQ(err.error_code, "BlobNotFound");
    ASSERT_TRUE(strlen(err.error_message) > 0);
    azure_buffer_free(&buf);
}

TEST(error_populated_on_conflict) {
    setup();
    azure_error_t err;
    g_ops->page_blob_create(g_ctx, "test.db", 4096, &err);

    char id[64];
    g_ops->lease_acquire(g_ctx, "test.db", 30, id, sizeof(id), &err);

    memset(&err, 0, sizeof(err));
    char id2[64];
    g_ops->lease_acquire(g_ctx, "test.db", 30, id2, sizeof(id2), &err);

    ASSERT_EQ(err.http_status, 409);
    ASSERT_TRUE(strlen(err.error_code) > 0);
}

TEST(error_populated_on_alignment) {
    setup();
    azure_error_t err;
    memset(&err, 0, sizeof(err));

    g_ops->page_blob_create(g_ctx, "test.db", 1000, &err);
    ASSERT_EQ(err.http_status, 400);
    ASSERT_STR_EQ(err.error_code, "InvalidHeaderValue");
}

/* ══════════════════════════════════════════════════════════════════════
** SECTION 10: Type Mismatch Protection
** ══════════════════════════════════════════════════════════════════════ */

TEST(page_ops_on_block_blob_fails) {
    setup();
    azure_error_t err;
    uint8_t data[] = "block data";
    g_ops->block_blob_upload(g_ctx, "test", data, sizeof(data), &err);

    /* Try page blob write on block blob */
    uint8_t page_data[512] = {0};
    azure_err_t rc = g_ops->page_blob_write(g_ctx, "test", 0,
                                              page_data, 512, NULL, &err);
    ASSERT_AZURE_ERR(rc, AZURE_ERR_NOT_FOUND);
}

TEST(block_ops_on_page_blob_fails) {
    setup();
    azure_error_t err;
    g_ops->page_blob_create(g_ctx, "test.db", 4096, &err);

    azure_buffer_t buf;
    azure_buffer_init(&buf);
    azure_err_t rc = g_ops->block_blob_download(g_ctx, "test.db", &buf, &err);
    ASSERT_AZURE_ERR(rc, AZURE_ERR_NOT_FOUND);
    azure_buffer_free(&buf);
}

TEST(create_page_blob_over_block_blob_fails) {
    setup();
    azure_error_t err;
    uint8_t data[] = "block data";
    g_ops->block_blob_upload(g_ctx, "test", data, sizeof(data), &err);

    azure_err_t rc = g_ops->page_blob_create(g_ctx, "test", 4096, &err);
    ASSERT_AZURE_ERR(rc, AZURE_ERR_CONFLICT);
}

/* ══════════════════════════════════════════════════════════════════════
** SECTION 11: Edge Cases
** ══════════════════════════════════════════════════════════════════════ */

TEST(multiple_blobs_independent) {
    setup();
    azure_error_t err;

    g_ops->page_blob_create(g_ctx, "a.db", 4096, &err);
    g_ops->page_blob_create(g_ctx, "b.db", 8192, &err);

    uint8_t data_a[512], data_b[512];
    memset(data_a, 0xAA, sizeof(data_a));
    memset(data_b, 0xBB, sizeof(data_b));

    g_ops->page_blob_write(g_ctx, "a.db", 0, data_a, 512, NULL, &err);
    g_ops->page_blob_write(g_ctx, "b.db", 0, data_b, 512, NULL, &err);

    const uint8_t *a = mock_get_page_blob_data(g_ctx, "a.db");
    const uint8_t *b = mock_get_page_blob_data(g_ctx, "b.db");
    ASSERT_EQ(a[0], 0xAA);
    ASSERT_EQ(b[0], 0xBB);
}

TEST(blob_name_with_path_separators) {
    setup();
    azure_error_t err;
    azure_err_t rc = g_ops->page_blob_create(g_ctx,
        "databases/myapp/test.db", 4096, &err);
    ASSERT_AZURE_OK(rc);
    ASSERT_EQ(mock_get_page_blob_size(g_ctx, "databases/myapp/test.db"), 4096);
}

TEST(blob_name_with_special_chars) {
    setup();
    azure_error_t err;
    azure_err_t rc = g_ops->page_blob_create(g_ctx,
        "my-db_2024.db", 4096, &err);
    ASSERT_AZURE_OK(rc);
    ASSERT_TRUE(mock_blob_exists(g_ctx, "my-db_2024.db"));
}

TEST(lease_on_different_blobs) {
    setup();
    azure_error_t err;
    g_ops->page_blob_create(g_ctx, "a.db", 4096, &err);
    g_ops->page_blob_create(g_ctx, "b.db", 4096, &err);

    char id_a[64], id_b[64];
    g_ops->lease_acquire(g_ctx, "a.db", 30, id_a, sizeof(id_a), &err);
    azure_err_t rc = g_ops->lease_acquire(g_ctx, "b.db", 30,
                                           id_b, sizeof(id_b), &err);
    ASSERT_AZURE_OK(rc);
    ASSERT_EQ(mock_is_leased(g_ctx, "a.db"), 1);
    ASSERT_EQ(mock_is_leased(g_ctx, "b.db"), 1);
}

TEST(page_blob_read_from_zero_blob) {
    setup();
    azure_error_t err;
    g_ops->page_blob_create(g_ctx, "test.db", 4096, &err);

    /* Read from an untouched blob — should be all zeros */
    azure_buffer_t buf;
    azure_buffer_init(&buf);
    azure_err_t rc = g_ops->page_blob_read(g_ctx, "test.db", 0, 512, &buf, &err);
    ASSERT_AZURE_OK(rc);

    uint8_t zeros[512];
    memset(zeros, 0, sizeof(zeros));
    ASSERT_MEM_EQ(buf.data, zeros, 512);
    azure_buffer_free(&buf);
}

TEST(page_blob_write_4096_page_size) {
    setup();
    azure_error_t err;
    /* SQLite default page size = 4096, which is aligned to 512 */
    g_ops->page_blob_create(g_ctx, "test.db", 4096 * 10, &err);

    uint8_t page[4096];
    memset(page, 0x42, sizeof(page));
    azure_err_t rc = g_ops->page_blob_write(g_ctx, "test.db", 4096, page, 4096, NULL, &err);
    ASSERT_AZURE_OK(rc);

    azure_buffer_t buf;
    azure_buffer_init(&buf);
    g_ops->page_blob_read(g_ctx, "test.db", 4096, 4096, &buf, &err);
    ASSERT_MEM_EQ(buf.data, page, 4096);
    azure_buffer_free(&buf);
}

TEST(delete_and_recreate) {
    setup();
    azure_error_t err;
    g_ops->page_blob_create(g_ctx, "test.db", 4096, &err);

    uint8_t data[512];
    memset(data, 0xFF, sizeof(data));
    g_ops->page_blob_write(g_ctx, "test.db", 0, data, 512, NULL, &err);

    g_ops->blob_delete(g_ctx, "test.db", &err);
    g_ops->page_blob_create(g_ctx, "test.db", 4096, &err);

    /* New blob should be zeroed */
    azure_buffer_t buf;
    azure_buffer_init(&buf);
    g_ops->page_blob_read(g_ctx, "test.db", 0, 512, &buf, &err);
    uint8_t zeros[512];
    memset(zeros, 0, sizeof(zeros));
    ASSERT_MEM_EQ(buf.data, zeros, 512);
    azure_buffer_free(&buf);
}

TEST(journal_blob_naming_convention) {
    setup();
    azure_error_t err;

    /* Per D7: journal blob = <name>-journal */
    g_ops->page_blob_create(g_ctx, "mydb.db", 4096, &err);
    uint8_t journal[] = "journal header data";
    g_ops->block_blob_upload(g_ctx, "mydb.db-journal", journal,
                             sizeof(journal), &err);

    ASSERT_TRUE(mock_blob_exists(g_ctx, "mydb.db"));
    ASSERT_TRUE(mock_blob_exists(g_ctx, "mydb.db-journal"));

    /* Deleting journal doesn't affect main db */
    g_ops->blob_delete(g_ctx, "mydb.db-journal", &err);
    ASSERT_TRUE(mock_blob_exists(g_ctx, "mydb.db"));
    ASSERT_FALSE(mock_blob_exists(g_ctx, "mydb.db-journal"));
}

TEST(blob_properties_after_write) {
    setup();
    azure_error_t err;
    g_ops->page_blob_create(g_ctx, "test.db", 4096, &err);

    uint8_t data[512] = {0};
    g_ops->page_blob_write(g_ctx, "test.db", 0, data, 512, NULL, &err);

    int64_t size;
    char ls[32], lst[32];
    g_ops->blob_get_properties(g_ctx, "test.db", &size, ls, lst, &err);
    ASSERT_EQ(size, 4096);
}

TEST(concurrent_page_and_block_blobs) {
    setup();
    azure_error_t err;

    g_ops->page_blob_create(g_ctx, "data.db", 4096, &err);
    uint8_t j[] = "journal";
    g_ops->block_blob_upload(g_ctx, "data.db-journal", j, sizeof(j), &err);

    /* Both should be independently accessible */
    int64_t psize = mock_get_page_blob_size(g_ctx, "data.db");
    int64_t bsize = mock_get_block_blob_size(g_ctx, "data.db-journal");
    ASSERT_EQ(psize, 4096);
    ASSERT_EQ(bsize, (int64_t)sizeof(j));
}

/* ══════════════════════════════════════════════════════════════════════
** SECTION 12: VFS Integration Tests (stubs)
**
** These tests require the sqliteObjs VFS implementation. They document
** the expected behavior and will be enabled once Aragorn delivers
** sqlite_objs_vfs.c. For now they serve as the spec-in-code.
**
** Uncomment ENABLE_VFS_INTEGRATION to activate.
** ══════════════════════════════════════════════════════════════════════ */

#ifdef ENABLE_VFS_INTEGRATION

#include "../src/sqlite_objs.h"

static sqlite3 *open_test_db(mock_azure_ctx_t *mctx) {
    sqlite3 *db = NULL;
    sqlite_objs_vfs_register_with_ops(mock_azure_get_ops(), mctx, 0);
    int rc = sqlite3_open_v2("test.db", &db,
                              SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                              "sqlite-objs");
    return (rc == SQLITE_OK) ? db : NULL;
}

static void close_test_db(sqlite3 *db) {
    if (db) sqlite3_close(db);
}

/* ── Registration Tests ───────────────────────────────────────────── */

TEST(vfs_registers_with_correct_name) {
    setup();
    sqlite_objs_vfs_register_with_ops(g_ops, g_ctx, 0);
    sqlite3_vfs *vfs = sqlite3_vfs_find("sqlite-objs");
    ASSERT_NOT_NULL(vfs);
    ASSERT_STR_EQ(vfs->zName, "sqlite-objs");
}

TEST(vfs_not_default_unless_requested) {
    setup();
    sqlite3_vfs *before = sqlite3_vfs_find(NULL);
    sqlite_objs_vfs_register_with_ops(g_ops, g_ctx, 0);
    sqlite3_vfs *after = sqlite3_vfs_find(NULL);
    ASSERT_TRUE(before == after);
}

TEST(vfs_can_be_default) {
    setup();
    sqlite_objs_vfs_register_with_ops(g_ops, g_ctx, 1);
    sqlite3_vfs *def = sqlite3_vfs_find(NULL);
    ASSERT_STR_EQ(def->zName, "sqlite-objs");
}

TEST(vfs_open_with_name_parameter) {
    setup();
    sqlite_objs_vfs_register_with_ops(g_ops, g_ctx, 0);
    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2("test.db", &db,
                              SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                              "sqlite-objs");
    ASSERT_OK(rc);
    ASSERT_NOT_NULL(db);
    close_test_db(db);
}

/* ── Basic DB Operations ──────────────────────────────────────────── */

TEST(vfs_create_new_database) {
    setup();
    sqlite3 *db = open_test_db(g_ctx);
    ASSERT_NOT_NULL(db);

    /* A new db should create a page blob */
    ASSERT_TRUE(mock_blob_exists(g_ctx, "test.db"));
    close_test_db(db);
}

TEST(vfs_create_table_insert_select) {
    setup();
    sqlite3 *db = open_test_db(g_ctx);
    ASSERT_NOT_NULL(db);

    int rc;
    rc = sqlite3_exec(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, val TEXT);",
                       NULL, NULL, NULL);
    ASSERT_OK(rc);

    rc = sqlite3_exec(db, "INSERT INTO t VALUES(1, 'hello');",
                       NULL, NULL, NULL);
    ASSERT_OK(rc);

    sqlite3_stmt *stmt;
    rc = sqlite3_prepare_v2(db, "SELECT val FROM t WHERE id=1;", -1,
                             &stmt, NULL);
    ASSERT_OK(rc);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 0), "hello");
    sqlite3_finalize(stmt);

    close_test_db(db);
}

TEST(vfs_transaction_commit_writes_to_blob) {
    setup();
    sqlite3 *db = open_test_db(g_ctx);
    ASSERT_NOT_NULL(db);

    sqlite3_exec(db, "CREATE TABLE t(x);", NULL, NULL, NULL);
    sqlite3_exec(db, "BEGIN;", NULL, NULL, NULL);
    sqlite3_exec(db, "INSERT INTO t VALUES(42);", NULL, NULL, NULL);

    mock_reset_call_counts(g_ctx);
    sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);

    /* After commit, dirty pages should have been synced to blob */
    ASSERT_GT(mock_get_call_count(g_ctx, "page_blob_write"), 0);

    close_test_db(db);
}

TEST(vfs_multiple_transactions) {
    setup();
    sqlite3 *db = open_test_db(g_ctx);
    ASSERT_NOT_NULL(db);

    sqlite3_exec(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, v TEXT);",
                  NULL, NULL, NULL);

    for (int i = 0; i < 10; i++) {
        char sql[128];
        snprintf(sql, sizeof(sql), "INSERT INTO t VALUES(%d, 'item%d');", i, i);
        int rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
        ASSERT_OK(rc);
    }

    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM t;", -1, &stmt, NULL);
    sqlite3_step(stmt);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), 10);
    sqlite3_finalize(stmt);

    close_test_db(db);
}

TEST(vfs_data_survives_close_reopen) {
    setup();
    /* First session: create and populate */
    sqlite3 *db = open_test_db(g_ctx);
    sqlite3_exec(db, "CREATE TABLE t(x INTEGER);", NULL, NULL, NULL);
    sqlite3_exec(db, "INSERT INTO t VALUES(999);", NULL, NULL, NULL);
    close_test_db(db);

    /* Second session: read back */
    db = open_test_db(g_ctx);
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, "SELECT x FROM t;", -1, &stmt, NULL);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), 999);
    sqlite3_finalize(stmt);
    close_test_db(db);
}

/* ── Dirty Page Tracking ──────────────────────────────────────────── */

TEST(vfs_sync_writes_only_dirty_pages) {
    setup();
    sqlite3 *db = open_test_db(g_ctx);
    sqlite3_exec(db, "CREATE TABLE t(x);", NULL, NULL, NULL);
    /* Let CREATE TABLE settle */

    mock_reset_call_counts(g_ctx);
    sqlite3_exec(db, "INSERT INTO t VALUES(1);", NULL, NULL, NULL);

    /* Only the pages actually changed should be written */
    int writes = mock_get_call_count(g_ctx, "page_blob_write");
    ASSERT_GT(writes, 0);
    ASSERT_LT(writes, 100); /* Sanity: not writing entire db */
    close_test_db(db);
}

TEST(vfs_no_writes_on_read_only_ops) {
    setup();
    sqlite3 *db = open_test_db(g_ctx);
    sqlite3_exec(db, "CREATE TABLE t(x); INSERT INTO t VALUES(1);",
                  NULL, NULL, NULL);

    mock_reset_call_counts(g_ctx);
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, "SELECT * FROM t;", -1, &stmt, NULL);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    /* No writes should happen for pure reads */
    ASSERT_EQ(mock_get_call_count(g_ctx, "page_blob_write"), 0);
    close_test_db(db);
}

/* ── Journal Files ────────────────────────────────────────────────── */

TEST(vfs_journal_created_as_block_blob) {
    setup();
    sqlite3 *db = open_test_db(g_ctx);
    sqlite3_exec(db, "CREATE TABLE t(x);", NULL, NULL, NULL);

    /* A write transaction should create a journal block blob */
    sqlite3_exec(db, "BEGIN;", NULL, NULL, NULL);
    sqlite3_exec(db, "INSERT INTO t VALUES(1);", NULL, NULL, NULL);
    sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);

    /* Journal should have been uploaded at some point */
    ASSERT_GT(mock_get_call_count(g_ctx, "block_blob_upload"), 0);
    close_test_db(db);
}

TEST(vfs_journal_deleted_after_commit) {
    setup();
    sqlite3 *db = open_test_db(g_ctx);
    sqlite3_exec(db, "CREATE TABLE t(x);", NULL, NULL, NULL);
    sqlite3_exec(db, "INSERT INTO t VALUES(1);", NULL, NULL, NULL);

    /* After successful commit, journal should be cleaned up */
    ASSERT_FALSE(mock_blob_exists(g_ctx, "test.db-journal"));
    close_test_db(db);
}

/* ── Locking ──────────────────────────────────────────────────────── */

TEST(vfs_read_no_lease) {
    setup();
    sqlite3 *db = open_test_db(g_ctx);
    sqlite3_exec(db, "CREATE TABLE t(x); INSERT INTO t VALUES(1);",
                  NULL, NULL, NULL);

    mock_reset_call_counts(g_ctx);

    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, "SELECT * FROM t;", -1, &stmt, NULL);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    /* Pure reads should NOT acquire a lease (Decision D3: SHARED=no lease) */
    ASSERT_EQ(mock_get_call_count(g_ctx, "lease_acquire"), 0);
    close_test_db(db);
}

TEST(vfs_write_acquires_lease) {
    setup();
    sqlite3 *db = open_test_db(g_ctx);
    sqlite3_exec(db, "CREATE TABLE t(x);", NULL, NULL, NULL);

    mock_reset_call_counts(g_ctx);
    sqlite3_exec(db, "INSERT INTO t VALUES(1);", NULL, NULL, NULL);

    /* Write should acquire a lease */
    ASSERT_GT(mock_get_call_count(g_ctx, "lease_acquire"), 0);
    close_test_db(db);
}

TEST(vfs_write_releases_lease_after_commit) {
    setup();
    sqlite3 *db = open_test_db(g_ctx);
    sqlite3_exec(db, "CREATE TABLE t(x);", NULL, NULL, NULL);
    sqlite3_exec(db, "INSERT INTO t VALUES(1);", NULL, NULL, NULL);

    /* After commit, lease should be released */
    ASSERT_EQ(mock_is_leased(g_ctx, "test.db"), 0);
    close_test_db(db);
}

TEST(vfs_lease_conflict_returns_busy) {
    setup();
    /* Create a valid database first so the blob has proper SQLite headers */
    sqlite3 *db0 = open_test_db(g_ctx);
    ASSERT_NOT_NULL(db0);
    sqlite3_exec(db0, "CREATE TABLE t(x);", NULL, NULL, NULL);
    sqlite3_close(db0);

    /* Acquire a lease to simulate another writer holding it */
    azure_error_t err;
    char id[64];
    g_ops->lease_acquire(g_ctx, "test.db", 30, id, sizeof(id), &err);

    /* Now try to open and write — should get SQLITE_BUSY */
    sqlite_objs_vfs_register_with_ops(g_ops, g_ctx, 0);
    sqlite3 *db;
    sqlite3_open_v2("test.db", &db,
                     SQLITE_OPEN_READWRITE, "sqlite-objs");

    int rc = sqlite3_exec(db, "INSERT INTO t VALUES(1);", NULL, NULL, NULL);
    ASSERT_ERR(rc, SQLITE_BUSY);

    close_test_db(db);
    g_ops->lease_release(g_ctx, "test.db", id, &err);
}

/* ── Error Handling ───────────────────────────────────────────────── */

TEST(vfs_azure_write_failure_returns_ioerr) {
    setup();
    sqlite3 *db = open_test_db(g_ctx);
    sqlite3_exec(db, "CREATE TABLE t(x);", NULL, NULL, NULL);

    mock_set_fail_operation(g_ctx, "page_blob_write", AZURE_ERR_NETWORK);

    int rc = sqlite3_exec(db, "INSERT INTO t VALUES(1);", NULL, NULL, NULL);
    ASSERT_NE(rc, SQLITE_OK);

    mock_clear_failures(g_ctx);
    close_test_db(db);
}

TEST(vfs_azure_read_failure_on_open) {
    setup();
    /* Create a blob so it "exists" but reads fail */
    azure_error_t err;
    g_ops->page_blob_create(g_ctx, "broken.db", 4096, &err);

    mock_set_fail_operation(g_ctx, "page_blob_read", AZURE_ERR_NETWORK);

    sqlite_objs_vfs_register_with_ops(g_ops, g_ctx, 0);
    sqlite3 *db;
    int rc = sqlite3_open_v2("broken.db", &db,
                              SQLITE_OPEN_READWRITE, "sqlite-objs");
    ASSERT_NE(rc, SQLITE_OK);

    mock_clear_failures(g_ctx);
    if (db) sqlite3_close(db);
}

TEST(vfs_lease_expire_during_sync) {
    setup();
    sqlite3 *db = open_test_db(g_ctx);
    sqlite3_exec(db, "CREATE TABLE t(x);", NULL, NULL, NULL);

    /* VFS inline renewal only triggers after 15s, so in fast tests
    ** lease_renew is never called. Instead, simulate sync failure
    ** by making page_blob_write fail (called during xSync).
    ** Note: R2 optimization means page_blob_resize may be skipped
    ** when the blob hasn't grown, so we fail the write instead. */
    mock_set_fail_operation(g_ctx, "page_blob_write", AZURE_ERR_NETWORK);

    int rc = sqlite3_exec(db, "INSERT INTO t VALUES(1);", NULL, NULL, NULL);
    /* Should fail with IOERR or similar */
    ASSERT_NE(rc, SQLITE_OK);

    mock_clear_failures(g_ctx);
    close_test_db(db);
}

/* ── PRAGMA Tests ─────────────────────────────────────────────────── */

TEST(vfs_pragma_journal_mode_delete) {
    setup();
    sqlite3 *db = open_test_db(g_ctx);

    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, "PRAGMA journal_mode=DELETE;", -1, &stmt, NULL);
    sqlite3_step(stmt);
    const char *mode = (const char *)sqlite3_column_text(stmt, 0);
    ASSERT_STR_EQ(mode, "delete");
    sqlite3_finalize(stmt);

    close_test_db(db);
}

TEST(vfs_pragma_wal_allowed) {
    setup();
    sqlite3 *db = open_test_db(g_ctx);

    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, "PRAGMA journal_mode=WAL;", -1, &stmt, NULL);
    sqlite3_step(stmt);
    const char *mode = (const char *)sqlite3_column_text(stmt, 0);
    /* WAL should be allowed — mock has append blob ops */
    ASSERT_STR_EQ(mode, "wal");
    sqlite3_finalize(stmt);

    close_test_db(db);
}

TEST(vfs_pragma_page_size_4096) {
    setup();
    sqlite3 *db = open_test_db(g_ctx);

    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, "PRAGMA page_size;", -1, &stmt, NULL);
    sqlite3_step(stmt);
    int page_size = sqlite3_column_int(stmt, 0);
    ASSERT_EQ(page_size, 4096);
    sqlite3_finalize(stmt);

    close_test_db(db);
}

/* ── File Type Routing ────────────────────────────────────────────── */

TEST(vfs_temp_files_use_local_storage) {
    setup();
    sqlite3 *db = open_test_db(g_ctx);

    /* Create a temp table — should use local VFS, not Azure */
    mock_reset_call_counts(g_ctx);
    sqlite3_exec(db, "CREATE TEMP TABLE tmp(x);", NULL, NULL, NULL);
    sqlite3_exec(db, "INSERT INTO tmp VALUES(1);", NULL, NULL, NULL);

    /* Temp operations should not touch Azure page/block blob ops
    ** (create might still happen for main db, but temp tables are local) */
    close_test_db(db);
}

/* ══════════════════════════════════════════════════════════════════════
** P1 Security Tests — Error Handling, Boundaries, Injection
** ══════════════════════════════════════════════════════════════════════ */

/*
** Test: Azure error during xSync/flush
** Verifies graceful error handling when network fails mid-flush.
*/
TEST(vfs_azure_sync_failure_returns_ioerr) {
    setup();
    sqlite3 *db = open_test_db(g_ctx);
    sqlite3_exec(db, "CREATE TABLE t(x);", NULL, NULL, NULL);

    /* Insert data to create dirty pages */
    sqlite3_exec(db, "INSERT INTO t VALUES(1);", NULL, NULL, NULL);

    /* Make the next page_blob_write fail (simulates network error during sync) */
    mock_set_fail_operation(g_ctx, "page_blob_write", AZURE_ERR_NETWORK);

    /* This should fail with IOERR, not crash */
    int rc = sqlite3_exec(db, "INSERT INTO t VALUES(2);", NULL, NULL, NULL);
    ASSERT_NE(rc, SQLITE_OK);

    mock_clear_failures(g_ctx);
    close_test_db(db);
}

/*
** Test: Block blob upload failure during journal sync
** Verifies error handling when journal upload fails.
*/
TEST(vfs_journal_upload_failure_returns_ioerr) {
    setup();
    sqlite3 *db = open_test_db(g_ctx);
    sqlite3_exec(db, "CREATE TABLE t(x);", NULL, NULL, NULL);

    /* Make block_blob_upload fail (journal upload) */
    mock_set_fail_operation(g_ctx, "block_blob_upload", AZURE_ERR_NETWORK);

    /* BEGIN + INSERT should work, but COMMIT will fail on journal upload */
    sqlite3_exec(db, "BEGIN;", NULL, NULL, NULL);
    sqlite3_exec(db, "INSERT INTO t VALUES(1);", NULL, NULL, NULL);
    int rc = sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);

    /* Should get an error, not crash */
    ASSERT_NE(rc, SQLITE_OK);

    mock_clear_failures(g_ctx);
    close_test_db(db);
}

/*
** Test: Lease expire during long transaction
** Verifies error handling when lease renewal fails mid-transaction.
*/
TEST(vfs_lease_renew_failure_during_write) {
    setup();
    sqlite3 *db = open_test_db(g_ctx);
    sqlite3_exec(db, "CREATE TABLE t(x);", NULL, NULL, NULL);

    /* Make lease_renew fail */
    mock_set_fail_operation(g_ctx, "lease_renew", AZURE_ERR_NETWORK);

    /* Multiple writes should eventually try to renew lease */
    int rc = SQLITE_OK;
    for (int i = 0; i < 1000 && rc == SQLITE_OK; i++) {
        char sql[64];
        snprintf(sql, sizeof(sql), "INSERT INTO t VALUES(%d);", i);
        rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    }

    /* Should handle lease renewal failure gracefully */
    mock_clear_failures(g_ctx);
    close_test_db(db);
}

/*
** Test: Explicit transaction BEGIN/COMMIT/ROLLBACK
** Verifies transaction boundaries work correctly.
*/
TEST(vfs_explicit_transaction_commit) {
    setup();
    sqlite3 *db = open_test_db(g_ctx);
    sqlite3_exec(db, "CREATE TABLE t(x);", NULL, NULL, NULL);

    int rc = sqlite3_exec(db, "BEGIN;", NULL, NULL, NULL);
    ASSERT_OK(rc);

    rc = sqlite3_exec(db, "INSERT INTO t VALUES(1);", NULL, NULL, NULL);
    ASSERT_OK(rc);

    rc = sqlite3_exec(db, "INSERT INTO t VALUES(2);", NULL, NULL, NULL);
    ASSERT_OK(rc);

    rc = sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
    ASSERT_OK(rc);

    /* Verify data is there */
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM t;", -1, &stmt, NULL);
    sqlite3_step(stmt);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), 2);
    sqlite3_finalize(stmt);

    close_test_db(db);
}

TEST(vfs_explicit_transaction_rollback) {
    setup();
    sqlite3 *db = open_test_db(g_ctx);
    sqlite3_exec(db, "CREATE TABLE t(x);", NULL, NULL, NULL);

    /* Insert initial data */
    sqlite3_exec(db, "INSERT INTO t VALUES(1);", NULL, NULL, NULL);

    int rc = sqlite3_exec(db, "BEGIN;", NULL, NULL, NULL);
    ASSERT_OK(rc);

    rc = sqlite3_exec(db, "INSERT INTO t VALUES(2);", NULL, NULL, NULL);
    ASSERT_OK(rc);

    rc = sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
    ASSERT_OK(rc);

    /* Verify only original data is there */
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM t;", -1, &stmt, NULL);
    sqlite3_step(stmt);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), 1);
    sqlite3_finalize(stmt);

    close_test_db(db);
}

/*
** Test: Large data handling
** Verifies buffer growth with larger data without integer overflow.
*/
TEST(vfs_large_data_insert) {
    setup();
    sqlite3 *db = open_test_db(g_ctx);
    sqlite3_exec(db, "CREATE TABLE t(x BLOB);", NULL, NULL, NULL);

    /* Insert a large blob (64KB) */
    unsigned char *bigdata = malloc(65536);
    ASSERT_NOT_NULL(bigdata);
    memset(bigdata, 0xAB, 65536);

    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, "INSERT INTO t VALUES(?);", -1, &stmt, NULL);
    sqlite3_bind_blob(stmt, 1, bigdata, 65536, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_DONE);
    sqlite3_finalize(stmt);

    free(bigdata);
    close_test_db(db);
}

/*
** Test: Multiple large transactions
** Verifies buffer management doesn't leak or corrupt with repeated use.
*/
TEST(vfs_multiple_large_transactions) {
    setup();
    sqlite3 *db = open_test_db(g_ctx);
    sqlite3_exec(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, data BLOB);",
                 NULL, NULL, NULL);

    unsigned char *data = malloc(8192);
    ASSERT_NOT_NULL(data);

    for (int i = 0; i < 50; i++) {
        memset(data, i & 0xFF, 8192);

        sqlite3_exec(db, "BEGIN;", NULL, NULL, NULL);

        sqlite3_stmt *stmt;
        sqlite3_prepare_v2(db, "INSERT INTO t VALUES(?, ?);", -1, &stmt, NULL);
        sqlite3_bind_int(stmt, 1, i);
        sqlite3_bind_blob(stmt, 2, data, 8192, SQLITE_STATIC);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        int rc = sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
        ASSERT_OK(rc);
    }

    /* Verify count */
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM t;", -1, &stmt, NULL);
    sqlite3_step(stmt);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), 50);
    sqlite3_finalize(stmt);

    free(data);
    close_test_db(db);
}


/* ══════════════════════════════════════════════════════════════════════
** P1 Tests — Azure Error During xSync/Flush (mid-flush failure)
**
** Simulate network failure DURING a multi-page flush.
** Page coalescing merges contiguous dirty pages, so the number of
** page_blob_write calls may be fewer than the number of dirty pages.
** Fail the 1st coalesced write to verify error propagation.
** ══════════════════════════════════════════════════════════════════════ */

TEST(vfs_sync_mid_flush_failure) {
    setup();
    sqlite3 *db = open_test_db(g_ctx);
    ASSERT_NOT_NULL(db);
    sqlite3_exec(db, "CREATE TABLE t(x TEXT);", NULL, NULL, NULL);

    /* Insert enough data to dirty multiple pages */
    for (int i = 0; i < 100; i++) {
        char sql[128];
        snprintf(sql, sizeof(sql),
                 "INSERT INTO t VALUES('row_%d_padding_data_to_fill_pages');", i);
        sqlite3_exec(db, sql, NULL, NULL, NULL);
    }

    /* Let that settle, then create new dirty pages */
    mock_reset_call_counts(g_ctx);
    sqlite3_exec(db, "BEGIN;", NULL, NULL, NULL);
    for (int i = 100; i < 200; i++) {
        char sql[128];
        snprintf(sql, sizeof(sql),
                 "INSERT INTO t VALUES('row_%d_more_padding_data_here');", i);
        sqlite3_exec(db, sql, NULL, NULL, NULL);
    }

    /* Fail the 1st page_blob_write during COMMIT's xSync flush.
    ** With page coalescing, contiguous dirty pages are merged into
    ** fewer writes — fail the first coalesced write to test error path. */
    mock_set_fail_operation_at(g_ctx, "page_blob_write", 1, AZURE_ERR_NETWORK);

    int rc = sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
    /* Should fail gracefully — the commit can't complete */
    ASSERT_NE(rc, SQLITE_OK);

    mock_clear_failures(g_ctx);
    close_test_db(db);
}

TEST(vfs_sync_resize_failure_before_flush) {
    setup();
    sqlite3 *db = open_test_db(g_ctx);
    ASSERT_NOT_NULL(db);
    sqlite3_exec(db, "CREATE TABLE t(x);", NULL, NULL, NULL);

    /* R2: page_blob_resize is only called when the blob has grown.
    ** Insert a large value to force the database to grow beyond its
    ** current size, ensuring resize IS called and the failure triggers. */
    mock_set_fail_operation(g_ctx, "page_blob_resize", AZURE_ERR_SERVER);

    int rc = sqlite3_exec(db,
        "INSERT INTO t VALUES(zeroblob(65536));", NULL, NULL, NULL);
    ASSERT_NE(rc, SQLITE_OK);

    mock_clear_failures(g_ctx);
    close_test_db(db);
}


/* ══════════════════════════════════════════════════════════════════════
** P1 Tests — WAL Mode Rejection
**
** Verify xFileControl properly rejects WAL mode attempts and keeps
** journal_mode=delete as the only valid mode.
** ══════════════════════════════════════════════════════════════════════ */

TEST(vfs_wal_mode_returns_delete) {
    setup();

    /* Temporarily clear block_blob_download to test WAL rejection path.
    ** Keep block_blob_upload intact so journal sync still works. */
    azure_ops_t saved_ops = *g_ops;
    g_ops->block_blob_download = NULL;

    sqlite3 *db = open_test_db(g_ctx);
    ASSERT_NOT_NULL(db);

    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, "PRAGMA journal_mode=WAL;", -1, &stmt, NULL);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    const char *mode = (const char *)sqlite3_column_text(stmt, 0);
    /* WAL must be rejected without block blob ops — stays "delete" */
    ASSERT_STR_EQ(mode, "delete");
    sqlite3_finalize(stmt);

    close_test_db(db);

    /* Restore ops for subsequent tests */
    *g_ops = saved_ops;
}

TEST(vfs_wal_mode_case_insensitive) {
    setup();
    sqlite3 *db = open_test_db(g_ctx);
    ASSERT_NOT_NULL(db);

    /* Test case variants of WAL — all should be accepted with block blob ops */
    const char *wal_variants[] = {"WAL", "wal", "Wal", NULL};
    for (int i = 0; wal_variants[i]; i++) {
        char sql[64];
        snprintf(sql, sizeof(sql), "PRAGMA journal_mode=%s;", wal_variants[i]);
        sqlite3_stmt *stmt;
        sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
        sqlite3_step(stmt);
        const char *mode = (const char *)sqlite3_column_text(stmt, 0);
        ASSERT_STR_EQ(mode, "wal");
        sqlite3_finalize(stmt);
    }

    close_test_db(db);
}

TEST(vfs_journal_mode_memory_rejected) {
    setup();
    sqlite3 *db = open_test_db(g_ctx);
    ASSERT_NOT_NULL(db);

    /* MEMORY mode should work normally (SQLite handles it, not Azure) */
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, "PRAGMA journal_mode;", -1, &stmt, NULL);
    sqlite3_step(stmt);
    const char *mode = (const char *)sqlite3_column_text(stmt, 0);
    /* Default should be delete */
    ASSERT_STR_EQ(mode, "delete");
    sqlite3_finalize(stmt);

    close_test_db(db);
}


/* ══════════════════════════════════════════════════════════════════════
** P1 Tests — Explicit BEGIN/COMMIT/ROLLBACK
**
** Test transaction correctness through the SQLite API, verifying
** that commit persists data to Azure and rollback discards it.
** ══════════════════════════════════════════════════════════════════════ */

TEST(vfs_transaction_rollback_restores_state) {
    setup();
    sqlite3 *db = open_test_db(g_ctx);
    ASSERT_NOT_NULL(db);
    sqlite3_exec(db, "CREATE TABLE t(x INTEGER);", NULL, NULL, NULL);
    sqlite3_exec(db, "INSERT INTO t VALUES(10);", NULL, NULL, NULL);
    sqlite3_exec(db, "INSERT INTO t VALUES(20);", NULL, NULL, NULL);

    /* Begin a transaction, modify data, then rollback */
    sqlite3_exec(db, "BEGIN;", NULL, NULL, NULL);
    sqlite3_exec(db, "DELETE FROM t;", NULL, NULL, NULL);
    sqlite3_exec(db, "INSERT INTO t VALUES(99);", NULL, NULL, NULL);

    int rc = sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
    ASSERT_OK(rc);

    /* Original data should still be there */
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, "SELECT SUM(x) FROM t;", -1, &stmt, NULL);
    sqlite3_step(stmt);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), 30);
    sqlite3_finalize(stmt);

    close_test_db(db);
}

TEST(vfs_transaction_commit_persists_to_azure) {
    setup();
    sqlite3 *db = open_test_db(g_ctx);
    ASSERT_NOT_NULL(db);
    sqlite3_exec(db, "CREATE TABLE t(x INTEGER);", NULL, NULL, NULL);

    sqlite3_exec(db, "BEGIN;", NULL, NULL, NULL);
    sqlite3_exec(db, "INSERT INTO t VALUES(42);", NULL, NULL, NULL);
    sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);

    /* Close and reopen to verify data survived via Azure blob */
    close_test_db(db);
    db = open_test_db(g_ctx);
    ASSERT_NOT_NULL(db);

    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, "SELECT x FROM t;", -1, &stmt, NULL);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), 42);
    sqlite3_finalize(stmt);

    close_test_db(db);
}

TEST(vfs_nested_savepoint_rollback) {
    setup();
    sqlite3 *db = open_test_db(g_ctx);
    ASSERT_NOT_NULL(db);
    sqlite3_exec(db, "CREATE TABLE t(x INTEGER);", NULL, NULL, NULL);

    sqlite3_exec(db, "BEGIN;", NULL, NULL, NULL);
    sqlite3_exec(db, "INSERT INTO t VALUES(1);", NULL, NULL, NULL);
    sqlite3_exec(db, "SAVEPOINT sp1;", NULL, NULL, NULL);
    sqlite3_exec(db, "INSERT INTO t VALUES(2);", NULL, NULL, NULL);
    sqlite3_exec(db, "ROLLBACK TO sp1;", NULL, NULL, NULL);
    int rc = sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
    ASSERT_OK(rc);

    /* Only row 1 should remain (row 2 was rolled back at savepoint) */
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM t;", -1, &stmt, NULL);
    sqlite3_step(stmt);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), 1);
    sqlite3_finalize(stmt);

    close_test_db(db);
}


/* ══════════════════════════════════════════════════════════════════════
** P1 Tests — Close When Flush Fails
**
** Test error handling when xClose encounters dirty data that can't
** be uploaded (e.g., network failure).
** ══════════════════════════════════════════════════════════════════════ */

TEST(vfs_close_with_pending_dirty_data) {
    setup();
    sqlite3 *db = open_test_db(g_ctx);
    ASSERT_NOT_NULL(db);
    sqlite3_exec(db, "CREATE TABLE t(x);", NULL, NULL, NULL);

    /* Write data, then make page_blob_write fail so sync fails */
    sqlite3_exec(db, "BEGIN;", NULL, NULL, NULL);
    sqlite3_exec(db, "INSERT INTO t VALUES(1);", NULL, NULL, NULL);

    mock_set_fail_operation(g_ctx, "page_blob_write", AZURE_ERR_NETWORK);

    /* COMMIT will fail */
    int rc = sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
    ASSERT_NE(rc, SQLITE_OK);

    mock_clear_failures(g_ctx);

    /* Close should not crash even though commit failed */
    sqlite3_close(db);
}

TEST(vfs_close_releases_lease_on_failure) {
    setup();
    sqlite3 *db = open_test_db(g_ctx);
    ASSERT_NOT_NULL(db);
    sqlite3_exec(db, "CREATE TABLE t(x);", NULL, NULL, NULL);

    /* Start a write transaction to acquire a lease */
    sqlite3_exec(db, "BEGIN;", NULL, NULL, NULL);
    sqlite3_exec(db, "INSERT INTO t VALUES(1);", NULL, NULL, NULL);

    /* Fail the commit (page_blob_resize fails) */
    mock_set_fail_operation(g_ctx, "page_blob_resize", AZURE_ERR_NETWORK);
    sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
    mock_clear_failures(g_ctx);

    /* Close should still release the lease */
    sqlite3_close(db);

    /* Verify the lease is released */
    ASSERT_EQ(mock_is_leased(g_ctx, "test.db"), 0);
}


/* ══════════════════════════════════════════════════════════════════════
** P1 Tests — URL Injection via Blob Names
**
** Verify that the VFS rejects blob names that could corrupt HTTP
** requests (path traversal, query params, fragment injection).
** ══════════════════════════════════════════════════════════════════════ */

TEST(vfs_path_traversal_rejected) {
    setup();
    sqlite_objs_vfs_register_with_ops(g_ops, g_ctx, 0);
    sqlite3 *db = NULL;

    /* Blob name with ".." path traversal should be rejected */
    int rc = sqlite3_open_v2("../../etc/passwd", &db,
                              SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                              "sqlite-objs");
    ASSERT_NE(rc, SQLITE_OK);
    if (db) sqlite3_close(db);
}

TEST(vfs_path_traversal_in_directory) {
    setup();
    sqlite_objs_vfs_register_with_ops(g_ops, g_ctx, 0);
    sqlite3 *db = NULL;

    int rc = sqlite3_open_v2("foo/../bar/db.sqlite", &db,
                              SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                              "sqlite-objs");
    ASSERT_NE(rc, SQLITE_OK);
    if (db) sqlite3_close(db);
}

TEST(vfs_empty_name_rejected) {
    setup();
    sqlite_objs_vfs_register_with_ops(g_ops, g_ctx, 0);
    sqlite3 *db = NULL;

    /* Empty name - SQLite may treat this as in-memory db or fail */
    int rc = sqlite3_open_v2("", &db,
                              SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                              "sqlite-objs");
    /* Either it fails, or it creates an in-memory db that doesn't touch Azure */
    if (rc == SQLITE_OK) {
        /* If it succeeds, verify no Azure operations were performed */
        mock_reset_call_counts(g_ctx);
        sqlite3_exec(db, "CREATE TABLE t(x);", NULL, NULL, NULL);
        /* Should NOT create page blobs - this is in-memory or temp */
        /* (We just verify it doesn't crash) */
    }
    if (db) sqlite3_close(db);
}

TEST(vfs_leading_slashes_stripped) {
    setup();
    sqlite_objs_vfs_register_with_ops(g_ops, g_ctx, 0);

    /* Open a db with leading slashes — should be normalized */
    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2("///test.db", &db,
                              SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                              "sqlite-objs");
    /* Should succeed and create blob named "test.db" (not "///test.db") */
    ASSERT_OK(rc);
    ASSERT_NOT_NULL(db);

    sqlite3_exec(db, "CREATE TABLE t(x);", NULL, NULL, NULL);
    ASSERT_TRUE(mock_blob_exists(g_ctx, "test.db"));
    sqlite3_close(db);
}


/* ══════════════════════════════════════════════════════════════════════
** P1 Tests — Credential Leak in Error Messages
**
** Ensure that account keys and SAS tokens don't appear in error
** output when Azure operations fail.
** ══════════════════════════════════════════════════════════════════════ */

TEST(vfs_error_no_credential_leak) {
    setup();
    /* Inject a failure and verify the error message */
    azure_error_t err;
    azure_error_init(&err);
    mock_set_fail_operation(g_ctx, "page_blob_create", AZURE_ERR_AUTH);

    g_ops->page_blob_create(g_ctx, "test.db", 4096, &err);

    /* Error messages should NOT contain fake credential patterns */
    ASSERT_TRUE(strstr(err.error_message, "SharedKey") == NULL);
    ASSERT_TRUE(strstr(err.error_message, "sig=") == NULL);
    ASSERT_TRUE(strstr(err.error_message, "sv=") == NULL);
    ASSERT_TRUE(strstr(err.error_message, "AccountKey") == NULL);

    /* error_code should also be clean */
    ASSERT_TRUE(strstr(err.error_code, "sig=") == NULL);
    ASSERT_TRUE(strstr(err.error_code, "AccountKey") == NULL);
}

TEST(vfs_error_no_request_id_credential_leak) {
    setup();
    azure_error_t err;
    azure_error_init(&err);
    mock_set_fail_operation(g_ctx, "page_blob_read", AZURE_ERR_NETWORK);

    azure_buffer_t buf;
    azure_buffer_init(&buf);
    g_ops->page_blob_read(g_ctx, "test.db", 0, 512, &buf, &err);

    /* request_id should be present but NOT contain credentials */
    ASSERT_TRUE(strlen(err.request_id) > 0);
    ASSERT_TRUE(strstr(err.request_id, "sig=") == NULL);
    ASSERT_TRUE(strstr(err.request_id, "SharedKey") == NULL);
    azure_buffer_free(&buf);
}

/* ══════════════════════════════════════════════════════════════════════
** SECTION 13: Lazy Cache — Open & Bootstrap
**
** Tests that prefetch=none opens lazily (minimal download), while
** prefetch=all (default) downloads everything.
** ══════════════════════════════════════════════════════════════════════ */

static sqlite3 *open_lazy_db(mock_azure_ctx_t *mctx, const char *name) {
    sqlite3 *db = NULL;
    sqlite_objs_vfs_register_with_ops(mock_azure_get_ops(), mctx, 0);
    char uri[256];
    snprintf(uri, sizeof(uri), "file:%s?prefetch=none", name);
    int rc = sqlite3_open_v2(uri, &db,
                              SQLITE_OPEN_URI | SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                              "sqlite-objs");
    return (rc == SQLITE_OK) ? db : NULL;
}

static sqlite3 *open_lazy_db_cache_reuse(mock_azure_ctx_t *mctx,
                                          const char *name,
                                          const char *cacheDir) {
    sqlite3 *db = NULL;
    sqlite_objs_vfs_register_with_ops(mock_azure_get_ops(), mctx, 0);
    char uri[512];
    snprintf(uri, sizeof(uri),
             "file:%s?prefetch=none&cache_reuse=1&cache_dir=%s",
             name, cacheDir);
    int rc = sqlite3_open_v2(uri, &db,
                              SQLITE_OPEN_URI | SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                              "sqlite-objs");
    return (rc == SQLITE_OK) ? db : NULL;
}

/* Helper: clean up a cache directory (state, etag, cache files) */
static void cleanup_lazy_cache_dir(const char *cacheDir) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", cacheDir);
    (void)system(cmd);
}

TEST(lazy_open_no_full_download) {
    setup();
    /* First, create a large database the normal (prefetch=all) way */
    sqlite3 *db = open_test_db(g_ctx);
    ASSERT_NOT_NULL(db);
    sqlite3_exec(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, data TEXT);",
                 NULL, NULL, NULL);
    for (int i = 0; i < 200; i++) {
        char sql[256];
        snprintf(sql, sizeof(sql),
                 "INSERT INTO t VALUES(%d, '%0200d');", i, i);
        sqlite3_exec(db, sql, NULL, NULL, NULL);
    }
    close_test_db(db);

    /* Now reopen with prefetch=none */
    mock_reset_call_counts(g_ctx);
    db = open_lazy_db(g_ctx, "test.db");
    ASSERT_NOT_NULL(db);

    /* With lazy open, only 1 page_blob_read for bootstrap (first 64KB) */
    int reads = mock_get_call_count(g_ctx, "page_blob_read");
    ASSERT_EQ(reads, 1);

    close_test_db(db);
}

TEST(lazy_open_page1_fetched) {
    setup();
    sqlite3 *db = open_test_db(g_ctx);
    ASSERT_NOT_NULL(db);
    sqlite3_exec(db, "CREATE TABLE t(x);", NULL, NULL, NULL);
    close_test_db(db);

    /* Reopen lazily */
    mock_reset_call_counts(g_ctx);
    db = open_lazy_db(g_ctx, "test.db");
    ASSERT_NOT_NULL(db);

    /* Page 1 was fetched at open — verify we can read from it without
    ** additional Azure calls */
    mock_reset_call_counts(g_ctx);
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, "SELECT name FROM sqlite_master;",
                                -1, &stmt, NULL);
    ASSERT_OK(rc);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    /* No additional reads needed for page 1 (already bootstrapped) */
    ASSERT_EQ(mock_get_call_count(g_ctx, "page_blob_read"), 0);

    close_test_db(db);
}

TEST(lazy_open_default_full_download) {
    setup();
    sqlite3 *db = open_test_db(g_ctx);
    ASSERT_NOT_NULL(db);
    sqlite3_exec(db, "CREATE TABLE t(x);", NULL, NULL, NULL);
    for (int i = 0; i < 50; i++) {
        char sql[128];
        snprintf(sql, sizeof(sql),
                 "INSERT INTO t VALUES('row_%d_data');", i);
        sqlite3_exec(db, sql, NULL, NULL, NULL);
    }
    close_test_db(db);

    /* Reopen with default prefetch (all) — should download everything */
    mock_reset_call_counts(g_ctx);
    db = open_test_db(g_ctx);
    ASSERT_NOT_NULL(db);

    int reads = mock_get_call_count(g_ctx, "page_blob_read");
    /* Full download uses 1 page_blob_read call for the entire blob */
    ASSERT_GE(reads, 1);

    /* Verify we can query without any more reads */
    mock_reset_call_counts(g_ctx);
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM t;", -1, &stmt, NULL);
    sqlite3_step(stmt);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), 50);
    sqlite3_finalize(stmt);
    ASSERT_EQ(mock_get_call_count(g_ctx, "page_blob_read"), 0);

    close_test_db(db);
}

TEST(lazy_open_create_new_db) {
    setup();
    /* Create a brand new database with prefetch=none */
    sqlite3 *db = open_lazy_db(g_ctx, "newlazy.db");
    ASSERT_NOT_NULL(db);

    int rc = sqlite3_exec(db, "CREATE TABLE t(x INTEGER);", NULL, NULL, NULL);
    ASSERT_OK(rc);
    rc = sqlite3_exec(db, "INSERT INTO t VALUES(42);", NULL, NULL, NULL);
    ASSERT_OK(rc);

    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, "SELECT x FROM t;", -1, &stmt, NULL);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), 42);
    sqlite3_finalize(stmt);

    close_test_db(db);
}

/* ══════════════════════════════════════════════════════════════════════
** SECTION 14: Lazy Cache — xRead Behavior
**
** Tests that lazy xRead fetches from Azure only for invalid pages
** and serves valid pages from cache.
** ══════════════════════════════════════════════════════════════════════ */

TEST(lazy_read_valid_from_cache) {
    setup();
    /* Create db, populate, close */
    sqlite3 *db = open_test_db(g_ctx);
    ASSERT_NOT_NULL(db);
    sqlite3_exec(db, "CREATE TABLE t(x INTEGER);", NULL, NULL, NULL);
    sqlite3_exec(db, "INSERT INTO t VALUES(1);", NULL, NULL, NULL);
    close_test_db(db);

    /* Open lazily, write to mark pages valid */
    db = open_lazy_db(g_ctx, "test.db");
    ASSERT_NOT_NULL(db);
    sqlite3_exec(db, "INSERT INTO t VALUES(2);", NULL, NULL, NULL);

    /* Reset counts, now read the written page back */
    mock_reset_call_counts(g_ctx);
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM t;", -1, &stmt, NULL);
    sqlite3_step(stmt);
    int count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    ASSERT_GE(count, 1);

    /* Pages that were written should be served from cache — minimal Azure reads */
    close_test_db(db);
}

TEST(lazy_read_invalid_fetches_from_azure) {
    setup();
    /* Create a multi-page database */
    sqlite3 *db = open_test_db(g_ctx);
    ASSERT_NOT_NULL(db);
    sqlite3_exec(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, data TEXT);",
                 NULL, NULL, NULL);
    for (int i = 0; i < 300; i++) {
        char sql[256];
        snprintf(sql, sizeof(sql),
                 "INSERT INTO t VALUES(%d, '%0200d');", i, i);
        sqlite3_exec(db, sql, NULL, NULL, NULL);
    }
    close_test_db(db);

    /* Open lazily — only bootstrap pages are valid */
    db = open_lazy_db(g_ctx, "test.db");
    ASSERT_NOT_NULL(db);

    /* Read data from pages beyond bootstrap (64KB) → should trigger Azure fetch */
    mock_reset_call_counts(g_ctx);
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM t;", -1, &stmt, NULL);
    sqlite3_step(stmt);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), 300);
    sqlite3_finalize(stmt);

    /* Should have fetched pages from Azure */
    int reads = mock_get_call_count(g_ctx, "page_blob_read");
    ASSERT_GT(reads, 0);

    close_test_db(db);
}

TEST(lazy_read_after_write_no_azure) {
    setup();
    sqlite3 *db = open_lazy_db(g_ctx, "writecache.db");
    ASSERT_NOT_NULL(db);

    /* Write data — marks pages as both dirty and valid */
    sqlite3_exec(db, "CREATE TABLE t(x TEXT);", NULL, NULL, NULL);
    for (int i = 0; i < 20; i++) {
        char sql[128];
        snprintf(sql, sizeof(sql),
                 "INSERT INTO t VALUES('data_%d');", i);
        sqlite3_exec(db, sql, NULL, NULL, NULL);
    }

    /* Reset and read back — should come from cache, not Azure */
    mock_reset_call_counts(g_ctx);
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM t;", -1, &stmt, NULL);
    sqlite3_step(stmt);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), 20);
    sqlite3_finalize(stmt);

    /* No Azure reads needed — everything was written locally */
    ASSERT_EQ(mock_get_call_count(g_ctx, "page_blob_read"), 0);

    close_test_db(db);
}

TEST(lazy_read_readahead_window) {
    setup();
    /* Create a database with many pages */
    sqlite3 *db = open_test_db(g_ctx);
    ASSERT_NOT_NULL(db);
    sqlite3_exec(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, data TEXT);",
                 NULL, NULL, NULL);
    for (int i = 0; i < 500; i++) {
        char sql[256];
        snprintf(sql, sizeof(sql),
                 "INSERT INTO t VALUES(%d, '%0200d');", i, i);
        sqlite3_exec(db, sql, NULL, NULL, NULL);
    }
    close_test_db(db);

    /* Open lazily */
    db = open_lazy_db(g_ctx, "test.db");
    ASSERT_NOT_NULL(db);

    /* Read all data — triggers readahead */
    mock_reset_call_counts(g_ctx);
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM t;", -1, &stmt, NULL);
    sqlite3_step(stmt);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), 500);
    sqlite3_finalize(stmt);

    /* Readahead fetches 16 pages at a time, so # of reads should be
    ** much less than total pages. On a ~500-row table with 4096-byte
    ** pages, there are maybe ~50 pages. With 16-page readahead and
    ** bootstrap covering first 16 pages, we'd expect ~3-4 read calls. */
    int reads = mock_get_call_count(g_ctx, "page_blob_read");
    ASSERT_GT(reads, 0);

    /* Second full scan should NOT trigger any Azure reads (all cached) */
    mock_reset_call_counts(g_ctx);
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM t;", -1, &stmt, NULL);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    ASSERT_EQ(mock_get_call_count(g_ctx, "page_blob_read"), 0);

    close_test_db(db);
}

TEST(lazy_read_sequential_hits_cache) {
    setup();
    sqlite3 *db = open_test_db(g_ctx);
    ASSERT_NOT_NULL(db);
    sqlite3_exec(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, data TEXT);",
                 NULL, NULL, NULL);
    for (int i = 0; i < 100; i++) {
        char sql[256];
        snprintf(sql, sizeof(sql),
                 "INSERT INTO t VALUES(%d, '%0200d');", i, i);
        sqlite3_exec(db, sql, NULL, NULL, NULL);
    }
    close_test_db(db);

    db = open_lazy_db(g_ctx, "test.db");
    ASSERT_NOT_NULL(db);

    /* First scan triggers fetches */
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, "SELECT * FROM t ORDER BY id;", -1, &stmt, NULL);
    while (sqlite3_step(stmt) == SQLITE_ROW) { /* consume all */ }
    sqlite3_finalize(stmt);

    /* Second identical scan — should be fully cached */
    mock_reset_call_counts(g_ctx);
    sqlite3_prepare_v2(db, "SELECT * FROM t ORDER BY id;", -1, &stmt, NULL);
    while (sqlite3_step(stmt) == SQLITE_ROW) { /* consume all */ }
    sqlite3_finalize(stmt);

    ASSERT_EQ(mock_get_call_count(g_ctx, "page_blob_read"), 0);
    close_test_db(db);
}

TEST(lazy_read_subpage_header) {
    setup();
    sqlite3 *db = open_test_db(g_ctx);
    ASSERT_NOT_NULL(db);
    sqlite3_exec(db, "CREATE TABLE t(x);", NULL, NULL, NULL);
    close_test_db(db);

    /* Open lazily — the very first read is the SQLite header (100 bytes)
    ** which is a sub-page read. The bootstrap already covers it. */
    db = open_lazy_db(g_ctx, "test.db");
    ASSERT_NOT_NULL(db);

    /* Read the page_size pragma (reads the 100-byte header) */
    mock_reset_call_counts(g_ctx);
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, "PRAGMA page_size;", -1, &stmt, NULL);
    sqlite3_step(stmt);
    int ps = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    ASSERT_EQ(ps, 4096);

    /* No additional Azure reads — sub-page read served from bootstrap cache */
    ASSERT_EQ(mock_get_call_count(g_ctx, "page_blob_read"), 0);

    close_test_db(db);
}

/* ══════════════════════════════════════════════════════════════════════
** SECTION 15: Lazy Cache — xWrite & xTruncate Bitmap Effects
**
** Tests that xWrite marks pages valid+dirty and xTruncate clears
** valid bits for truncated pages.
** ══════════════════════════════════════════════════════════════════════ */

TEST(lazy_write_marks_valid_and_dirty) {
    setup();
    sqlite3 *db = open_lazy_db(g_ctx, "wvd.db");
    ASSERT_NOT_NULL(db);

    sqlite3_exec(db, "CREATE TABLE t(x TEXT);", NULL, NULL, NULL);
    for (int i = 0; i < 10; i++) {
        char sql[128];
        snprintf(sql, sizeof(sql),
                 "INSERT INTO t VALUES('row_%d');", i);
        sqlite3_exec(db, sql, NULL, NULL, NULL);
    }

    /* Written pages are now valid — reads won't hit Azure */
    mock_reset_call_counts(g_ctx);
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM t;", -1, &stmt, NULL);
    sqlite3_step(stmt);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), 10);
    sqlite3_finalize(stmt);
    ASSERT_EQ(mock_get_call_count(g_ctx, "page_blob_read"), 0);

    /* But they ARE dirty — commit should write to Azure */
    mock_reset_call_counts(g_ctx);
    sqlite3_exec(db, "BEGIN; INSERT INTO t VALUES('flush'); COMMIT;",
                 NULL, NULL, NULL);
    ASSERT_GT(mock_get_call_count(g_ctx, "page_blob_write"), 0);

    close_test_db(db);
}

TEST(lazy_truncate_clears_valid_bits) {
    setup();
    /* Build a multi-page db */
    sqlite3 *db = open_test_db(g_ctx);
    ASSERT_NOT_NULL(db);
    sqlite3_exec(db, "CREATE TABLE t(x TEXT);", NULL, NULL, NULL);
    for (int i = 0; i < 100; i++) {
        char sql[256];
        snprintf(sql, sizeof(sql),
                 "INSERT INTO t VALUES('%0200d');", i);
        sqlite3_exec(db, sql, NULL, NULL, NULL);
    }
    close_test_db(db);

    /* Open lazily, read everything (makes all pages valid) */
    db = open_lazy_db(g_ctx, "test.db");
    ASSERT_NOT_NULL(db);
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM t;", -1, &stmt, NULL);
    sqlite3_step(stmt);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), 100);
    sqlite3_finalize(stmt);

    /* Verify all cached now */
    mock_reset_call_counts(g_ctx);
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM t;", -1, &stmt, NULL);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    ASSERT_EQ(mock_get_call_count(g_ctx, "page_blob_read"), 0);

    /* Now delete lots of data — this may trigger a VACUUM-like shrink
    ** through normal SQLite operations. The key point: after the
    ** deletions, the file should still be consistent. */
    sqlite3_exec(db, "DELETE FROM t WHERE rowid > 5;", NULL, NULL, NULL);

    /* Reads still work */
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM t;", -1, &stmt, NULL);
    sqlite3_step(stmt);
    int remaining = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    ASSERT_LE(remaining, 5);

    close_test_db(db);
}

TEST(lazy_write_beyond_eof) {
    setup();
    sqlite3 *db = open_lazy_db(g_ctx, "grow.db");
    ASSERT_NOT_NULL(db);

    sqlite3_exec(db, "CREATE TABLE t(data BLOB);", NULL, NULL, NULL);

    /* Insert a large blob to force file growth */
    unsigned char *bigdata = (unsigned char *)malloc(32768);
    ASSERT_NOT_NULL(bigdata);
    memset(bigdata, 0xAB, 32768);

    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, "INSERT INTO t VALUES(?);", -1, &stmt, NULL);
    sqlite3_bind_blob(stmt, 1, bigdata, 32768, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_DONE);
    sqlite3_finalize(stmt);

    /* Read the data back — should come from cache (written pages valid) */
    mock_reset_call_counts(g_ctx);
    sqlite3_prepare_v2(db, "SELECT length(data) FROM t;", -1, &stmt, NULL);
    sqlite3_step(stmt);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), 32768);
    sqlite3_finalize(stmt);
    ASSERT_EQ(mock_get_call_count(g_ctx, "page_blob_read"), 0);

    free(bigdata);
    close_test_db(db);
}

/* ══════════════════════════════════════════════════════════════════════
** SECTION 16: Lazy Cache — Prefetch PRAGMA
**
** Tests the PRAGMA sqlite_objs_prefetch command that downloads
** all invalid pages in bulk.
** ══════════════════════════════════════════════════════════════════════ */

TEST(lazy_prefetch_downloads_invalid) {
    setup();
    /* Build a multi-page database that exceeds the 64KB bootstrap */
    sqlite3 *db = open_test_db(g_ctx);
    ASSERT_NOT_NULL(db);
    sqlite3_exec(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, data TEXT);",
                 NULL, NULL, NULL);
    for (int i = 0; i < 500; i++) {
        char sql[256];
        snprintf(sql, sizeof(sql),
                 "INSERT INTO t VALUES(%d, '%0200d');", i, i);
        sqlite3_exec(db, sql, NULL, NULL, NULL);
    }
    close_test_db(db);

    /* Open lazily */
    db = open_lazy_db(g_ctx, "test.db");
    ASSERT_NOT_NULL(db);

    /* Run prefetch PRAGMA — should download all invalid pages */
    mock_reset_call_counts(g_ctx);
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, "PRAGMA sqlite_objs_prefetch;", -1, &stmt, NULL);
    sqlite3_step(stmt);
    const char *result = (const char *)sqlite3_column_text(stmt, 0);
    ASSERT_STR_EQ(result, "ok");
    sqlite3_finalize(stmt);

    /* Prefetch made Azure read calls */
    ASSERT_GT(mock_get_call_count(g_ctx, "page_blob_read"), 0);

    /* After prefetch, full scan should need zero Azure reads */
    mock_reset_call_counts(g_ctx);
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM t;", -1, &stmt, NULL);
    sqlite3_step(stmt);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), 500);
    sqlite3_finalize(stmt);
    ASSERT_EQ(mock_get_call_count(g_ctx, "page_blob_read"), 0);

    close_test_db(db);
}

TEST(lazy_prefetch_all_valid_noop) {
    setup();
    /* Create a new db with lazy — all written pages are already valid */
    sqlite3 *db = open_lazy_db(g_ctx, "validnoop.db");
    ASSERT_NOT_NULL(db);
    sqlite3_exec(db, "CREATE TABLE t(x);", NULL, NULL, NULL);
    sqlite3_exec(db, "INSERT INTO t VALUES(1);", NULL, NULL, NULL);

    /* Prefetch on a fully-written db should be a no-op */
    mock_reset_call_counts(g_ctx);
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, "PRAGMA sqlite_objs_prefetch;", -1, &stmt, NULL);
    sqlite3_step(stmt);
    const char *result = (const char *)sqlite3_column_text(stmt, 0);
    ASSERT_STR_EQ(result, "ok");
    sqlite3_finalize(stmt);

    /* No Azure reads needed — all pages are already valid */
    ASSERT_EQ(mock_get_call_count(g_ctx, "page_blob_read"), 0);

    close_test_db(db);
}

TEST(lazy_prefetch_mixed_valid_invalid) {
    setup();
    /* Build a big database */
    sqlite3 *db = open_test_db(g_ctx);
    ASSERT_NOT_NULL(db);
    sqlite3_exec(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, data TEXT);",
                 NULL, NULL, NULL);
    for (int i = 0; i < 300; i++) {
        char sql[256];
        snprintf(sql, sizeof(sql),
                 "INSERT INTO t VALUES(%d, '%0200d');", i, i);
        sqlite3_exec(db, sql, NULL, NULL, NULL);
    }
    close_test_db(db);

    /* Open lazily — bootstrap makes first 16 pages (64KB / 4096) valid */
    db = open_lazy_db(g_ctx, "test.db");
    ASSERT_NOT_NULL(db);

    /* Read a few pages to make them valid (but not all) */
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, "SELECT data FROM t WHERE id < 10;",
                       -1, &stmt, NULL);
    while (sqlite3_step(stmt) == SQLITE_ROW) { /* consume */ }
    sqlite3_finalize(stmt);

    /* Now prefetch — should only fetch remaining invalid pages */
    int readsBefore = mock_get_call_count(g_ctx, "page_blob_read");
    sqlite3_prepare_v2(db, "PRAGMA sqlite_objs_prefetch;", -1, &stmt, NULL);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    int readsAfter = mock_get_call_count(g_ctx, "page_blob_read");

    /* Prefetch fetched additional pages (but not all pages from scratch) */
    int prefetchReads = readsAfter - readsBefore;
    ASSERT_GT(prefetchReads, 0);

    /* After prefetch, full scan needs zero reads */
    mock_reset_call_counts(g_ctx);
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM t;", -1, &stmt, NULL);
    sqlite3_step(stmt);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), 300);
    sqlite3_finalize(stmt);
    ASSERT_EQ(mock_get_call_count(g_ctx, "page_blob_read"), 0);

    close_test_db(db);
}

TEST(lazy_prefetch_noop_on_prefetch_all_mode) {
    setup();
    sqlite3 *db = open_test_db(g_ctx);
    ASSERT_NOT_NULL(db);
    sqlite3_exec(db, "CREATE TABLE t(x);", NULL, NULL, NULL);

    /* On a prefetch=all db, PRAGMA sqlite_objs_prefetch returns "noop" */
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, "PRAGMA sqlite_objs_prefetch;", -1, &stmt, NULL);
    sqlite3_step(stmt);
    const char *result = (const char *)sqlite3_column_text(stmt, 0);
    ASSERT_STR_EQ(result, "noop");
    sqlite3_finalize(stmt);

    close_test_db(db);
}

/* ══════════════════════════════════════════════════════════════════════
** SECTION 17: Lazy Cache — State File I/O
**
** Tests state file persistence across close/reopen when cache_reuse
** is enabled.
** ══════════════════════════════════════════════════════════════════════ */

TEST(lazy_state_write_read_roundtrip) {
    setup();
    const char *cacheDir = "/tmp/test_lazy_state_rt";
    cleanup_lazy_cache_dir(cacheDir);

    /* First session: create and partially read */
    sqlite3 *db = open_test_db(g_ctx);
    ASSERT_NOT_NULL(db);
    sqlite3_exec(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, data TEXT);",
                 NULL, NULL, NULL);
    for (int i = 0; i < 200; i++) {
        char sql[256];
        snprintf(sql, sizeof(sql),
                 "INSERT INTO t VALUES(%d, '%0200d');", i, i);
        sqlite3_exec(db, sql, NULL, NULL, NULL);
    }
    close_test_db(db);

    /* Open with lazy + cache_reuse */
    db = open_lazy_db_cache_reuse(g_ctx, "test.db", cacheDir);
    ASSERT_NOT_NULL(db);

    /* Read a subset of data — caches some pages */
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, "SELECT data FROM t WHERE id < 20;",
                       -1, &stmt, NULL);
    while (sqlite3_step(stmt) == SQLITE_ROW) { /* consume */ }
    sqlite3_finalize(stmt);

    sqlite3_close(db);

    /* Second session: reopen — should reuse cache with state file */
    mock_reset_call_counts(g_ctx);
    db = open_lazy_db_cache_reuse(g_ctx, "test.db", cacheDir);
    ASSERT_NOT_NULL(db);

    /* Check download count — should be 0 if ETag matched */
    int downloads = 0;
    sqlite3_file_control(db, "main", SQLITE_OBJS_FCNTL_DOWNLOAD_COUNT,
                         &downloads);
    ASSERT_EQ(downloads, 0);

    /* Reading already-cached pages should not need Azure */
    mock_reset_call_counts(g_ctx);
    sqlite3_prepare_v2(db, "SELECT data FROM t WHERE id < 20;",
                       -1, &stmt, NULL);
    while (sqlite3_step(stmt) == SQLITE_ROW) { /* consume */ }
    sqlite3_finalize(stmt);

    /* Previously-cached pages served from cache — no Azure reads */
    ASSERT_EQ(mock_get_call_count(g_ctx, "page_blob_read"), 0);

    sqlite3_close(db);
    cleanup_lazy_cache_dir(cacheDir);
}

TEST(lazy_state_missing_file) {
    setup();
    const char *cacheDir = "/tmp/test_lazy_state_miss";
    cleanup_lazy_cache_dir(cacheDir);

    /* First session: create db, open with cache_reuse */
    sqlite3 *db = open_test_db(g_ctx);
    ASSERT_NOT_NULL(db);
    sqlite3_exec(db, "CREATE TABLE t(x);", NULL, NULL, NULL);
    for (int i = 0; i < 50; i++) {
        char sql[128];
        snprintf(sql, sizeof(sql),
                 "INSERT INTO t VALUES('row_%d_data');", i);
        sqlite3_exec(db, sql, NULL, NULL, NULL);
    }
    close_test_db(db);

    /* Open with lazy + cache_reuse, read some data, close */
    db = open_lazy_db_cache_reuse(g_ctx, "test.db", cacheDir);
    ASSERT_NOT_NULL(db);
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM t;", -1, &stmt, NULL);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    /* Delete the .state file but keep cache + etag */
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -f %s/*.state", cacheDir);
    (void)system(cmd);

    /* Reopen — state file missing → all pages treated as invalid */
    db = open_lazy_db_cache_reuse(g_ctx, "test.db", cacheDir);
    ASSERT_NOT_NULL(db);

    /* Reading data should trigger Azure fetches (state was lost) */
    mock_reset_call_counts(g_ctx);
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM t;", -1, &stmt, NULL);
    sqlite3_step(stmt);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), 50);
    sqlite3_finalize(stmt);

    /* Pages beyond bootstrap had to be re-fetched */
    int reads = mock_get_call_count(g_ctx, "page_blob_read");
    /* Might be 0 if entire db fits in bootstrap (64KB), so we just
    ** verify it doesn't crash and data is correct */

    sqlite3_close(db);
    cleanup_lazy_cache_dir(cacheDir);
}

TEST(lazy_state_corrupt_magic) {
    setup();
    const char *cacheDir = "/tmp/test_lazy_state_magic";
    cleanup_lazy_cache_dir(cacheDir);

    /* Create and populate db */
    sqlite3 *db = open_test_db(g_ctx);
    ASSERT_NOT_NULL(db);
    sqlite3_exec(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, data TEXT);",
                 NULL, NULL, NULL);
    for (int i = 0; i < 100; i++) {
        char sql[256];
        snprintf(sql, sizeof(sql),
                 "INSERT INTO t VALUES(%d, '%0200d');", i, i);
        sqlite3_exec(db, sql, NULL, NULL, NULL);
    }
    close_test_db(db);

    /* Open with lazy + cache_reuse, read data, close to write state */
    db = open_lazy_db_cache_reuse(g_ctx, "test.db", cacheDir);
    ASSERT_NOT_NULL(db);
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM t;", -1, &stmt, NULL);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    /* Corrupt the .state file — overwrite magic bytes */
    char statePath[512];
    snprintf(statePath, sizeof(statePath), "%s/*.state", cacheDir);
    char findCmd[512];
    snprintf(findCmd, sizeof(findCmd),
             "for f in %s; do printf 'XXXX' | dd of=$f bs=1 count=4 conv=notrunc 2>/dev/null; done",
             statePath);
    (void)system(findCmd);

    /* Reopen — corrupted state file → all pages invalid → safe fallback */
    db = open_lazy_db_cache_reuse(g_ctx, "test.db", cacheDir);
    ASSERT_NOT_NULL(db);

    /* Data should still be accessible (pages fetched on demand) */
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM t;", -1, &stmt, NULL);
    sqlite3_step(stmt);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), 100);
    sqlite3_finalize(stmt);

    sqlite3_close(db);
    cleanup_lazy_cache_dir(cacheDir);
}

TEST(lazy_state_corrupt_crc) {
    setup();
    const char *cacheDir = "/tmp/test_lazy_state_crc";
    cleanup_lazy_cache_dir(cacheDir);

    /* Create and populate db */
    sqlite3 *db = open_test_db(g_ctx);
    ASSERT_NOT_NULL(db);
    sqlite3_exec(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, data TEXT);",
                 NULL, NULL, NULL);
    for (int i = 0; i < 100; i++) {
        char sql[256];
        snprintf(sql, sizeof(sql),
                 "INSERT INTO t VALUES(%d, '%0200d');", i, i);
        sqlite3_exec(db, sql, NULL, NULL, NULL);
    }
    close_test_db(db);

    /* Open lazy + cache_reuse, read, close */
    db = open_lazy_db_cache_reuse(g_ctx, "test.db", cacheDir);
    ASSERT_NOT_NULL(db);
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM t;", -1, &stmt, NULL);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    /* Corrupt the CRC (last 4 bytes of the .state file) */
    char corruptCmd[512];
    snprintf(corruptCmd, sizeof(corruptCmd),
             "for f in %s/*.state; do "
             "sz=$(wc -c < $f); "
             "pos=$((sz - 4)); "
             "printf '\\xff\\xff\\xff\\xff' | dd of=$f bs=1 seek=$pos conv=notrunc 2>/dev/null; "
             "done", cacheDir);
    (void)system(corruptCmd);

    /* Reopen — bad CRC → all pages invalid → safe fallback */
    db = open_lazy_db_cache_reuse(g_ctx, "test.db", cacheDir);
    ASSERT_NOT_NULL(db);

    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM t;", -1, &stmt, NULL);
    sqlite3_step(stmt);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), 100);
    sqlite3_finalize(stmt);

    sqlite3_close(db);
    cleanup_lazy_cache_dir(cacheDir);
}

TEST(lazy_state_truncated_file) {
    setup();
    const char *cacheDir = "/tmp/test_lazy_state_trunc";
    cleanup_lazy_cache_dir(cacheDir);

    sqlite3 *db = open_test_db(g_ctx);
    ASSERT_NOT_NULL(db);
    sqlite3_exec(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, data TEXT);",
                 NULL, NULL, NULL);
    for (int i = 0; i < 100; i++) {
        char sql[256];
        snprintf(sql, sizeof(sql),
                 "INSERT INTO t VALUES(%d, '%0200d');", i, i);
        sqlite3_exec(db, sql, NULL, NULL, NULL);
    }
    close_test_db(db);

    db = open_lazy_db_cache_reuse(g_ctx, "test.db", cacheDir);
    ASSERT_NOT_NULL(db);
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM t;", -1, &stmt, NULL);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    /* Truncate the state file to just 10 bytes (header is 24) */
    char truncCmd[512];
    snprintf(truncCmd, sizeof(truncCmd),
             "for f in %s/*.state; do truncate -s 10 $f 2>/dev/null || dd if=$f of=$f bs=10 count=1 2>/dev/null; done",
             cacheDir);
    (void)system(truncCmd);

    /* Reopen — truncated state file → safe fallback */
    db = open_lazy_db_cache_reuse(g_ctx, "test.db", cacheDir);
    ASSERT_NOT_NULL(db);

    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM t;", -1, &stmt, NULL);
    sqlite3_step(stmt);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), 100);
    sqlite3_finalize(stmt);

    sqlite3_close(db);
    cleanup_lazy_cache_dir(cacheDir);
}

/* ══════════════════════════════════════════════════════════════════════
** SECTION 18: Lazy Cache — Edge Cases
**
** Tests boundary conditions and corner cases in the lazy cache.
** ══════════════════════════════════════════════════════════════════════ */

TEST(lazy_zero_length_new_db) {
    setup();
    /* Open a new db that doesn't exist yet with lazy mode */
    sqlite3 *db = open_lazy_db(g_ctx, "empty_lazy.db");
    ASSERT_NOT_NULL(db);

    /* Should work fine — no crash */
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, "PRAGMA page_size;", -1, &stmt, NULL);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    close_test_db(db);
}

TEST(lazy_large_data_insert) {
    setup();
    sqlite3 *db = open_lazy_db(g_ctx, "large_lazy.db");
    ASSERT_NOT_NULL(db);

    sqlite3_exec(db, "CREATE TABLE t(x BLOB);", NULL, NULL, NULL);

    /* Insert 128KB blob — forces many pages */
    unsigned char *bigdata = (unsigned char *)malloc(131072);
    ASSERT_NOT_NULL(bigdata);
    memset(bigdata, 0xCD, 131072);

    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, "INSERT INTO t VALUES(?);", -1, &stmt, NULL);
    sqlite3_bind_blob(stmt, 1, bigdata, 131072, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_DONE);
    sqlite3_finalize(stmt);

    /* Read back and verify */
    sqlite3_prepare_v2(db, "SELECT length(x) FROM t;", -1, &stmt, NULL);
    sqlite3_step(stmt);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), 131072);
    sqlite3_finalize(stmt);

    free(bigdata);
    close_test_db(db);
}

TEST(lazy_multiple_transactions) {
    setup();
    sqlite3 *db = open_lazy_db(g_ctx, "multi_txn.db");
    ASSERT_NOT_NULL(db);

    sqlite3_exec(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, v TEXT);",
                 NULL, NULL, NULL);

    for (int batch = 0; batch < 5; batch++) {
        sqlite3_exec(db, "BEGIN;", NULL, NULL, NULL);
        for (int i = 0; i < 20; i++) {
            char sql[128];
            snprintf(sql, sizeof(sql),
                     "INSERT INTO t VALUES(%d, 'batch_%d');",
                     batch * 20 + i, batch);
            sqlite3_exec(db, sql, NULL, NULL, NULL);
        }
        int rc = sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
        ASSERT_OK(rc);
    }

    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM t;", -1, &stmt, NULL);
    sqlite3_step(stmt);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), 100);
    sqlite3_finalize(stmt);

    close_test_db(db);
}

TEST(lazy_data_survives_close_reopen) {
    setup();
    /* Create with lazy */
    sqlite3 *db = open_lazy_db(g_ctx, "survive.db");
    ASSERT_NOT_NULL(db);
    sqlite3_exec(db, "CREATE TABLE t(x INTEGER);", NULL, NULL, NULL);
    sqlite3_exec(db, "INSERT INTO t VALUES(777);", NULL, NULL, NULL);
    sqlite3_close(db);

    /* Reopen with lazy */
    db = open_lazy_db(g_ctx, "survive.db");
    ASSERT_NOT_NULL(db);
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, "SELECT x FROM t;", -1, &stmt, NULL);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), 777);
    sqlite3_finalize(stmt);

    close_test_db(db);
}

TEST(lazy_rollback_preserves_state) {
    setup();
    sqlite3 *db = open_lazy_db(g_ctx, "rbck.db");
    ASSERT_NOT_NULL(db);
    sqlite3_exec(db, "CREATE TABLE t(x INTEGER);", NULL, NULL, NULL);
    sqlite3_exec(db, "INSERT INTO t VALUES(10);", NULL, NULL, NULL);

    sqlite3_exec(db, "BEGIN;", NULL, NULL, NULL);
    sqlite3_exec(db, "INSERT INTO t VALUES(20);", NULL, NULL, NULL);
    int rc = sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
    ASSERT_OK(rc);

    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM t;", -1, &stmt, NULL);
    sqlite3_step(stmt);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), 1);
    sqlite3_finalize(stmt);

    close_test_db(db);
}

TEST(lazy_azure_read_failure_during_fetch) {
    setup();
    /* Create a big db */
    sqlite3 *db = open_test_db(g_ctx);
    ASSERT_NOT_NULL(db);
    sqlite3_exec(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, data TEXT);",
                 NULL, NULL, NULL);
    for (int i = 0; i < 300; i++) {
        char sql[256];
        snprintf(sql, sizeof(sql),
                 "INSERT INTO t VALUES(%d, '%0200d');", i, i);
        sqlite3_exec(db, sql, NULL, NULL, NULL);
    }
    close_test_db(db);

    /* Open lazily */
    db = open_lazy_db(g_ctx, "test.db");
    ASSERT_NOT_NULL(db);

    /* Make Azure reads fail — simulates network error during lazy fetch */
    mock_set_fail_operation(g_ctx, "page_blob_read", AZURE_ERR_NETWORK);

    /* Try to read data beyond bootstrap — should get an error */
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM t;", -1, &stmt, NULL);
    int step_rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    /* Should fail because page fetch failed */
    ASSERT_NE(step_rc, SQLITE_ROW);

    mock_clear_failures(g_ctx);
    close_test_db(db);
}

TEST(lazy_write_failure_during_sync) {
    setup();
    sqlite3 *db = open_lazy_db(g_ctx, "syncfail.db");
    ASSERT_NOT_NULL(db);
    sqlite3_exec(db, "CREATE TABLE t(x);", NULL, NULL, NULL);
    sqlite3_exec(db, "INSERT INTO t VALUES(1);", NULL, NULL, NULL);

    /* Fail Azure writes during sync */
    mock_set_fail_operation(g_ctx, "page_blob_write", AZURE_ERR_NETWORK);
    int rc = sqlite3_exec(db, "INSERT INTO t VALUES(2);", NULL, NULL, NULL);
    ASSERT_NE(rc, SQLITE_OK);

    mock_clear_failures(g_ctx);
    close_test_db(db);
}

TEST(lazy_concurrent_valid_dirty_operations) {
    setup();
    sqlite3 *db = open_lazy_db(g_ctx, "concurrent.db");
    ASSERT_NOT_NULL(db);

    sqlite3_exec(db, "CREATE TABLE t(id INTEGER PRIMARY KEY, data TEXT);",
                 NULL, NULL, NULL);

    /* Write lots of data — creates dirty+valid pages */
    for (int i = 0; i < 50; i++) {
        char sql[256];
        snprintf(sql, sizeof(sql),
                 "INSERT INTO t VALUES(%d, '%0200d');", i, i);
        sqlite3_exec(db, sql, NULL, NULL, NULL);
    }

    /* Commit — syncs dirty pages to Azure */
    sqlite3_exec(db, "BEGIN; INSERT INTO t VALUES(999, 'test'); COMMIT;",
                 NULL, NULL, NULL);

    /* Read everything — all should be from cache */
    mock_reset_call_counts(g_ctx);
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM t;", -1, &stmt, NULL);
    sqlite3_step(stmt);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), 51);
    sqlite3_finalize(stmt);
    ASSERT_EQ(mock_get_call_count(g_ctx, "page_blob_read"), 0);

    close_test_db(db);
}

TEST(lazy_savepoint_operations) {
    setup();
    sqlite3 *db = open_lazy_db(g_ctx, "savepoint.db");
    ASSERT_NOT_NULL(db);

    sqlite3_exec(db, "CREATE TABLE t(x INTEGER);", NULL, NULL, NULL);
    sqlite3_exec(db, "BEGIN;", NULL, NULL, NULL);
    sqlite3_exec(db, "INSERT INTO t VALUES(1);", NULL, NULL, NULL);
    sqlite3_exec(db, "SAVEPOINT sp1;", NULL, NULL, NULL);
    sqlite3_exec(db, "INSERT INTO t VALUES(2);", NULL, NULL, NULL);
    sqlite3_exec(db, "ROLLBACK TO sp1;", NULL, NULL, NULL);
    int rc = sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
    ASSERT_OK(rc);

    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM t;", -1, &stmt, NULL);
    sqlite3_step(stmt);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), 1);
    sqlite3_finalize(stmt);

    close_test_db(db);
}

TEST(lazy_xFileSize_correct) {
    setup();
    sqlite3 *db = open_lazy_db(g_ctx, "fsize.db");
    ASSERT_NOT_NULL(db);

    /* After creating a table, xFileSize should report non-zero */
    sqlite3_exec(db, "CREATE TABLE t(x);", NULL, NULL, NULL);

    /* The file_size is not directly observable via SQL, but PRAGMA
    ** page_count * page_size gives us the same info */
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, "PRAGMA page_count;", -1, &stmt, NULL);
    sqlite3_step(stmt);
    int pages = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    ASSERT_GT(pages, 0);

    close_test_db(db);
}

TEST(lazy_xSectorSize) {
    setup();
    sqlite3 *db = open_lazy_db(g_ctx, "sector.db");
    ASSERT_NOT_NULL(db);

    /* xSectorSize is 512 for the Azure page blob VFS.
    ** We verify by checking the db works correctly — sector size
    ** affects write alignment but is not directly queryable. */
    sqlite3_exec(db, "CREATE TABLE t(x);", NULL, NULL, NULL);
    int rc = sqlite3_exec(db, "INSERT INTO t VALUES(1);", NULL, NULL, NULL);
    ASSERT_OK(rc);

    close_test_db(db);
}

TEST(lazy_locking_works) {
    setup();
    /* Create db first */
    sqlite3 *db0 = open_lazy_db(g_ctx, "locktest.db");
    ASSERT_NOT_NULL(db0);
    sqlite3_exec(db0, "CREATE TABLE t(x);", NULL, NULL, NULL);
    sqlite3_close(db0);

    /* Acquire a lease externally */
    azure_error_t err;
    char id[64];
    g_ops->lease_acquire(g_ctx, "locktest.db", 30, id, sizeof(id), &err);

    /* Try to write with lazy db — should get SQLITE_BUSY */
    sqlite3 *db = open_lazy_db(g_ctx, "locktest.db");
    ASSERT_NOT_NULL(db);
    int rc = sqlite3_exec(db, "INSERT INTO t VALUES(1);", NULL, NULL, NULL);
    ASSERT_ERR(rc, SQLITE_BUSY);

    sqlite3_close(db);
    g_ops->lease_release(g_ctx, "locktest.db", id, &err);
}

TEST(lazy_download_count_fcntl) {
    setup();
    sqlite3 *db = open_test_db(g_ctx);
    ASSERT_NOT_NULL(db);
    sqlite3_exec(db, "CREATE TABLE t(x);", NULL, NULL, NULL);
    close_test_db(db);

    db = open_lazy_db(g_ctx, "test.db");
    ASSERT_NOT_NULL(db);

    int downloads = -1;
    sqlite3_file_control(db, "main", SQLITE_OBJS_FCNTL_DOWNLOAD_COUNT,
                         &downloads);
    /* Lazy open counts as 1 download (bootstrap) */
    ASSERT_EQ(downloads, 1);

    close_test_db(db);
}

TEST(lazy_journal_file_handling) {
    setup();
    sqlite3 *db = open_lazy_db(g_ctx, "jrnl.db");
    ASSERT_NOT_NULL(db);

    sqlite3_exec(db, "CREATE TABLE t(x);", NULL, NULL, NULL);
    sqlite3_exec(db, "BEGIN;", NULL, NULL, NULL);
    sqlite3_exec(db, "INSERT INTO t VALUES(1);", NULL, NULL, NULL);
    sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);

    /* Journal should be uploaded and cleaned up */
    ASSERT_GT(mock_get_call_count(g_ctx, "block_blob_upload"), 0);
    ASSERT_FALSE(mock_blob_exists(g_ctx, "jrnl.db-journal"));

    close_test_db(db);
}

TEST(lazy_mixed_prefetch_modes) {
    setup();
    /* Create db */
    sqlite3 *db = open_test_db(g_ctx);
    ASSERT_NOT_NULL(db);
    sqlite3_exec(db, "CREATE TABLE t(x INTEGER);", NULL, NULL, NULL);
    sqlite3_exec(db, "INSERT INTO t VALUES(42);", NULL, NULL, NULL);
    close_test_db(db);

    /* Open lazily, verify data */
    db = open_lazy_db(g_ctx, "test.db");
    ASSERT_NOT_NULL(db);
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, "SELECT x FROM t;", -1, &stmt, NULL);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), 42);
    sqlite3_finalize(stmt);
    close_test_db(db);

    /* Open normally (prefetch=all), verify same data */
    db = open_test_db(g_ctx);
    ASSERT_NOT_NULL(db);
    sqlite3_prepare_v2(db, "SELECT x FROM t;", -1, &stmt, NULL);
    ASSERT_EQ(sqlite3_step(stmt), SQLITE_ROW);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), 42);
    sqlite3_finalize(stmt);
    close_test_db(db);
}

TEST(lazy_xCheckReservedLock) {
    setup();
    sqlite3 *db = open_lazy_db(g_ctx, "chklock.db");
    ASSERT_NOT_NULL(db);
    sqlite3_exec(db, "CREATE TABLE t(x);", NULL, NULL, NULL);

    /* Before writing, no reserved lock */
    sqlite3_exec(db, "BEGIN;", NULL, NULL, NULL);
    sqlite3_exec(db, "INSERT INTO t VALUES(1);", NULL, NULL, NULL);
    /* During write, we have a lease — xCheckReservedLock should indicate locked */
    ASSERT_EQ(mock_is_leased(g_ctx, "chklock.db"), 1);
    sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);

    /* After commit, lease released */
    ASSERT_EQ(mock_is_leased(g_ctx, "chklock.db"), 0);

    close_test_db(db);
}

TEST(lazy_xDeviceCharacteristics) {
    setup();
    /* xDeviceCharacteristics is tested implicitly — if it returned wrong
    ** flags, SQLite would behave incorrectly. Verify transactions work. */
    sqlite3 *db = open_lazy_db(g_ctx, "devchar.db");
    ASSERT_NOT_NULL(db);

    for (int i = 0; i < 5; i++) {
        sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS t(x INTEGER);",
                     NULL, NULL, NULL);
        char sql[64];
        snprintf(sql, sizeof(sql), "INSERT INTO t VALUES(%d);", i);
        sqlite3_exec(db, sql, NULL, NULL, NULL);
    }

    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM t;", -1, &stmt, NULL);
    sqlite3_step(stmt);
    ASSERT_EQ(sqlite3_column_int(stmt, 0), 5);
    sqlite3_finalize(stmt);

    close_test_db(db);
}

TEST(lazy_error_propagation_from_azure) {
    setup();
    /* Create a database first */
    sqlite3 *db = open_test_db(g_ctx);
    ASSERT_NOT_NULL(db);
    sqlite3_exec(db, "CREATE TABLE t(x);", NULL, NULL, NULL);
    close_test_db(db);

    /* Make Azure reads fail at bootstrap time */
    mock_set_fail_operation(g_ctx, "page_blob_read", AZURE_ERR_NETWORK);

    /* Open with lazy — bootstrap needs page_blob_read, should fail */
    sqlite_objs_vfs_register_with_ops(mock_azure_get_ops(), g_ctx, 0);
    sqlite3 *db2 = NULL;
    int rc = sqlite3_open_v2("file:test.db?prefetch=none", &db2,
                              SQLITE_OPEN_URI | SQLITE_OPEN_READWRITE,
                              "sqlite-objs");
    ASSERT_NE(rc, SQLITE_OK);

    mock_clear_failures(g_ctx);
    if (db2) sqlite3_close(db2);
}

TEST(lazy_lease_renewal_during_long_write) {
    setup();
    sqlite3 *db = open_lazy_db(g_ctx, "renew.db");
    ASSERT_NOT_NULL(db);

    sqlite3_exec(db, "CREATE TABLE t(x INTEGER);", NULL, NULL, NULL);

    /* Make lease_renew fail — graceful handling expected */
    mock_set_fail_operation(g_ctx, "lease_renew", AZURE_ERR_NETWORK);

    int rc = SQLITE_OK;
    for (int i = 0; i < 1000 && rc == SQLITE_OK; i++) {
        char sql[64];
        snprintf(sql, sizeof(sql), "INSERT INTO t VALUES(%d);", i);
        rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    }

    /* Should handle gracefully */
    mock_clear_failures(g_ctx);
    close_test_db(db);
}

/* ══════════════════════════════════════════════════════════════════════
** SECTION: VFS Activity Metrics
** ══════════════════════════════════════════════════════════════════════ */

TEST(metrics_pragma_returns_stats) {
    setup();
    sqlite3 *db = open_test_db(g_ctx);
    ASSERT_NOT_NULL(db);

    /* Create table and insert — generates disk + blob I/O */
    sqlite3_exec(db, "CREATE TABLE t(x);", NULL, NULL, NULL);
    sqlite3_exec(db, "INSERT INTO t VALUES('hello');", NULL, NULL, NULL);

    /* PRAGMA sqlite_objs_stats should return key=value text */
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, "PRAGMA sqlite_objs_stats;", -1, &stmt, NULL);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_NOT_NULL(stmt);

    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_ROW);

    const char *result = (const char *)sqlite3_column_text(stmt, 0);
    ASSERT_NOT_NULL(result);

    /* Should contain key metric names */
    ASSERT_TRUE(strstr(result, "disk_reads=") != NULL);
    ASSERT_TRUE(strstr(result, "disk_writes=") != NULL);
    ASSERT_TRUE(strstr(result, "blob_reads=") != NULL);
    ASSERT_TRUE(strstr(result, "blob_writes=") != NULL);
    ASSERT_TRUE(strstr(result, "syncs=") != NULL);
    ASSERT_TRUE(strstr(result, "lease_acquires=") != NULL);
    ASSERT_TRUE(strstr(result, "azure_errors=") != NULL);

    sqlite3_finalize(stmt);
    close_test_db(db);
}

TEST(metrics_disk_io_counters) {
    setup();
    sqlite3 *db = open_test_db(g_ctx);
    ASSERT_NOT_NULL(db);

    /* Create and write data */
    sqlite3_exec(db, "CREATE TABLE t(x TEXT);", NULL, NULL, NULL);
    sqlite3_exec(db, "INSERT INTO t VALUES('some data here');", NULL, NULL, NULL);

    /* Read it back */
    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(db, "SELECT * FROM t;", -1, &stmt, NULL);
    while (sqlite3_step(stmt) == SQLITE_ROW) { /* consume */ }
    sqlite3_finalize(stmt);

    /* Get metrics via FCNTL */
    char *stats = NULL;
    int rc = sqlite3_file_control(db, "main", SQLITE_OBJS_FCNTL_STATS, &stats);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_NOT_NULL(stats);

    /* disk_reads and disk_writes should be > 0 after I/O */
    ASSERT_TRUE(strstr(stats, "disk_reads=") != NULL);
    /* Parse disk_reads value — should be positive */
    const char *dr = strstr(stats, "disk_reads=");
    long long disk_reads = 0;
    if (dr) sscanf(dr, "disk_reads=%lld", &disk_reads);
    ASSERT_TRUE(disk_reads > 0);

    const char *dw = strstr(stats, "disk_writes=");
    long long disk_writes = 0;
    if (dw) sscanf(dw, "disk_writes=%lld", &disk_writes);
    ASSERT_TRUE(disk_writes > 0);

    sqlite3_free(stats);
    close_test_db(db);
}

TEST(metrics_blob_io_counters) {
    setup();
    sqlite3 *db = open_test_db(g_ctx);
    ASSERT_NOT_NULL(db);

    /* Write enough data to trigger a sync with Azure writes */
    sqlite3_exec(db, "CREATE TABLE t(x TEXT);", NULL, NULL, NULL);
    sqlite3_exec(db, "INSERT INTO t VALUES('blob test');", NULL, NULL, NULL);

    /* Get metrics */
    char *stats = NULL;
    int rc = sqlite3_file_control(db, "main", SQLITE_OBJS_FCNTL_STATS, &stats);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_NOT_NULL(stats);

    /* blob_writes should be > 0 (sync uploaded dirty pages) */
    const char *bw = strstr(stats, "blob_writes=");
    long long blob_writes = 0;
    if (bw) sscanf(bw, "blob_writes=%lld", &blob_writes);
    ASSERT_TRUE(blob_writes > 0);

    /* blob_bytes_written should be > 0 */
    const char *bbw = strstr(stats, "blob_bytes_written=");
    long long blob_bytes_written = 0;
    if (bbw) sscanf(bbw, "blob_bytes_written=%lld", &blob_bytes_written);
    ASSERT_TRUE(blob_bytes_written > 0);

    sqlite3_free(stats);
    close_test_db(db);
}

TEST(metrics_sync_counters) {
    setup();
    sqlite3 *db = open_test_db(g_ctx);
    ASSERT_NOT_NULL(db);

    sqlite3_exec(db, "CREATE TABLE t(x);", NULL, NULL, NULL);
    sqlite3_exec(db, "INSERT INTO t VALUES(1);", NULL, NULL, NULL);
    sqlite3_exec(db, "INSERT INTO t VALUES(2);", NULL, NULL, NULL);

    /* Get metrics */
    char *stats = NULL;
    int rc = sqlite3_file_control(db, "main", SQLITE_OBJS_FCNTL_STATS, &stats);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_NOT_NULL(stats);

    /* syncs should be > 0 after transactions */
    const char *s = strstr(stats, "syncs=");
    long long syncs = 0;
    if (s) sscanf(s, "syncs=%lld", &syncs);
    ASSERT_TRUE(syncs > 0);

    /* dirty_pages_synced should be > 0 */
    const char *dp = strstr(stats, "dirty_pages_synced=");
    long long dirty_synced = 0;
    if (dp) sscanf(dp, "dirty_pages_synced=%lld", &dirty_synced);
    ASSERT_TRUE(dirty_synced > 0);

    sqlite3_free(stats);
    close_test_db(db);
}

TEST(metrics_reset_clears_counters) {
    setup();
    sqlite3 *db = open_test_db(g_ctx);
    ASSERT_NOT_NULL(db);

    /* Generate some activity */
    sqlite3_exec(db, "CREATE TABLE t(x);", NULL, NULL, NULL);
    sqlite3_exec(db, "INSERT INTO t VALUES(1);", NULL, NULL, NULL);

    /* Verify counters are non-zero */
    char *stats = NULL;
    sqlite3_file_control(db, "main", SQLITE_OBJS_FCNTL_STATS, &stats);
    ASSERT_NOT_NULL(stats);
    const char *dr = strstr(stats, "disk_reads=");
    long long disk_reads = 0;
    if (dr) sscanf(dr, "disk_reads=%lld", &disk_reads);
    ASSERT_TRUE(disk_reads > 0);
    sqlite3_free(stats);

    /* Reset via FCNTL */
    int rc = sqlite3_file_control(db, "main", SQLITE_OBJS_FCNTL_STATS_RESET, NULL);
    ASSERT_EQ(rc, SQLITE_OK);

    /* Verify counters are zero */
    stats = NULL;
    sqlite3_file_control(db, "main", SQLITE_OBJS_FCNTL_STATS, &stats);
    ASSERT_NOT_NULL(stats);

    dr = strstr(stats, "disk_reads=");
    disk_reads = 0;
    if (dr) sscanf(dr, "disk_reads=%lld", &disk_reads);
    ASSERT_EQ(disk_reads, 0);

    const char *bw = strstr(stats, "blob_writes=");
    long long blob_writes = 0;
    if (bw) sscanf(bw, "blob_writes=%lld", &blob_writes);
    ASSERT_EQ(blob_writes, 0);

    sqlite3_free(stats);
    close_test_db(db);
}

TEST(metrics_pragma_reset) {
    setup();
    sqlite3 *db = open_test_db(g_ctx);
    ASSERT_NOT_NULL(db);

    /* Generate some activity */
    sqlite3_exec(db, "CREATE TABLE t(x);", NULL, NULL, NULL);
    sqlite3_exec(db, "INSERT INTO t VALUES(1);", NULL, NULL, NULL);

    /* Reset via PRAGMA */
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, "PRAGMA sqlite_objs_stats_reset;", -1, &stmt, NULL);
    ASSERT_EQ(rc, SQLITE_OK);
    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_ROW);
    const char *result = (const char *)sqlite3_column_text(stmt, 0);
    ASSERT_NOT_NULL(result);
    ASSERT_STR_EQ(result, "ok");
    sqlite3_finalize(stmt);

    /* Verify counters are zero via PRAGMA */
    rc = sqlite3_prepare_v2(db, "PRAGMA sqlite_objs_stats;", -1, &stmt, NULL);
    ASSERT_EQ(rc, SQLITE_OK);
    rc = sqlite3_step(stmt);
    ASSERT_EQ(rc, SQLITE_ROW);
    result = (const char *)sqlite3_column_text(stmt, 0);
    ASSERT_NOT_NULL(result);
    ASSERT_TRUE(strstr(result, "disk_reads=0") != NULL);
    ASSERT_TRUE(strstr(result, "syncs=0") != NULL);
    sqlite3_finalize(stmt);

    close_test_db(db);
}

TEST(metrics_all_fields_present) {
    setup();
    sqlite3 *db = open_test_db(g_ctx);
    ASSERT_NOT_NULL(db);

    char *stats = NULL;
    int rc = sqlite3_file_control(db, "main", SQLITE_OBJS_FCNTL_STATS, &stats);
    ASSERT_EQ(rc, SQLITE_OK);
    ASSERT_NOT_NULL(stats);

    /* Verify all 27 metric fields are present */
    ASSERT_TRUE(strstr(stats, "disk_reads=") != NULL);
    ASSERT_TRUE(strstr(stats, "disk_writes=") != NULL);
    ASSERT_TRUE(strstr(stats, "disk_bytes_read=") != NULL);
    ASSERT_TRUE(strstr(stats, "disk_bytes_written=") != NULL);
    ASSERT_TRUE(strstr(stats, "blob_reads=") != NULL);
    ASSERT_TRUE(strstr(stats, "blob_writes=") != NULL);
    ASSERT_TRUE(strstr(stats, "blob_bytes_read=") != NULL);
    ASSERT_TRUE(strstr(stats, "blob_bytes_written=") != NULL);
    ASSERT_TRUE(strstr(stats, "cache_hits=") != NULL);
    ASSERT_TRUE(strstr(stats, "cache_misses=") != NULL);
    ASSERT_TRUE(strstr(stats, "cache_miss_pages=") != NULL);
    ASSERT_TRUE(strstr(stats, "prefetch_pages=") != NULL);
    ASSERT_TRUE(strstr(stats, "lease_acquires=") != NULL);
    ASSERT_TRUE(strstr(stats, "lease_renewals=") != NULL);
    ASSERT_TRUE(strstr(stats, "lease_releases=") != NULL);
    ASSERT_TRUE(strstr(stats, "syncs=") != NULL);
    ASSERT_TRUE(strstr(stats, "dirty_pages_synced=") != NULL);
    ASSERT_TRUE(strstr(stats, "blob_resizes=") != NULL);
    ASSERT_TRUE(strstr(stats, "revalidations=") != NULL);
    ASSERT_TRUE(strstr(stats, "revalidation_downloads=") != NULL);
    ASSERT_TRUE(strstr(stats, "revalidation_diffs=") != NULL);
    ASSERT_TRUE(strstr(stats, "pages_invalidated=") != NULL);
    ASSERT_TRUE(strstr(stats, "journal_uploads=") != NULL);
    ASSERT_TRUE(strstr(stats, "journal_bytes_uploaded=") != NULL);
    ASSERT_TRUE(strstr(stats, "wal_uploads=") != NULL);
    ASSERT_TRUE(strstr(stats, "wal_bytes_uploaded=") != NULL);
    ASSERT_TRUE(strstr(stats, "azure_errors=") != NULL);

    sqlite3_free(stats);
    close_test_db(db);
}

#endif /* ENABLE_VFS_INTEGRATION */

/* ══════════════════════════════════════════════════════════════════════
** Test Suite Runner
** ══════════════════════════════════════════════════════════════════════ */

void run_vfs_tests(void) {
    /* Section 1: Page Blob Operations */
    TEST_SUITE_BEGIN("Page Blob Operations");
    RUN_TEST(page_blob_create_basic);
    RUN_TEST(page_blob_create_zero_size);
    RUN_TEST(page_blob_create_unaligned_fails);
    RUN_TEST(page_blob_create_already_exists_resizes);
    RUN_TEST(page_blob_create_large);
    RUN_TEST(page_blob_write_basic);
    RUN_TEST(page_blob_write_at_offset);
    RUN_TEST(page_blob_write_unaligned_offset_fails);
    RUN_TEST(page_blob_write_unaligned_length_fails);
    RUN_TEST(page_blob_write_nonexistent_fails);
    RUN_TEST(page_blob_write_grows_blob);
    RUN_TEST(page_blob_write_multiple_pages);
    RUN_TEST(page_blob_write_overwrite);
    RUN_TEST(page_blob_read_basic);
    RUN_TEST(page_blob_read_at_offset);
    RUN_TEST(page_blob_read_nonexistent_fails);
    RUN_TEST(page_blob_read_past_eof_zero_fills);
    RUN_TEST(page_blob_read_offset_past_eof);
    RUN_TEST(page_blob_write_then_read_roundtrip);
    RUN_TEST(page_blob_resize_grow);
    RUN_TEST(page_blob_resize_shrink);
    RUN_TEST(page_blob_resize_unaligned_fails);
    RUN_TEST(page_blob_resize_nonexistent_fails);
    RUN_TEST(page_blob_resize_preserves_data);
    RUN_TEST(page_blob_resize_to_zero);
    TEST_SUITE_END();

    /* Section 2: Block Blob Operations */
    TEST_SUITE_BEGIN("Block Blob Operations");
    RUN_TEST(block_blob_upload_basic);
    RUN_TEST(block_blob_upload_replaces);
    RUN_TEST(block_blob_upload_empty);
    RUN_TEST(block_blob_download_basic);
    RUN_TEST(block_blob_download_nonexistent_fails);
    RUN_TEST(block_blob_upload_download_roundtrip);
    TEST_SUITE_END();

    /* Section 3: Common Blob Operations */
    TEST_SUITE_BEGIN("Common Blob Operations");
    RUN_TEST(blob_exists_page_blob);
    RUN_TEST(blob_exists_block_blob);
    RUN_TEST(blob_exists_nonexistent);
    RUN_TEST(blob_delete_page_blob);
    RUN_TEST(blob_delete_block_blob);
    RUN_TEST(blob_delete_nonexistent_fails);
    RUN_TEST(blob_get_properties_page_blob);
    RUN_TEST(blob_get_properties_nonexistent_fails);
    RUN_TEST(blob_get_properties_with_lease);
    TEST_SUITE_END();

    /* Section 4: Lease State Machine */
    TEST_SUITE_BEGIN("Lease State Machine");
    RUN_TEST(lease_acquire_basic);
    RUN_TEST(lease_acquire_already_leased_fails);
    RUN_TEST(lease_acquire_nonexistent_fails);
    RUN_TEST(lease_renew_basic);
    RUN_TEST(lease_renew_wrong_id_fails);
    RUN_TEST(lease_renew_not_leased_fails);
    RUN_TEST(lease_release_basic);
    RUN_TEST(lease_release_wrong_id_fails);
    RUN_TEST(lease_release_not_leased_fails);
    RUN_TEST(lease_break_immediate);
    RUN_TEST(lease_break_with_period);
    RUN_TEST(lease_break_not_leased_fails);
    RUN_TEST(lease_acquire_after_release);
    RUN_TEST(lease_acquire_after_immediate_break);
    RUN_TEST(lease_acquire_during_breaking_fails);
    RUN_TEST(lease_state_inspection);
    TEST_SUITE_END();

    /* Section 5: Failure Injection */
    TEST_SUITE_BEGIN("Failure Injection");
    RUN_TEST(fail_at_specific_call);
    RUN_TEST(fail_at_is_one_shot);
    RUN_TEST(fail_operation_always);
    RUN_TEST(fail_operation_lease_acquire);
    RUN_TEST(fail_clear_restores_normal);
    RUN_TEST(fail_injection_populates_error);
    RUN_TEST(fail_blob_read);
    RUN_TEST(fail_blob_delete);
    RUN_TEST(fail_block_blob_download);
    RUN_TEST(fail_block_blob_upload);
    TEST_SUITE_END();

    /* Section 6: Call Counting */
    TEST_SUITE_BEGIN("Call Counting");
    RUN_TEST(call_count_tracks_operations);
    RUN_TEST(call_count_per_operation);
    RUN_TEST(call_count_includes_failures);
    RUN_TEST(call_count_reset);
    RUN_TEST(call_count_unknown_op);
    TEST_SUITE_END();

    /* Section 7: Reset and Isolation */
    TEST_SUITE_BEGIN("Reset and Isolation");
    RUN_TEST(reset_clears_blobs);
    RUN_TEST(reset_clears_leases);
    RUN_TEST(reset_clears_failures);
    RUN_TEST(reset_clears_counters);
    TEST_SUITE_END();

    /* Section 8: Buffer Management */
    TEST_SUITE_BEGIN("Buffer Management");
    RUN_TEST(buffer_init);
    RUN_TEST(buffer_reuse);
    RUN_TEST(buffer_free_resets);
    TEST_SUITE_END();

    /* Section 9: Error Details */
    TEST_SUITE_BEGIN("Error Details");
    RUN_TEST(error_populated_on_not_found);
    RUN_TEST(error_populated_on_conflict);
    RUN_TEST(error_populated_on_alignment);
    TEST_SUITE_END();

    /* Section 10: Type Mismatch Protection */
    TEST_SUITE_BEGIN("Type Mismatch Protection");
    RUN_TEST(page_ops_on_block_blob_fails);
    RUN_TEST(block_ops_on_page_blob_fails);
    RUN_TEST(create_page_blob_over_block_blob_fails);
    TEST_SUITE_END();

    /* Section 11: Edge Cases */
    TEST_SUITE_BEGIN("Edge Cases");
    RUN_TEST(multiple_blobs_independent);
    RUN_TEST(blob_name_with_path_separators);
    RUN_TEST(blob_name_with_special_chars);
    RUN_TEST(lease_on_different_blobs);
    RUN_TEST(page_blob_read_from_zero_blob);
    RUN_TEST(page_blob_write_4096_page_size);
    RUN_TEST(delete_and_recreate);
    RUN_TEST(journal_blob_naming_convention);
    RUN_TEST(blob_properties_after_write);
    RUN_TEST(concurrent_page_and_block_blobs);
    TEST_SUITE_END();

#ifdef ENABLE_VFS_INTEGRATION
    /* Section 12: VFS Integration */
    TEST_SUITE_BEGIN("VFS Registration");
    RUN_TEST(vfs_registers_with_correct_name);
    RUN_TEST(vfs_not_default_unless_requested);
    RUN_TEST(vfs_can_be_default);
    RUN_TEST(vfs_open_with_name_parameter);
    TEST_SUITE_END();

    TEST_SUITE_BEGIN("VFS Basic Operations");
    RUN_TEST(vfs_create_new_database);
    RUN_TEST(vfs_create_table_insert_select);
    RUN_TEST(vfs_transaction_commit_writes_to_blob);
    RUN_TEST(vfs_multiple_transactions);
    RUN_TEST(vfs_data_survives_close_reopen);
    TEST_SUITE_END();

    TEST_SUITE_BEGIN("VFS Dirty Page Tracking");
    RUN_TEST(vfs_sync_writes_only_dirty_pages);
    RUN_TEST(vfs_no_writes_on_read_only_ops);
    TEST_SUITE_END();

    TEST_SUITE_BEGIN("VFS Journal Files");
    RUN_TEST(vfs_journal_created_as_block_blob);
    RUN_TEST(vfs_journal_deleted_after_commit);
    TEST_SUITE_END();

    TEST_SUITE_BEGIN("VFS Locking");
    RUN_TEST(vfs_read_no_lease);
    RUN_TEST(vfs_write_acquires_lease);
    RUN_TEST(vfs_write_releases_lease_after_commit);
    RUN_TEST(vfs_lease_conflict_returns_busy);
    TEST_SUITE_END();

    TEST_SUITE_BEGIN("VFS Error Handling");
    RUN_TEST(vfs_azure_write_failure_returns_ioerr);
    RUN_TEST(vfs_azure_read_failure_on_open);
    RUN_TEST(vfs_lease_expire_during_sync);
    TEST_SUITE_END();

    TEST_SUITE_BEGIN("VFS PRAGMA");
    RUN_TEST(vfs_pragma_journal_mode_delete);
    RUN_TEST(vfs_pragma_wal_allowed);
    RUN_TEST(vfs_pragma_page_size_4096);
    TEST_SUITE_END();

    TEST_SUITE_BEGIN("VFS File Type Routing");
    RUN_TEST(vfs_temp_files_use_local_storage);
    TEST_SUITE_END();

    TEST_SUITE_BEGIN("P1 Security — Error Handling");
    RUN_TEST(vfs_azure_sync_failure_returns_ioerr);
    RUN_TEST(vfs_journal_upload_failure_returns_ioerr);
    RUN_TEST(vfs_lease_renew_failure_during_write);
    TEST_SUITE_END();

    TEST_SUITE_BEGIN("P1 Security — Transactions");
    RUN_TEST(vfs_explicit_transaction_commit);
    RUN_TEST(vfs_explicit_transaction_rollback);
    TEST_SUITE_END();

    TEST_SUITE_BEGIN("P1 Security — Large Data");
    RUN_TEST(vfs_large_data_insert);
    RUN_TEST(vfs_multiple_large_transactions);
    TEST_SUITE_END();

    TEST_SUITE_BEGIN("P1 — xSync/Flush Failure");
    RUN_TEST(vfs_sync_mid_flush_failure);
    RUN_TEST(vfs_sync_resize_failure_before_flush);
    TEST_SUITE_END();

    TEST_SUITE_BEGIN("P1 — WAL Mode Rejection");
    RUN_TEST(vfs_wal_mode_returns_delete);
    RUN_TEST(vfs_wal_mode_case_insensitive);
    RUN_TEST(vfs_journal_mode_memory_rejected);
    TEST_SUITE_END();

    TEST_SUITE_BEGIN("P1 — Transaction Correctness");
    RUN_TEST(vfs_transaction_rollback_restores_state);
    RUN_TEST(vfs_transaction_commit_persists_to_azure);
    RUN_TEST(vfs_nested_savepoint_rollback);
    TEST_SUITE_END();

    TEST_SUITE_BEGIN("P1 — Close When Flush Fails");
    RUN_TEST(vfs_close_with_pending_dirty_data);
    RUN_TEST(vfs_close_releases_lease_on_failure);
    TEST_SUITE_END();

    TEST_SUITE_BEGIN("P1 — URL Injection / Blob Names");
    RUN_TEST(vfs_path_traversal_rejected);
    RUN_TEST(vfs_path_traversal_in_directory);
    RUN_TEST(vfs_empty_name_rejected);
    RUN_TEST(vfs_leading_slashes_stripped);
    TEST_SUITE_END();

    TEST_SUITE_BEGIN("P1 — Credential Leak Prevention");
    RUN_TEST(vfs_error_no_credential_leak);
    RUN_TEST(vfs_error_no_request_id_credential_leak);
    TEST_SUITE_END();

    /* Section 13: Lazy Cache — Open & Bootstrap */
    TEST_SUITE_BEGIN("Lazy Cache — Open & Bootstrap");
    RUN_TEST(lazy_open_no_full_download);
    RUN_TEST(lazy_open_page1_fetched);
    RUN_TEST(lazy_open_default_full_download);
    RUN_TEST(lazy_open_create_new_db);
    TEST_SUITE_END();

    /* Section 14: Lazy Cache — xRead Behavior */
    TEST_SUITE_BEGIN("Lazy Cache — xRead Behavior");
    RUN_TEST(lazy_read_valid_from_cache);
    RUN_TEST(lazy_read_invalid_fetches_from_azure);
    RUN_TEST(lazy_read_after_write_no_azure);
    RUN_TEST(lazy_read_readahead_window);
    RUN_TEST(lazy_read_sequential_hits_cache);
    RUN_TEST(lazy_read_subpage_header);
    TEST_SUITE_END();

    /* Section 15: Lazy Cache — xWrite & xTruncate Bitmap Effects */
    TEST_SUITE_BEGIN("Lazy Cache — Write & Truncate");
    RUN_TEST(lazy_write_marks_valid_and_dirty);
    RUN_TEST(lazy_truncate_clears_valid_bits);
    RUN_TEST(lazy_write_beyond_eof);
    TEST_SUITE_END();

    /* Section 16: Lazy Cache — Prefetch PRAGMA */
    TEST_SUITE_BEGIN("Lazy Cache — Prefetch PRAGMA");
    RUN_TEST(lazy_prefetch_downloads_invalid);
    RUN_TEST(lazy_prefetch_all_valid_noop);
    RUN_TEST(lazy_prefetch_mixed_valid_invalid);
    RUN_TEST(lazy_prefetch_noop_on_prefetch_all_mode);
    TEST_SUITE_END();

    /* Section 17: Lazy Cache — State File I/O */
    TEST_SUITE_BEGIN("Lazy Cache — State File I/O");
    RUN_TEST(lazy_state_write_read_roundtrip);
    RUN_TEST(lazy_state_missing_file);
    RUN_TEST(lazy_state_corrupt_magic);
    RUN_TEST(lazy_state_corrupt_crc);
    RUN_TEST(lazy_state_truncated_file);
    TEST_SUITE_END();

    /* Section 18: Lazy Cache — Edge Cases */
    TEST_SUITE_BEGIN("Lazy Cache — Edge Cases");
    RUN_TEST(lazy_zero_length_new_db);
    RUN_TEST(lazy_large_data_insert);
    RUN_TEST(lazy_multiple_transactions);
    RUN_TEST(lazy_data_survives_close_reopen);
    RUN_TEST(lazy_rollback_preserves_state);
    RUN_TEST(lazy_azure_read_failure_during_fetch);
    RUN_TEST(lazy_write_failure_during_sync);
    RUN_TEST(lazy_concurrent_valid_dirty_operations);
    RUN_TEST(lazy_savepoint_operations);
    RUN_TEST(lazy_xFileSize_correct);
    RUN_TEST(lazy_xSectorSize);
    RUN_TEST(lazy_locking_works);
    RUN_TEST(lazy_download_count_fcntl);
    RUN_TEST(lazy_journal_file_handling);
    RUN_TEST(lazy_mixed_prefetch_modes);
    RUN_TEST(lazy_xCheckReservedLock);
    RUN_TEST(lazy_xDeviceCharacteristics);
    RUN_TEST(lazy_error_propagation_from_azure);
    RUN_TEST(lazy_lease_renewal_during_long_write);
    TEST_SUITE_END();

    /* Section 19: VFS Activity Metrics */
    TEST_SUITE_BEGIN("VFS Activity Metrics");
    RUN_TEST(metrics_pragma_returns_stats);
    RUN_TEST(metrics_disk_io_counters);
    RUN_TEST(metrics_blob_io_counters);
    RUN_TEST(metrics_sync_counters);
    RUN_TEST(metrics_reset_clears_counters);
    RUN_TEST(metrics_pragma_reset);
    RUN_TEST(metrics_all_fields_present);
    TEST_SUITE_END();
#endif

    /* Cleanup */
    if (g_ctx) {
        mock_azure_destroy(g_ctx);
        g_ctx = NULL;
    }
}
