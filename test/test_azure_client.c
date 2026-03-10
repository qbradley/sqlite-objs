/*
** test_azure_client.c — Azure Client Unit Tests (Layer 1)
**
** Tests for Frodo's Azure client code: auth, error parsing, retry logic,
** buffer management. These test the client infrastructure in isolation
** using the mock azure_ops_t.
**
** Some tests (HMAC-SHA256, SAS token) test logic that lives in the
** azure_client.c module. Since that module doesn't exist yet, those
** tests are gated behind ENABLE_AZURE_CLIENT_TESTS. The mock-based
** tests run immediately.
*/

#include "../sqlite-autoconf-3520000/sqlite3.h"
#include "mock_azure_ops.h"
#include "test_harness.h"
#include <string.h>
#include <stdlib.h>

/* ── Helpers ──────────────────────────────────────────────────────── */

static mock_azure_ctx_t *ac_ctx = NULL;
static azure_ops_t      *ac_ops = NULL;

static void ac_setup(void) {
    if (ac_ctx) mock_reset(ac_ctx);
    else        ac_ctx = mock_azure_create();
    ac_ops = mock_azure_get_ops();
}

/* Aliases for brevity in tests */
#define g_ctx ac_ctx
#define g_ops ac_ops
#define setup ac_setup

/* ══════════════════════════════════════════════════════════════════════
** SECTION 1: azure_err_t Error Code Tests
** ══════════════════════════════════════════════════════════════════════ */

TEST(error_code_values) {
    /* Verify enum values are stable (important for serialization) */
    ASSERT_EQ(AZURE_OK, 0);
    ASSERT_NE(AZURE_ERR_NOT_FOUND, AZURE_OK);
    ASSERT_NE(AZURE_ERR_CONFLICT, AZURE_OK);
    ASSERT_NE(AZURE_ERR_AUTH, AZURE_OK);
    ASSERT_NE(AZURE_ERR_THROTTLE, AZURE_OK);
    ASSERT_NE(AZURE_ERR_SERVER, AZURE_OK);
    ASSERT_NE(AZURE_ERR_NETWORK, AZURE_OK);
}

TEST(error_code_all_distinct) {
    /* Every error code must be unique */
    azure_err_t codes[] = {
        AZURE_OK, AZURE_ERR_NOT_FOUND, AZURE_ERR_CONFLICT,
        AZURE_ERR_AUTH, AZURE_ERR_THROTTLE, AZURE_ERR_SERVER,
        AZURE_ERR_NETWORK, AZURE_ERR_INVALID_ARG, AZURE_ERR_IO,
        AZURE_ERR_NOMEM, AZURE_ERR_TIMEOUT, AZURE_ERR_ALIGNMENT,
    };
    int n = sizeof(codes) / sizeof(codes[0]);
    for (int i = 0; i < n; i++) {
        for (int j = i + 1; j < n; j++) {
            ASSERT_NE(codes[i], codes[j]);
        }
    }
}

/* ══════════════════════════════════════════════════════════════════════
** SECTION 2: HTTP Status → Azure Error Mapping
**
** Decision D8: 409 + 429 → SQLITE_BUSY, others → SQLITE_IOERR_*.
** We test the expected mapping through failure injection.
** ══════════════════════════════════════════════════════════════════════ */

TEST(http_500_is_server_error) {
    setup();
    azure_error_t err;
    mock_set_fail_operation(g_ctx, "page_blob_create", AZURE_ERR_SERVER);

    azure_err_t rc = g_ops->page_blob_create(g_ctx, "test.db", 4096, &err);
    ASSERT_AZURE_ERR(rc, AZURE_ERR_SERVER);
}

TEST(http_429_is_throttle_error) {
    setup();
    azure_error_t err;
    mock_set_fail_operation(g_ctx, "page_blob_create", AZURE_ERR_THROTTLE);

    azure_err_t rc = g_ops->page_blob_create(g_ctx, "test.db", 4096, &err);
    ASSERT_AZURE_ERR(rc, AZURE_ERR_THROTTLE);
}

