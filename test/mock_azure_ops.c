/*
** mock_azure_ops.c — In-memory mock implementation of azure_ops_t
**
** Simulates Azure Blob Storage entirely in memory for unit testing.
** Page blobs enforce 512-byte alignment. Block blobs are simple key-value.
** Lease management is a proper state machine.
** Failure injection lets tests simulate Azure errors at precise moments.
*/

#include "mock_azure_ops.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

/* ── Constants ────────────────────────────────────────────────────── */

#define PAGE_BLOB_ALIGNMENT    512
#define MAX_BLOBS              128
#define MAX_FAIL_RULES         32
#define LEASE_ID_LEN           37  /* UUID-like: 36 chars + NUL */
#define OP_NAME_LEN            32
#define BLOB_NAME_LEN          256

/* ── Operation index mapping ──────────────────────────────────────── */

typedef enum {
    OP_PAGE_BLOB_CREATE = 0,
    OP_PAGE_BLOB_WRITE,
    OP_PAGE_BLOB_READ,
    OP_PAGE_BLOB_RESIZE,
    OP_BLOCK_BLOB_UPLOAD,
    OP_BLOCK_BLOB_DOWNLOAD,
    OP_BLOB_GET_PROPERTIES,
    OP_BLOB_DELETE,
    OP_BLOB_EXISTS,
    OP_LEASE_ACQUIRE,
    OP_LEASE_RENEW,
    OP_LEASE_RELEASE,
    OP_LEASE_BREAK,
    OP_COUNT
} op_index_t;

static const char *op_names[OP_COUNT] = {
    "page_blob_create",
    "page_blob_write",
    "page_blob_read",
    "page_blob_resize",
    "block_blob_upload",
    "block_blob_download",
    "blob_get_properties",
    "blob_delete",
    "blob_exists",
    "lease_acquire",
    "lease_renew",
    "lease_release",
    "lease_break",
};

static op_index_t op_name_to_index(const char *name) {
    for (int i = 0; i < OP_COUNT; i++) {
        if (strcmp(op_names[i], name) == 0) return (op_index_t)i;
    }
    return OP_COUNT; /* not found */
}

/* ── Blob storage structures ──────────────────────────────────────── */

typedef enum {
    BLOB_TYPE_NONE  = 0,
    BLOB_TYPE_PAGE  = 1,
    BLOB_TYPE_BLOCK = 2,
} blob_type_t;

typedef struct {
    char            name[BLOB_NAME_LEN];
    blob_type_t     type;
    uint8_t        *data;
    int64_t         size;      /* Logical size */
    int64_t         capacity;  /* Allocated capacity */

    /* Lease state */
    mock_lease_state_t lease_state;
    char               lease_id[LEASE_ID_LEN];
    int                lease_duration;
    time_t             lease_acquired_at;
    int                break_period;
} mock_blob_t;

/* ── Failure injection ────────────────────────────────────────────── */

typedef struct {
    int         call_number;  /* 0 = by operation name, >0 = by call # */
    op_index_t  op;           /* Which operation (for op-name failures) */
    azure_err_t error_code;
    int         active;
} fail_rule_t;

/* ── Mock context ─────────────────────────────────────────────────── */

struct mock_azure_ctx {
    mock_blob_t  blobs[MAX_BLOBS];
    int          blob_count;

    /* Call counting */
    int          call_counts[OP_COUNT];
    int          total_calls;

    /* Failure injection */
    fail_rule_t  fail_rules[MAX_FAIL_RULES];
    int          fail_rule_count;

    /* Counter for next lease ID */
    int          next_lease_num;
};

/* ── Internal helpers ─────────────────────────────────────────────── */

static mock_blob_t *find_blob(mock_azure_ctx_t *ctx, const char *name) {
    for (int i = 0; i < ctx->blob_count; i++) {
        if (strcmp(ctx->blobs[i].name, name) == 0) {
            return &ctx->blobs[i];
        }
    }
    return NULL;
}

static mock_blob_t *create_blob(mock_azure_ctx_t *ctx, const char *name,
                                 blob_type_t type) {
    if (ctx->blob_count >= MAX_BLOBS) return NULL;
    mock_blob_t *b = &ctx->blobs[ctx->blob_count++];
    memset(b, 0, sizeof(*b));
    strncpy(b->name, name, BLOB_NAME_LEN - 1);
    b->type = type;
    b->lease_state = LEASE_AVAILABLE;
    return b;
}

