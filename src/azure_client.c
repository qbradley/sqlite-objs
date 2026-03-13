/*
 * azure_client.c — Production Azure Blob Storage REST API client
 *
 * Implements all azure_ops_t vtable functions for page blobs, block blobs,
 * leases, and common blob operations. Every HTTP call goes through a
 * unified request executor with built-in retry logic (5 retries,
 * exponential backoff, Retry-After support).
 *
 * Evolved from research/azure-poc/azure_blob.c into production quality.
 *
 * Dependencies: libcurl, OpenSSL (via azure_auth.c)
 * Part of the sqliteObjs project. License: MIT
 */

#include "azure_client_impl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <limits.h>
#include <stdint.h>
#include <curl/curl.h>

/* ================================================================
 * Secure memory zeroing — guaranteed not to be optimized away (S-C4)
 *
 * Uses a volatile function pointer to prevent the compiler from
 * recognizing and eliminating the memset call as a dead store.
 * This is the most portable approach across all C compilers.
 * ================================================================ */
static void *(*volatile secure_memset)(void *, int, size_t) = memset;
#define secure_zero(ptr, len) secure_memset((ptr), 0, (len))

/* ================================================================
 * Thread-safe random for retry jitter (S-H4)
 * ================================================================ */
#if defined(__APPLE__) || defined(__FreeBSD__)
#include <stdlib.h>  /* arc4random_uniform */
#else
#include <sys/random.h>  /* getrandom on Linux */
#endif

/* ================================================================
 * Safe curl_slist_append macro (S-H5)
 *
 * curl_slist_append returns NULL on allocation failure.
 * When it fails, the previous list has already been freed
 * internally, so we must set our pointer to NULL to avoid
 * double-free, then jump to cleanup.
 * ================================================================ */
#define SLIST_APPEND(list, str) do { \
    struct curl_slist *_tmp = curl_slist_append(list, str); \
    if (!_tmp) { list = NULL; goto cleanup; } \
    list = _tmp; \
} while(0)

/* ================================================================
 * Safe long-to-int clamping for HTTP status codes (S-H7)
 * ================================================================ */
static inline int clamp_http_status(long s) {
    if (s > INT_MAX) return INT_MAX;
    if (s < INT_MIN) return INT_MIN;
    return (int)s;
}

/* ================================================================
 * Debug timing — opt-in via SQLITE_OBJS_DEBUG_TIMING=1 env var
 * ================================================================ */

static int g_az_debug_timing = -1;

static int az_debug_timing(void) {
    if (g_az_debug_timing < 0) {
        const char *val = getenv("SQLITE_OBJS_DEBUG_TIMING");
        g_az_debug_timing = (val && val[0] == '1') ? 1 : 0;
    }
    return g_az_debug_timing;
}

static double az_time_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

/* Per-request stats for debug timing */
static int g_http_request_count = 0;
static int g_tls_reuse_count = 0;
static int g_tls_new_count = 0;

/* ================================================================
 * Buffer management (init/free are inline in azure_client.h)
 * ================================================================ */

int azure_buffer_append(azure_buffer_t *buf, const uint8_t *data, size_t len)
{
    /* S-C1: overflow check for buf->size + len */
    if (len > SIZE_MAX - buf->size) return -1;
    size_t needed = buf->size + len;

    if (needed > buf->capacity) {
        size_t new_cap = (buf->capacity == 0) ? 4096 : buf->capacity;
        while (new_cap < needed) {
            if (new_cap > SIZE_MAX / 2) { new_cap = needed; break; }
            new_cap *= 2;
        }
        uint8_t *new_data = realloc(buf->data, new_cap);
        if (!new_data) return -1;
        buf->data = new_data;
        buf->capacity = new_cap;
    }
    memcpy(buf->data + buf->size, data, len);
    buf->size += len;
    return 0;
}

/* ================================================================
 * libcurl callbacks
 * ================================================================ */

/* Write callback: accumulates response body into azure_buffer_t */
static size_t curl_write_cb(void *contents, size_t size, size_t nmemb,
                            void *userp)
{
    /* S-C1: overflow check for size * nmemb */
    if (size != 0 && nmemb > SIZE_MAX / size) return 0;
    size_t real_size = size * nmemb;
    azure_buffer_t *buf = (azure_buffer_t *)userp;
    if (azure_buffer_append(buf, (const uint8_t *)contents, real_size) != 0)
        return 0;  /* Signal error to curl */
    return real_size;
}

/* Header callback: captures specific response headers we need */
static size_t curl_header_cb(char *buffer, size_t size, size_t nitems,
                             void *userp)
{
    size_t len = size * nitems;
    azure_response_headers_t *h = (azure_response_headers_t *)userp;

    char *colon = memchr(buffer, ':', len);
    if (!colon) return len;

    size_t name_len = (size_t)(colon - buffer);
    char *value = colon + 1;
    while (*value == ' ') value++;

    /* Strip trailing \r\n */
    size_t value_len = len - (size_t)(value - buffer);
    while (value_len > 0 &&
           (value[value_len - 1] == '\r' || value[value_len - 1] == '\n'))
        value_len--;

    /* Helper macro: compare header name (case-insensitive) and copy value */
    #define CAPTURE_HEADER(hdr_name, hdr_name_len, dest) \
        if (name_len == (hdr_name_len) && \
            strncasecmp(buffer, (hdr_name), name_len) == 0) { \
            size_t cpy = value_len < sizeof(dest) - 1 \
                         ? value_len : sizeof(dest) - 1; \
            memcpy((dest), value, cpy); \
            (dest)[cpy] = '\0'; \
            return len; \
        }

    CAPTURE_HEADER("x-ms-lease-id",     13, h->lease_id)
    CAPTURE_HEADER("x-ms-lease-state",  16, h->lease_state)
    CAPTURE_HEADER("x-ms-lease-status", 17, h->lease_status)
    CAPTURE_HEADER("x-ms-request-id",   15, h->request_id)
    CAPTURE_HEADER("x-ms-error-code",   15, h->error_code)

    #undef CAPTURE_HEADER

    /* Content-Length → int64_t */
    if (name_len == 14 &&
        strncasecmp(buffer, "Content-Length", name_len) == 0) {
        char tmp[32];
        size_t cpy = value_len < sizeof(tmp) - 1 ? value_len : sizeof(tmp) - 1;
        memcpy(tmp, value, cpy);
        tmp[cpy] = '\0';
        h->content_length = strtoll(tmp, NULL, 10);
        return len;
    }

    /* x-ms-lease-time → int (break lease remaining seconds) */
    if (name_len == 15 &&
        strncasecmp(buffer, "x-ms-lease-time", name_len) == 0) {
        char tmp[32];
        size_t cpy = value_len < sizeof(tmp) - 1 ? value_len : sizeof(tmp) - 1;
        memcpy(tmp, value, cpy);
        tmp[cpy] = '\0';
        h->lease_time = atoi(tmp);
        return len;
    }

    /* Retry-After → int seconds */
    if (name_len == 11 &&
        strncasecmp(buffer, "Retry-After", name_len) == 0) {
        char tmp[32];
        size_t cpy = value_len < sizeof(tmp) - 1 ? value_len : sizeof(tmp) - 1;
        memcpy(tmp, value, cpy);
        tmp[cpy] = '\0';
        h->retry_after = atoi(tmp);
        return len;
    }

    return len;
}

/* ================================================================
 * URI percent-encoding helper (RFC 3986) — S-H12
 *
 * Encodes all characters except unreserved (A-Z a-z 0-9 - . _ ~)
 * and forward slash (/) which is a path separator.
 * Returns number of bytes written (excluding NUL), or -1 on overflow.
 * ================================================================ */
static int uri_encode(const char *input, char *output, size_t output_size)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t j = 0;
    for (size_t i = 0; input[i]; i++) {
        unsigned char c = (unsigned char)input[i];
        int unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                         (c >= '0' && c <= '9') ||
                         c == '-' || c == '.' || c == '_' || c == '~' ||
                         c == '/';
        if (unreserved) {
            if (j + 1 >= output_size) return -1;
            output[j++] = (char)c;
        } else {
            if (j + 3 >= output_size) return -1;
            output[j++] = '%';
            output[j++] = hex[c >> 4];
            output[j++] = hex[c & 0x0F];
        }
    }
    if (j >= output_size) return -1;
    output[j] = '\0';
    return (int)j;
}

/* ================================================================
 * URL construction
 *
 * Format: https://<account>.blob.core.windows.net/<container>/<blob>
 * OR (for custom endpoint): <endpoint>/<container>/<blob>
 * ================================================================ */