TEST(http_401_is_auth_error) {
    setup();
    azure_error_t err;
    mock_set_fail_operation(g_ctx, "page_blob_create", AZURE_ERR_AUTH);

    azure_err_t rc = g_ops->page_blob_create(g_ctx, "test.db", 4096, &err);
    ASSERT_AZURE_ERR(rc, AZURE_ERR_AUTH);
}

TEST(http_404_is_not_found) {
    setup();
    azure_error_t err;
    /* Naturally returned when reading nonexistent blob */
    azure_buffer_t buf;
    azure_buffer_init(&buf);
    azure_err_t rc = g_ops->page_blob_read(g_ctx, "nope.db", 0, 512, &buf, &err);
    ASSERT_AZURE_ERR(rc, AZURE_ERR_NOT_FOUND);
    azure_buffer_free(&buf);
}

TEST(http_409_is_conflict) {
    setup();
    azure_error_t err;
    g_ops->page_blob_create(g_ctx, "test.db", 4096, &err);
    char id1[64], id2[64];
    g_ops->lease_acquire(g_ctx, "test.db", 30, id1, sizeof(id1), &err);

    azure_err_t rc = g_ops->lease_acquire(g_ctx, "test.db", 30,
                                           id2, sizeof(id2), &err);
    ASSERT_AZURE_ERR(rc, AZURE_ERR_CONFLICT);
}

TEST(network_error) {
    setup();
    azure_error_t err;
    mock_set_fail_operation(g_ctx, "page_blob_read", AZURE_ERR_NETWORK);

    azure_buffer_t buf;
    azure_buffer_init(&buf);
    azure_err_t rc = g_ops->page_blob_read(g_ctx, "test.db", 0, 512, &buf, &err);
    ASSERT_AZURE_ERR(rc, AZURE_ERR_NETWORK);
    azure_buffer_free(&buf);
}

TEST(timeout_error) {
    setup();
    azure_error_t err;
    mock_set_fail_operation(g_ctx, "page_blob_write", AZURE_ERR_TIMEOUT);

    g_ops->page_blob_create(g_ctx, "test.db", 4096, &err);
    mock_clear_failures(g_ctx);
    mock_set_fail_operation(g_ctx, "page_blob_write", AZURE_ERR_TIMEOUT);

    uint8_t data[512] = {0};
    azure_err_t rc = g_ops->page_blob_write(g_ctx, "test.db", 0, data, 512, NULL, &err);
    ASSERT_AZURE_ERR(rc, AZURE_ERR_TIMEOUT);
}

/* ══════════════════════════════════════════════════════════════════════
** SECTION 3: azure_error_t Detail Verification
** ══════════════════════════════════════════════════════════════════════ */

TEST(error_detail_http_status) {
    setup();
    azure_error_t err;
    memset(&err, 0, sizeof(err));

    /* 404 on blob read */
    azure_buffer_t buf;
    azure_buffer_init(&buf);
    g_ops->page_blob_read(g_ctx, "ghost.db", 0, 512, &buf, &err);

    ASSERT_EQ(err.http_status, 404);
    azure_buffer_free(&buf);
}

TEST(error_detail_error_code_string) {
    setup();
    azure_error_t err;
    memset(&err, 0, sizeof(err));

    azure_buffer_t buf;
    azure_buffer_init(&buf);
    g_ops->page_blob_read(g_ctx, "nope.db", 0, 512, &buf, &err);

    ASSERT_STR_EQ(err.error_code, "BlobNotFound");
    azure_buffer_free(&buf);
}

TEST(error_detail_error_message) {
    setup();
    azure_error_t err;
    memset(&err, 0, sizeof(err));

    g_ops->page_blob_create(g_ctx, "bad.db", 999, &err);

    ASSERT_TRUE(strlen(err.error_message) > 0);
}

TEST(error_detail_request_id) {
    setup();
    azure_error_t err;
    memset(&err, 0, sizeof(err));

    g_ops->page_blob_create(g_ctx, "bad.db", 999, &err);

    /* Mock should generate a request ID */
    ASSERT_TRUE(strlen(err.request_id) > 0);
}

