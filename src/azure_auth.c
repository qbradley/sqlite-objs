/*
 * azure_auth.c — HMAC-SHA256 Shared Key authentication for Azure Storage
 *
 * Implements StringToSign construction and HMAC-SHA256 signing per:
 *   https://learn.microsoft.com/en-us/rest/api/storageservices/authorize-with-shared-key
 *
 * Evolved from research/azure-poc/azure_auth.c into production quality.
 *
 * Dependencies: OpenSSL (HMAC-SHA256, base64), standard C library
 * Part of the sqliteObjs project. License: MIT
 */

#include "azure_client_impl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>

/* ================================================================
 * Base64 encode/decode using OpenSSL BIO
 * ================================================================ */

int azure_base64_encode(const uint8_t *input, size_t input_len,
                        char *output, size_t output_size)
{
    if (!input || !output || output_size == 0) return -1;

    BIO *b64 = BIO_new(BIO_f_base64());
    BIO *mem = BIO_new(BIO_s_mem());
    if (!b64 || !mem) {
        if (b64) BIO_free(b64);
        if (mem) BIO_free(mem);
        return -1;
    }

    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    b64 = BIO_push(b64, mem);

    if (BIO_write(b64, input, (int)input_len) <= 0) {
        BIO_free_all(b64);
        return -1;
    }
    if (BIO_flush(b64) <= 0) {
        BIO_free_all(b64);
        return -1;
    }

    BUF_MEM *bptr;
    BIO_get_mem_ptr(b64, &bptr);

    if (bptr->length + 1 > output_size) {
        BIO_free_all(b64);
        return -1;
    }

    memcpy(output, bptr->data, bptr->length);
    output[bptr->length] = '\0';

    BIO_free_all(b64);
    return 0;
}

int azure_base64_decode(const char *input, uint8_t *output,
                        size_t output_size, size_t *output_len)
{
    if (!input || !output || !output_len) return -1;

    size_t input_len = strlen(input);
    BIO *b64 = BIO_new(BIO_f_base64());
    BIO *mem = BIO_new_mem_buf(input, (int)input_len);
    if (!b64 || !mem) {
        if (b64) BIO_free(b64);
        if (mem) BIO_free(mem);
        return -1;
    }

    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    b64 = BIO_push(b64, mem);

    int decoded_len = BIO_read(b64, output, (int)output_size);
    BIO_free_all(b64);

    if (decoded_len < 0) return -1;
    *output_len = (size_t)decoded_len;
    return 0;
}

/* ================================================================
 * HMAC-SHA256 using OpenSSL
 * ================================================================ */

azure_err_t azure_hmac_sha256(const uint8_t *key, size_t key_len,
                              const uint8_t *data, size_t data_len,
                              uint8_t *out, size_t *out_len)
{
    unsigned int len = 0;
    uint8_t *result = HMAC(EVP_sha256(), key, (int)key_len,
                           data, data_len, out, &len);
    if (!result) return AZURE_ERR_OPENSSL;
    if (out_len) *out_len = (size_t)len;
    return AZURE_OK;
}

/* ================================================================
 * RFC 1123 date formatting (for x-ms-date header)
 * ================================================================ */

void azure_rfc1123_time(char *buf, size_t buf_size)
{
    time_t now = time(NULL);
    struct tm gmt;
    gmtime_r(&now, &gmt);
    strftime(buf, buf_size, "%a, %d %b %Y %H:%M:%S GMT", &gmt);
}

/* ================================================================
 * Header sorting for canonicalization
 * ================================================================ */

static int compare_headers(const void *a, const void *b)
{
    return strcasecmp(*(const char **)a, *(const char **)b);
}