static void build_blob_url(const azure_client_t *client,
                           const char *blob_name,
                           char *url_buf, size_t url_buf_size)
{
    /* S-H12: percent-encode blob name for safe URL construction */
    char encoded_name[2048];
    if (uri_encode(blob_name, encoded_name, sizeof(encoded_name)) < 0) {
        /* Fallback: use raw name if encoding buffer too small */
        snprintf(encoded_name, sizeof(encoded_name), "%s", blob_name);
    }

    if (client->endpoint[0]) {
        /* Custom endpoint (e.g., Azurite): http://127.0.0.1:10000 
         * Build URL as: endpoint/account/container/blob */
        snprintf(url_buf, url_buf_size, "%s/%s/%s/%s",
                 client->endpoint, client->account, client->container, encoded_name);
    } else {
        /* Default Azure endpoint */
        snprintf(url_buf, url_buf_size,
                 "https://%s.blob.core.windows.net/%s/%s",
                 client->account, client->container, encoded_name);
    }
}

/* ================================================================
 * Core HTTP request execution (single attempt, no retry)
 *
 * This is the foundation that all operations use. It:
 * 1. Builds the URL with query params and optional SAS token
 * 2. Constructs headers including auth
 * 3. Executes via libcurl
 * 4. Parses response and classifies errors
 * ================================================================ */

static azure_err_t execute_single(
    azure_client_t *client,
    const char *method,
    const char *blob_name,
    const char *query,
    const char *const *extra_x_ms,
    const char *content_type,
    const uint8_t *body,
    size_t body_len,
    const char *range_header,
    azure_buffer_t *response_body,
    azure_response_headers_t *resp_headers,
    azure_error_t *err)
{
    CURL *curl = (CURL *)client->curl_handle;
    CURLcode res;

    /* Build URL with enough room for base + query + SAS token */
    char url[4096];
    build_blob_url(client, blob_name, url, sizeof(url));
    size_t url_len = strlen(url);

    /* Append query parameters (bounds-checked) */
    if (query && *query) {
        int n = snprintf(url + url_len, sizeof(url) - url_len, "?%s", query);
        if (n > 0) url_len += (size_t)n;
    }

    /* Append SAS token (bounds-checked) */
    if (client->use_sas) {
        const char *sep = (query && *query) ? "&" : "?";
        int n = snprintf(url + url_len, sizeof(url) - url_len,
                         "%s%s", sep, client->sas_token);
        if (n > 0) url_len += (size_t)n;
        (void)url_len;
    }

    /* Build x-ms-date and x-ms-version headers */
    char date_buf[64];
    azure_rfc1123_time(date_buf, sizeof(date_buf));

    char date_header[128];
    snprintf(date_header, sizeof(date_header), "x-ms-date:%s", date_buf);

    char version_header[64];
    snprintf(version_header, sizeof(version_header),
             "x-ms-version:%s", AZURE_API_VERSION);

    /* Collect all x-ms-* headers for auth signing */
    const char *all_x_ms[32];
    int x_ms_count = 0;
    all_x_ms[x_ms_count++] = date_header;
    all_x_ms[x_ms_count++] = version_header;
    if (extra_x_ms) {
        for (int i = 0; extra_x_ms[i] && x_ms_count < 30; i++)
            all_x_ms[x_ms_count++] = extra_x_ms[i];
    }
    all_x_ms[x_ms_count] = NULL;

    /* Content-Length string for auth signing */
    char content_length_str[32] = "";
    if (body_len > 0)
        snprintf(content_length_str, sizeof(content_length_str), "%zu", body_len);

    /* Shared Key auth signing (skip for SAS) */
    char auth_header[512] = "";
    if (!client->use_sas) {
        char path[1024];
        /* Canonicalized resource format differs between production and Azurite:
         * - Production Azure: /account/container/blob
         * - Azurite: /account/account/container/blob (account doubled due to emulator quirk)
         *   This is because Azurite prepends the account from the URL path to the configured
         *   account name internally. See Azurite GitHub issues #223, #647 for details.
         */
        if (client->endpoint[0]) {
            /* Custom endpoint (Azurite): double the account name */
            snprintf(path, sizeof(path), "/%s/%s/%s",
                     client->account, client->container, blob_name);
        } else {
            /* Production Azure: account will be prepended in azure_auth.c */
            snprintf(path, sizeof(path), "/%s/%s",
                     client->container, blob_name);
        }

        azure_err_t rc = azure_auth_sign_request(
            client, method, path, query,
            content_length_str,
            content_type ? content_type : "",
            range_header ? range_header : "",
            (const char *const *)all_x_ms,
            auth_header, sizeof(auth_header));
        if (rc != AZURE_OK) {
            err->code = rc;
            return rc;
        }
    }

    /* Reset curl handle for reuse (preserves connection) */
    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url);

    /* HTTP method */
    if (strcmp(method, "PUT") == 0) {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
    } else if (strcmp(method, "DELETE") == 0) {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    } else if (strcmp(method, "HEAD") == 0) {
        curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    }
    /* GET is the default */

    /* Build curl header list (S-H5: check every curl_slist_append) */
    struct curl_slist *headers = NULL;

    char h_date[256];
    snprintf(h_date, sizeof(h_date), "x-ms-date: %s", date_buf);
    SLIST_APPEND(headers, h_date);

    char h_version[128];
    snprintf(h_version, sizeof(h_version), "x-ms-version: %s",
             AZURE_API_VERSION);
    SLIST_APPEND(headers, h_version);

    if (auth_header[0]) {
        char h_auth[600];
        snprintf(h_auth, sizeof(h_auth), "Authorization: %s", auth_header);
        SLIST_APPEND(headers, h_auth);
    }

    if (content_type && *content_type) {
        char h_ct[256];
        snprintf(h_ct, sizeof(h_ct), "Content-Type: %s", content_type);
        SLIST_APPEND(headers, h_ct);
    } else {
        /* Disable curl's automatic Content-Type header to avoid signature mismatch */
        SLIST_APPEND(headers, "Content-Type:");
    }

    if (body_len > 0) {
        char h_cl[64];
        snprintf(h_cl, sizeof(h_cl), "Content-Length: %zu", body_len);
        SLIST_APPEND(headers, h_cl);
    } else if (strcmp(method, "PUT") == 0) {
        SLIST_APPEND(headers, "Content-Length: 0");
    }

    if (range_header && *range_header) {
        char h_range[256];
        snprintf(h_range, sizeof(h_range), "x-ms-range: %s", range_header);
        SLIST_APPEND(headers, h_range);
    }

    /* Extra x-ms-* headers (blob type, lease action, etc.) */
    if (extra_x_ms) {
        for (int i = 0; extra_x_ms[i]; i++) {
            char h_extra[512];
            const char *colon = strchr(extra_x_ms[i], ':');
            if (colon) {
                size_t nlen = (size_t)(colon - extra_x_ms[i]);
                snprintf(h_extra, sizeof(h_extra), "%.*s: %s",
                         (int)nlen, extra_x_ms[i], colon + 1);
            } else {
                snprintf(h_extra, sizeof(h_extra), "%s", extra_x_ms[i]);
            }
            SLIST_APPEND(headers, h_extra);
        }
    }

    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    /* Request body for PUT */
    if (body && body_len > 0) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE,
                         (curl_off_t)body_len);
    } else if (strcmp(method, "PUT") == 0) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, 0L);
    }

    /* Response body callback */
    azure_buffer_t local_buf;
    azure_buffer_init(&local_buf);
    azure_buffer_t *body_buf = response_body ? response_body : &local_buf;
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, body_buf);

    /* Response header callback */
    azure_response_headers_t local_headers;
    memset(&local_headers, 0, sizeof(local_headers));
    local_headers.retry_after = -1;  /* -1 = not present */
    azure_response_headers_t *rh = resp_headers ? resp_headers : &local_headers;
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, curl_header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, rh);

    /* Timeouts */
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);

    /* TCP keep-alive for connection reuse */
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPIDLE, 60L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPINTVL, 30L);

    /* Execute */
    double req_t0 = 0;
    if (az_debug_timing()) req_t0 = az_time_ms();

    res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    headers = NULL;

    if (az_debug_timing()) {
        double req_elapsed = az_time_ms() - req_t0;
        g_http_request_count++;

        /* Check connection reuse: connect_time==0 means reused */
        double connect_time = 0, appconnect_time = 0, namelookup_time = 0;
        curl_easy_getinfo(curl, CURLINFO_CONNECT_TIME, &connect_time);
        curl_easy_getinfo(curl, CURLINFO_APPCONNECT_TIME, &appconnect_time);
        curl_easy_getinfo(curl, CURLINFO_NAMELOOKUP_TIME, &namelookup_time);

        int reused = (connect_time == 0.0) ? 1 : 0;
        if (reused) g_tls_reuse_count++;
        else g_tls_new_count++;

        fprintf(stderr, "[TIMING] HTTP %s %s: %.1fms (dns=%.1fms tcp=%.1fms tls=%.1fms %s) "
                "body=%zu req#%d\n",
                method, blob_name, req_elapsed,
                namelookup_time * 1000.0,
                connect_time * 1000.0,
                appconnect_time * 1000.0,
                reused ? "REUSED" : "NEW",
                body_len, g_http_request_count);
    }

    if (res != CURLE_OK) {
        err->code = AZURE_ERR_CURL;
        snprintf(err->error_message, sizeof(err->error_message),
                 "curl error: %s", curl_easy_strerror(res));
        if (!response_body) azure_buffer_free(&local_buf);

        /* Timeouts are transient */
        if (res == CURLE_OPERATION_TIMEDOUT || res == CURLE_COULDNT_CONNECT)
            return AZURE_ERR_TRANSIENT;
        return AZURE_ERR_CURL;
    }

    /* HTTP status (S-H7: clamp long → int to avoid narrowing) */
    long http_status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
    err->http_status = clamp_http_status(http_status);
    strncpy(err->request_id, rh->request_id, sizeof(err->request_id) - 1);

    if (http_status >= 400) {
        /* Parse error XML from response body */
        if (body_buf->size > 0)
            azure_parse_error_xml((const char *)body_buf->data,
                                  body_buf->size, err);
        /* x-ms-error-code header takes precedence if XML had no code */
        if (rh->error_code[0] && !err->error_code[0])
            strncpy(err->error_code, rh->error_code,
                    sizeof(err->error_code) - 1);

        err->code = azure_classify_http_error(http_status, err->error_code);
        if (!response_body) azure_buffer_free(&local_buf);
        return err->code;
    }

    if (!response_body) azure_buffer_free(&local_buf);
    return AZURE_OK;

