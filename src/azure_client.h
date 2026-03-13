/*
** azure_client.h — Canonical Azure client interface for sqliteObjs
**
** This header defines the azure_ops_t vtable — the swappable interface
** between the VFS layer and the Azure Blob Storage client.  In production,
** operations go through libcurl to Azure REST APIs.  In tests, they go
** through mock implementations with in-memory buffers.
**
** This is THE single source of truth for all Azure-facing types.
** Other headers (azure_client_impl.h, mock_azure_ops.h) must include
** this header rather than redefining types.
*/
#ifndef SQLITE_OBJS_AZURE_CLIENT_H
#define SQLITE_OBJS_AZURE_CLIENT_H

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
** Error codes returned by all azure_ops_t functions.
** Superset covering VFS, production client, and mock needs.
*/
typedef enum {
    AZURE_OK = 0,
    AZURE_ERR_NOT_FOUND,        /* 404 — blob does not exist */
    AZURE_ERR_CONFLICT,         /* 409 — lease conflict */
    AZURE_ERR_PRECONDITION,     /* 412 — ETag mismatch */
    AZURE_ERR_THROTTLED,        /* 429 — rate limited */
    AZURE_ERR_AUTH,             /* 401/403 — authentication failure */
    AZURE_ERR_BAD_REQUEST,      /* 400 — malformed request (bug) */
    AZURE_ERR_SERVER,           /* 5xx — Azure-side failure */
    AZURE_ERR_NETWORK,          /* Connection/DNS/timeout failure */
    AZURE_ERR_LEASE_EXPIRED,    /* Lease expired (detected locally) */
    AZURE_ERR_NOMEM,            /* malloc failure */
    AZURE_ERR_INVALID_ARG,      /* Bad argument to API */
    AZURE_ERR_IO,               /* Generic I/O error */
    AZURE_ERR_TIMEOUT,          /* Operation timed out */
    AZURE_ERR_ALIGNMENT,        /* Page blob 512-byte alignment violation */
    AZURE_ERR_UNKNOWN           /* Catch-all */
} azure_err_t;

/* Compatibility alias — tests use THROTTLE, VFS uses THROTTLED */
#define AZURE_ERR_THROTTLE AZURE_ERR_THROTTLED

/*
** Extended error information populated on failure.
** All strings are NUL-terminated and stored inline (no heap allocation).
*/
typedef struct azure_error {
    azure_err_t code;
    int http_status;                /* Raw HTTP status code, or 0 */
    char error_code[128];           /* Azure error code string, e.g. "BlobNotFound" */
    char error_message[256];        /* Human-readable error */
    char request_id[64];            /* Azure x-ms-request-id for debugging */
    char etag[128];                 /* ETag from last Azure response (Phase 3) */
} azure_error_t;

/*
** Growable buffer for data transfer with Azure operations.
** The caller allocates the struct; the callee fills data/size.
** If data is non-NULL after a call, the caller must free it.
*/
typedef struct azure_buffer {
    uint8_t *data;      /* Data pointer (caller frees) */
    size_t size;        /* Length of valid data */
    size_t capacity;    /* Allocated capacity */
} azure_buffer_t;

/*
** Initialize an azure_error_t to a clean state.
*/
static inline void azure_error_init(azure_error_t *e) {
    if (e) {
        e->code = AZURE_OK;
        e->http_status = 0;
        e->error_code[0] = '\0';
        e->error_message[0] = '\0';
        e->request_id[0] = '\0';
        e->etag[0] = '\0';
    }
}

/*
** Clear an azure_error_t (forward-compatible with future heap fields).
*/
static inline void azure_error_clear(azure_error_t *e) {
    if (e) {
        e->code = AZURE_OK;
        e->http_status = 0;
        e->error_code[0] = '\0';
        e->error_message[0] = '\0';
        e->request_id[0] = '\0';
        e->etag[0] = '\0';
    }
}

/*
** Buffer lifecycle helpers.
*/
static inline void azure_buffer_init(azure_buffer_t *buf) {
    buf->data = NULL;
    buf->size = 0;
    buf->capacity = 0;
}

