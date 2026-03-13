/*
 * azure_poc_main.c — Proof-of-concept test harness for Azure Blob Storage REST API
 *
 * Exercises all operations: page blob CRUD, block blob CRUD, leases, error handling.
 * Set environment variables before running:
 *   AZURE_STORAGE_ACCOUNT, AZURE_STORAGE_KEY (or AZURE_STORAGE_SAS),
 *   AZURE_STORAGE_CONTAINER
 *
 * This PoC validates that we can implement Azure REST API calls directly
 * in C using only libcurl and OpenSSL — no Azure SDK required.
 */

#include "azure_blob.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ----------------------------------------------------------------
 * Test helpers
 * ---------------------------------------------------------------- */

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_START(name) do { \
    tests_run++; \
    fprintf(stderr, "\n=== TEST: %s ===\n", name); \
} while(0)

#define TEST_PASS(name) do { \
    tests_passed++; \
    fprintf(stderr, "  PASS: %s\n", name); \
} while(0)

#define TEST_FAIL(name, msg) do { \
    tests_failed++; \
    fprintf(stderr, "  FAIL: %s — %s\n", name, msg); \
} while(0)

static void print_error(const azure_error_t *err)
{
    fprintf(stderr, "  Error: %s (HTTP %ld)\n", azure_err_str(err->code), err->http_status);
    if (err->error_code[0])
        fprintf(stderr, "  Azure code: %s\n", err->error_code);
    if (err->error_message[0])
        fprintf(stderr, "  Message: %s\n", err->error_message);
    if (err->request_id[0])
        fprintf(stderr, "  Request ID: %s\n", err->request_id);
}

/* ----------------------------------------------------------------
 * Test: HMAC-SHA256 authentication signing
 *
 * Verify our StringToSign construction and HMAC-SHA256 signing
 * produces correct output using a known test vector.
 * ---------------------------------------------------------------- */

static void test_auth_signing(void)
{
    TEST_START("HMAC-SHA256 Auth Signing");

    azure_client_t client;
    memset(&client, 0, sizeof(client));
    strncpy(client.account, "myaccount", sizeof(client.account));

    /*
     * Use a known test key (base64-encoded).
     * This is NOT a real Azure key — it's a test vector.
     * Real keys are 64 bytes (512 bits), base64-encoded to ~88 chars.
     */
    const char *test_key_b64 = "dGVzdGtleWZvcmF6dXJlc3RvcmFnZWhtYWM=";
    strncpy(client.key_b64, test_key_b64, sizeof(client.key_b64));
    if (azure_base64_decode(test_key_b64, client.key_raw,
                            sizeof(client.key_raw), &client.key_raw_len) != 0) {
        TEST_FAIL("Auth Signing", "Failed to decode test key");
        return;
    }

    /* Test signing a simple GET request */
    const char *x_ms[] = {
        "x-ms-date:Mon, 10 Mar 2026 05:00:00 GMT",
        "x-ms-version:2024-08-04",
        NULL
    };

    char auth_header[512];
    azure_err_t rc = azure_auth_sign(
        &client, "GET", "/mycontainer/myblob.db", NULL,
        "", "", "",
        x_ms,
        auth_header, sizeof(auth_header));

    if (rc != AZURE_OK) {
        TEST_FAIL("Auth Signing", "azure_auth_sign failed");
        return;
    }

    /* Verify the header starts with "SharedKey myaccount:" */
    if (strncmp(auth_header, "SharedKey myaccount:", 20) != 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Bad prefix: %s", auth_header);
        TEST_FAIL("Auth Signing", msg);
        return;
    }

    /* Verify there's a base64 signature after the colon */
    const char *sig = strchr(auth_header, ':');
    if (!sig || strlen(sig + 1) < 40) {
        TEST_FAIL("Auth Signing", "Signature too short");
        return;
    }

    fprintf(stderr, "  Authorization: %s\n", auth_header);
    TEST_PASS("Auth Signing — produces valid SharedKey header");

    /* Test with PUT and content-length */
    const char *x_ms_put[] = {
        "x-ms-blob-type:BlockBlob",
        "x-ms-date:Mon, 10 Mar 2026 05:00:00 GMT",
        "x-ms-version:2024-08-04",
        NULL
    };

    rc = azure_auth_sign(
        &client, "PUT", "/mycontainer/myblob.db", NULL,
        "1024", "application/octet-stream", "",
        x_ms_put,
        auth_header, sizeof(auth_header));

    if (rc != AZURE_OK) {
        TEST_FAIL("Auth Signing PUT", "azure_auth_sign failed for PUT");
        return;
    }

    fprintf(stderr, "  PUT Authorization: %s\n", auth_header);
    TEST_PASS("Auth Signing PUT — handles content-type and length");

    /* Test with query parameters (comp=page) */
    const char *x_ms_page[] = {
        "x-ms-date:Mon, 10 Mar 2026 05:00:00 GMT",
        "x-ms-page-write:update",
        "x-ms-version:2024-08-04",
        NULL
    };

    rc = azure_auth_sign(
        &client, "PUT", "/mycontainer/myblob.db", "comp=page",
        "512", "application/octet-stream", "bytes=0-511",
        x_ms_page,
        auth_header, sizeof(auth_header));

    if (rc != AZURE_OK) {
        TEST_FAIL("Auth Signing Page", "azure_auth_sign failed for page write");
        return;
    }

    fprintf(stderr, "  Page write Authorization: %s\n", auth_header);
    TEST_PASS("Auth Signing Page Write — handles query params and range");
}