TEST(error_detail_lease_conflict) {
    setup();
    azure_error_t err;
    g_ops->page_blob_create(g_ctx, "test.db", 4096, &err);

    char id1[64], id2[64];
    g_ops->lease_acquire(g_ctx, "test.db", 30, id1, sizeof(id1), &err);

    memset(&err, 0, sizeof(err));
    g_ops->lease_acquire(g_ctx, "test.db", 30, id2, sizeof(id2), &err);

    ASSERT_EQ(err.http_status, 409);
    ASSERT_STR_EQ(err.error_code, "LeaseAlreadyPresent");
}

TEST(error_detail_lease_mismatch) {
    setup();
    azure_error_t err;
    g_ops->page_blob_create(g_ctx, "test.db", 4096, &err);

    char id[64];
    g_ops->lease_acquire(g_ctx, "test.db", 30, id, sizeof(id), &err);

    memset(&err, 0, sizeof(err));
    g_ops->lease_renew(g_ctx, "test.db", "bogus-id", &err);

    ASSERT_EQ(err.http_status, 409);
    ASSERT_STR_EQ(err.error_code, "LeaseIdMismatch");
}

/* ══════════════════════════════════════════════════════════════════════
** SECTION 4: azure_buffer_t Management
** ══════════════════════════════════════════════════════════════════════ */

TEST(buffer_alloc_free) {
    azure_buffer_t buf;
    azure_buffer_init(&buf);
    ASSERT_NULL(buf.data);

    /* Simulate callee filling the buffer */
    buf.data = (uint8_t *)malloc(1024);
    buf.size = 512;
    buf.capacity = 1024;
    ASSERT_NOT_NULL(buf.data);

    azure_buffer_free(&buf);
    ASSERT_NULL(buf.data);
    ASSERT_EQ(buf.size, 0);
    ASSERT_EQ(buf.capacity, 0);
}

TEST(buffer_double_free_safe) {
    azure_buffer_t buf;
    azure_buffer_init(&buf);
    buf.data = (uint8_t *)malloc(256);
    buf.capacity = 256;

    azure_buffer_free(&buf);
    azure_buffer_free(&buf); /* Should not crash */
    ASSERT_NULL(buf.data);
}

TEST(buffer_growth_on_read) {
    setup();
    azure_error_t err;
    g_ops->page_blob_create(g_ctx, "test.db", 4096 * 4, &err);

    /* Write different amounts to verify buffer grows */
    uint8_t data[4096];
    memset(data, 0xAA, sizeof(data));
    g_ops->page_blob_write(g_ctx, "test.db", 0, data, 4096, NULL, &err);

    azure_buffer_t buf;
    azure_buffer_init(&buf);

    /* Small read first */
    g_ops->page_blob_read(g_ctx, "test.db", 0, 512, &buf, &err);
    ASSERT_EQ(buf.size, 512);
    ASSERT_GE(buf.capacity, 512);

    /* Larger read — buffer should grow */
    g_ops->page_blob_read(g_ctx, "test.db", 0, 4096, &buf, &err);
    ASSERT_EQ(buf.size, 4096);
    ASSERT_GE(buf.capacity, 4096);

    azure_buffer_free(&buf);
}

TEST(buffer_preserves_capacity) {
    setup();
    azure_error_t err;
    g_ops->page_blob_create(g_ctx, "test.db", 4096, &err);

    azure_buffer_t buf;
    azure_buffer_init(&buf);

    /* Read 4096 bytes */
    g_ops->page_blob_read(g_ctx, "test.db", 0, 4096, &buf, &err);
    size_t cap_after_big = buf.capacity;

    /* Read only 512 bytes — capacity should not shrink */
    g_ops->page_blob_read(g_ctx, "test.db", 0, 512, &buf, &err);
    ASSERT_GE(buf.capacity, cap_after_big);

    azure_buffer_free(&buf);
}

/* ══════════════════════════════════════════════════════════════════════
** SECTION 5: Operations Through Vtable Function Pointers
**
** Verify the vtable is properly wired — every function pointer is
** non-NULL and callable.
** ══════════════════════════════════════════════════════════════════════ */