static void free_blob_data(mock_blob_t *b) {
    if (b->data) { free(b->data); b->data = NULL; }
    b->size = 0;
    b->capacity = 0;
}

static void generate_lease_id(mock_azure_ctx_t *ctx, char *out, size_t size) {
    snprintf(out, size, "mock-lease-%04d-%08x",
             ctx->next_lease_num++, (unsigned)(time(NULL) & 0xFFFFFFFF));
}

static void set_error(azure_error_t *err, int http_status,
                      const char *code, const char *msg) {
    if (!err) return;
    memset(err, 0, sizeof(*err));
    err->http_status = http_status;
    if (code) strncpy(err->error_code, code, sizeof(err->error_code) - 1);
    if (msg) strncpy(err->error_message, msg, sizeof(err->error_message) - 1);
    snprintf(err->request_id, sizeof(err->request_id), "mock-%08x",
             (unsigned)(rand() & 0xFFFFFFFF));
}

/* Check failure injection. Returns the error to inject, or AZURE_OK. */
static azure_err_t check_failures(mock_azure_ctx_t *ctx, op_index_t op,
                                   azure_error_t *err) {
    int call_num = ctx->total_calls; /* Already incremented by caller */

    for (int i = 0; i < ctx->fail_rule_count; i++) {
        fail_rule_t *r = &ctx->fail_rules[i];
        if (!r->active) continue;

        /* Fail at specific call number */
        if (r->call_number > 0 && r->call_number == call_num) {
            r->active = 0; /* One-shot */
            set_error(err, 500, "InjectedFailure", "Mock failure injection");
            return r->error_code;
        }

        /* Fail specific operation always */
        if (r->call_number == 0 && r->op == op) {
            set_error(err, 500, "InjectedFailure", "Mock failure injection (by op)");
            return r->error_code;
        }
    }
    return AZURE_OK;
}

/* Increment counters and check for injected failures */
static azure_err_t pre_call(mock_azure_ctx_t *ctx, op_index_t op,
                             azure_error_t *err) {
    ctx->call_counts[op]++;
    ctx->total_calls++;
    return check_failures(ctx, op, err);
}

static int ensure_capacity(mock_blob_t *b, int64_t needed) {
    if (needed <= b->capacity) return 0;
    int64_t new_cap = needed;
    /* Round up to alignment for page blobs */
    if (b->type == BLOB_TYPE_PAGE) {
        new_cap = (new_cap + PAGE_BLOB_ALIGNMENT - 1) & ~(PAGE_BLOB_ALIGNMENT - 1);
    }
    uint8_t *new_data = (uint8_t *)realloc(b->data, (size_t)new_cap);
    if (!new_data) return -1;
    /* Zero new memory */
    memset(new_data + b->capacity, 0, (size_t)(new_cap - b->capacity));
    b->data = new_data;
    b->capacity = new_cap;
    return 0;
}

/* ── Page Blob Operations ─────────────────────────────────────────── */

static azure_err_t mock_page_blob_create(void *vctx, const char *name,
                                          int64_t size, azure_error_t *err) {
    mock_azure_ctx_t *ctx = (mock_azure_ctx_t *)vctx;
    azure_err_t rc = pre_call(ctx, OP_PAGE_BLOB_CREATE, err);
    if (rc != AZURE_OK) return rc;

    /* Size must be aligned to 512 bytes */
    if (size < 0 || (size % PAGE_BLOB_ALIGNMENT != 0 && size != 0)) {
        set_error(err, 400, "InvalidHeaderValue",
                  "Page blob size must be aligned to 512 bytes");
        return AZURE_ERR_ALIGNMENT;
    }

    mock_blob_t *b = find_blob(ctx, name);
    if (b) {
        /* Already exists — resize it */
        if (b->type != BLOB_TYPE_PAGE) {
            set_error(err, 409, "BlobTypeMismatch", "Blob exists as different type");
            return AZURE_ERR_CONFLICT;
        }
        if (ensure_capacity(b, size) != 0) {
            set_error(err, 500, "OutOfMemory", "Failed to allocate blob data");
            return AZURE_ERR_NOMEM;
        }
        b->size = size;
        return AZURE_OK;
    }

    b = create_blob(ctx, name, BLOB_TYPE_PAGE);
    if (!b) {
        set_error(err, 500, "OutOfMemory", "Too many blobs");
        return AZURE_ERR_NOMEM;
    }

    if (size > 0) {
        if (ensure_capacity(b, size) != 0) {
            set_error(err, 500, "OutOfMemory", "Failed to allocate blob data");
            return AZURE_ERR_NOMEM;
        }
    }
    b->size = size;
    return AZURE_OK;
}