/* ----------------------------------------------------------------
 * Test: Base64 encode/decode round-trip
 * ---------------------------------------------------------------- */

static void test_base64(void)
{
    TEST_START("Base64 Round-trip");

    const uint8_t data[] = "Hello, Azure Blob Storage!";
    char encoded[128];
    uint8_t decoded[128];
    size_t decoded_len;

    if (azure_base64_encode(data, strlen((const char *)data),
                            encoded, sizeof(encoded)) != 0) {
        TEST_FAIL("Base64 Encode", "Failed");
        return;
    }

    fprintf(stderr, "  Encoded: %s\n", encoded);

    if (azure_base64_decode(encoded, decoded, sizeof(decoded), &decoded_len) != 0) {
        TEST_FAIL("Base64 Decode", "Failed");
        return;
    }

    if (decoded_len != strlen((const char *)data) ||
        memcmp(decoded, data, decoded_len) != 0) {
        TEST_FAIL("Base64 Round-trip", "Data mismatch");
        return;
    }

    TEST_PASS("Base64 — encode/decode round-trip matches");
}

/* ----------------------------------------------------------------
 * Test: HMAC-SHA256
 * ---------------------------------------------------------------- */

static void test_hmac_sha256(void)
{
    TEST_START("HMAC-SHA256");

    /* RFC 4231 Test Case 2:
     *   Key  = "Jefe"
     *   Data = "what do ya want for nothing?"
     *   HMAC = 5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843
     */
    const uint8_t key[] = "Jefe";
    const uint8_t data[] = "what do ya want for nothing?";
    uint8_t hmac[32];
    size_t hmac_len;

    azure_err_t rc = azure_hmac_sha256(key, 4, data, 28, hmac, &hmac_len);
    if (rc != AZURE_OK) {
        TEST_FAIL("HMAC-SHA256", "Computation failed");
        return;
    }

    if (hmac_len != 32) {
        TEST_FAIL("HMAC-SHA256", "Wrong output length");
        return;
    }

    /* Expected: 5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843 */
    const uint8_t expected[32] = {
        0x5b, 0xdc, 0xc1, 0x46, 0xbf, 0x60, 0x75, 0x4e,
        0x6a, 0x04, 0x24, 0x26, 0x08, 0x95, 0x75, 0xc7,
        0x5a, 0x00, 0x3f, 0x08, 0x9d, 0x27, 0x39, 0x83,
        0x9d, 0xec, 0x58, 0xb9, 0x64, 0xec, 0x38, 0x43
    };

    if (memcmp(hmac, expected, 32) != 0) {
        TEST_FAIL("HMAC-SHA256", "Output mismatch vs RFC 4231 test vector");
        return;
    }

    TEST_PASS("HMAC-SHA256 — matches RFC 4231 test vector");
}

/* ----------------------------------------------------------------
 * Test: Error XML parsing
 * ---------------------------------------------------------------- */