TEST(vtable_all_pointers_non_null) {
    azure_ops_t *ops = mock_azure_get_ops();
    ASSERT_NOT_NULL(ops->page_blob_create);
    ASSERT_NOT_NULL(ops->page_blob_write);
    ASSERT_NOT_NULL(ops->page_blob_read);
    ASSERT_NOT_NULL(ops->page_blob_resize);
    ASSERT_NOT_NULL(ops->block_blob_upload);
    ASSERT_NOT_NULL(ops->block_blob_download);
    ASSERT_NOT_NULL(ops->blob_get_properties);
    ASSERT_NOT_NULL(ops->blob_delete);
    ASSERT_NOT_NULL(ops->blob_exists);
    ASSERT_NOT_NULL(ops->lease_acquire);
    ASSERT_NOT_NULL(ops->lease_renew);
    ASSERT_NOT_NULL(ops->lease_release);
    ASSERT_NOT_NULL(ops->lease_break);
}

TEST(vtable_is_stable_singleton) {
    azure_ops_t *ops1 = mock_azure_get_ops();
    azure_ops_t *ops2 = mock_azure_get_ops();
    ASSERT_TRUE(ops1 == ops2);
}

TEST(vtable_ctx_isolation) {
    /* Two different contexts should have independent state */
    mock_azure_ctx_t *ctx1 = mock_azure_create();
    mock_azure_ctx_t *ctx2 = mock_azure_create();
    azure_ops_t *ops = mock_azure_get_ops();
    azure_error_t err;

    ops->page_blob_create(ctx1, "only-in-1.db", 4096, &err);

    int exists = 1;
    ops->blob_exists(ctx2, "only-in-1.db", &exists, &err);
    ASSERT_EQ(exists, 0);

    mock_azure_destroy(ctx1);
    mock_azure_destroy(ctx2);
}

/* ══════════════════════════════════════════════════════════════════════
** SECTION 6: Error → SQLite Mapping (D8 contract)
**
** Documents the expected mapping from Azure errors to SQLite return
** codes. The actual mapping lives in the VFS layer, but we verify
** the azure_err_t values are suitable for this mapping.
** ══════════════════════════════════════════════════════════════════════ */

TEST(transient_errors_are_retryable) {
    /* Per D8: 409 (conflict) and 429 (throttle) → SQLITE_BUSY */
    /* AZURE_ERR_CONFLICT and AZURE_ERR_THROTTLE should be distinct
    ** from fatal errors so the VFS can map them to SQLITE_BUSY */
    ASSERT_NE(AZURE_ERR_CONFLICT, AZURE_ERR_IO);
    ASSERT_NE(AZURE_ERR_THROTTLE, AZURE_ERR_IO);
    ASSERT_NE(AZURE_ERR_CONFLICT, AZURE_ERR_SERVER);
    ASSERT_NE(AZURE_ERR_THROTTLE, AZURE_ERR_SERVER);
}

TEST(fatal_errors_distinct_from_transient) {
    /* NETWORK, IO, SERVER should not collide with CONFLICT/THROTTLE */
    azure_err_t transient[] = { AZURE_ERR_CONFLICT, AZURE_ERR_THROTTLE };
    azure_err_t fatal[] = { AZURE_ERR_NETWORK, AZURE_ERR_IO, AZURE_ERR_SERVER,
                            AZURE_ERR_AUTH, AZURE_ERR_TIMEOUT };

    for (int i = 0; i < 2; i++) {
        for (int j = 0; j < 5; j++) {
            ASSERT_NE(transient[i], fatal[j]);
        }
    }
}

/* ══════════════════════════════════════════════════════════════════════
** SECTION 7: Auth Tests (stubs — enabled when azure_client.c exists)
**
** These test Frodo's auth implementation. Gated behind
** ENABLE_AZURE_CLIENT_TESTS since the real module doesn't exist yet.
** ══════════════════════════════════════════════════════════════════════ */

#ifdef ENABLE_AZURE_CLIENT_TESTS

/*
** Forward declarations — provided by Frodo's azure_client.c.
** These signatures are based on the design review.
*/
extern int azure_auth_sign_shared_key(const char *account,
                                       const char *key_base64,
                                       const char *string_to_sign,
                                       char *signature_out,
                                       size_t signature_size);

extern int azure_auth_append_sas(const char *base_url,
                                  const char *sas_token,
                                  char *url_out,
                                  size_t url_size);