/* S-H5: cleanup target for SLIST_APPEND allocation failures */
cleanup:
    if (headers) curl_slist_free_all(headers);
    err->code = AZURE_ERR_NOMEM;
    snprintf(err->error_message, sizeof(err->error_message),
             "curl_slist_append allocation failed");
    return AZURE_ERR_NOMEM;
}

/* ================================================================
 * HTTP request executor with built-in retry
 *
 * Wraps execute_single with retry logic:
 *   - Up to AZURE_MAX_RETRIES retries on transient/throttled errors
 *   - Exponential backoff with jitter
 *   - Retry-After header support (from 429/503 responses)
 *   - Permanent errors returned immediately (no retry)
 *
 * All vtable functions call this instead of execute_single directly.
 * ================================================================ */

static azure_err_t execute_with_retry(
    azure_client_t *client,
    const char *method,
    const char *blob_name,
    const char *query,
    const char *const *extra_x_ms,
    const char *content_type,
    const uint8_t *body,
    size_t body_len,
    const char *range_header,
    azure_buffer_t *response_body,
    azure_response_headers_t *resp_headers,
    azure_error_t *err)
{
    azure_response_headers_t rh;
    azure_err_t rc = AZURE_OK;

    for (int attempt = 0; attempt <= AZURE_MAX_RETRIES; attempt++) {
        memset(&rh, 0, sizeof(rh));
        rh.retry_after = -1;
        memset(err, 0, sizeof(*err));

        /* Reset response buffer for retry (keep allocated memory) */
        if (response_body)
            response_body->size = 0;

        rc = execute_single(client, method, blob_name, query, extra_x_ms,
                            content_type, body, body_len, range_header,
                            response_body, &rh, err);

        if (rc == AZURE_OK) {
            if (resp_headers) *resp_headers = rh;
            return AZURE_OK;
        }

        if (!azure_is_retryable(rc)) {
            if (resp_headers) *resp_headers = rh;
            return rc;
        }

        /* Transient/throttled error — retry with backoff */
        if (attempt < AZURE_MAX_RETRIES) {
            int delay_ms = azure_compute_retry_delay(attempt, rh.retry_after);
            fprintf(stderr, "[sqlite-objs] %s %s: %s (HTTP %d) — "
                    "retry %d/%d in %dms\n",
                    method, blob_name, azure_err_str(rc),
                    err->http_status, attempt + 1, AZURE_MAX_RETRIES,
                    delay_ms);
            azure_retry_sleep_ms(delay_ms);
        }
    }

    fprintf(stderr, "[sqlite-objs] %s %s: all %d retries exhausted\n",
            method, blob_name, AZURE_MAX_RETRIES);

    if (resp_headers) *resp_headers = rh;
    return rc;
}

/* ================================================================
 * PAGE BLOB OPERATIONS
 * ================================================================ */

/*
 * Create a page blob.
 * PUT with x-ms-blob-type: PageBlob, x-ms-blob-content-length: <size>
 * Size must be 512-byte aligned. Content-Length: 0 (no body).
 */
static azure_err_t az_page_blob_create(void *ctx, const char *name,
                                       int64_t size, azure_error_t *err)
{
    azure_client_t *c = (azure_client_t *)ctx;

    if (size < 0 || size % AZURE_PAGE_SIZE != 0) {
        err->code = AZURE_ERR_INVALID_ARG;
        snprintf(err->error_message, sizeof(err->error_message),
                 "Page blob size must be non-negative and 512-byte aligned, "
                 "got %lld", (long long)size);
        return AZURE_ERR_INVALID_ARG;
    }

    char size_header[64];
    snprintf(size_header, sizeof(size_header),
             "x-ms-blob-content-length:%lld", (long long)size);

    const char *extra[] = {
        "x-ms-blob-type:PageBlob",
        size_header,
        NULL
    };

    return execute_with_retry(c, "PUT", name, NULL,
                              extra, NULL, NULL, 0, NULL,
                              NULL, NULL, err);
}

/*
 * Write pages to a page blob.
 * PUT ?comp=page with x-ms-page-write: update, x-ms-range: bytes=start-end
 * Offset and length must be 512-byte aligned. Max 4 MiB per write.
 */
static azure_err_t az_page_blob_write(void *ctx, const char *name,
                                      int64_t offset, const uint8_t *data,
                                      size_t len, const char *lease_id,
                                      azure_error_t *err)
{
    azure_client_t *c = (azure_client_t *)ctx;

    if (offset < 0 || offset % AZURE_PAGE_SIZE != 0 ||
        len % AZURE_PAGE_SIZE != 0) {
        err->code = AZURE_ERR_INVALID_ARG;
        snprintf(err->error_message, sizeof(err->error_message),
                 "Page write offset (%lld) and length (%zu) must be "
                 "512-byte aligned", (long long)offset, len);
        return AZURE_ERR_INVALID_ARG;
    }
    if (len > AZURE_MAX_PAGE_WRITE) {
        err->code = AZURE_ERR_INVALID_ARG;
        snprintf(err->error_message, sizeof(err->error_message),
                 "Page write length %zu exceeds 4 MiB maximum", len);
        return AZURE_ERR_INVALID_ARG;
    }
    if (!data || len == 0) {
        err->code = AZURE_ERR_INVALID_ARG;
        snprintf(err->error_message, sizeof(err->error_message),
                 "Page write data must be non-NULL with length > 0");
        return AZURE_ERR_INVALID_ARG;
    }

    char range_header[128];
    snprintf(range_header, sizeof(range_header), "x-ms-range:bytes=%lld-%lld",
             (long long)offset, (long long)(offset + (int64_t)len - 1));

    char lease_header[128];
    const char *extra_with_lease[] = {
        "x-ms-page-write:update",
        range_header,
        lease_header,
        NULL
    };
    const char *extra_no_lease[] = {
        "x-ms-page-write:update",
        range_header,
        NULL
    };

    const char **extra;
    if (lease_id && lease_id[0] != '\0') {
        snprintf(lease_header, sizeof(lease_header),
                 "x-ms-lease-id:%s", lease_id);
        extra = extra_with_lease;
    } else {
        extra = extra_no_lease;
    }

    return execute_with_retry(c, "PUT", name, "comp=page",
                              extra, "application/octet-stream",
                              data, len, NULL,
                              NULL, NULL, err);
}

/*
 * Read from a page blob using Range header.
 * GET with x-ms-range: bytes=start-end
 * Range reads do NOT require 512-byte alignment.
 */
