/*
** mock_azure_ops.h — In-memory mock of the azure_ops_t interface
**
** Provides a complete mock implementation of the Azure operations vtable
** for unit testing the azqlite VFS without any network or Azure dependency.
**
** Features:
**   - Page blob simulation with 512-byte alignment enforcement
**   - Block blob simulation (key-value store)
**   - Lease state machine (AVAILABLE → LEASED → BREAKING → AVAILABLE)
**   - Failure injection (by call number or by operation name)
**   - Call counting and state inspection
**
** All public types (azure_ops_t, azure_err_t, etc.) come from azure_client.h.
*/
#ifndef MOCK_AZURE_OPS_H
#define MOCK_AZURE_OPS_H

#include "../src/azure_client.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ══════════════════════════════════════════════════════════════════════
** Lease states
** ══════════════════════════════════════════════════════════════════════ */

typedef enum {
    LEASE_AVAILABLE = 0,
    LEASE_LEASED    = 1,
    LEASE_BREAKING  = 2,
} mock_lease_state_t;

/* ══════════════════════════════════════════════════════════════════════
** Mock context — the "azure_mock_t" opaque pointer
** ══════════════════════════════════════════════════════════════════════ */

/* Forward declaration */
typedef struct mock_azure_ctx mock_azure_ctx_t;

/* ══════════════════════════════════════════════════════════════════════
** Mock lifecycle
** ══════════════════════════════════════════════════════════════════════ */

/* Create a new mock context. Returns NULL on allocation failure. */
mock_azure_ctx_t *mock_azure_create(void);

/* Destroy mock context and free all blobs/leases. */
void mock_azure_destroy(mock_azure_ctx_t *ctx);

/* Get the azure_ops_t vtable pointing to mock implementations. */
azure_ops_t *mock_azure_get_ops(void);

/* Reset all state (blobs, leases, failures, counters). */
void mock_reset(mock_azure_ctx_t *ctx);

/* ══════════════════════════════════════════════════════════════════════
** Failure injection
** ══════════════════════════════════════════════════════════════════════ */

/*
** Make the Nth overall call (1-based) fail with the given error code.
** Example: mock_set_fail_at(ctx, 3, AZURE_ERR_NETWORK)
**   → first two calls succeed, third call fails.
*/
void mock_set_fail_at(mock_azure_ctx_t *ctx, int call_number,
                      azure_err_t error_code);

/*
** Make ALL calls to a specific operation fail with the given error code.
** op_name is one of: "page_blob_create", "page_blob_write",
**   "page_blob_read", "page_blob_resize", "block_blob_upload",
**   "block_blob_download", "blob_get_properties", "blob_delete",
**   "blob_exists", "lease_acquire", "lease_renew", "lease_release",
**   "lease_break"
*/
void mock_set_fail_operation(mock_azure_ctx_t *ctx, const char *op_name,
                             azure_err_t error_code);

/*
** Make the Nth call (1-based) to a specific operation fail.
** Example: mock_set_fail_operation_at(ctx, "page_blob_write", 2, AZURE_ERR_NETWORK)
**   → first page_blob_write succeeds, second page_blob_write fails.
*/
void mock_set_fail_operation_at(mock_azure_ctx_t *ctx, const char *op_name,
                                int op_call_number, azure_err_t error_code);

/* Clear all failure injection rules. */
void mock_clear_failures(mock_azure_ctx_t *ctx);

/* ══════════════════════════════════════════════════════════════════════
** Call counting
** ══════════════════════════════════════════════════════════════════════ */

/* Get the number of times a specific operation was called. */
int mock_get_call_count(mock_azure_ctx_t *ctx, const char *op_name);

/* Get total number of calls across all operations. */
int mock_get_total_call_count(mock_azure_ctx_t *ctx);

/* Reset all call counters. */
void mock_reset_call_counts(mock_azure_ctx_t *ctx);