static azure_err_t mock_page_blob_write(void *vctx, const char *name,
                                         int64_t offset, const uint8_t *data,
                                         size_t len, azure_error_t *err) {
    mock_azure_ctx_t *ctx = (mock_azure_ctx_t *)vctx;
    azure_err_t rc = pre_call(ctx, OP_PAGE_BLOB_WRITE, err);
    if (rc != AZURE_OK) return rc;

    /* Enforce 512-byte alignment on offset AND length */
    if (offset % PAGE_BLOB_ALIGNMENT != 0) {
        set_error(err, 400, "InvalidHeaderValue",
                  "Page write offset must be aligned to 512 bytes");
        return AZURE_ERR_ALIGNMENT;
    }
    if (len % PAGE_BLOB_ALIGNMENT != 0) {
        set_error(err, 400, "InvalidHeaderValue",
                  "Page write length must be aligned to 512 bytes");
        return AZURE_ERR_ALIGNMENT;
    }

    mock_blob_t *b = find_blob(ctx, name);
    if (!b || b->type != BLOB_TYPE_PAGE) {
        set_error(err, 404, "BlobNotFound", "Page blob not found");
        return AZURE_ERR_NOT_FOUND;
    }

    /* Auto-grow if needed */
    int64_t end = offset + (int64_t)len;
    if (end > b->size) {
        if (ensure_capacity(b, end) != 0) {
            set_error(err, 500, "OutOfMemory", "Failed to grow blob");
            return AZURE_ERR_NOMEM;
        }
        b->size = end;
    }

    memcpy(b->data + offset, data, len);
    return AZURE_OK;
}

static azure_err_t mock_page_blob_read(void *vctx, const char *name,
                                        int64_t offset, size_t len,
                                        azure_buffer_t *out,
                                        azure_error_t *err) {
    mock_azure_ctx_t *ctx = (mock_azure_ctx_t *)vctx;
    azure_err_t rc = pre_call(ctx, OP_PAGE_BLOB_READ, err);
    if (rc != AZURE_OK) return rc;

    mock_blob_t *b = find_blob(ctx, name);
    if (!b || b->type != BLOB_TYPE_PAGE) {
        set_error(err, 404, "BlobNotFound", "Page blob not found");
        return AZURE_ERR_NOT_FOUND;
    }

    if (offset > b->size) {
        set_error(err, 416, "InvalidRange", "Offset past end of blob");
        return AZURE_ERR_INVALID_ARG;
    }

    /* Clamp read to blob size */
    size_t avail = (size_t)(b->size - offset);
    size_t to_read = len < avail ? len : avail;

    /* Allocate/reallocate output buffer */
    if (out->capacity < len) {
        uint8_t *new_data = (uint8_t *)realloc(out->data, len);
        if (!new_data) {
            set_error(err, 500, "OutOfMemory", "Failed to allocate read buffer");
            return AZURE_ERR_NOMEM;
        }
        out->data = new_data;
        out->capacity = len;
    }

    /* Copy available data, zero-fill the rest (short read) */
    if (to_read > 0) {
        memcpy(out->data, b->data + offset, to_read);
    }
    if (to_read < len) {
        memset(out->data + to_read, 0, len - to_read);
    }
    out->size = len;

    return AZURE_OK;
}

static azure_err_t mock_page_blob_resize(void *vctx, const char *name,
                                          int64_t new_size,
                                          azure_error_t *err) {
    mock_azure_ctx_t *ctx = (mock_azure_ctx_t *)vctx;
    azure_err_t rc = pre_call(ctx, OP_PAGE_BLOB_RESIZE, err);
    if (rc != AZURE_OK) return rc;

    if (new_size < 0 || (new_size % PAGE_BLOB_ALIGNMENT != 0 && new_size != 0)) {
        set_error(err, 400, "InvalidHeaderValue",
                  "Page blob size must be aligned to 512 bytes");
        return AZURE_ERR_ALIGNMENT;
    }

    mock_blob_t *b = find_blob(ctx, name);
    if (!b || b->type != BLOB_TYPE_PAGE) {
        set_error(err, 404, "BlobNotFound", "Page blob not found");
        return AZURE_ERR_NOT_FOUND;
    }

    if (new_size > b->size) {
        if (ensure_capacity(b, new_size) != 0) {
            set_error(err, 500, "OutOfMemory", "Failed to grow blob");
            return AZURE_ERR_NOMEM;
        }
    }
    b->size = new_size;
    return AZURE_OK;
}