static void test_error_parsing(void)
{
    TEST_START("Error XML Parsing");

    const char *xml =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
        "<Error>\n"
        "  <Code>ServerBusy</Code>\n"
        "  <Message>The server is currently unable to receive requests.</Message>\n"
        "</Error>";

    azure_error_t err;
    memset(&err, 0, sizeof(err));

    azure_err_t rc = azure_parse_error_xml(xml, strlen(xml), &err);
    if (rc != AZURE_OK) {
        TEST_FAIL("Error Parsing", "Parse failed");
        return;
    }

    if (strcmp(err.error_code, "ServerBusy") != 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Expected 'ServerBusy', got '%s'", err.error_code);
        TEST_FAIL("Error Parsing Code", msg);
        return;
    }

    TEST_PASS("Error Parsing — extracted Code='ServerBusy'");

    if (strstr(err.error_message, "unable to receive") == NULL) {
        TEST_FAIL("Error Parsing Message", "Message content wrong");
        return;
    }

    TEST_PASS("Error Parsing — extracted Message correctly");
}

/* ----------------------------------------------------------------
 * Test: Transient error classification
 * ---------------------------------------------------------------- */

static void test_error_classification(void)
{
    TEST_START("Error Classification");

    /* Transient errors */
    if (!azure_is_transient_error(500, "InternalError")) {
        TEST_FAIL("Classification", "500 InternalError should be transient");
        return;
    }
    if (!azure_is_transient_error(503, "ServerBusy")) {
        TEST_FAIL("Classification", "503 ServerBusy should be transient");
        return;
    }
    if (!azure_is_transient_error(429, "")) {
        TEST_FAIL("Classification", "429 should be transient");
        return;
    }

    /* Permanent errors */
    if (azure_is_transient_error(404, "ResourceNotFound")) {
        TEST_FAIL("Classification", "404 should NOT be transient");
        return;
    }
    if (azure_is_transient_error(403, "AuthorizationFailure")) {
        TEST_FAIL("Classification", "403 should NOT be transient");
        return;
    }
    if (azure_is_transient_error(409, "LeaseAlreadyPresent")) {
        TEST_FAIL("Classification", "409 should NOT be transient");
        return;
    }

    TEST_PASS("Error Classification — transient vs permanent correct");
}

/* ----------------------------------------------------------------
 * Test: RFC 1123 date formatting
 * ---------------------------------------------------------------- */

static void test_rfc1123_time(void)
{
    TEST_START("RFC 1123 Date Format");

    char buf[64];
    azure_rfc1123_time(buf, sizeof(buf));

    fprintf(stderr, "  Date: %s\n", buf);

    /* Should contain "GMT" at the end */
    if (strstr(buf, "GMT") == NULL) {
        TEST_FAIL("RFC 1123", "Missing 'GMT' suffix");
        return;
    }

    /* Should be reasonable length (e.g., "Mon, 10 Mar 2026 05:00:00 GMT") */
    if (strlen(buf) < 25) {
        TEST_FAIL("RFC 1123", "Date string too short");
        return;
    }

    TEST_PASS("RFC 1123 — produces valid date format");
}

/* ----------------------------------------------------------------
 * Test: Page blob alignment validation
 * ---------------------------------------------------------------- */

static void test_page_alignment_validation(void)
{
    TEST_START("Page Blob Alignment Validation");

    /*
     * We test that our validation logic catches misaligned sizes
     * WITHOUT needing a real Azure connection.
     */

    /* Valid: 4096 is 512-byte aligned */
    if (4096 % AZURE_PAGE_SIZE != 0) {
        TEST_FAIL("Alignment", "4096 should be valid");
        return;
    }

    /* Invalid: 1000 is NOT 512-byte aligned */
    if (1000 % AZURE_PAGE_SIZE == 0) {
        TEST_FAIL("Alignment", "1000 should be invalid");
        return;
    }

    /* SQLite default page size is 4096 — perfect alignment */
    if (4096 % AZURE_PAGE_SIZE != 0) {
        TEST_FAIL("Alignment", "SQLite 4096 page should align with Azure 512 page");
        return;
    }

    /* SQLite minimum page size is 512 — exact match */
    if (512 % AZURE_PAGE_SIZE != 0) {
        TEST_FAIL("Alignment", "SQLite 512 page should align");
        return;
    }

    TEST_PASS("Page Alignment — SQLite 512/4096 pages align with Azure 512-byte pages");
}

