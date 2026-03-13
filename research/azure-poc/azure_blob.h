/*
 * azure_blob.h — Azure Blob Storage REST API client (PoC)
 *
 * Direct REST API implementation using libcurl + OpenSSL.
 * No Azure SDK dependency. For the sqliteObjs project.
 *
 * Environment variables:
 *   AZURE_STORAGE_ACCOUNT   — Storage account name
 *   AZURE_STORAGE_KEY       — Base64-encoded account key
 *   AZURE_STORAGE_CONTAINER — Container name
 *   AZURE_STORAGE_SAS       — (optional) SAS token (alternative to key)
 *
 * API version: 2024-08-04 (latest stable as of 2024)
 */

#ifndef AZURE_BLOB_H
#define AZURE_BLOB_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ----------------------------------------------------------------
 * Constants
 * ---------------------------------------------------------------- */

#define AZURE_API_VERSION       "2024-08-04"
#define AZURE_PAGE_SIZE         512        /* Page blob alignment */
#define AZURE_MAX_PAGE_WRITE    (4*1024*1024)  /* 4 MiB max per Put Page */
#define AZURE_MAX_BLOCK_SIZE    (100*1024*1024) /* 100 MiB max per block */
#define AZURE_MAX_PAGE_BLOB_SIZE ((int64_t)8*1024*1024*1024*1024) /* 8 TiB */

/* Retry configuration */
#define AZURE_MAX_RETRIES       5
#define AZURE_RETRY_BASE_MS     500
#define AZURE_RETRY_MAX_MS      30000

/* ----------------------------------------------------------------
 * Error types
 * ---------------------------------------------------------------- */

typedef enum {
    AZURE_OK = 0,
    AZURE_ERR_HTTP,           /* Non-retryable HTTP error (4xx) */
    AZURE_ERR_TRANSIENT,      /* Retryable server error (5xx, timeout) */
    AZURE_ERR_AUTH,            /* Authentication failure */
    AZURE_ERR_NOT_FOUND,      /* 404 — blob/container not found */
    AZURE_ERR_CONFLICT,       /* 409 — lease conflict, blob exists, etc */
    AZURE_ERR_PRECONDITION,   /* 412 — precondition failed */
    AZURE_ERR_CURL,           /* libcurl-level failure */
    AZURE_ERR_OPENSSL,        /* OpenSSL failure */
    AZURE_ERR_XML_PARSE,      /* Failed to parse Azure error XML */
    AZURE_ERR_INVALID_ARG,    /* Bad argument to our function */
    AZURE_ERR_ALLOC,          /* Memory allocation failure */
} azure_err_t;

/* Structured error from Azure REST API */
typedef struct {
    azure_err_t code;
    long        http_status;
    char        error_code[128];   /* Azure error code string, e.g. "ServerBusy" */
    char        error_message[512]; /* Azure error message */
    char        request_id[64];    /* x-ms-request-id for debugging */
} azure_error_t;

/* ----------------------------------------------------------------
 * Response buffer — accumulates HTTP response body
 * ---------------------------------------------------------------- */

typedef struct {
    uint8_t *data;
    size_t   size;
    size_t   capacity;
} azure_buffer_t;

void azure_buffer_init(azure_buffer_t *buf);
void azure_buffer_free(azure_buffer_t *buf);

/* ----------------------------------------------------------------
 * Client context
 * ---------------------------------------------------------------- */

typedef struct {
    char  account[256];
    char  key_b64[256];       /* Base64-encoded account key */
    uint8_t key_raw[64];     /* Decoded binary key */
    size_t  key_raw_len;
    char  container[256];
    char  sas_token[2048];    /* SAS token if using SAS auth */
    int   use_sas;            /* 1 = use SAS, 0 = use Shared Key */
    void *curl_handle;        /* CURL* — opaque to keep header clean */
} azure_client_t;

/* Initialize/cleanup. Call once. */
azure_err_t azure_client_init(azure_client_t *client);
void        azure_client_cleanup(azure_client_t *client);

/* Build the base URL for a blob */
void azure_blob_url(const azure_client_t *client, const char *blob_name,
                    char *url_buf, size_t url_buf_size);

/* ----------------------------------------------------------------
 * Authentication — HMAC-SHA256 Shared Key
 * ---------------------------------------------------------------- */

/*
 * Build the StringToSign and compute Authorization header value.
 *
 * The StringToSign for Blob service (version 2009-09-19+) is:
 *   VERB\n
 *   Content-Encoding\n
 *   Content-Language\n
 *   Content-Length\n
 *   Content-MD5\n
 *   Content-Type\n
 *   Date\n
 *   If-Modified-Since\n
 *   If-Match\n
 *   If-None-Match\n
 *   If-Unmodified-Since\n
 *   Range\n
 *   CanonicalizedHeaders\n     (x-ms-* headers, sorted, lowercase)
 *   CanonicalizedResource       (/account/container/blob?comp=X&...)
 */
azure_err_t azure_auth_sign(
    const azure_client_t *client,
    const char *method,           /* "GET", "PUT", "DELETE", "HEAD" */
    const char *path,             /* "/container/blob" */
    const char *query,            /* "comp=page&..." or NULL */
    const char *content_length,   /* "512" or "" */
    const char *content_type,     /* "application/octet-stream" or "" */
    const char *range,            /* "bytes=0-511" or "" */
    const char *const *x_ms_headers, /* NULL-terminated array: "x-ms-date:...", ... */
    char *auth_header,            /* Output: "SharedKey account:sig" */
    size_t auth_header_size
);

