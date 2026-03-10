/*
** azure_client_stub.c — Stub Azure client implementation
**
** These are placeholder implementations that return errors.
** Frodo will replace this file with the real libcurl-based client.
** This stub allows the project to compile and link before the
** production Azure client is ready.
*/

#include "azure_client.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ---- Stub ops that always return AZURE_ERR_UNKNOWN ---- */

static azure_err_t stub_page_blob_create(void *ctx, const char *name,
                                          int64_t size, azure_error_t *err) {
    (void)ctx; (void)name; (void)size;
    if (err) {
        err->code = AZURE_ERR_UNKNOWN;
        snprintf(err->error_message, sizeof(err->error_message),
                 "Azure client not implemented (stub)");
    }
    return AZURE_ERR_UNKNOWN;
}

static azure_err_t stub_page_blob_write(void *ctx, const char *name,
                                         int64_t offset, const uint8_t *data,
                                         size_t len, const char *lease_id,
                                         azure_error_t *err) {
    (void)ctx; (void)name; (void)offset; (void)data; (void)len; (void)lease_id;
    if (err) {
        err->code = AZURE_ERR_UNKNOWN;
        snprintf(err->error_message, sizeof(err->error_message),
                 "Azure client not implemented (stub)");
    }
    return AZURE_ERR_UNKNOWN;
}

static azure_err_t stub_page_blob_read(void *ctx, const char *name,
                                        int64_t offset, size_t len,
                                        azure_buffer_t *out, azure_error_t *err) {
    (void)ctx; (void)name; (void)offset; (void)len; (void)out;
    if (err) {
        err->code = AZURE_ERR_UNKNOWN;
        snprintf(err->error_message, sizeof(err->error_message),
                 "Azure client not implemented (stub)");
    }
    return AZURE_ERR_UNKNOWN;
}

static azure_err_t stub_page_blob_resize(void *ctx, const char *name,
                                          int64_t new_size,
                                          const char *lease_id,
                                          azure_error_t *err) {
    (void)ctx; (void)name; (void)new_size; (void)lease_id;
    if (err) {
        err->code = AZURE_ERR_UNKNOWN;
        snprintf(err->error_message, sizeof(err->error_message),
                 "Azure client not implemented (stub)");
    }
    return AZURE_ERR_UNKNOWN;
}

static azure_err_t stub_block_blob_upload(void *ctx, const char *name,
                                           const uint8_t *data, size_t len,
                                           azure_error_t *err) {
    (void)ctx; (void)name; (void)data; (void)len;
    if (err) {
        err->code = AZURE_ERR_UNKNOWN;
        snprintf(err->error_message, sizeof(err->error_message),
                 "Azure client not implemented (stub)");
    }
    return AZURE_ERR_UNKNOWN;
}

static azure_err_t stub_block_blob_download(void *ctx, const char *name,
                                             azure_buffer_t *out,
                                             azure_error_t *err) {
    (void)ctx; (void)name; (void)out;
    if (err) {
        err->code = AZURE_ERR_UNKNOWN;
        snprintf(err->error_message, sizeof(err->error_message),
                 "Azure client not implemented (stub)");
    }
    return AZURE_ERR_UNKNOWN;
}

static azure_err_t stub_blob_get_properties(void *ctx, const char *name,
                                             int64_t *size,
                                             char *lease_state,
                                             char *lease_status,
                                             azure_error_t *err) {
    (void)ctx; (void)name; (void)size;
    (void)lease_state; (void)lease_status;
    if (err) {
        err->code = AZURE_ERR_UNKNOWN;
        snprintf(err->error_message, sizeof(err->error_message),
                 "Azure client not implemented (stub)");
    }
    return AZURE_ERR_UNKNOWN;
}

static azure_err_t stub_blob_delete(void *ctx, const char *name,
                                     azure_error_t *err) {
    (void)ctx; (void)name;
    if (err) {
        err->code = AZURE_ERR_UNKNOWN;
        snprintf(err->error_message, sizeof(err->error_message),
                 "Azure client not implemented (stub)");
    }
    return AZURE_ERR_UNKNOWN;
}

static azure_err_t stub_blob_exists(void *ctx, const char *name,
                                     int *exists, azure_error_t *err) {
    (void)ctx; (void)name;
    if (exists) *exists = 0;
    if (err) {
        err->code = AZURE_ERR_UNKNOWN;
        snprintf(err->error_message, sizeof(err->error_message),
                 "Azure client not implemented (stub)");
    }
    return AZURE_ERR_UNKNOWN;
}

static azure_err_t stub_lease_acquire(void *ctx, const char *name,
                                       int duration_secs, char *lease_id_out,
                                       size_t lease_id_size,
                                       azure_error_t *err) {
    (void)ctx; (void)name; (void)duration_secs;
    (void)lease_id_out; (void)lease_id_size;
    if (err) {
        err->code = AZURE_ERR_UNKNOWN;
        snprintf(err->error_message, sizeof(err->error_message),
                 "Azure client not implemented (stub)");
    }
    return AZURE_ERR_UNKNOWN;
}

static azure_err_t stub_lease_renew(void *ctx, const char *name,
                                     const char *lease_id, azure_error_t *err) {
    (void)ctx; (void)name; (void)lease_id;
    if (err) {
        err->code = AZURE_ERR_UNKNOWN;
        snprintf(err->error_message, sizeof(err->error_message),
                 "Azure client not implemented (stub)");
    }
    return AZURE_ERR_UNKNOWN;
}

static azure_err_t stub_lease_release(void *ctx, const char *name,
                                       const char *lease_id, azure_error_t *err) {
    (void)ctx; (void)name; (void)lease_id;
    if (err) {
        err->code = AZURE_ERR_UNKNOWN;
        snprintf(err->error_message, sizeof(err->error_message),
                 "Azure client not implemented (stub)");
    }
    return AZURE_ERR_UNKNOWN;
}

static azure_err_t stub_lease_break(void *ctx, const char *name,
                                     int break_period_secs,
                                     int *remaining_secs,
                                     azure_error_t *err) {
    (void)ctx; (void)name; (void)break_period_secs;
    (void)remaining_secs;
    if (err) {
        err->code = AZURE_ERR_UNKNOWN;
        snprintf(err->error_message, sizeof(err->error_message),
                 "Azure client not implemented (stub)");
    }
    return AZURE_ERR_UNKNOWN;
}

/* ---- Static stub ops vtable ---- */

static azure_ops_t g_stubOps = {
    stub_page_blob_create,
    stub_page_blob_write,
    stub_page_blob_read,
    stub_page_blob_resize,
    stub_block_blob_upload,
    stub_block_blob_download,
    stub_blob_get_properties,
    stub_blob_delete,
    stub_blob_exists,
    stub_lease_acquire,
    stub_lease_renew,
    stub_lease_release,
    stub_lease_break
};

/* ---- Public functions ---- */

azure_err_t azure_client_create(const azure_client_config_t *config,
                                azure_client_t **client,
                                azure_error_t *err) {
    (void)config;
    if (client) *client = NULL;
    if (err) {
        err->code = AZURE_ERR_UNKNOWN;
        snprintf(err->error_message, sizeof(err->error_message),
                 "Azure client not implemented — stub only. "
                 "Frodo will provide the real implementation.");
    }
    return AZURE_ERR_UNKNOWN;
}

void azure_client_destroy(azure_client_t *client) {
    (void)client;
}

const azure_ops_t *azure_client_get_ops(void) {
    return &g_stubOps;
}

void *azure_client_get_ctx(azure_client_t *client) {
    return (void *)client;
}