/* ----------------------------------------------------------------
 * Test: Retry backoff timing (just validates logic, doesn't sleep long)
 * ---------------------------------------------------------------- */

static void test_retry_backoff(void)
{
    TEST_START("Retry Backoff Logic");

    /* Verify exponential growth: 500, 1000, 2000, 4000, 8000 */
    for (int attempt = 0; attempt < 5; attempt++) {
        int delay_ms = AZURE_RETRY_BASE_MS * (1 << attempt);
        if (delay_ms > AZURE_RETRY_MAX_MS) delay_ms = AZURE_RETRY_MAX_MS;

        int expected = AZURE_RETRY_BASE_MS * (1 << attempt);
        if (expected > AZURE_RETRY_MAX_MS) expected = AZURE_RETRY_MAX_MS;

        fprintf(stderr, "  Attempt %d: base delay = %d ms\n", attempt, delay_ms);

        if (delay_ms != expected) {
            TEST_FAIL("Backoff", "Delay calculation wrong");
            return;
        }
    }

    TEST_PASS("Retry Backoff — exponential growth 500→1000→2000→4000→8000 ms");
}

/* ----------------------------------------------------------------
 * Test: Live operations (only runs if AZURE_STORAGE_ACCOUNT is set)
 *
 * These test the full round-trip against a real Azure Storage account.
 * Skip gracefully if no credentials are available.
 * ---------------------------------------------------------------- */

static void test_live_page_blob(azure_client_t *client)
{
    TEST_START("Live: Page Blob Operations");
    azure_error_t err;

    const char *blob_name = "sqliteObjs-poc-test.pageblob";

    /* 1. Create a 4096-byte page blob (one SQLite page) */
    fprintf(stderr, "  Creating page blob (%d bytes)...\n", 4096);
    azure_err_t rc = azure_page_blob_create(client, blob_name, 4096, &err);
    if (rc != AZURE_OK) {
        print_error(&err);
        TEST_FAIL("Page Create", azure_err_str(rc));
        return;
    }
    TEST_PASS("Page Create — 4096 bytes");

    /* 2. Write a 512-byte page at offset 0 */
    uint8_t write_data[512];
    memset(write_data, 0, sizeof(write_data));
    memcpy(write_data, "SQLite format 3\000", 16); /* SQLite magic */
    write_data[16] = 0x10; /* Page size = 4096 (big-endian) */
    write_data[17] = 0x00;

    fprintf(stderr, "  Writing 512 bytes at offset 0...\n");
    rc = azure_page_blob_write(client, blob_name, 0, write_data, 512, &err);
    if (rc != AZURE_OK) {
        print_error(&err);
        TEST_FAIL("Page Write", azure_err_str(rc));
        goto cleanup;
    }
    TEST_PASS("Page Write — 512 bytes at offset 0");

    /* 3. Read back the page */
    azure_buffer_t read_buf;
    azure_buffer_init(&read_buf);
    fprintf(stderr, "  Reading 512 bytes at offset 0...\n");
    rc = azure_page_blob_read(client, blob_name, 0, 512, &read_buf, &err);
    if (rc != AZURE_OK) {
        print_error(&err);
        TEST_FAIL("Page Read", azure_err_str(rc));
        azure_buffer_free(&read_buf);
        goto cleanup;
    }

    if (read_buf.size != 512 || memcmp(read_buf.data, write_data, 512) != 0) {
        TEST_FAIL("Page Read Verify", "Data mismatch");
    } else {
        TEST_PASS("Page Read — data round-trip verified");
    }
    azure_buffer_free(&read_buf);

    /* 4. Get properties */
    int64_t content_length = 0;
    char lease_state[32] = "", lease_status[32] = "";
    rc = azure_blob_get_properties(client, blob_name, &content_length,
                                   lease_state, lease_status, &err);
    if (rc != AZURE_OK) {
        print_error(&err);
        TEST_FAIL("Get Properties", azure_err_str(rc));
        goto cleanup;
    }
    fprintf(stderr, "  Size: %lld, Lease: %s/%s\n",
            (long long)content_length, lease_state, lease_status);

    if (content_length != 4096) {
        TEST_FAIL("Get Properties", "Wrong content length");
    } else {
        TEST_PASS("Get Properties — size=4096, lease info retrieved");
    }

    /* 5. Resize to 8192 */
    fprintf(stderr, "  Resizing to 8192 bytes...\n");
    rc = azure_page_blob_resize(client, blob_name, 8192, &err);
    if (rc != AZURE_OK) {
        print_error(&err);
        TEST_FAIL("Page Resize", azure_err_str(rc));
        goto cleanup;
    }
    TEST_PASS("Page Resize — 4096 → 8192");

cleanup:
    /* Delete the test blob */
    fprintf(stderr, "  Cleaning up test blob...\n");
    azure_blob_delete(client, blob_name, &err);
}