/* ── Block Blob Operations ────────────────────────────────────────── */

static azure_err_t mock_block_blob_upload(void *vctx, const char *name,
                                           const uint8_t *data, size_t len,
                                           azure_error_t *err) {
    mock_azure_ctx_t *ctx = (mock_azure_ctx_t *)vctx;
    azure_err_t rc = pre_call(ctx, OP_BLOCK_BLOB_UPLOAD, err);
    if (rc != AZURE_OK) return rc;

    mock_blob_t *b = find_blob(ctx, name);
    if (b && b->type != BLOB_TYPE_BLOCK) {
        set_error(err, 409, "BlobTypeMismatch", "Blob exists as different type");
        return AZURE_ERR_CONFLICT;
    }

    if (!b) {
        b = create_blob(ctx, name, BLOB_TYPE_BLOCK);
        if (!b) {
            set_error(err, 500, "OutOfMemory", "Too many blobs");
            return AZURE_ERR_NOMEM;
        }
    }

    /* Replace entire blob content */
    free_blob_data(b);
    if (len > 0) {
        b->data = (uint8_t *)malloc(len);
        if (!b->data) {
            set_error(err, 500, "OutOfMemory", "Failed to allocate blob data");
            return AZURE_ERR_NOMEM;
        }
        memcpy(b->data, data, len);
    }
    b->size = (int64_t)len;
    b->capacity = (int64_t)len;
    b->type = BLOB_TYPE_BLOCK;
    return AZURE_OK;
}

static azure_err_t mock_block_blob_download(void *vctx, const char *name,
                                              azure_buffer_t *out,
                                              azure_error_t *err) {
    mock_azure_ctx_t *ctx = (mock_azure_ctx_t *)vctx;
    azure_err_t rc = pre_call(ctx, OP_BLOCK_BLOB_DOWNLOAD, err);
    if (rc != AZURE_OK) return rc;

    mock_blob_t *b = find_blob(ctx, name);
    if (!b || b->type != BLOB_TYPE_BLOCK) {
        set_error(err, 404, "BlobNotFound", "Block blob not found");
        return AZURE_ERR_NOT_FOUND;
    }

    if (b->size > 0) {
        if (out->capacity < (size_t)b->size) {
            uint8_t *new_data = (uint8_t *)realloc(out->data, (size_t)b->size);
            if (!new_data) {
                set_error(err, 500, "OutOfMemory", "Failed to allocate download buffer");
                return AZURE_ERR_NOMEM;
            }
            out->data = new_data;
            out->capacity = (size_t)b->size;
        }
        memcpy(out->data, b->data, (size_t)b->size);
    }
    out->size = (size_t)b->size;
    return AZURE_OK;
}

/* ── Common Blob Operations ───────────────────────────────────────── */

static azure_err_t mock_blob_get_properties(void *vctx, const char *name,
                                              int64_t *size,
                                              char *lease_state_out,
                                              char *lease_status_out,
                                              azure_error_t *err) {
    mock_azure_ctx_t *ctx = (mock_azure_ctx_t *)vctx;
    azure_err_t rc = pre_call(ctx, OP_BLOB_GET_PROPERTIES, err);
    if (rc != AZURE_OK) return rc;

    mock_blob_t *b = find_blob(ctx, name);
    if (!b) {
        set_error(err, 404, "BlobNotFound", "Blob not found");
        return AZURE_ERR_NOT_FOUND;
    }

    if (size) *size = b->size;

    if (lease_state_out) {
        switch (b->lease_state) {
            case LEASE_AVAILABLE: strcpy(lease_state_out, "available"); break;
            case LEASE_LEASED:    strcpy(lease_state_out, "leased"); break;
            case LEASE_BREAKING:  strcpy(lease_state_out, "breaking"); break;
        }
    }

    if (lease_status_out) {
        if (b->lease_state == LEASE_LEASED || b->lease_state == LEASE_BREAKING)
            strcpy(lease_status_out, "locked");
        else
            strcpy(lease_status_out, "unlocked");
    }

    return AZURE_OK;
}

