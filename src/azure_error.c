/*
 * azure_error.c — Error handling, XML parsing, retry logic
 *
 * Parses Azure Storage REST API error responses (XML format).
 * Classifies errors as transient vs permanent for retry decisions.
 * Implements exponential backoff with jitter and Retry-After support.
 *
 * Evolved from research/azure-poc/azure_error.c into production quality.
 *
 * Part of the sqliteObjs project. License: MIT
 */

#include "azure_client_impl.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>

#if defined(__APPLE__) || defined(__FreeBSD__)
#include <stdlib.h>  /* arc4random_uniform */
#else
#include <sys/random.h>  /* getrandom on Linux */
#endif

/* ================================================================
 * Error code to human-readable string
 * ================================================================ */

const char *azure_err_str(azure_err_t code)
{
    switch (code) {
        case AZURE_OK:              return "OK";
        case AZURE_ERR_BAD_REQUEST: return "Bad request";
        case AZURE_ERR_SERVER:      return "Server error (retryable)";
        case AZURE_ERR_THROTTLED:   return "Throttled (429, retryable)";
        case AZURE_ERR_AUTH:        return "Authentication failure";
        case AZURE_ERR_NOT_FOUND:   return "Not found (404)";
        case AZURE_ERR_CONFLICT:    return "Conflict (409)";
        case AZURE_ERR_PRECONDITION:return "Precondition failed (412)";
        case AZURE_ERR_NETWORK:     return "Network error";
        case AZURE_ERR_LEASE_EXPIRED:return "Lease expired";
        case AZURE_ERR_INVALID_ARG: return "Invalid argument";
        case AZURE_ERR_NOMEM:       return "Memory allocation failure";
        case AZURE_ERR_IO:          return "I/O error";
        case AZURE_ERR_TIMEOUT:     return "Timeout";
        case AZURE_ERR_ALIGNMENT:   return "Alignment violation";
        case AZURE_ERR_UNKNOWN:     return "Unknown error";
    }
    return "Unknown error";
}

/* ================================================================
 * Simple XML tag extractor
 *
 * Azure error responses are small and well-structured:
 *   <Error>
 *     <Code>ServerBusy</Code>
 *     <Message>The server is busy.</Message>
 *   </Error>
 *
 * We just need <Code> and <Message> — strstr suffices.
 * No libxml2 dependency.
 * ================================================================ */

static int extract_xml_tag(const char *xml, const char *tag,
                           char *out, size_t out_size)
{
    char open_tag[64], close_tag[64];
    snprintf(open_tag, sizeof(open_tag), "<%s>", tag);
    snprintf(close_tag, sizeof(close_tag), "</%s>", tag);

    const char *start = strstr(xml, open_tag);
    if (!start) return -1;
    start += strlen(open_tag);

    const char *end_ptr = strstr(start, close_tag);
    if (!end_ptr) return -1;

    size_t len = (size_t)(end_ptr - start);
    if (len >= out_size) len = out_size - 1;

    memcpy(out, start, len);
    out[len] = '\0';
    return 0;
}

azure_err_t azure_parse_error_xml(const char *xml, size_t xml_len,
                                  azure_error_t *err)
{
    if (!xml || xml_len == 0 || !err) return AZURE_ERR_INVALID_ARG;

    /* Extract <Code> — not all error responses have XML bodies
     * (e.g., connection timeouts). That's fine; HTTP status suffices. */
    if (extract_xml_tag(xml, "Code",
                        err->error_code, sizeof(err->error_code)) != 0) {
        err->error_code[0] = '\0';
    }

    if (extract_xml_tag(xml, "Message",
                        err->error_message, sizeof(err->error_message)) != 0) {
        err->error_message[0] = '\0';
    }

    return AZURE_OK;
}

