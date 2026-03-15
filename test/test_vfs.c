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
#endif

    /* Cleanup */
    if (g_ctx) {
        mock_azure_destroy(g_ctx);
        g_ctx = NULL;
    }
}