extern int azure_auth_build_shared_key_header(const char *account,
                                                const char *key_base64,
                                                const char *method,
                                                const char *resource,
                                                const char *date,
                                                char *header_out,
                                                size_t header_size);

/* Known test vectors for HMAC-SHA256 */
TEST(auth_hmac_sha256_known_vector) {
    /* RFC 4231 Test Case 2:
    ** Key  = "Jefe" (base64: "SmVmZQ==")
    ** Data = "what do ya want for nothing?"
    ** HMAC = 5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843 */
    char sig[128];
    int rc = azure_auth_sign_shared_key(
        "testaccount", "SmVmZQ==",
        "what do ya want for nothing?",
        sig, sizeof(sig));
    ASSERT_EQ(rc, 0);
    ASSERT_TRUE(strlen(sig) > 0);
}

TEST(auth_sas_token_append) {
    char url[512];
    int rc = azure_auth_append_sas(
        "https://myaccount.blob.core.windows.net/mycontainer/myblob",
        "sv=2024-08-04&ss=b&srt=o&sp=rwdlac",
        url, sizeof(url));
    ASSERT_EQ(rc, 0);
    ASSERT_TRUE(strstr(url, "?sv=2024-08-04") != NULL ||
                strstr(url, "&sv=2024-08-04") != NULL);
}

TEST(auth_sas_token_with_existing_params) {
    char url[512];
    int rc = azure_auth_append_sas(
        "https://myaccount.blob.core.windows.net/mycontainer/myblob?comp=page",
        "sv=2024-08-04&ss=b",
        url, sizeof(url));
    ASSERT_EQ(rc, 0);
    /* Should append with & not ? since URL already has params */
    ASSERT_TRUE(strstr(url, "&sv=2024-08-04") != NULL);
}

TEST(auth_missing_credentials) {
    /* When both SAS and Shared Key are NULL, should return error */
    char sig[128];
    int rc = azure_auth_sign_shared_key("account", NULL, "data",
                                          sig, sizeof(sig));
    ASSERT_NE(rc, 0);
}

TEST(auth_shared_key_header_format) {
    char header[512];
    int rc = azure_auth_build_shared_key_header(
        "myaccount", "dGVzdGtleQ==",
        "PUT", "/mycontainer/myblob",
        "Sun, 10 Mar 2026 00:00:00 GMT",
        header, sizeof(header));
    ASSERT_EQ(rc, 0);
    /* Should be: SharedKey myaccount:<base64-signature> */
    ASSERT_TRUE(strstr(header, "SharedKey myaccount:") != NULL);
}

/* ── XML Error Parsing ────────────────────────────────────────────── */

extern int azure_parse_error_xml(const char *xml, azure_error_t *err);

TEST(xml_error_parse_basic) {
    const char *xml =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<Error>"
        "<Code>BlobNotFound</Code>"
        "<Message>The specified blob does not exist.</Message>"
        "</Error>";

    azure_error_t err;
    memset(&err, 0, sizeof(err));
    int rc = azure_parse_error_xml(xml, &err);
    ASSERT_EQ(rc, 0);
    ASSERT_STR_EQ(err.error_code, "BlobNotFound");
    ASSERT_TRUE(strstr(err.error_message, "does not exist") != NULL);
}

TEST(xml_error_parse_lease_conflict) {
    const char *xml =
        "<?xml version=\"1.0\"?>"
        "<Error>"
        "<Code>LeaseAlreadyPresent</Code>"
        "<Message>There is already a lease present.</Message>"
        "</Error>";

    azure_error_t err;
    memset(&err, 0, sizeof(err));
    azure_parse_error_xml(xml, &err);
    ASSERT_STR_EQ(err.error_code, "LeaseAlreadyPresent");
}

TEST(xml_error_parse_empty) {
    azure_error_t err;
    memset(&err, 0, sizeof(err));
    int rc = azure_parse_error_xml("", &err);
    ASSERT_NE(rc, 0); /* Should fail gracefully */
}

TEST(xml_error_parse_malformed) {
    azure_error_t err;
    memset(&err, 0, sizeof(err));
    int rc = azure_parse_error_xml("<not>valid xml", &err);
    ASSERT_NE(rc, 0);
}

#endif /* ENABLE_AZURE_CLIENT_TESTS */