static inline void azure_buffer_free(azure_buffer_t *buf) {
    if (buf->data) { free(buf->data); buf->data = NULL; }
    buf->size = 0;
    buf->capacity = 0;
}

/* A contiguous page range for batch writes */
typedef struct azure_page_range {
    int64_t offset;          /* Must be 512-byte aligned */
    const uint8_t *data;     /* Pointer into cache buffer */
    size_t len;              /* Must be 512-byte aligned, max 4 MiB */
} azure_page_range_t;

/* Maximum bytes per append_blob_append call (Azure limit) */
#define AZURE_MAX_APPEND_SIZE (4 * 1024 * 1024)

#ifndef SQLITE_OBJS_MAX_PARALLEL_PUTS
#define SQLITE_OBJS_MAX_PARALLEL_PUTS 32
#endif

/* -----------------------------------------------------------------------
** azure_ops_t — The swappable Azure operations vtable
**
** This is the single most important interface in the system.  It sits
** between the VFS layer (Aragorn) and the Azure client layer (Frodo).
**
** Contract:
**   1. All functions return azure_err_t.  On error, *err is populated.
**   2. The ctx pointer is opaque — in production it's azure_client_t*,
**      in tests it's a mock context.
**   3. Retry logic lives INSIDE the production implementation.
**   4. azure_buffer_t.pData is callee-allocated; caller frees.
**   5. blob_exists is separate from blob_get_properties (lightweight).
** ----------------------------------------------------------------------- */

typedef struct azure_ops azure_ops_t;
struct azure_ops {
    /* ---- Page Blob Operations (for MAIN_DB) ---- */

    /* Create a page blob of the given size (must be 512-byte aligned). */
    azure_err_t (*page_blob_create)(void *ctx, const char *name,
                                    int64_t size, azure_error_t *err);

    /* Write data to a page blob at the given offset.
    ** offset and len must be 512-byte aligned.
    ** lease_id may be NULL if no lease is held. */
    azure_err_t (*page_blob_write)(void *ctx, const char *name,
                                   int64_t offset, const uint8_t *data,
                                   size_t len, const char *lease_id,
                                   azure_error_t *err);

    /* Read data from a page blob at the given offset. */
    azure_err_t (*page_blob_read)(void *ctx, const char *name,
                                  int64_t offset, size_t len,
                                  azure_buffer_t *out, azure_error_t *err);

    /* Resize a page blob (new_size must be 512-byte aligned).
    ** lease_id may be NULL if no lease is held. */
    azure_err_t (*page_blob_resize)(void *ctx, const char *name,
                                    int64_t new_size, const char *lease_id,
                                    azure_error_t *err);

    /* ---- Block Blob Operations (for MAIN_JOURNAL) ---- */

    /* Upload data as a block blob (replaces existing). */
    azure_err_t (*block_blob_upload)(void *ctx, const char *name,
                                     const uint8_t *data, size_t len,
                                     azure_error_t *err);

    /* Download entire block blob contents. */
    azure_err_t (*block_blob_download)(void *ctx, const char *name,
                                       azure_buffer_t *out,
                                       azure_error_t *err);

    /* ---- Common Blob Operations ---- */

    /* Get blob properties: size, lease state ("available"/"leased"/etc),
    ** lease status ("locked"/"unlocked"). */
    azure_err_t (*blob_get_properties)(void *ctx, const char *name,
                                       int64_t *size,
                                       char *lease_state,
                                       char *lease_status,
                                       azure_error_t *err);

    /* Delete a blob. */
    azure_err_t (*blob_delete)(void *ctx, const char *name,
                                azure_error_t *err);

    /* Check if a blob exists (lightweight HEAD). */
    azure_err_t (*blob_exists)(void *ctx, const char *name,
                                int *exists, azure_error_t *err);

    /* ---- Lease Operations (for Locking) ---- */

    /* Acquire a lease for duration_secs (15-60, or -1 for infinite).
    ** On success, writes lease ID to lease_id_out. */
    azure_err_t (*lease_acquire)(void *ctx, const char *name,
                                  int duration_secs, char *lease_id_out,
                                  size_t lease_id_size,
                                  azure_error_t *err);