static azure_err_t az_page_blob_read(void *ctx, const char *name,
                                     int64_t offset, size_t len,
                                     azure_buffer_t *out,
                                     azure_error_t *err)
{
    azure_client_t *c = (azure_client_t *)ctx;

    if (offset < 0 || len == 0 || !out) {
        err->code = AZURE_ERR_INVALID_ARG;
        snprintf(err->error_message, sizeof(err->error_message),
                 "Invalid read parameters: offset=%lld len=%zu",
                 (long long)offset, len);
        return AZURE_ERR_INVALID_ARG;
    }

    char range_header[128];
    snprintf(range_header, sizeof(range_header), "x-ms-range:bytes=%lld-%lld",
             (long long)offset, (long long)(offset + (int64_t)len - 1));

    const char *extra[] = {
        range_header,
        NULL
    };

    return execute_with_retry(c, "GET", name, NULL,
                              extra, NULL, NULL, 0, NULL,
                              out, NULL, err);
}

/*
 * Resize a page blob.
 * PUT ?comp=properties with x-ms-blob-content-length: <new_size>
 * New size must be 512-byte aligned.
 */
static azure_err_t az_page_blob_resize(void *ctx, const char *name,
                                       int64_t new_size,
                                       const char *lease_id,
                                       azure_error_t *err)
{
    azure_client_t *c = (azure_client_t *)ctx;

    if (new_size < 0 || new_size % AZURE_PAGE_SIZE != 0) {
        err->code = AZURE_ERR_INVALID_ARG;
        snprintf(err->error_message, sizeof(err->error_message),
                 "Page blob resize must be 512-byte aligned, got %lld",
                 (long long)new_size);
        return AZURE_ERR_INVALID_ARG;
    }

    char size_header[64];
    snprintf(size_header, sizeof(size_header),
             "x-ms-blob-content-length:%lld", (long long)new_size);

    char lease_header[128];
    const char *extra_with_lease[] = {
        size_header,
        lease_header,
        NULL
    };
    const char *extra_no_lease[] = {
        size_header,
        NULL
    };

    const char **extra;
    if (lease_id && lease_id[0] != '\0') {
        snprintf(lease_header, sizeof(lease_header),
                 "x-ms-lease-id:%s", lease_id);
        extra = extra_with_lease;
    } else {
        extra = extra_no_lease;
    }

    return execute_with_retry(c, "PUT", name, "comp=properties",
                              extra, NULL, NULL, 0, NULL,
                              NULL, NULL, err);
}

/* ================================================================
 * BLOCK BLOB OPERATIONS
 * ================================================================ */

/*
 * Upload a block blob (simple Put Blob — entire content at once).
 * PUT with x-ms-blob-type: BlockBlob
 */
static azure_err_t az_block_blob_upload(void *ctx, const char *name,
                                        const uint8_t *data, size_t len,
                                        azure_error_t *err)
{
    azure_client_t *c = (azure_client_t *)ctx;

    const char *extra[] = {
        "x-ms-blob-type:BlockBlob",
        NULL
    };

    return execute_with_retry(c, "PUT", name, NULL,
                              extra, "application/octet-stream",
                              data, len, NULL,
                              NULL, NULL, err);
}

/*
 * Download a block blob entirely.
 * GET (full download, no range).
 */
static azure_err_t az_block_blob_download(void *ctx, const char *name,
                                          azure_buffer_t *out,
                                          azure_error_t *err)
{
    azure_client_t *c = (azure_client_t *)ctx;

    if (!out) {
        err->code = AZURE_ERR_INVALID_ARG;
        snprintf(err->error_message, sizeof(err->error_message),
                 "Output buffer must be non-NULL");
        return AZURE_ERR_INVALID_ARG;
    }

    return execute_with_retry(c, "GET", name, NULL,
                              NULL, NULL, NULL, 0, NULL,
                              out, NULL, err);
}

/* ================================================================
 * COMMON BLOB OPERATIONS
 * ================================================================ */

/*
 * Get blob properties via HEAD request.
 * Returns size, lease_state, lease_status.
 * Any output pointer may be NULL if not needed.
 */
static azure_err_t az_blob_get_properties(void *ctx, const char *name,
                                          int64_t *size,
                                          char *lease_state,
                                          char *lease_status,
                                          azure_error_t *err)
{
    azure_client_t *c = (azure_client_t *)ctx;
    azure_response_headers_t rh;

    azure_err_t rc = execute_with_retry(c, "HEAD", name, NULL,
                                        NULL, NULL, NULL, 0, NULL,
                                        NULL, &rh, err);
    if (rc == AZURE_OK) {
        if (size) *size = rh.content_length;
        if (lease_state) {
            strncpy(lease_state, rh.lease_state, 31);
            lease_state[31] = '\0';
        }
        if (lease_status) {
            strncpy(lease_status, rh.lease_status, 31);
            lease_status[31] = '\0';
        }
    }

    return rc;
}

/*
 * Delete a blob (any type).
 * DELETE request.
 */
static azure_err_t az_blob_delete(void *ctx, const char *name,
                                  azure_error_t *err)
{
    azure_client_t *c = (azure_client_t *)ctx;

    return execute_with_retry(c, "DELETE", name, NULL,
                              NULL, NULL, NULL, 0, NULL,
                              NULL, NULL, err);
}

/*
 * Check if a blob exists.
 * HEAD request: 2xx = exists, 404 = doesn't exist, others = error.
 */
static azure_err_t az_blob_exists(void *ctx, const char *name,
                                  int *exists, azure_error_t *err)
{
    if (!exists) {
        err->code = AZURE_ERR_INVALID_ARG;
        return AZURE_ERR_INVALID_ARG;
    }

    azure_err_t rc = az_blob_get_properties(ctx, name, NULL, NULL, NULL, err);
    if (rc == AZURE_OK) {
        *exists = 1;
        return AZURE_OK;
    }
    if (rc == AZURE_ERR_NOT_FOUND) {
        *exists = 0;
        memset(err, 0, sizeof(*err));
        return AZURE_OK;
    }
    return rc;
}

/* ================================================================
 * LEASE OPERATIONS
 *
 * All use: PUT ?comp=lease with x-ms-lease-action: <action>
 *
 * For SQLite locking:
 *   SHARED    → no lease (reads always work)
 *   RESERVED  → acquire 30s lease (blocks other writers)
 *   EXCLUSIVE → same lease from RESERVED
 *   Unlock    → release lease
 * ================================================================ */

/*
 * Acquire a lease on a blob.
 * duration_secs: 15-60 or -1 for infinite.
 * lease_id_out receives the Azure-assigned lease GUID.
 */
static azure_err_t az_lease_acquire(void *ctx, const char *name,
                                    int duration_secs, char *lease_id_out,
                                    size_t lease_id_size,
                                    azure_error_t *err)
{
    azure_client_t *c = (azure_client_t *)ctx;

    if (duration_secs != -1 && (duration_secs < 15 || duration_secs > 60)) {
        err->code = AZURE_ERR_INVALID_ARG;
        snprintf(err->error_message, sizeof(err->error_message),
                 "Lease duration must be 15-60 or -1, got %d", duration_secs);
        return AZURE_ERR_INVALID_ARG;
    }

    char duration_header[64];
    snprintf(duration_header, sizeof(duration_header),
             "x-ms-lease-duration:%d", duration_secs);

    const char *extra[] = {
        "x-ms-lease-action:acquire",
        duration_header,
        NULL
    };

    azure_response_headers_t rh;
    azure_err_t rc = execute_with_retry(c, "PUT", name, "comp=lease",
                                        extra, NULL, NULL, 0, NULL,
                                        NULL, &rh, err);
    if (rc == AZURE_OK && lease_id_out) {
        strncpy(lease_id_out, rh.lease_id, lease_id_size - 1);
        lease_id_out[lease_id_size - 1] = '\0';
    }

    return rc;
}

/*
 * Renew an existing lease.
 * Must provide the current lease_id.
 */
static azure_err_t az_lease_renew(void *ctx, const char *name,
                                  const char *lease_id,
                                  azure_error_t *err)
{
    azure_client_t *c = (azure_client_t *)ctx;

    if (!lease_id || !*lease_id) {
        err->code = AZURE_ERR_INVALID_ARG;
        snprintf(err->error_message, sizeof(err->error_message),
                 "lease_id must be non-empty for renewal");
        return AZURE_ERR_INVALID_ARG;
    }

    char lease_header[128];
    snprintf(lease_header, sizeof(lease_header),
             "x-ms-lease-id:%s", lease_id);

    const char *extra[] = {
        "x-ms-lease-action:renew",
        lease_header,
        NULL
    };

    return execute_with_retry(c, "PUT", name, "comp=lease",
                              extra, NULL, NULL, 0, NULL,
                              NULL, NULL, err);
}

/*
 * Release a lease.
 * Must provide the current lease_id.
 */