/* Format current UTC time as RFC 1123 */
void azure_rfc1123_time(char *buf, size_t buf_size);

/* Base64 encode/decode */
int azure_base64_encode(const uint8_t *input, size_t input_len,
                        char *output, size_t output_size);
int azure_base64_decode(const char *input, uint8_t *output, size_t output_size,
                        size_t *output_len);

/* HMAC-SHA256 */
azure_err_t azure_hmac_sha256(const uint8_t *key, size_t key_len,
                              const uint8_t *data, size_t data_len,
                              uint8_t *out, size_t *out_len);

/* ----------------------------------------------------------------
 * Page Blob Operations
 * ---------------------------------------------------------------- */

/* Create a page blob with the given size (must be 512-byte aligned) */
azure_err_t azure_page_blob_create(
    azure_client_t *client,
    const char *blob_name,
    int64_t blob_size,
    azure_error_t *err
);

/* Write pages to a page blob. offset and length must be 512-byte aligned.
 * length must be <= 4 MiB. */
azure_err_t azure_page_blob_write(
    azure_client_t *client,
    const char *blob_name,
    int64_t offset,
    const uint8_t *data,
    size_t length,
    azure_error_t *err
);

/* Read data from a page blob using Range header.
 * offset and length can be any values (Range reads don't require alignment). */
azure_err_t azure_page_blob_read(
    azure_client_t *client,
    const char *blob_name,
    int64_t offset,
    size_t length,
    azure_buffer_t *out,
    azure_error_t *err
);

/* Get blob properties (Content-Length, lease state, etc.) */
azure_err_t azure_blob_get_properties(
    azure_client_t *client,
    const char *blob_name,
    int64_t *content_length,   /* out: blob size */
    char *lease_state,         /* out: buffer for lease state, >=32 bytes */
    char *lease_status,        /* out: buffer for lease status, >=32 bytes */
    azure_error_t *err
);

/* Resize a page blob */
azure_err_t azure_page_blob_resize(
    azure_client_t *client,
    const char *blob_name,
    int64_t new_size,          /* Must be 512-byte aligned */
    azure_error_t *err
);

/* ----------------------------------------------------------------
 * Block Blob Operations
 * ---------------------------------------------------------------- */

/* Upload a block blob (entire content at once, max 5000 MiB for single put,
 * but we'll use the simple Put Blob which handles up to 5000 MiB in API 2024+) */
azure_err_t azure_block_blob_upload(
    azure_client_t *client,
    const char *blob_name,
    const uint8_t *data,
    size_t length,
    const char *content_type,  /* "application/octet-stream" or NULL */
    azure_error_t *err
);

/* Download a block blob entirely */
azure_err_t azure_block_blob_download(
    azure_client_t *client,
    const char *blob_name,
    azure_buffer_t *out,
    azure_error_t *err
);

/* Delete any blob */
azure_err_t azure_blob_delete(
    azure_client_t *client,
    const char *blob_name,
    azure_error_t *err
);

/* ----------------------------------------------------------------
 * Lease Operations
 * ---------------------------------------------------------------- */

typedef enum {
    AZURE_LEASE_ACQUIRE,
    AZURE_LEASE_RENEW,
    AZURE_LEASE_RELEASE,
    AZURE_LEASE_BREAK,
} azure_lease_action_t;

/* Acquire a lease. duration_secs: 15-60, or -1 for infinite.
 * lease_id_out must be >= 64 bytes. */
azure_err_t azure_lease_acquire(
    azure_client_t *client,
    const char *blob_name,
    int duration_secs,
    char *lease_id_out,
    size_t lease_id_size,
    azure_error_t *err
);

/* Renew an existing lease */
azure_err_t azure_lease_renew(
    azure_client_t *client,
    const char *blob_name,
    const char *lease_id,
    azure_error_t *err
);

/* Release a lease */
azure_err_t azure_lease_release(
    azure_client_t *client,
    const char *blob_name,
    const char *lease_id,
    azure_error_t *err
);

/* Break a lease. break_period_secs: 0-60, or -1 for immediate.
 * Returns remaining lease time in *remaining_secs. */
azure_err_t azure_lease_break(
    azure_client_t *client,
    const char *blob_name,
    int break_period_secs,
    int *remaining_secs,
    azure_error_t *err
);

/* ----------------------------------------------------------------
 * Error handling
 * ---------------------------------------------------------------- */

/* Parse Azure error XML from response body */
azure_err_t azure_parse_error_xml(const char *xml, size_t xml_len,
                                  azure_error_t *err);

/* Classify HTTP status code as transient or permanent */
int azure_is_transient_error(long http_status, const char *error_code);

/* Classify HTTP status into our error type */
azure_err_t azure_classify_http_error(long http_status, const char *error_code);

/* Execute with retry + exponential backoff */
typedef azure_err_t (*azure_operation_fn)(void *ctx, azure_error_t *err);
azure_err_t azure_retry_execute(azure_operation_fn fn, void *ctx,
                                azure_error_t *err);

/* Sleep with jitter for retry backoff */
void azure_retry_sleep(int attempt);

/* Format error for logging */
const char *azure_err_str(azure_err_t code);

#ifdef __cplusplus
}
#endif

#endif /* AZURE_BLOB_H */