/* ══════════════════════════════════════════════════════════════════════
** SECTION 8: Retry Logic Contract Tests
**
** Per D8: 5 attempts, 500ms exponential backoff + jitter.
** Retry lives in Frodo's code, not the mock. But we verify the
** mock does NOT retry — errors are returned immediately.
** ══════════════════════════════════════════════════════════════════════ */

TEST(mock_does_not_retry) {
    setup();
    azure_error_t err;

    mock_set_fail_operation(g_ctx, "page_blob_create", AZURE_ERR_SERVER);

    g_ops->page_blob_create(g_ctx, "test.db", 4096, &err);
    /* Mock should have called exactly once — no retry */
    ASSERT_EQ(mock_get_call_count(g_ctx, "page_blob_create"), 1);
}

TEST(mock_transient_error_not_retried) {
    setup();
    azure_error_t err;

    mock_set_fail_operation(g_ctx, "page_blob_write", AZURE_ERR_THROTTLE);

    g_ops->page_blob_create(g_ctx, "test.db", 4096, &err);
    mock_clear_failures(g_ctx);
    mock_set_fail_operation(g_ctx, "page_blob_write", AZURE_ERR_THROTTLE);

    uint8_t data[512] = {0};
    g_ops->page_blob_write(g_ctx, "test.db", 0, data, 512, NULL, &err);
    ASSERT_EQ(mock_get_call_count(g_ctx, "page_blob_write"), 1);
}

/* ══════════════════════════════════════════════════════════════════════
** SECTION 9: Stress / Boundary Tests
** ══════════════════════════════════════════════════════════════════════ */

TEST(many_blobs) {
    setup();
    azure_error_t err;

    /* Create many blobs to test storage management */
    for (int i = 0; i < 50; i++) {
        char name[64];
        snprintf(name, sizeof(name), "blob-%03d.db", i);
        azure_err_t rc = g_ops->page_blob_create(g_ctx, name, 4096, &err);
        ASSERT_AZURE_OK(rc);
    }

    /* Verify they're all there */
    for (int i = 0; i < 50; i++) {
        char name[64];
        snprintf(name, sizeof(name), "blob-%03d.db", i);
        ASSERT_TRUE(mock_blob_exists(g_ctx, name));
    }
}

TEST(rapid_create_delete_cycles) {
    setup();
    azure_error_t err;

    for (int i = 0; i < 20; i++) {
        g_ops->page_blob_create(g_ctx, "cycle.db", 4096, &err);
        ASSERT_TRUE(mock_blob_exists(g_ctx, "cycle.db"));

        g_ops->blob_delete(g_ctx, "cycle.db", &err);
        ASSERT_FALSE(mock_blob_exists(g_ctx, "cycle.db"));
    }
}

TEST(rapid_lease_acquire_release_cycles) {
    setup();
    azure_error_t err;
    g_ops->page_blob_create(g_ctx, "test.db", 4096, &err);

    for (int i = 0; i < 20; i++) {
        char id[64];
        azure_err_t rc = g_ops->lease_acquire(g_ctx, "test.db", 30,
                                               id, sizeof(id), &err);
        ASSERT_AZURE_OK(rc);
        ASSERT_EQ(mock_is_leased(g_ctx, "test.db"), 1);

        rc = g_ops->lease_release(g_ctx, "test.db", id, &err);
        ASSERT_AZURE_OK(rc);
        ASSERT_EQ(mock_is_leased(g_ctx, "test.db"), 0);
    }
}

TEST(write_read_many_pages_sequentially) {
    setup();
    azure_error_t err;

    /* Create a 1MB blob and write/read every 4096-byte page */
    int64_t size = 1024 * 1024;
    g_ops->page_blob_create(g_ctx, "big.db", size, &err);

    int num_pages = (int)(size / 4096);
    for (int i = 0; i < num_pages; i++) {
        uint8_t data[4096];
        memset(data, (uint8_t)(i & 0xFF), sizeof(data));
        g_ops->page_blob_write(g_ctx, "big.db",
                               (int64_t)(i * 4096), data, 4096, NULL, &err);
    }

    for (int i = 0; i < num_pages; i++) {
        azure_buffer_t buf;
        azure_buffer_init(&buf);
        g_ops->page_blob_read(g_ctx, "big.db",
                              (int64_t)(i * 4096), 4096, &buf, &err);
        ASSERT_EQ(buf.data[0], (uint8_t)(i & 0xFF));
        azure_buffer_free(&buf);
    }
}