static azure_err_t az_lease_release(void *ctx, const char *name,
                                    const char *lease_id,
                                    azure_error_t *err)
{
    azure_client_t *c = (azure_client_t *)ctx;

    if (!lease_id || !*lease_id) {
        err->code = AZURE_ERR_INVALID_ARG;
        snprintf(err->error_message, sizeof(err->error_message),
                 "lease_id must be non-empty for release");
        return AZURE_ERR_INVALID_ARG;
    }

    char lease_header[128];
    snprintf(lease_header, sizeof(lease_header),
             "x-ms-lease-id:%s", lease_id);

    const char *extra[] = {
        "x-ms-lease-action:release",
        lease_header,
        NULL
    };

    return execute_with_retry(c, "PUT", name, "comp=lease",
                              extra, NULL, NULL, 0, NULL,
                              NULL, NULL, err);
}

/*
 * Break a lease (recovery path for stale leases).
 * break_period_secs: 0-60, or -1 for immediate.
 * remaining_secs receives the remaining lease time (may be NULL).
 */
static azure_err_t az_lease_break(void *ctx, const char *name,
                                  int break_period_secs,
                                  int *remaining_secs,
                                  azure_error_t *err)
{
    azure_client_t *c = (azure_client_t *)ctx;

    const char *extra[4];
    int idx = 0;
    extra[idx++] = "x-ms-lease-action:break";

    char period_header[64];
    if (break_period_secs >= 0) {
        snprintf(period_header, sizeof(period_header),
                 "x-ms-lease-break-period:%d", break_period_secs);
        extra[idx++] = period_header;
    }
    extra[idx] = NULL;

    azure_response_headers_t rh;
    azure_err_t rc = execute_with_retry(c, "PUT", name, "comp=lease",
                                        extra, NULL, NULL, 0, NULL,
                                        NULL, &rh, err);
    if (rc == AZURE_OK && remaining_secs)
        *remaining_secs = rh.lease_time;

    return rc;
}

/* ================================================================
 * BATCH PAGE BLOB WRITES — curl_multi parallel flush (Phase 2)
 *
 * Writes multiple page ranges concurrently using libcurl's multi
 * interface.  Each range becomes a separate PUT Page request driven
 * by a single-threaded event loop.
 *
 * Retry: failed ranges are retried up to 3 times with exponential
 * backoff.  Lease is renewed every 15 seconds during long flushes.
 *
 * Precondition: range data pointers are stable (btree mutex held
 * during xSync — see decision D17).
 * ================================================================ */

#define BATCH_MAX_RETRIES        3
#define BATCH_LEASE_RENEWAL_SEC  15

/*
 * Lazily initialize the persistent CURLM multi handle on the client.
 * The multi handle is reused across write_batch calls so that libcurl's
 * internal connection pool and TLS session cache persist between xSync
 * invocations.  This avoids TCP handshake + TLS negotiation overhead
 * on subsequent flushes.
 *
 * Thread-safety: this is safe because xSync is serialized by SQLite's
 * btree mutex (D17).  No concurrent calls to write_batch are possible.
 *
 * Pool tuning:
 *   CURLMOPT_MAX_HOST_CONNECTIONS = SQLITE_OBJS_MAX_PARALLEL_PUTS (32)
 *   CURLMOPT_MAXCONNECTS          = SQLITE_OBJS_MAX_PARALLEL_PUTS (32)
 */
static CURLM *ensure_multi_handle(azure_client_t *c)
{
    if (c->multi_handle) return (CURLM *)c->multi_handle;

    CURLM *multi = curl_multi_init();
    if (!multi) return NULL;

    curl_multi_setopt(multi, CURLMOPT_MAX_HOST_CONNECTIONS,
                      (long)SQLITE_OBJS_MAX_PARALLEL_PUTS);
    curl_multi_setopt(multi, CURLMOPT_MAXCONNECTS,
                      (long)SQLITE_OBJS_MAX_PARALLEL_PUTS);

    c->multi_handle = multi;
    return multi;
}

/* Per-request context for one range in a batch */
typedef struct {
    CURL                     *easy;
    struct curl_slist        *hdrs;
    azure_buffer_t            resp_body;
    azure_response_headers_t  resp_hdrs;
    int                       range_idx;
} batch_req_t;

/*
 * Configure one CURL easy handle for a Put Page request.
 * url must remain valid for the handle's lifetime.
 */