/* ══════════════════════════════════════════════════════════════════════
** State inspection
** ══════════════════════════════════════════════════════════════════════ */

/*
** Get a pointer to the raw page blob data for inspection.
** Returns NULL if blob doesn't exist. Do NOT free the pointer.
*/
const uint8_t *mock_get_page_blob_data(mock_azure_ctx_t *ctx,
                                        const char *name);

/* Get the current size of a page blob. Returns -1 if not found. */
int64_t mock_get_page_blob_size(mock_azure_ctx_t *ctx, const char *name);

/* Check if a blob has an active lease. Returns 1 if leased, 0 if not. */
int mock_is_leased(mock_azure_ctx_t *ctx, const char *name);

/* Get the lease state of a blob. Returns LEASE_AVAILABLE if blob not found. */
mock_lease_state_t mock_get_lease_state(mock_azure_ctx_t *ctx,
                                         const char *name);

/* Get the lease ID of a blob. Returns NULL if not leased. */
const char *mock_get_lease_id(mock_azure_ctx_t *ctx, const char *name);

/*
** Check if a block blob exists and get its size.
** Returns -1 if not found.
*/
int64_t mock_get_block_blob_size(mock_azure_ctx_t *ctx, const char *name);

/* Get a pointer to block blob data. Returns NULL if not found. */
const uint8_t *mock_get_block_blob_data(mock_azure_ctx_t *ctx,
                                         const char *name);

/* Check if any blob (page or block) exists by name. */
int mock_blob_exists(mock_azure_ctx_t *ctx, const char *name);

/* ══════════════════════════════════════════════════════════════════════
** Write recording — tracks page_blob_write calls for coalescing tests
** ══════════════════════════════════════════════════════════════════════ */

#define MOCK_MAX_WRITE_RECORDS 1024

typedef struct {
    int64_t offset;
    size_t  len;
} mock_write_record_t;

/* Get the number of recorded page_blob_write calls since last clear. */
int mock_get_write_record_count(mock_azure_ctx_t *ctx);

/* Get a specific write record (0-based). Returns zeroed record if out of range. */
mock_write_record_t mock_get_write_record(mock_azure_ctx_t *ctx, int idx);

/* Clear all write records. Also called by mock_reset(). */
void mock_clear_write_records(mock_azure_ctx_t *ctx);

/* ══════════════════════════════════════════════════════════════════════
** Append call recording — tracks append_blob_append data lengths
** for chunking tests (4 MiB boundary verification)
** ══════════════════════════════════════════════════════════════════════ */

#define MOCK_MAX_APPEND_RECORDS 1024

typedef struct {
    size_t  len;
} mock_append_record_t;

/* Get the number of recorded append_blob_append calls since last clear. */
int mock_get_append_record_count(mock_azure_ctx_t *ctx);

/* Get a specific append record (0-based). Returns zeroed record if out of range. */
mock_append_record_t mock_get_append_record(mock_azure_ctx_t *ctx, int idx);

/* Clear all append records. Also called by mock_reset(). */
void mock_clear_append_records(mock_azure_ctx_t *ctx);

/* ══════════════════════════════════════════════════════════════════════
** Append blob state inspection (for WAL mode tests)
** ══════════════════════════════════════════════════════════════════════ */

/* Get append blob accumulated data. Returns NULL if blob doesn't exist. */
const unsigned char *mock_get_append_data(mock_azure_ctx_t *ctx,
                                           const char *name);

/* Get total size of append blob data. Returns -1 if not found. */
int64_t mock_get_append_size(mock_azure_ctx_t *ctx, const char *name);

/* Clear append blob data (reset buffer to empty, keep blob alive). */
void mock_reset_append_data(mock_azure_ctx_t *ctx, const char *name);

/* Bump the ETag of a blob to simulate an external write. */
void mock_bump_etag(mock_azure_ctx_t *ctx, const char *name);

#ifdef __cplusplus
}
#endif

#endif /* MOCK_AZURE_OPS_H */