/* ================================================================
 * StringToSign construction and HMAC-SHA256 signing
 *
 * StringToSign format (Blob service, version 2009-09-19+):
 *
 *   VERB\n
 *   Content-Encoding\n
 *   Content-Language\n
 *   Content-Length\n         (empty string if zero)
 *   Content-MD5\n
 *   Content-Type\n
 *   Date\n                   (empty — we use x-ms-date instead)
 *   If-Modified-Since\n
 *   If-Match\n
 *   If-None-Match\n
 *   If-Unmodified-Since\n
 *   Range\n
 *   CanonicalizedHeaders\n   (sorted x-ms-* headers, lowercase names)
 *   CanonicalizedResource    (/account/path\nparam:value sorted)
 * ================================================================ */

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
    size_t auth_header_size)
{
    if (!client || !method || !path || !auth_header) {
        return AZURE_ERR_INVALID_ARG;
    }

    char string_to_sign[4096];
    char *p = string_to_sign;
    char *end = string_to_sign + sizeof(string_to_sign);

    if (!content_length) content_length = "";
    if (!content_type) content_type = "";
    if (!range) range = "";

    /*
     * Lines 1-12: Standard headers.
     * Date is empty because we always use x-ms-date instead
     * (avoids proxy mangling of the Date header).
     */
    {
        int n = snprintf(p, (size_t)(end - p),
            "%s\n"    /* VERB */
            "\n"      /* Content-Encoding */
            "\n"      /* Content-Language */
            "%s\n"    /* Content-Length */
            "\n"      /* Content-MD5 */
            "%s\n"    /* Content-Type */
            "\n"      /* Date (empty — using x-ms-date) */
            "\n"      /* If-Modified-Since */
            "\n"      /* If-Match */
            "\n"      /* If-None-Match */
            "\n"      /* If-Unmodified-Since */
            "%s\n",   /* Range */
            method,
            content_length,
            content_type,
            range);
        if (n < 0 || p + n >= end) return AZURE_ERR_INVALID_ARG;
        p += n;
    }

    /* Canonicalized headers: sort x-ms-* headers, lowercase name */
    if (x_ms_headers) {
        int count = 0;
        while (x_ms_headers[count]) count++;

        if (count > 0) {
            const char **sorted = malloc(sizeof(char *) * (size_t)count);
            if (!sorted) return AZURE_ERR_ALLOC;
            memcpy(sorted, x_ms_headers, sizeof(char *) * (size_t)count);
            qsort(sorted, (size_t)count, sizeof(char *), compare_headers);

            for (int i = 0; i < count; i++) {
                const char *h = sorted[i];
                const char *colon = strchr(h, ':');
                if (!colon) continue;

                /* Write lowercase header name */
                for (const char *c = h; c < colon; c++) {
                    if (p >= end - 1) { free(sorted); return AZURE_ERR_INVALID_ARG; }
                    *p++ = (char)tolower((unsigned char)*c);
                }
                /* Write :value\n */
                size_t val_len = strlen(colon);
                if (p + val_len + 1 >= end) { free(sorted); return AZURE_ERR_INVALID_ARG; }
                memcpy(p, colon, val_len);
                p += val_len;
                *p++ = '\n';
            }
            free(sorted);
        }
    }

    /*
     * Canonicalized resource: /account/path
     * For production Azure: path = /container/blob, result = /account/container/blob
     * For Azurite: path = /account/container/blob (already has account), 
     *              result = /account/account/container/blob (doubled to match Azurite's quirk)
     */
    {
        int n = snprintf(p, (size_t)(end - p), "/%s%s", client->account, path);
        if (n < 0 || p + n >= end) return AZURE_ERR_INVALID_ARG;
        p += n;
    }

    if (query && *query) {
        char qbuf[1024];
        strncpy(qbuf, query, sizeof(qbuf) - 1);
        qbuf[sizeof(qbuf) - 1] = '\0';

        char *pairs[32];
        int npairs = 0;
        char *saveptr;
        char *tok = strtok_r(qbuf, "&", &saveptr);
        while (tok && npairs < 32) {
            pairs[npairs++] = tok;
            tok = strtok_r(NULL, "&", &saveptr);
        }

        qsort(pairs, (size_t)npairs, sizeof(char *), compare_headers);

        for (int i = 0; i < npairs; i++) {
            char *eq = strchr(pairs[i], '=');
            if (eq) {
                *eq = '\0';
                int n = snprintf(p, (size_t)(end - p), "\n%s:%s",
                                  pairs[i], eq + 1);
                if (n < 0 || p + n >= end) return AZURE_ERR_INVALID_ARG;
                p += n;
            } else {
                int n = snprintf(p, (size_t)(end - p), "\n%s:", pairs[i]);
                if (n < 0 || p + n >= end) return AZURE_ERR_INVALID_ARG;
                p += n;
            }
        }
    }

    *p = '\0';

    /* HMAC-SHA256 sign the StringToSign */
    uint8_t hmac_out[32];
    size_t hmac_len = 0;
    azure_err_t rc = azure_hmac_sha256(
        client->key_raw, client->key_raw_len,
        (const uint8_t *)string_to_sign, strlen(string_to_sign),
        hmac_out, &hmac_len);
    if (rc != AZURE_OK) return rc;

    /* Base64-encode the signature */
    char sig_b64[64];
    if (azure_base64_encode(hmac_out, hmac_len, sig_b64, sizeof(sig_b64)) != 0) {
        return AZURE_ERR_OPENSSL;
    }

    /* Format: "SharedKey account:signature" */
    snprintf(auth_header, auth_header_size,
             "SharedKey %s:%s", client->account, sig_b64);

    return AZURE_OK;
}