/* ================================================================
 * Error classification
 *
 * Transient errors (retry):
 *   408 Request Timeout, 429 Too Many Requests,
 *   500 Internal Server Error, 502 Bad Gateway,
 *   503 Service Unavailable, 504 Gateway Timeout
 *   Azure codes: ServerBusy, InternalError, OperationTimedOut
 *
 * Permanent errors (no retry):
 *   400 Bad Request, 401/403 Auth failure,
 *   404 Not Found, 409 Conflict, 412 Precondition Failed
 *
 * Note: 429 maps to AZURE_ERR_THROTTLED (→ SQLITE_BUSY after retry)
 *       5xx maps to AZURE_ERR_TRANSIENT (→ SQLITE_IOERR after retry)
 * ================================================================ */

azure_err_t azure_classify_http_error(long http_status,
                                      const char *error_code)
{
    if (http_status >= 200 && http_status < 300)
        return AZURE_OK;

    /* 429 is throttling — distinct from other transient errors */
    if (http_status == 429)
        return AZURE_ERR_THROTTLED;

    /* Server errors and timeouts are transient */
    switch (http_status) {
        case 408: /* Request Timeout */
        case 500: /* Internal Server Error */
        case 502: /* Bad Gateway */
        case 503: /* Service Unavailable */
        case 504: /* Gateway Timeout */
            return AZURE_ERR_SERVER;
    }

    /* Azure-specific transient error codes (may arrive with various status) */
    if (error_code && *error_code) {
        if (strcmp(error_code, "ServerBusy") == 0) return AZURE_ERR_SERVER;
        if (strcmp(error_code, "InternalError") == 0) return AZURE_ERR_SERVER;
        if (strcmp(error_code, "OperationTimedOut") == 0) return AZURE_ERR_SERVER;
    }

    /* Permanent errors */
    switch (http_status) {
        case 401:
        case 403: return AZURE_ERR_AUTH;
        case 404: return AZURE_ERR_NOT_FOUND;
        case 409: return AZURE_ERR_CONFLICT;
        case 412: return AZURE_ERR_PRECONDITION;
        default:  return AZURE_ERR_BAD_REQUEST;
    }
}

/* Check if an error is retryable */
int azure_is_retryable(azure_err_t code)
{
    return code == AZURE_ERR_SERVER || code == AZURE_ERR_THROTTLED;
}

/* ================================================================
 * Retry delay computation
 *
 * Base algorithm: exponential backoff with jitter
 *   delay = min(base_ms * 2^attempt + random(0, base_ms), max_ms)
 *
 * Retry-After override: If Azure sends Retry-After header (seconds),
 * use that instead — but still cap at max delay.
 *
 * This matches Azure SDK patterns (Go, .NET, Python):
 *   - Base: 500ms → 500, 1000, 2000, 4000, 8000, ...
 *   - Jitter: 0-500ms random
 *   - Max: 30 seconds
 * ================================================================ */

int azure_compute_retry_delay(int attempt, int retry_after_secs)
{
    int delay_ms;

    if (retry_after_secs > 0) {
        /* Azure told us how long to wait — respect it.
         * Check overflow: retry_after_secs * 1000 must fit in int. */
        if (retry_after_secs > INT_MAX / 1000)
            delay_ms = AZURE_RETRY_MAX_MS;
        else
            delay_ms = retry_after_secs * 1000;
    } else {
        /* Exponential backoff: base * 2^attempt */
        delay_ms = AZURE_RETRY_BASE_MS * (1 << attempt);
        /* Add jitter: 0 to base_ms random (thread-safe) */
#if defined(__APPLE__) || defined(__FreeBSD__)
        delay_ms += (int)arc4random_uniform(AZURE_RETRY_BASE_MS);
#else
        {
            unsigned int r;
            if (getrandom(&r, sizeof(r), 0) == (ssize_t)sizeof(r))
                delay_ms += (int)(r % (unsigned)AZURE_RETRY_BASE_MS);
            /* else: no jitter — safer than using rand() */
        }
#endif
    }

    if (delay_ms > AZURE_RETRY_MAX_MS)
        delay_ms = AZURE_RETRY_MAX_MS;

    return delay_ms;
}

void azure_retry_sleep_ms(int delay_ms)
{
    if (delay_ms > 0) {
        usleep((useconds_t)delay_ms * 1000U);
    }
}