static azure_err_t mock_blob_delete_impl(void *vctx, const char *name,
                                          azure_error_t *err) {
    mock_azure_ctx_t *ctx = (mock_azure_ctx_t *)vctx;
    azure_err_t rc = pre_call(ctx, OP_BLOB_DELETE, err);
    if (rc != AZURE_OK) return rc;

    for (int i = 0; i < ctx->blob_count; i++) {
        if (strcmp(ctx->blobs[i].name, name) == 0) {
            free_blob_data(&ctx->blobs[i]);
            /* Shift remaining blobs down */
            if (i < ctx->blob_count - 1) {
                memmove(&ctx->blobs[i], &ctx->blobs[i + 1],
                        sizeof(mock_blob_t) * (size_t)(ctx->blob_count - 1 - i));
            }
            ctx->blob_count--;
            return AZURE_OK;
        }
    }

    set_error(err, 404, "BlobNotFound", "Blob not found");
    return AZURE_ERR_NOT_FOUND;
}

static azure_err_t mock_blob_exists_impl(void *vctx, const char *name,
                                          int *exists, azure_error_t *err) {
    mock_azure_ctx_t *ctx = (mock_azure_ctx_t *)vctx;
    azure_err_t rc = pre_call(ctx, OP_BLOB_EXISTS, err);
    if (rc != AZURE_OK) return rc;

    mock_blob_t *b = find_blob(ctx, name);
    if (exists) *exists = (b != NULL) ? 1 : 0;
    return AZURE_OK;
}

/* ── Lease Operations ─────────────────────────────────────────────── */

static azure_err_t mock_lease_acquire_impl(void *vctx, const char *name,
                                            int duration_secs,
                                            char *lease_id_out,
                                            size_t lease_id_size,
                                            azure_error_t *err) {
    mock_azure_ctx_t *ctx = (mock_azure_ctx_t *)vctx;
    azure_err_t rc = pre_call(ctx, OP_LEASE_ACQUIRE, err);
    if (rc != AZURE_OK) return rc;

    mock_blob_t *b = find_blob(ctx, name);
    if (!b) {
        set_error(err, 404, "BlobNotFound", "Blob not found");
        return AZURE_ERR_NOT_FOUND;
    }

    if (b->lease_state == LEASE_LEASED) {
        set_error(err, 409, "LeaseAlreadyPresent",
                  "There is already a lease present");
        return AZURE_ERR_CONFLICT;
    }

    if (b->lease_state == LEASE_BREAKING) {
        set_error(err, 409, "LeaseIsBreaking",
                  "The lease is in breaking state");
        return AZURE_ERR_CONFLICT;
    }

    /* Acquire the lease */
    b->lease_state = LEASE_LEASED;
    b->lease_duration = duration_secs;
    b->lease_acquired_at = time(NULL);
    generate_lease_id(ctx, b->lease_id, LEASE_ID_LEN);

    if (lease_id_out && lease_id_size > 0) {
        strncpy(lease_id_out, b->lease_id, lease_id_size - 1);
        lease_id_out[lease_id_size - 1] = '\0';
    }

    return AZURE_OK;
}

static azure_err_t mock_lease_renew_impl(void *vctx, const char *name,
                                          const char *lease_id,
                                          azure_error_t *err) {
    mock_azure_ctx_t *ctx = (mock_azure_ctx_t *)vctx;
    azure_err_t rc = pre_call(ctx, OP_LEASE_RENEW, err);
    if (rc != AZURE_OK) return rc;

    mock_blob_t *b = find_blob(ctx, name);
    if (!b) {
        set_error(err, 404, "BlobNotFound", "Blob not found");
        return AZURE_ERR_NOT_FOUND;
    }

    if (b->lease_state != LEASE_LEASED) {
        set_error(err, 409, "LeaseNotPresent",
                  "There is currently no lease on the blob");
        return AZURE_ERR_CONFLICT;
    }

    if (strcmp(b->lease_id, lease_id) != 0) {
        set_error(err, 409, "LeaseIdMismatch",
                  "The lease ID does not match");
        return AZURE_ERR_CONFLICT;
    }

    b->lease_acquired_at = time(NULL);
    return AZURE_OK;
}