static azure_err_t batch_init_easy(
    azure_client_t *c,
    const char *url,
    const char *blob_name,
    const azure_page_range_t *range,
    const char *lease_id,
    batch_req_t *req)
{
    req->easy = curl_easy_init();
    if (!req->easy) return AZURE_ERR_NETWORK;

    req->hdrs = NULL;
    azure_buffer_init(&req->resp_body);
    memset(&req->resp_hdrs, 0, sizeof(req->resp_hdrs));
    req->resp_hdrs.retry_after = -1;

    curl_easy_setopt(req->easy, CURLOPT_URL, url);
    curl_easy_setopt(req->easy, CURLOPT_CUSTOMREQUEST, "PUT");

    /* Body — pointer stable during xSync (D17: btree mutex held) */
    curl_easy_setopt(req->easy, CURLOPT_POSTFIELDS, range->data);
    curl_easy_setopt(req->easy, CURLOPT_POSTFIELDSIZE_LARGE,
                     (curl_off_t)range->len);

    /* Timestamp for this request */
    char date_buf[64];
    azure_rfc1123_time(date_buf, sizeof(date_buf));

    char range_val[128];
    snprintf(range_val, sizeof(range_val), "bytes=%lld-%lld",
             (long long)range->offset,
             (long long)(range->offset + (int64_t)range->len - 1));

    /* ---- SharedKey auth signing ---- */
    char auth_hdr[512] = "";
    if (!c->use_sas) {
        char h_date[128], h_ver[64], h_pw[64], h_rng[192], h_lid[192];
        snprintf(h_date, sizeof(h_date), "x-ms-date:%s", date_buf);
        snprintf(h_ver, sizeof(h_ver), "x-ms-version:%s", AZURE_API_VERSION);
        snprintf(h_pw, sizeof(h_pw), "x-ms-page-write:update");
        snprintf(h_rng, sizeof(h_rng), "x-ms-range:%s", range_val);

        const char *xms[8];
        int n = 0;
        xms[n++] = h_date;
        if (lease_id && *lease_id) {
            snprintf(h_lid, sizeof(h_lid), "x-ms-lease-id:%s", lease_id);
            xms[n++] = h_lid;
        }
        xms[n++] = h_pw;
        xms[n++] = h_rng;
        xms[n++] = h_ver;
        xms[n] = NULL;

        char path[1024];
        if (c->endpoint[0]) {
            /* Azurite: account doubled in canonicalized resource (D12) */
            snprintf(path, sizeof(path), "/%s/%s/%s",
                     c->account, c->container, blob_name);
        } else {
            snprintf(path, sizeof(path), "/%s/%s",
                     c->container, blob_name);
        }

        char cl_str[32];
        snprintf(cl_str, sizeof(cl_str), "%zu", range->len);

        azure_err_t rc = azure_auth_sign_request(
            c, "PUT", path, "comp=page",
            cl_str, "application/octet-stream", "",
            (const char *const *)xms,
            auth_hdr, sizeof(auth_hdr));
        if (rc != AZURE_OK) {
            curl_easy_cleanup(req->easy);
            req->easy = NULL;
            return rc;
        }
    }

    /* ---- HTTP headers (each handle owns its own list) (S-H5: checked) ---- */
    char h[600];
    struct curl_slist *list = NULL;

    snprintf(h, sizeof(h), "x-ms-date: %s", date_buf);
    SLIST_APPEND(list, h);

    snprintf(h, sizeof(h), "x-ms-version: %s", AZURE_API_VERSION);
    SLIST_APPEND(list, h);

    SLIST_APPEND(list, "x-ms-page-write: update");

    snprintf(h, sizeof(h), "x-ms-range: %s", range_val);
    SLIST_APPEND(list, h);

    if (lease_id && *lease_id) {
        snprintf(h, sizeof(h), "x-ms-lease-id: %s", lease_id);
        SLIST_APPEND(list, h);
    }

    snprintf(h, sizeof(h), "Content-Length: %zu", range->len);
    SLIST_APPEND(list, h);

    SLIST_APPEND(list, "Content-Type: application/octet-stream");

    if (auth_hdr[0]) {
        snprintf(h, sizeof(h), "Authorization: %s", auth_hdr);
        SLIST_APPEND(list, h);
    }

    req->hdrs = list;
    curl_easy_setopt(req->easy, CURLOPT_HTTPHEADER, req->hdrs);

    /* Response callbacks (per-handle buffers) */
    curl_easy_setopt(req->easy, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(req->easy, CURLOPT_WRITEDATA, &req->resp_body);
    curl_easy_setopt(req->easy, CURLOPT_HEADERFUNCTION, curl_header_cb);
    curl_easy_setopt(req->easy, CURLOPT_HEADERDATA, &req->resp_hdrs);

    /* Timeouts and keep-alive (match execute_single settings) */
    curl_easy_setopt(req->easy, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(req->easy, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(req->easy, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(req->easy, CURLOPT_TCP_KEEPIDLE, 30L);
    curl_easy_setopt(req->easy, CURLOPT_TCP_KEEPINTVL, 15L);

    /* Explicitly enable TLS session ID caching.  The persistent CURLM
     * handle's connection pool manages TLS session reuse across calls. */
    curl_easy_setopt(req->easy, CURLOPT_SSL_SESSIONID_CACHE, 1L);

    /* Link back to batch context for result collection */
    curl_easy_setopt(req->easy, CURLOPT_PRIVATE, (char *)req);

    return AZURE_OK;

/* S-H5: cleanup for SLIST_APPEND failures in batch_init_easy */
cleanup:
    if (list) curl_slist_free_all(list);
    req->hdrs = NULL;
    curl_easy_cleanup(req->easy);
    req->easy = NULL;
    return AZURE_ERR_NOMEM;
}

/* Free all resources owned by a batch request */
static void batch_free_req(batch_req_t *req)
{
    if (req->easy) {
        curl_easy_cleanup(req->easy);
        req->easy = NULL;
    }
    if (req->hdrs) {
        curl_slist_free_all(req->hdrs);
        req->hdrs = NULL;
    }
    azure_buffer_free(&req->resp_body);
}

/*
 * Write multiple page ranges in parallel using curl_multi.
 *
 * nRanges ≤ 1: delegates to az_page_blob_write (simple path).
 * nRanges > 1: concurrent PUT Page with retry on transient errors.
 * Returns AZURE_OK only when ALL ranges succeed.
 */
static azure_err_t az_page_blob_write_batch(
    void *ctx, const char *name,
    const azure_page_range_t *ranges, int nRanges,
    const char *lease_id, azure_error_t *err)
{
    if (!ctx || !name || !err) return AZURE_ERR_INVALID_ARG;
    azure_error_init(err);

    if (nRanges <= 0) return AZURE_OK;

    /* Single range — use sequential path (has its own retry logic) */
    if (nRanges == 1) {
        return az_page_blob_write(ctx, name, ranges[0].offset,
                                  ranges[0].data, ranges[0].len,
                                  lease_id, err);
    }

    azure_client_t *c = (azure_client_t *)ctx;

    /* Build URL once — same for all ranges and retry attempts */
    char url[4096];
    build_blob_url(c, name, url, sizeof(url));
    size_t url_len = strlen(url);
    {
        int n = snprintf(url + url_len, sizeof(url) - url_len, "?comp=page");
        if (n > 0) url_len += (size_t)n;
    }
    if (c->use_sas) {
        snprintf(url + url_len, sizeof(url) - url_len, "&%s", c->sas_token);
    }

    /* Per-range completion tracking across retry attempts */
    int *done = calloc((size_t)nRanges, sizeof(int));
    if (!done) {
        err->code = AZURE_ERR_NOMEM;
        snprintf(err->error_message, sizeof(err->error_message),
                 "batch write: allocation failed");
        return AZURE_ERR_NOMEM;
    }

    azure_err_t result = AZURE_OK;

    /* ---- Persistent multi handle (lazy init, connection pool reused) ---- */
    CURLM *multi = ensure_multi_handle(c);
    if (!multi) {
        free(done);
        err->code = AZURE_ERR_NETWORK;
        snprintf(err->error_message, sizeof(err->error_message),
                 "curl_multi_init() failed");
        return AZURE_ERR_NETWORK;
    }

    for (int attempt = 0; attempt <= BATCH_MAX_RETRIES; attempt++) {
        int pending = 0;
        for (int i = 0; i < nRanges; i++) {
            if (!done[i]) pending++;
        }
        if (pending == 0) break;

        /* Exponential backoff + random jitter before retry (skip first attempt).
         * Jitter prevents thundering herd when multiple clients retry. */
        if (attempt > 0) {
            int delay_ms = AZURE_RETRY_BASE_MS * (1 << (attempt - 1));
            if (delay_ms > AZURE_RETRY_MAX_MS)
                delay_ms = AZURE_RETRY_MAX_MS;
#if defined(__APPLE__) || defined(__FreeBSD__)
            delay_ms += (int)arc4random_uniform(100);
#else
            {
                unsigned int r;
                if (getrandom(&r, sizeof(r), 0) == (ssize_t)sizeof(r))
                    delay_ms += (int)(r % 100);
            }
#endif
            fprintf(stderr,
                    "[sqlite-objs] batch write: %d/%d ranges pending, "
                    "retry %d/%d in %dms\n",
                    pending, nRanges, attempt, BATCH_MAX_RETRIES, delay_ms);
            azure_retry_sleep_ms(delay_ms);
        }

        /* ---- Set up easy handles for pending ranges ---- */
        batch_req_t *reqs = calloc((size_t)pending, sizeof(batch_req_t));
        if (!reqs) {
            free(done);
            err->code = AZURE_ERR_NOMEM;
            return AZURE_ERR_NOMEM;
        }

        int req_count = 0;
        int setup_ok = 1;
        for (int i = 0; i < nRanges && setup_ok; i++) {
            if (done[i]) continue;
            reqs[req_count].range_idx = i;
            azure_err_t rc = batch_init_easy(c, url, name, &ranges[i],
                                              lease_id, &reqs[req_count]);
            if (rc != AZURE_OK) {
                result = rc;
                err->code = rc;
                snprintf(err->error_message, sizeof(err->error_message),
                         "batch write: request setup failed");
                setup_ok = 0;
                break;
            }
            curl_multi_add_handle(multi, reqs[req_count].easy);
            req_count++;
        }

        if (!setup_ok) {
            for (int j = 0; j < req_count; j++) {
                curl_multi_remove_handle(multi, reqs[j].easy);
                batch_free_req(&reqs[j]);
            }
            free(reqs);
            free(done);
            return result;
        }

        /* ---- Event loop ---- */
        int still_running = 0;
        time_t last_renewal = time(NULL);
        int lease_lost = 0;

        double batch_t0 = 0;
        if (az_debug_timing()) batch_t0 = az_time_ms();

        curl_multi_perform(multi, &still_running);

        while (still_running > 0) {
            curl_multi_wait(multi, NULL, 0, 1000, NULL);
            curl_multi_perform(multi, &still_running);

            /* Renew lease to prevent 30s expiry during large flushes */
            if (lease_id && *lease_id) {
                time_t now = time(NULL);
                if (now - last_renewal >= BATCH_LEASE_RENEWAL_SEC) {
                    azure_error_t le;
                    azure_error_init(&le);
                    azure_err_t lrc = az_lease_renew(ctx, name,
                                                     lease_id, &le);
                    if (lrc != AZURE_OK) {
                        fprintf(stderr,
                                "[sqlite-objs] batch write: lease renewal "
                                "failed (%s), aborting\n",
                                azure_err_str(lrc));
                        lease_lost = 1;
                        break;
                    }
                    last_renewal = now;
                }
            }
        }

        /* ---- Collect results ---- */
        azure_err_t attempt_err = AZURE_OK;
        CURLMsg *msg;
        int msgs_left;

        while ((msg = curl_multi_info_read(multi, &msgs_left))) {
            if (msg->msg != CURLMSG_DONE) continue;

            char *priv = NULL;
            curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, &priv);
            batch_req_t *req = (batch_req_t *)priv;
            if (!req) continue;

            if (msg->data.result != CURLE_OK) {
                /* Classify curl-level errors as transient or fatal */
                azure_err_t rc =
                    (msg->data.result == CURLE_OPERATION_TIMEDOUT ||
                     msg->data.result == CURLE_COULDNT_CONNECT)
                    ? AZURE_ERR_SERVER : AZURE_ERR_NETWORK;
                if (attempt_err == AZURE_OK) attempt_err = rc;
                continue;
            }

            long http_status = 0;
            curl_easy_getinfo(msg->easy_handle,
                              CURLINFO_RESPONSE_CODE, &http_status);

            if (http_status >= 200 && http_status < 300) {
                done[req->range_idx] = 1;
            } else {
                azure_err_t rc = azure_classify_http_error(
                    http_status, req->resp_hdrs.error_code);
                if (attempt_err == AZURE_OK) {
                    attempt_err = rc;
                    err->http_status = clamp_http_status(http_status);
                    if (req->resp_body.size > 0)
                        azure_parse_error_xml(
                            (const char *)req->resp_body.data,
                            req->resp_body.size, err);
                    if (req->resp_hdrs.error_code[0])
                        strncpy(err->error_code,
                                req->resp_hdrs.error_code,
                                sizeof(err->error_code) - 1);
                }
            }
        }

        /* ---- Cleanup easy handles (multi handle persists) ---- */
        if (az_debug_timing()) {
            double batch_elapsed = az_time_ms() - batch_t0;
            int completed = 0;
            for (int i = 0; i < nRanges; i++) {
                if (done[i]) completed++;
            }
            fprintf(stderr, "[TIMING] batch_multi: %.1fms attempt=%d handles=%d "
                    "completed=%d/%d (reuse=%d new=%d)\n",
                    batch_elapsed, attempt, req_count,
                    completed, nRanges, g_tls_reuse_count, g_tls_new_count);
        }

        for (int j = 0; j < req_count; j++) {
            curl_multi_remove_handle(multi, reqs[j].easy);
            batch_free_req(&reqs[j]);
        }
        free(reqs);

        if (lease_lost) {
            free(done);
            err->code = AZURE_ERR_LEASE_EXPIRED;
            snprintf(err->error_message, sizeof(err->error_message),
                     "Lease lost during batch write");
            return AZURE_ERR_LEASE_EXPIRED;
        }

        result = attempt_err;

        /* All done? Or non-retryable error? */
        int all_done = 1;
        for (int i = 0; i < nRanges; i++) {
            if (!done[i]) { all_done = 0; break; }
        }
        if (all_done) break;
        if (!azure_is_retryable(attempt_err)) break;
    }

    /* ---- Final verdict ---- */
    int all_ok = 1;
    for (int i = 0; i < nRanges; i++) {
        if (!done[i]) { all_ok = 0; break; }
    }
    free(done);

    if (all_ok) {
        azure_error_init(err);
        return AZURE_OK;
    }

    if (result == AZURE_OK) result = AZURE_ERR_IO;
    err->code = result;
    if (!err->error_message[0])
        snprintf(err->error_message, sizeof(err->error_message),
                 "Batch write: ranges failed after %d retries",
                 BATCH_MAX_RETRIES);
    return result;
}

/* ================================================================
 * APPEND BLOB OPERATIONS (for WAL mode)
 *
 * Append blobs support sequential append-only writes — a natural
 * fit for WAL files.  Create, append data, delete on checkpoint.
 * ================================================================ */

/*
 * Create an empty append blob.
 * PUT with x-ms-blob-type: AppendBlob, Content-Length: 0
 */
static azure_err_t az_append_blob_create(void *ctx, const char *blob_name,
                                          const char *lease_id,
                                          azure_error_t *err)
{
    azure_client_t *c = (azure_client_t *)ctx;

    char lease_header[128];
    const char *extra_with_lease[] = {
        "x-ms-blob-type:AppendBlob",
        lease_header,
        NULL
    };
    const char *extra_no_lease[] = {
        "x-ms-blob-type:AppendBlob",
        NULL
    };

    const char *const *extra;
    if (lease_id && *lease_id) {
        snprintf(lease_header, sizeof(lease_header),
                 "x-ms-lease-id:%s", lease_id);
        extra = extra_with_lease;
    } else {
        extra = extra_no_lease;
    }

    return execute_with_retry(c, "PUT", blob_name, NULL,
                              extra, NULL, NULL, 0, NULL,
                              NULL, NULL, err);
}

/*
 * Append data to an existing append blob.
 * PUT ?comp=appendblock with raw body bytes.
 * Max 4 MiB per call — caller is responsible for splitting.
 */
static azure_err_t az_append_blob_append(void *ctx, const char *blob_name,
                                          const unsigned char *data,
                                          int data_len,
                                          const char *lease_id,
                                          azure_error_t *err)
{
    azure_client_t *c = (azure_client_t *)ctx;

    if (!data || data_len <= 0) {
        err->code = AZURE_ERR_INVALID_ARG;
        snprintf(err->error_message, sizeof(err->error_message),
                 "Append data must be non-NULL with length > 0");
        return AZURE_ERR_INVALID_ARG;
    }
    if (data_len > AZURE_MAX_PAGE_WRITE) {  /* 4 MiB limit */
        err->code = AZURE_ERR_INVALID_ARG;
        snprintf(err->error_message, sizeof(err->error_message),
                 "Append data length %d exceeds 4 MiB maximum", data_len);
        return AZURE_ERR_INVALID_ARG;
    }

    char lease_header[128];
    const char *extra_with_lease[] = {
        "x-ms-blob-type:AppendBlob",
        lease_header,
        NULL
    };
    const char *extra_no_lease[] = {
        "x-ms-blob-type:AppendBlob",
        NULL
    };

    const char *const *extra;
    if (lease_id && *lease_id) {
        snprintf(lease_header, sizeof(lease_header),
                 "x-ms-lease-id:%s", lease_id);
        extra = extra_with_lease;
    } else {
        extra = extra_no_lease;
    }

    return execute_with_retry(c, "PUT", blob_name, "comp=appendblock",
                              extra, "application/octet-stream",
                              data, (size_t)data_len, NULL,
                              NULL, NULL, err);
}

/*
 * Delete an append blob.
 * Reuses the same DELETE logic as block_blob_delete.
 * If lease_id is provided, includes x-ms-lease-id header.
 */
static azure_err_t az_append_blob_delete(void *ctx, const char *blob_name,
                                          const char *lease_id,
                                          azure_error_t *err)
{
    azure_client_t *c = (azure_client_t *)ctx;

    char lease_header[128];
    const char *extra_with_lease[] = {
        lease_header,
        NULL
    };

    const char *const *extra = NULL;
    if (lease_id && *lease_id) {
        snprintf(lease_header, sizeof(lease_header),
                 "x-ms-lease-id:%s", lease_id);
        extra = extra_with_lease;
    }

    return execute_with_retry(c, "DELETE", blob_name, NULL,
                              extra, NULL, NULL, 0, NULL,
                              NULL, NULL, err);
}

/* ================================================================
 * Static vtable instance — populated with all production functions
 * ================================================================ */

static const azure_ops_t azure_production_ops = {
    /* Page blob */
    .page_blob_create  = az_page_blob_create,
    .page_blob_write   = az_page_blob_write,
    .page_blob_read    = az_page_blob_read,
    .page_blob_resize  = az_page_blob_resize,
    /* Block blob */
    .block_blob_upload   = az_block_blob_upload,
    .block_blob_download = az_block_blob_download,
    /* Common */
    .blob_get_properties = az_blob_get_properties,
    .blob_delete         = az_blob_delete,
    .blob_exists         = az_blob_exists,
    /* Leases */
    .lease_acquire = az_lease_acquire,
    .lease_renew   = az_lease_renew,
    .lease_release = az_lease_release,
    .lease_break   = az_lease_break,
    /* Batch write — parallel flush via curl_multi (Phase 2) */
    .page_blob_write_batch = az_page_blob_write_batch,
    /* Append blob — WAL mode */
    .append_blob_create = az_append_blob_create,
    .append_blob_append = az_append_blob_append,
    .append_blob_delete = az_append_blob_delete,
};

/* ================================================================
 * Client lifecycle
 * ================================================================ */

/*
 * Create a new Azure client with the given credentials.
 * SAS takes precedence over Shared Key (per Decision 9).
 * Matches canonical signature from azure_client.h.
 */
azure_err_t azure_client_create(const azure_client_config_t *config,
                                azure_client_t **client_out,
                                azure_error_t *err)
{
    if (client_out) *client_out = NULL;

    const char *account = config ? config->account : NULL;
    const char *container = config ? config->container : NULL;
    const char *sas_token = config ? config->sas_token : NULL;
    const char *shared_key = config ? config->account_key : NULL;

    if (!account || !*account) {
        if (err) {
            err->code = AZURE_ERR_INVALID_ARG;
            snprintf(err->error_message, sizeof(err->error_message),
                     "account name is required");
        }
        return AZURE_ERR_INVALID_ARG;
    }
    if (!container || !*container) {
        if (err) {
            err->code = AZURE_ERR_INVALID_ARG;
            snprintf(err->error_message, sizeof(err->error_message),
                     "container name is required");
        }
        return AZURE_ERR_INVALID_ARG;
    }

    int have_sas = (sas_token && *sas_token);
    int have_key = (shared_key && *shared_key);
    if (!have_sas && !have_key) {
        if (err) {
            err->code = AZURE_ERR_INVALID_ARG;
            snprintf(err->error_message, sizeof(err->error_message),
                     "either sas_token or account_key is required");
        }
        return AZURE_ERR_INVALID_ARG;
    }

    azure_client_t *c = calloc(1, sizeof(azure_client_t));
    if (!c) {
        if (err) {
            err->code = AZURE_ERR_NOMEM;
            snprintf(err->error_message, sizeof(err->error_message),
                     "failed to allocate azure_client_t");
        }
        return AZURE_ERR_NOMEM;
    }

    strncpy(c->account, account, sizeof(c->account) - 1);
    strncpy(c->container, container, sizeof(c->container) - 1);

    /* Copy custom endpoint if provided (for Azurite support) */
    const char *endpoint = config ? config->endpoint : NULL;
    if (endpoint && *endpoint) {
        strncpy(c->endpoint, endpoint, sizeof(c->endpoint) - 1);
    } else {
        c->endpoint[0] = '\0';  /* Empty = use default Azure endpoint */
    }

    /* SAS preferred over Shared Key (per D9) */
    if (have_sas) {
        const char *tok = sas_token;
        if (*tok == '?') tok++;
        strncpy(c->sas_token, tok, sizeof(c->sas_token) - 1);
        c->use_sas = 1;
    } else {
        strncpy(c->key_b64, shared_key, sizeof(c->key_b64) - 1);
        if (azure_base64_decode(shared_key, c->key_raw,
                                sizeof(c->key_raw),
                                &c->key_raw_len) != 0) {
            if (err) {
                err->code = AZURE_ERR_AUTH;
                snprintf(err->error_message, sizeof(err->error_message),
                         "failed to decode shared_key");
            }
            free(c);
            return AZURE_ERR_AUTH;
        }
        c->use_sas = 0;
    }

    /* Initialize libcurl handle (reused across requests for keep-alive) */
    CURL *curl = curl_easy_init();
    if (!curl) {
        if (err) {
            err->code = AZURE_ERR_NETWORK;
            snprintf(err->error_message, sizeof(err->error_message),
                     "curl_easy_init() failed");
        }
        secure_zero(c->key_raw, sizeof(c->key_raw));
        secure_zero(c->key_b64, sizeof(c->key_b64));
        free(c);
        return AZURE_ERR_NETWORK;
    }
    c->curl_handle = curl;

    if (client_out) *client_out = c;
    return AZURE_OK;
}

/*
 * Destroy client: clean up curl handle, scrub key material.
 */
void azure_client_destroy(azure_client_t *client)
{
    if (!client) return;

    /* Destroy persistent multi handle (connection pool + TLS cache) */
    if (client->multi_handle) {
        curl_multi_cleanup((CURLM *)client->multi_handle);
        client->multi_handle = NULL;
    }

    if (client->curl_handle) {
        curl_easy_cleanup((CURL *)client->curl_handle);
        client->curl_handle = NULL;
    }

    /* Scrub key material from memory (S-C4: secure_zero cannot be optimized away) */
    secure_zero(client->key_raw, sizeof(client->key_raw));
    secure_zero(client->key_b64, sizeof(client->key_b64));
    secure_zero(client->sas_token, sizeof(client->sas_token));

    free(client);
}

/*
 * Get the static production vtable.
 * The VFS layer stores this pointer and calls through it.
 */
const azure_ops_t *azure_client_get_ops(void)
{
    return &azure_production_ops;
}

void *azure_client_get_ctx(azure_client_t *client)
{
    return (void *)client;
}

/*
 * Create a container (idempotent).
 * PUT /<container>?restype=container
 * Returns OK on 201 (created) or 409 (already exists).
 * This is a public API function (not part of azure_ops_t) used
 * primarily during test setup.
 */
azure_err_t azure_container_create(azure_client_t *client,
                                   azure_error_t *err)
{
    if (!client || !err) {
        if (err) {
            err->code = AZURE_ERR_INVALID_ARG;
            snprintf(err->error_message, sizeof(err->error_message),
                     "client and err must be non-NULL");
        }
        return AZURE_ERR_INVALID_ARG;
    }

    azure_error_init(err);

    CURL *curl = (CURL *)client->curl_handle;
    CURLcode res;

    /* Build container URL (no blob name) */
    char url[4096];
    if (client->endpoint[0]) {
        /* Custom endpoint (Azurite): endpoint/account/container?restype=container */
        snprintf(url, sizeof(url), "%s/%s/%s?restype=container",
                 client->endpoint, client->account, client->container);
    } else {
        /* Production Azure */
        snprintf(url, sizeof(url),
                 "https://%s.blob.core.windows.net/%s?restype=container",
                 client->account, client->container);
    }

    /* Add SAS token if using SAS auth */
    if (client->use_sas) {
        size_t url_len = strlen(url);
        snprintf(url + url_len, sizeof(url) - url_len, "&%s", client->sas_token);
    }

    /* Build x-ms-date and x-ms-version headers */
    char date_buf[64];
    azure_rfc1123_time(date_buf, sizeof(date_buf));

    char date_header[128];
    snprintf(date_header, sizeof(date_header), "x-ms-date:%s", date_buf);

    char version_header[64];
    snprintf(version_header, sizeof(version_header),
             "x-ms-version:%s", AZURE_API_VERSION);

    /* Shared Key auth signing (skip for SAS) */
    char auth_header[512] = "";
    if (!client->use_sas) {
        /* Canonicalized resource for container creation */
        char path[1024];
        if (client->endpoint[0]) {
            /* Azurite quirk: double account name */
            snprintf(path, sizeof(path), "/%s/%s",
                     client->account, client->container);
        } else {
            /* Production Azure */
            snprintf(path, sizeof(path), "/%s", client->container);
        }

        const char *x_ms_headers[] = { date_header, version_header, NULL };

        azure_err_t rc = azure_auth_sign_request(
            client, "PUT", path, "restype=container",
            "",  /* content_length = "" for zero-length */
            "",  /* content_type */
            "",  /* range */
            x_ms_headers,
            auth_header, sizeof(auth_header));
        if (rc != AZURE_OK) {
            err->code = rc;
            snprintf(err->error_message, sizeof(err->error_message),
                     "Failed to sign container create request");
            return rc;
        }
    }

    /* Execute the PUT request */
    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 0L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, 0L);

    /* Build headers (S-H5: checked) */
    struct curl_slist *headers = NULL;
    char h_date[256];
    snprintf(h_date, sizeof(h_date), "x-ms-date: %s", date_buf);
    SLIST_APPEND(headers, h_date);

    char h_version[128];
    snprintf(h_version, sizeof(h_version), "x-ms-version: %s", AZURE_API_VERSION);
    SLIST_APPEND(headers, h_version);

    if (auth_header[0]) {
        char h_auth[600];
        snprintf(h_auth, sizeof(h_auth), "Authorization: %s", auth_header);
        SLIST_APPEND(headers, h_auth);
    }

    SLIST_APPEND(headers, "Content-Length: 0");
    SLIST_APPEND(headers, "Content-Type:");

    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    /* Response capture */
    azure_buffer_t response_body;
    azure_buffer_init(&response_body);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);

    azure_response_headers_t resp_headers;
    memset(&resp_headers, 0, sizeof(resp_headers));
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, curl_header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &resp_headers);

    /* Execute */
    res = curl_easy_perform(curl);
    curl_slist_free_all(headers);
    headers = NULL;

    if (res != CURLE_OK) {
        err->code = AZURE_ERR_NETWORK;
        snprintf(err->error_message, sizeof(err->error_message),
                 "curl_easy_perform() failed: %s", curl_easy_strerror(res));
        azure_buffer_free(&response_body);
        return AZURE_ERR_NETWORK;
    }

    long http_status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
    err->http_status = clamp_http_status(http_status);

    /* 201 Created or 409 ContainerAlreadyExists are both success */
    if (http_status == 201 || http_status == 409) {
        azure_buffer_free(&response_body);
        return AZURE_OK;
    }

    /* Error case: parse Azure error response */
    if (response_body.size > 0) {
        azure_parse_error_xml((const char *)response_body.data,
                              response_body.size, err);
    }
    azure_buffer_free(&response_body);

    err->code = azure_classify_http_error(http_status, resp_headers.error_code);
    if (resp_headers.request_id[0]) {
        strncpy(err->request_id, resp_headers.request_id,
                sizeof(err->request_id) - 1);
    }
    if (!err->error_message[0]) {
        snprintf(err->error_message, sizeof(err->error_message),
                 "Container create failed with HTTP %ld", http_status);
    }

    return err->code;

/* S-H5: cleanup for SLIST_APPEND failures in azure_container_create */
cleanup:
    if (headers) curl_slist_free_all(headers);
    err->code = AZURE_ERR_NOMEM;
    snprintf(err->error_message, sizeof(err->error_message),
             "curl_slist_append allocation failed");
    return AZURE_ERR_NOMEM;
}