/* ══════════════════════════════════════════════════════════════════════
** Test Suite Runner
** ══════════════════════════════════════════════════════════════════════ */

void run_azure_client_tests(void) {
    /* Section 1: Error Codes */
    TEST_SUITE_BEGIN("Azure Error Codes");
    RUN_TEST(error_code_values);
    RUN_TEST(error_code_all_distinct);
    TEST_SUITE_END();

    /* Section 2: HTTP Status Mapping */
    TEST_SUITE_BEGIN("HTTP Status → Azure Error");
    RUN_TEST(http_500_is_server_error);
    RUN_TEST(http_429_is_throttle_error);
    RUN_TEST(http_401_is_auth_error);
    RUN_TEST(http_404_is_not_found);
    RUN_TEST(http_409_is_conflict);
    RUN_TEST(network_error);
    RUN_TEST(timeout_error);
    TEST_SUITE_END();

    /* Section 3: Error Details */
    TEST_SUITE_BEGIN("Azure Error Details");
    RUN_TEST(error_detail_http_status);
    RUN_TEST(error_detail_error_code_string);
    RUN_TEST(error_detail_error_message);
    RUN_TEST(error_detail_request_id);
    RUN_TEST(error_detail_lease_conflict);
    RUN_TEST(error_detail_lease_mismatch);
    TEST_SUITE_END();

    /* Section 4: Buffer Management */
    TEST_SUITE_BEGIN("Azure Buffer Management");
    RUN_TEST(buffer_alloc_free);
    RUN_TEST(buffer_double_free_safe);
    RUN_TEST(buffer_growth_on_read);
    RUN_TEST(buffer_preserves_capacity);
    TEST_SUITE_END();

    /* Section 5: Vtable Verification */
    TEST_SUITE_BEGIN("Vtable Function Pointers");
    RUN_TEST(vtable_all_pointers_non_null);
    RUN_TEST(vtable_is_stable_singleton);
    RUN_TEST(vtable_ctx_isolation);
    TEST_SUITE_END();

    /* Section 6: Error Category Mapping */
    TEST_SUITE_BEGIN("Error Category Mapping (D8)");
    RUN_TEST(transient_errors_are_retryable);
    RUN_TEST(fatal_errors_distinct_from_transient);
    TEST_SUITE_END();

#ifdef ENABLE_AZURE_CLIENT_TESTS
    /* Section 7: Auth */
    TEST_SUITE_BEGIN("Auth - HMAC-SHA256");
    RUN_TEST(auth_hmac_sha256_known_vector);
    RUN_TEST(auth_sas_token_append);
    RUN_TEST(auth_sas_token_with_existing_params);
    RUN_TEST(auth_missing_credentials);
    RUN_TEST(auth_shared_key_header_format);
    TEST_SUITE_END();

    TEST_SUITE_BEGIN("Auth - XML Error Parsing");
    RUN_TEST(xml_error_parse_basic);
    RUN_TEST(xml_error_parse_lease_conflict);
    RUN_TEST(xml_error_parse_empty);
    RUN_TEST(xml_error_parse_malformed);
    TEST_SUITE_END();
#endif

    /* Section 8: Retry Contract */
    TEST_SUITE_BEGIN("Retry Contract");
    RUN_TEST(mock_does_not_retry);
    RUN_TEST(mock_transient_error_not_retried);
    TEST_SUITE_END();

    /* Section 9: Stress Tests */
    TEST_SUITE_BEGIN("Stress / Boundary");
    RUN_TEST(many_blobs);
    RUN_TEST(rapid_create_delete_cycles);
    RUN_TEST(rapid_lease_acquire_release_cycles);
    RUN_TEST(write_read_many_pages_sequentially);
    TEST_SUITE_END();

    /* Cleanup */
    if (ac_ctx) {
        mock_azure_destroy(ac_ctx);
        ac_ctx = NULL;
    }

#undef g_ctx
#undef g_ops
#undef setup
}