    /* Renew an existing lease. */
    azure_err_t (*lease_renew)(void *ctx, const char *name,
                                const char *lease_id, azure_error_t *err);

    /* Release a lease. */
    azure_err_t (*lease_release)(void *ctx, const char *name,
                                  const char *lease_id, azure_error_t *err);

    /* Break a lease.  break_period_secs=0 for immediate break.
    ** remaining_secs receives the remaining lease time (may be NULL). */
    azure_err_t (*lease_break)(void *ctx, const char *name,
                                int break_period_secs, int *remaining_secs,
                                azure_error_t *err);

    /* ---- Batch Write (Phase 2 — NULL until curl_multi implemented) ---- */

    /* Write multiple page ranges in parallel.
    ** NULL = VFS falls back to sequential page_blob_write().
    ** Returns AZURE_OK only if ALL writes succeed. */
    azure_err_t (*page_blob_write_batch)(
        void *ctx, const char *name,
        const azure_page_range_t *ranges, int nRanges,
        const char *lease_id, azure_error_t *err);

    /* ---- Append Blob Operations (for WAL mode) ---- */

    /* Create an empty append blob.
    ** If lease_id is non-NULL, includes x-ms-lease-id header.
    ** Returns AZURE_OK on 201 Created. */
    azure_err_t (*append_blob_create)(
        void *ctx,
        const char *blob_name,
        const char *lease_id,     /* NULL if no lease */
        azure_error_t *err
    );

    /* Append data to an existing append blob.
    ** data_len max 4 MiB per call — caller splits if needed.
    ** If lease_id is non-NULL, includes x-ms-lease-id header.
    ** Returns AZURE_OK on 201 Created. */
    azure_err_t (*append_blob_append)(
        void *ctx,
        const char *blob_name,
        const unsigned char *data,
        int data_len,             /* max 4 MiB per append */
        const char *lease_id,     /* NULL if no lease */
        azure_error_t *err
    );

    /* Delete an append blob.
    ** If lease_id is non-NULL, includes x-ms-lease-id header.
    ** Returns AZURE_OK on 202 Accepted. */
    azure_err_t (*append_blob_delete)(
        void *ctx,
        const char *blob_name,
        const char *lease_id,     /* NULL if no lease */
        azure_error_t *err
    );
};


/* -----------------------------------------------------------------------
** azure_client_t — Opaque production Azure client
**
** Created via azure_client_create(), destroyed via azure_client_destroy().
** Provides the production azure_ops_t via azure_client_get_ops().
**
** NOTE: Frodo will implement the real versions of these functions.
**       The stubs below return AZURE_ERR_UNKNOWN until then.
** ----------------------------------------------------------------------- */

typedef struct azure_client azure_client_t;

/*
** Configuration for creating an Azure client.
*/
typedef struct azure_client_config {
    const char *account;        /* Storage account name */
    const char *container;      /* Container name */
    const char *sas_token;      /* SAS token (preferred), or NULL */
    const char *account_key;    /* Shared Key (fallback), or NULL */
    const char *endpoint;       /* Optional custom endpoint (for Azurite), or NULL for Azure */
} azure_client_config_t;

/*
** Create a production Azure client.  Returns AZURE_OK on success.
** On failure, *client is set to NULL.
*/
azure_err_t azure_client_create(const azure_client_config_t *config,
                                azure_client_t **client,
                                azure_error_t *err);

/*
** Destroy a production Azure client and free all resources.
*/
void azure_client_destroy(azure_client_t *client);

/*
** Get the production azure_ops_t vtable.
** The returned pointer is valid for the lifetime of the client.
*/
const azure_ops_t *azure_client_get_ops(void);

/*
** Get the opaque context pointer for use with azure_ops_t functions.
** This is the azure_client_t* cast to void*.
*/
void *azure_client_get_ctx(azure_client_t *client);

/*
** Create a container (idempotent).
** Returns AZURE_OK if the container was created (201) or already exists (409).
** This is typically called once during test setup.
*/
azure_err_t azure_container_create(azure_client_t *client,
                                   azure_error_t *err);

#ifdef __cplusplus
}
#endif

#endif /* SQLITE_OBJS_AZURE_CLIENT_H */