static azure_err_t mock_lease_release_impl(void *vctx, const char *name,
                                            const char *lease_id,
                                            azure_error_t *err) {
    mock_azure_ctx_t *ctx = (mock_azure_ctx_t *)vctx;
    azure_err_t rc = pre_call(ctx, OP_LEASE_RELEASE, err);
    if (rc != AZURE_OK) return rc;

    mock_blob_t *b = find_blob(ctx, name);
    if (!b) {
        set_error(err, 404, "BlobNotFound", "Blob not found");
        return AZURE_ERR_NOT_FOUND;
    }

    if (b->lease_state != LEASE_LEASED) {
        set_error(err, 409, "LeaseNotPresent",
                  "There is currently no lease on the blob");
        return AZURE_ERR_CONFLICT;
    }

    if (strcmp(b->lease_id, lease_id) != 0) {
        set_error(err, 409, "LeaseIdMismatch",
                  "The lease ID does not match");
        return AZURE_ERR_CONFLICT;
    }

    b->lease_state = LEASE_AVAILABLE;
    memset(b->lease_id, 0, LEASE_ID_LEN);
    return AZURE_OK;
}

static azure_err_t mock_lease_break_impl(void *vctx, const char *name,
                                          int break_period_secs,
                                          int *remaining_secs,
                                          azure_error_t *err) {
    mock_azure_ctx_t *ctx = (mock_azure_ctx_t *)vctx;
    azure_err_t rc = pre_call(ctx, OP_LEASE_BREAK, err);
    if (rc != AZURE_OK) return rc;

    mock_blob_t *b = find_blob(ctx, name);
    if (!b) {
        set_error(err, 404, "BlobNotFound", "Blob not found");
        return AZURE_ERR_NOT_FOUND;
    }

    if (b->lease_state == LEASE_AVAILABLE) {
        set_error(err, 409, "LeaseNotPresent",
                  "There is currently no lease on the blob");
        return AZURE_ERR_CONFLICT;
    }

    if (break_period_secs <= 0) {
        /* Immediate break */
        b->lease_state = LEASE_AVAILABLE;
        memset(b->lease_id, 0, LEASE_ID_LEN);
        if (remaining_secs) *remaining_secs = 0;
    } else {
        b->lease_state = LEASE_BREAKING;
        b->break_period = break_period_secs;
        if (remaining_secs) *remaining_secs = break_period_secs;
        /*
        ** In the real mock, we'd use a timer. For testing, we transition
        ** to AVAILABLE immediately since we don't have async timers.
        ** Tests that need BREAKING state can inspect it before the next call.
        */
    }

    return AZURE_OK;
}

/* ── The vtable singleton ─────────────────────────────────────────── */

static azure_ops_t mock_ops = {
    .page_blob_create   = mock_page_blob_create,
    .page_blob_write    = mock_page_blob_write,
    .page_blob_read     = mock_page_blob_read,
    .page_blob_resize   = mock_page_blob_resize,
    .block_blob_upload  = mock_block_blob_upload,
    .block_blob_download = mock_block_blob_download,
    .blob_get_properties = mock_blob_get_properties,
    .blob_delete        = mock_blob_delete_impl,
    .blob_exists        = mock_blob_exists_impl,
    .lease_acquire      = mock_lease_acquire_impl,
    .lease_renew        = mock_lease_renew_impl,
    .lease_release      = mock_lease_release_impl,
    .lease_break        = mock_lease_break_impl,
};

/* ══════════════════════════════════════════════════════════════════════
** Public API — lifecycle
** ══════════════════════════════════════════════════════════════════════ */

mock_azure_ctx_t *mock_azure_create(void) {
    mock_azure_ctx_t *ctx = (mock_azure_ctx_t *)calloc(1, sizeof(mock_azure_ctx_t));
    return ctx;
}

void mock_azure_destroy(mock_azure_ctx_t *ctx) {
    if (!ctx) return;
    for (int i = 0; i < ctx->blob_count; i++) {
        free_blob_data(&ctx->blobs[i]);
    }
    free(ctx);
}

azure_ops_t *mock_azure_get_ops(void) {
    return &mock_ops;
}

void mock_reset(mock_azure_ctx_t *ctx) {
    if (!ctx) return;
    for (int i = 0; i < ctx->blob_count; i++) {
        free_blob_data(&ctx->blobs[i]);
    }
    ctx->blob_count = 0;
    memset(ctx->call_counts, 0, sizeof(ctx->call_counts));
    ctx->total_calls = 0;
    ctx->fail_rule_count = 0;
    ctx->next_lease_num = 0;
}