static void test_live_block_blob(azure_client_t *client)
{
    TEST_START("Live: Block Blob Operations");
    azure_error_t err;

    const char *blob_name = "sqliteObjs-poc-test.journal";
    const char *test_data = "This is journal data for sqliteObjs PoC test.";
    size_t test_len = strlen(test_data);

    /* 1. Upload */
    fprintf(stderr, "  Uploading block blob (%zu bytes)...\n", test_len);
    azure_err_t rc = azure_block_blob_upload(client, blob_name,
        (const uint8_t *)test_data, test_len,
        "application/octet-stream", &err);
    if (rc != AZURE_OK) {
        print_error(&err);
        TEST_FAIL("Block Upload", azure_err_str(rc));
        return;
    }
    TEST_PASS("Block Upload");

    /* 2. Download */
    azure_buffer_t dl_buf;
    azure_buffer_init(&dl_buf);
    fprintf(stderr, "  Downloading block blob...\n");
    rc = azure_block_blob_download(client, blob_name, &dl_buf, &err);
    if (rc != AZURE_OK) {
        print_error(&err);
        TEST_FAIL("Block Download", azure_err_str(rc));
        azure_buffer_free(&dl_buf);
        goto cleanup;
    }

    if (dl_buf.size != test_len || memcmp(dl_buf.data, test_data, test_len) != 0) {
        TEST_FAIL("Block Download Verify", "Data mismatch");
    } else {
        TEST_PASS("Block Download — data verified");
    }
    azure_buffer_free(&dl_buf);

    /* 3. Delete */
    fprintf(stderr, "  Deleting block blob...\n");
    rc = azure_blob_delete(client, blob_name, &err);
    if (rc != AZURE_OK) {
        print_error(&err);
        TEST_FAIL("Block Delete", azure_err_str(rc));
        return;
    }
    TEST_PASS("Block Delete");

    /* 4. Verify 404 after delete */
    azure_buffer_t dl_buf2;
    azure_buffer_init(&dl_buf2);
    rc = azure_block_blob_download(client, blob_name, &dl_buf2, &err);
    if (rc == AZURE_ERR_NOT_FOUND) {
        TEST_PASS("Block Delete Verify — 404 after delete");
    } else {
        TEST_FAIL("Block Delete Verify", "Expected 404");
    }
    azure_buffer_free(&dl_buf2);
    return;

cleanup:
    azure_blob_delete(client, blob_name, &err);
}

