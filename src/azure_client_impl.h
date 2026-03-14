/*
 * azure_client_impl.h — Internal header for Azure Blob Storage client
 *
 * This header is private to the azure_client, azure_auth, and azure_error
 * compilation units. Public types come from azure_client.h (included below).
 * This header adds only internal constants, the concrete azure_client struct,
 * internal response types, and internal function declarations.
 *
 * Part of the sqliteObjs project — Azure Blob-backed SQLite VFS.
 * License: MIT
 */

#ifndef AZURE_CLIENT_IMPL_H
#define AZURE_CLIENT_IMPL_H

#include "azure_client.h"
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * Constants
 * ================================================================ */

#define AZURE_API_VERSION        "2024-08-04"
#define AZURE_PAGE_SIZE          512
#define AZURE_MAX_PAGE_WRITE     (4 * 1024 * 1024)   /* 4 MiB per Put Page */
#define AZURE_MAX_RETRIES        5
#define AZURE_RETRY_BASE_MS      500
#define AZURE_RETRY_MAX_MS       30000

/* ================================================================
 * Compatibility aliases for Frodo's original error codes
 * (mapped to canonical codes from azure_client.h)
 * ================================================================ */

#define AZURE_ERR_HTTP       AZURE_ERR_BAD_REQUEST
#define AZURE_ERR_TRANSIENT  AZURE_ERR_SERVER
#define AZURE_ERR_CURL       AZURE_ERR_NETWORK
#define AZURE_ERR_OPENSSL    AZURE_ERR_AUTH
#define AZURE_ERR_XML_PARSE  AZURE_ERR_UNKNOWN
#define AZURE_ERR_ALLOC      AZURE_ERR_NOMEM

/* ================================================================
 * Internal types — not exposed to consumers
 * ================================================================ */

/* Concrete definition of the opaque azure_client_t */
struct azure_client {
    char     account[256];
    char     container[256];
    char     endpoint[512];      /* Custom endpoint (e.g., Azurite), or empty for Azure */
    char     key_b64[256];       /* Base64-encoded Shared Key */
    uint8_t  key_raw[64];        /* Decoded binary key */
    size_t   key_raw_len;
    char     sas_token[2048];    /* SAS token (without leading ?) */
    int      use_sas;            /* 1 = SAS auth, 0 = Shared Key */
    void    *curl_handle;        /* CURL* — opaque to avoid curl.h in header */
    void    *multi_handle;       /* CURLM* — persistent multi handle for batch writes.
                                  * Lazily initialized on first write_batch call.
                                  * Manages connection pool + TLS session cache.
                                  * Thread-safety: safe because xSync is serialized
                                  * by SQLite's btree mutex (D17). */
};

/* Captured HTTP response headers from Azure */
typedef struct {
    char    lease_id[64];
    char    lease_state[32];
    char    lease_status[32];
    char    request_id[64];
    char    error_code[128];
    char    etag[128];           /* ETag header value */
    int64_t content_length;
    int     lease_time;          /* x-ms-lease-time (break lease remaining) */
    int     retry_after;         /* Retry-After header in seconds (-1 = not present) */
} azure_response_headers_t;

/* ================================================================
 * Authentication — azure_auth.c
 * ================================================================ */

/*
 * Construct StringToSign and compute "SharedKey account:signature" header.
 *
 * Parameters:
 *   client          — provides account name and decoded key
 *   method          — "GET", "PUT", "DELETE", "HEAD"
 *   path            — "/container/blob"
 *   query           — "comp=page&timeout=30" or NULL
 *   content_length  — "512" or "" (empty for zero-length)
 *   content_type    — "application/octet-stream" or ""
 *   range           — "bytes=0-511" or ""
 *   x_ms_headers    — NULL-terminated array: "x-ms-date:...", ...
 *   auth_header     — output buffer for "SharedKey account:sig"
 *   auth_header_size — size of output buffer
 */
azure_err_t azure_auth_sign_request(
    const azure_client_t *client,
    const char *method,
    const char *path,
    const char *query,
    const char *content_length,
    const char *content_type,
    const char *range,
    const char *const *x_ms_headers,
    char *auth_header,
    size_t auth_header_size);

/* Base64 encode/decode using OpenSSL BIO */
int azure_base64_encode(const uint8_t *input, size_t input_len,
                        char *output, size_t output_size);
int azure_base64_decode(const char *input, uint8_t *output,
                        size_t output_size, size_t *output_len);

/* HMAC-SHA256 using OpenSSL */
azure_err_t azure_hmac_sha256(const uint8_t *key, size_t key_len,
                              const uint8_t *data, size_t data_len,
                              uint8_t *out, size_t *out_len);

/* Format current UTC time as RFC 1123 (for x-ms-date header) */
void azure_rfc1123_time(char *buf, size_t buf_size);

/* ================================================================
 * Error handling — azure_error.c
 * ================================================================ */

/* Human-readable error code string */
const char *azure_err_str(azure_err_t code);

/* Parse Azure XML error response body */
azure_err_t azure_parse_error_xml(const char *xml, size_t xml_len,
                                  azure_error_t *err);

/* Classify HTTP status + Azure error code into azure_err_t */
azure_err_t azure_classify_http_error(long http_status,
                                      const char *error_code);

/* Check if an error code is retryable (transient or throttled) */
int azure_is_retryable(azure_err_t code);

/*
 * Compute retry delay in milliseconds.
 *   attempt          — 0-based attempt number
 *   retry_after_secs — Retry-After header value, or -1 if not present
 * Returns delay in milliseconds (capped at AZURE_RETRY_MAX_MS).
 */
int azure_compute_retry_delay(int attempt, int retry_after_secs);

/* Sleep for the given number of milliseconds */
void azure_retry_sleep_ms(int delay_ms);

/* ================================================================
 * Internal buffer utility — azure_client.c only
 * ================================================================ */

int azure_buffer_append(azure_buffer_t *buf, const uint8_t *data, size_t len);

/*
 * Public API (azure_client_create, azure_client_destroy,
 * azure_client_get_ops, azure_client_get_ctx) is declared
 * in azure_client.h — included above.
 */

#ifdef __cplusplus
}
#endif

#endif /* AZURE_CLIENT_IMPL_H */