/* ══════════════════════════════════════════════════════════════════════
** Public API — failure injection
** ══════════════════════════════════════════════════════════════════════ */

void mock_set_fail_at(mock_azure_ctx_t *ctx, int call_number,
                      azure_err_t error_code) {
    if (!ctx || ctx->fail_rule_count >= MAX_FAIL_RULES) return;
    fail_rule_t *r = &ctx->fail_rules[ctx->fail_rule_count++];
    r->call_number = call_number;
    r->op = OP_COUNT;
    r->error_code = error_code;
    r->active = 1;
}

void mock_set_fail_operation(mock_azure_ctx_t *ctx, const char *op_name,
                             azure_err_t error_code) {
    if (!ctx || ctx->fail_rule_count >= MAX_FAIL_RULES) return;
    op_index_t idx = op_name_to_index(op_name);
    if (idx == OP_COUNT) return; /* Unknown operation name */
    fail_rule_t *r = &ctx->fail_rules[ctx->fail_rule_count++];
    r->call_number = 0; /* 0 = by operation name */
    r->op = idx;
    r->error_code = error_code;
    r->active = 1;
}

void mock_clear_failures(mock_azure_ctx_t *ctx) {
    if (!ctx) return;
    ctx->fail_rule_count = 0;
}

/* ══════════════════════════════════════════════════════════════════════
** Public API — call counting
** ══════════════════════════════════════════════════════════════════════ */

int mock_get_call_count(mock_azure_ctx_t *ctx, const char *op_name) {
    if (!ctx) return 0;
    op_index_t idx = op_name_to_index(op_name);
    if (idx == OP_COUNT) return 0;
    return ctx->call_counts[idx];
}

int mock_get_total_call_count(mock_azure_ctx_t *ctx) {
    if (!ctx) return 0;
    return ctx->total_calls;
}

void mock_reset_call_counts(mock_azure_ctx_t *ctx) {
    if (!ctx) return;
    memset(ctx->call_counts, 0, sizeof(ctx->call_counts));
    ctx->total_calls = 0;
}

/* ══════════════════════════════════════════════════════════════════════
** Public API — state inspection
** ══════════════════════════════════════════════════════════════════════ */

const uint8_t *mock_get_page_blob_data(mock_azure_ctx_t *ctx,
                                        const char *name) {
    if (!ctx) return NULL;
    mock_blob_t *b = find_blob(ctx, name);
    if (!b || b->type != BLOB_TYPE_PAGE) return NULL;
    return b->data;
}

int64_t mock_get_page_blob_size(mock_azure_ctx_t *ctx, const char *name) {
    if (!ctx) return -1;
    mock_blob_t *b = find_blob(ctx, name);
    if (!b || b->type != BLOB_TYPE_PAGE) return -1;
    return b->size;
}

int mock_is_leased(mock_azure_ctx_t *ctx, const char *name) {
    if (!ctx) return 0;
    mock_blob_t *b = find_blob(ctx, name);
    if (!b) return 0;
    return b->lease_state == LEASE_LEASED ? 1 : 0;
}

mock_lease_state_t mock_get_lease_state(mock_azure_ctx_t *ctx,
                                         const char *name) {
    if (!ctx) return LEASE_AVAILABLE;
    mock_blob_t *b = find_blob(ctx, name);
    if (!b) return LEASE_AVAILABLE;
    return b->lease_state;
}

const char *mock_get_lease_id(mock_azure_ctx_t *ctx, const char *name) {
    if (!ctx) return NULL;
    mock_blob_t *b = find_blob(ctx, name);
    if (!b || b->lease_state != LEASE_LEASED) return NULL;
    return b->lease_id;
}

int64_t mock_get_block_blob_size(mock_azure_ctx_t *ctx, const char *name) {
    if (!ctx) return -1;
    mock_blob_t *b = find_blob(ctx, name);
    if (!b || b->type != BLOB_TYPE_BLOCK) return -1;
    return b->size;
}

const uint8_t *mock_get_block_blob_data(mock_azure_ctx_t *ctx,
                                         const char *name) {
    if (!ctx) return NULL;
    mock_blob_t *b = find_blob(ctx, name);
    if (!b || b->type != BLOB_TYPE_BLOCK) return NULL;
    return b->data;
}

int mock_blob_exists(mock_azure_ctx_t *ctx, const char *name) {
    if (!ctx) return 0;
    return find_blob(ctx, name) != NULL ? 1 : 0;
}