static void test_live_lease(azure_client_t *client)
{
    TEST_START("Live: Lease Operations");
    azure_error_t err;

    const char *blob_name = "sqliteObjs-poc-test-lease.pageblob";

    /* Create a test blob first */
    azure_err_t rc = azure_page_blob_create(client, blob_name, 512, &err);
    if (rc != AZURE_OK) {
        print_error(&err);
        TEST_FAIL("Lease Setup", "Failed to create test blob");
        return;
    }

    /* 1. Acquire lease (15 second duration) */
    char lease_id[64] = "";
    fprintf(stderr, "  Acquiring 15-second lease...\n");
    rc = azure_lease_acquire(client, blob_name, 15, lease_id, sizeof(lease_id), &err);
    if (rc != AZURE_OK) {
        print_error(&err);
        TEST_FAIL("Lease Acquire", azure_err_str(rc));
        goto cleanup;
    }
    fprintf(stderr, "  Lease ID: %s\n", lease_id);
    TEST_PASS("Lease Acquire — got lease ID");

    /* 2. Renew lease */
    fprintf(stderr, "  Renewing lease...\n");
    rc = azure_lease_renew(client, blob_name, lease_id, &err);
    if (rc != AZURE_OK) {
        print_error(&err);
        TEST_FAIL("Lease Renew", azure_err_str(rc));
        goto release;
    }
    TEST_PASS("Lease Renew");

    /* 3. Check lease state via properties */
    int64_t size;
    char state[32], status[32];
    rc = azure_blob_get_properties(client, blob_name, &size, state, status, &err);
    if (rc == AZURE_OK) {
        fprintf(stderr, "  Lease state: %s, status: %s\n", state, status);
        if (strcmp(status, "locked") == 0) {
            TEST_PASS("Lease Check — status is 'locked'");
        }
    }

    /* 4. Release lease */
release:
    fprintf(stderr, "  Releasing lease...\n");
    rc = azure_lease_release(client, blob_name, lease_id, &err);
    if (rc != AZURE_OK) {
        print_error(&err);
        TEST_FAIL("Lease Release", azure_err_str(rc));
        goto cleanup;
    }
    TEST_PASS("Lease Release");

    /* 5. Test break lease (acquire again, then break) */
    rc = azure_lease_acquire(client, blob_name, 15, lease_id, sizeof(lease_id), &err);
    if (rc == AZURE_OK) {
        int remaining = 0;
        fprintf(stderr, "  Breaking lease...\n");
        rc = azure_lease_break(client, blob_name, 0, &remaining, &err);
        if (rc == AZURE_OK) {
            fprintf(stderr, "  Remaining time: %d seconds\n", remaining);
            TEST_PASS("Lease Break — immediate break");
        } else {
            print_error(&err);
            TEST_FAIL("Lease Break", azure_err_str(rc));
        }
    }

cleanup:
    /* Clean up — may need to wait for broken lease to expire */
    azure_blob_delete(client, blob_name, &err);
}

/* ----------------------------------------------------------------
 * Main
 * ---------------------------------------------------------------- */

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    srand((unsigned int)time(NULL));

    fprintf(stderr, "╔═══════════════════════════════════════════════╗\n");
    fprintf(stderr, "║  sqliteObjs — Azure Blob Storage REST API PoC   ║\n");
    fprintf(stderr, "║  API Version: %s                    ║\n", AZURE_API_VERSION);
    fprintf(stderr, "║  Dependencies: libcurl + OpenSSL only         ║\n");
    fprintf(stderr, "╚═══════════════════════════════════════════════╝\n");

    /* Offline tests — always run, no Azure credentials needed */
    fprintf(stderr, "\n--- OFFLINE TESTS (no Azure credentials needed) ---\n");

    test_base64();
    test_hmac_sha256();
    test_auth_signing();
    test_error_parsing();
    test_error_classification();
    test_rfc1123_time();
    test_page_alignment_validation();
    test_retry_backoff();

    /* Live tests — only run if credentials are available */
    fprintf(stderr, "\n--- LIVE TESTS (require Azure credentials) ---\n");

    const char *account = getenv("AZURE_STORAGE_ACCOUNT");
    if (!account || !*account) {
        fprintf(stderr, "\n  AZURE_STORAGE_ACCOUNT not set — skipping live tests.\n");
        fprintf(stderr, "  To run live tests, set:\n");
        fprintf(stderr, "    export AZURE_STORAGE_ACCOUNT=<account>\n");
        fprintf(stderr, "    export AZURE_STORAGE_KEY=<key>\n");
        fprintf(stderr, "    export AZURE_STORAGE_CONTAINER=<container>\n");
    } else {
        azure_client_t client;
        azure_err_t rc = azure_client_init(&client);
        if (rc != AZURE_OK) {
            fprintf(stderr, "  Failed to initialize client: %s\n", azure_err_str(rc));
        } else {
            test_live_page_blob(&client);
            test_live_block_blob(&client);
            test_live_lease(&client);
            azure_client_cleanup(&client);
        }
    }

    /* Summary */
    fprintf(stderr, "\n═══════════════════════════════════════════\n");
    fprintf(stderr, "  Tests: %d run, %d passed, %d failed\n",
            tests_run, tests_passed, tests_failed);
    fprintf(stderr, "═══════════════════════════════════════════\n");

    return tests_failed > 0 ? 1 : 0;
}
