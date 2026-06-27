/*
** test_chaos.c — Phase 1/P0 Chaos Testing for Azure Mock
**
** Tests for deterministic chaos scenarios without Toxiproxy:
**   - Missing ETag response simulation
**   - Mock time advancement for lease expiry
**   - Lease acquire race conditions
**   - Lease expiry after time advancement
**
** These tests validate that the VFS handles edge cases correctly.
*/

#include "../sqlite-autoconf-3520000/sqlite3.h"
#include "sqlite_objs.h"
#include "mock_azure_ops.h"
#include "test_harness.h"
#include <string.h>
#include <stdlib.h>

/* ── Helpers ──────────────────────────────────────────────────────── */

static mock_azure_ctx_t *ch_ctx = NULL;
static azure_ops_t      *ch_ops = NULL;
static time_t ch_fake_time = 0;

static void ch_setup(void) {
    if (ch_ctx) mock_reset(ch_ctx);
    else        ch_ctx = mock_azure_create();
    ch_ops = mock_azure_get_ops();
}

/* Aliases for brevity */
#define g_ctx ch_ctx
#define g_ops ch_ops
#define setup ch_setup

static time_t ch_fake_time_fn(time_t *out) {
    if (out) *out = ch_fake_time;
    return ch_fake_time;
}

/* ══════════════════════════════════════════════════════════════════════
** SECTION 1: ETag Chaos Testing
** ══════════════════════════════════════════════════════════════════════ */

TEST(chaos_clear_etag_basic) {
    setup();
    azure_error_t err;
    const char *name = "test-etag-chaos.db";
    
    /* Create blob and verify it has an ETag */
    azure_err_t rc = g_ops->page_blob_create(g_ctx, name, 4096, &err);
    ASSERT_AZURE_OK(rc);
    ASSERT_NE(err.etag[0], '\0');
    
    /* Suppress the next ETag response */
    int clear_rc = mock_chaos_clear_etag(g_ctx, name);
    ASSERT_EQ(clear_rc, 0);
    
    /* The next blob_get_properties should return an empty ETag */
    memset(&err, 0, sizeof(err));
    int64_t size = 0;
    rc = g_ops->blob_get_properties(g_ctx, name, &size, NULL, NULL, &err);
    ASSERT_AZURE_OK(rc);
    ASSERT_EQ(size, 4096);
    ASSERT_EQ(err.etag[0], '\0');

    /* The stored ETag remains intact for later responses. */
    memset(&err, 0, sizeof(err));
    rc = g_ops->blob_get_properties(g_ctx, name, &size, NULL, NULL, &err);
    ASSERT_AZURE_OK(rc);
    ASSERT_NE(err.etag[0], '\0');
}

TEST(chaos_clear_etag_nonexistent_blob) {
    setup();
    
    /* Clearing ETag on non-existent blob should fail */
    int rc = mock_chaos_clear_etag(g_ctx, "nonexistent.db");
    ASSERT_EQ(rc, -1);
}

TEST(chaos_cleared_etag_write_preserves_clear) {
    setup();
    azure_error_t err;
    const char *name = "test-etag-write.db";
    
    /* Create blob */
    azure_err_t rc = g_ops->page_blob_create(g_ctx, name, 4096, &err);
    ASSERT_AZURE_OK(rc);
    
    /* Suppress one ETag response, then write to blob */
    mock_chaos_clear_etag(g_ctx, name);

    uint8_t data[512];
    memset(data, 0x42, sizeof(data));
    rc = g_ops->page_blob_write(g_ctx, name, 0, data, sizeof(data),
                                NULL, NULL, &err);
    ASSERT_AZURE_OK(rc);
    ASSERT_EQ(err.etag[0], '\0');
    
    /* Only the response was suppressed; the blob still has an ETag afterward. */
    memset(&err, 0, sizeof(err));
    int64_t size = 0;
    rc = g_ops->blob_get_properties(g_ctx, name, &size, NULL, NULL, &err);
    ASSERT_AZURE_OK(rc);
    ASSERT_NE(err.etag[0], '\0');
}

/* ══════════════════════════════════════════════════════════════════════
** SECTION 2: Mock Time Advancement for Lease Expiry
** ══════════════════════════════════════════════════════════════════════ */

TEST(chaos_time_offset_basic) {
    setup();
    
    /* Initial offset should be zero */
    ASSERT_EQ(mock_chaos_get_time_offset(g_ctx), 0);
    
    /* Advance time by 60 seconds */
    mock_chaos_advance_time(g_ctx, 60);
    ASSERT_EQ(mock_chaos_get_time_offset(g_ctx), 60);
    
    /* Advance again */
    mock_chaos_advance_time(g_ctx, 30);
    ASSERT_EQ(mock_chaos_get_time_offset(g_ctx), 90);
    
    /* Reset */
    mock_chaos_reset_time(g_ctx);
    ASSERT_EQ(mock_chaos_get_time_offset(g_ctx), 0);
}

TEST(chaos_vfs_time_hook_links) {
    ch_fake_time = 12345;
    sqlite_objs_test_set_time_fn(ch_fake_time_fn);
    sqlite_objs_test_set_time_fn(NULL);
}

TEST(chaos_lease_acquire_with_time_offset) {
    setup();
    azure_error_t err;
    const char *name = "test-lease-time.db";
    char lease_id[128];
    
    /* Create blob */
    azure_err_t rc = g_ops->page_blob_create(g_ctx, name, 4096, &err);
    ASSERT_AZURE_OK(rc);
    
    /* Advance time before lease acquire */
    mock_chaos_advance_time(g_ctx, 100);
    
    /* Acquire lease */
    rc = g_ops->lease_acquire(g_ctx, name, 60, lease_id, sizeof(lease_id), &err);
    ASSERT_AZURE_OK(rc);
    ASSERT_NE(lease_id[0], '\0');
    
    /* Verify blob is leased */
    ASSERT_EQ(mock_is_leased(g_ctx, name), 1);
}

TEST(chaos_lease_renew_tracks_advanced_time) {
    setup();
    azure_error_t err;
    const char *name = "test-lease-renew-time.db";
    char lease_id[128];
    
    /* Create blob and acquire lease */
    g_ops->page_blob_create(g_ctx, name, 4096, &err);
    g_ops->lease_acquire(g_ctx, name, 60, lease_id, sizeof(lease_id), &err);
    
    /* Advance time 30 seconds */
    mock_chaos_advance_time(g_ctx, 30);
    
    /* Renew lease */
    azure_err_t rc = g_ops->lease_renew(g_ctx, name, lease_id, &err);
    ASSERT_AZURE_OK(rc);
    
    /* Lease should still be active */
    ASSERT_EQ(mock_is_leased(g_ctx, name), 1);
}

TEST(chaos_lease_renew_after_expiry_fails) {
    setup();
    azure_error_t err;
    const char *name = "test-lease-expired-renew.db";
    char lease_id[128];

    g_ops->page_blob_create(g_ctx, name, 4096, &err);
    g_ops->lease_acquire(g_ctx, name, 30, lease_id, sizeof(lease_id), &err);

    mock_chaos_advance_time(g_ctx, 31);

    azure_err_t rc = g_ops->lease_renew(g_ctx, name, lease_id, &err);
    ASSERT_AZURE_ERR(rc, AZURE_ERR_LEASE_EXPIRED);
    ASSERT_EQ(mock_get_lease_state(g_ctx, name), LEASE_AVAILABLE);
}

TEST(chaos_expired_lease_allows_new_acquire) {
    setup();
    azure_error_t err;
    const char *name = "test-lease-expired-reacquire.db";
    char lease_id1[128];
    char lease_id2[128];

    g_ops->page_blob_create(g_ctx, name, 4096, &err);
    g_ops->lease_acquire(g_ctx, name, 30, lease_id1, sizeof(lease_id1), &err);
    mock_chaos_advance_time(g_ctx, 31);

    azure_err_t rc = g_ops->lease_acquire(g_ctx, name, 30, lease_id2, sizeof(lease_id2), &err);
    ASSERT_AZURE_OK(rc);
    ASSERT_STR_NE(lease_id1, lease_id2);
}

TEST(chaos_write_with_expired_lease_fails) {
    setup();
    azure_error_t err;
    const char *name = "test-expired-lease-write.db";
    char lease_id[128];
    uint8_t data[512];
    memset(data, 0x5a, sizeof(data));

    g_ops->page_blob_create(g_ctx, name, 4096, &err);
    g_ops->lease_acquire(g_ctx, name, 30, lease_id, sizeof(lease_id), &err);
    mock_chaos_advance_time(g_ctx, 31);

    azure_err_t rc = g_ops->page_blob_write(g_ctx, name, 0, data, sizeof(data),
                                            lease_id, NULL, &err);
    ASSERT_AZURE_ERR(rc, AZURE_ERR_LEASE_EXPIRED);
    ASSERT_EQ(mock_get_lease_state(g_ctx, name), LEASE_AVAILABLE);
}

/* ══════════════════════════════════════════════════════════════════════
** SECTION 3: Deterministic Lease Race Conditions
** ══════════════════════════════════════════════════════════════════════ */

TEST(chaos_lease_race_double_acquire_fails) {
    setup();
    azure_error_t err1, err2;
    const char *name = "test-lease-race.db";
    char lease_id1[128], lease_id2[128];
    
    /* Create blob */
    g_ops->page_blob_create(g_ctx, name, 4096, &err1);
    
    /* First acquire succeeds */
    azure_err_t rc1 = g_ops->lease_acquire(g_ctx, name, 60,
                                            lease_id1, sizeof(lease_id1), &err1);
    ASSERT_AZURE_OK(rc1);
    
    /* Second acquire fails with CONFLICT */
    azure_err_t rc2 = g_ops->lease_acquire(g_ctx, name, 60,
                                            lease_id2, sizeof(lease_id2), &err2);
    ASSERT_AZURE_ERR(rc2, AZURE_ERR_CONFLICT);
    ASSERT_EQ(err2.http_status, 409);
}

TEST(chaos_lease_race_renew_wrong_id_fails) {
    setup();
    azure_error_t err;
    const char *name = "test-lease-wrong-id.db";
    char lease_id[128];
    
    /* Create blob and acquire lease */
    g_ops->page_blob_create(g_ctx, name, 4096, &err);
    g_ops->lease_acquire(g_ctx, name, 60, lease_id, sizeof(lease_id), &err);
    
    /* Try to renew with wrong lease ID */
    azure_err_t rc = g_ops->lease_renew(g_ctx, name, "wrong-lease-id", &err);
    ASSERT_AZURE_ERR(rc, AZURE_ERR_CONFLICT);
    ASSERT_EQ(err.http_status, 409);
}

TEST(chaos_lease_race_release_then_acquire_succeeds) {
    setup();
    azure_error_t err;
    const char *name = "test-lease-release-acquire.db";
    char lease_id1[128], lease_id2[128];
    
    /* Create blob and acquire lease */
    g_ops->page_blob_create(g_ctx, name, 4096, &err);
    g_ops->lease_acquire(g_ctx, name, 60, lease_id1, sizeof(lease_id1), &err);
    
    /* Release lease */
    azure_err_t rc = g_ops->lease_release(g_ctx, name, lease_id1, &err);
    ASSERT_AZURE_OK(rc);
    
    /* Second acquire should succeed */
    rc = g_ops->lease_acquire(g_ctx, name, 60, lease_id2, sizeof(lease_id2), &err);
    ASSERT_AZURE_OK(rc);
    
    /* Lease IDs should be different */
    ASSERT_STR_NE(lease_id1, lease_id2);
}

/* ══════════════════════════════════════════════════════════════════════
** SECTION 4: Deterministic Lease Expiry (conceptual)
**
** Note: Current mock doesn't auto-expire leases based on duration.
** These tests document the expected behavior for future implementation.
** For Phase 1/P0, we focus on time advancement infrastructure.
** ══════════════════════════════════════════════════════════════════════ */

TEST(chaos_lease_state_transitions) {
    setup();
    azure_error_t err;
    const char *name = "test-lease-states.db";
    char lease_id[128];
    
    /* Create blob — initially AVAILABLE */
    g_ops->page_blob_create(g_ctx, name, 4096, &err);
    ASSERT_EQ(mock_get_lease_state(g_ctx, name), LEASE_AVAILABLE);
    
    /* Acquire lease — becomes LEASED */
    g_ops->lease_acquire(g_ctx, name, 60, lease_id, sizeof(lease_id), &err);
    ASSERT_EQ(mock_get_lease_state(g_ctx, name), LEASE_LEASED);
    
    /* Release lease — back to AVAILABLE */
    g_ops->lease_release(g_ctx, name, lease_id, &err);
    ASSERT_EQ(mock_get_lease_state(g_ctx, name), LEASE_AVAILABLE);
}

TEST(chaos_lease_break_transitions_to_breaking) {
    setup();
    azure_error_t err;
    const char *name = "test-lease-break.db";
    char lease_id[128];
    
    /* Create blob and acquire lease */
    g_ops->page_blob_create(g_ctx, name, 4096, &err);
    g_ops->lease_acquire(g_ctx, name, 60, lease_id, sizeof(lease_id), &err);
    ASSERT_EQ(mock_get_lease_state(g_ctx, name), LEASE_LEASED);
    
    /* Break lease */
    int remaining = 0;
    azure_err_t rc = g_ops->lease_break(g_ctx, name, 0, &remaining, &err);
    ASSERT_AZURE_OK(rc);
    
    /* Should transition to BREAKING (in real Azure) or AVAILABLE (immediate) */
    mock_lease_state_t state = mock_get_lease_state(g_ctx, name);
    ASSERT_TRUE(state == LEASE_BREAKING || state == LEASE_AVAILABLE);
}

/* ══════════════════════════════════════════════════════════════════════
** SECTION 5: Combined Chaos Scenarios
** ══════════════════════════════════════════════════════════════════════ */

TEST(chaos_combined_etag_and_time) {
    setup();
    azure_error_t err;
    const char *name = "test-chaos-combined.db";
    char lease_id[128];
    
    /* Create blob */
    g_ops->page_blob_create(g_ctx, name, 4096, &err);
    
    /* Clear ETag */
    mock_chaos_clear_etag(g_ctx, name);
    
    /* Advance time */
    mock_chaos_advance_time(g_ctx, 50);
    
    /* Acquire lease — should work despite cleared ETag */
    azure_err_t rc = g_ops->lease_acquire(g_ctx, name, 60,
                                          lease_id, sizeof(lease_id), &err);
    ASSERT_AZURE_OK(rc);
    
    /* Verify state */
    ASSERT_EQ(mock_is_leased(g_ctx, name), 1);
    ASSERT_EQ(mock_chaos_get_time_offset(g_ctx), 50);
}

TEST(chaos_reset_clears_all_chaos_state) {
    setup();
    azure_error_t err;
    const char *name = "test-chaos-reset.db";
    
    /* Create blob */
    g_ops->page_blob_create(g_ctx, name, 4096, &err);
    
    /* Apply chaos */
    mock_chaos_clear_etag(g_ctx, name);
    mock_chaos_advance_time(g_ctx, 100);
    
    /* Reset context */
    mock_reset(g_ctx);
    
    /* Time offset should be reset */
    ASSERT_EQ(mock_chaos_get_time_offset(g_ctx), 0);
    
    /* Blob should be gone */
    ASSERT_EQ(mock_blob_exists(g_ctx, name), 0);
}

/* ══════════════════════════════════════════════════════════════════════
** SECTION 6: Phase 2 — Transient Failure/Recovery Scenarios
** ══════════════════════════════════════════════════════════════════════ */

TEST(chaos_transient_failure_single_operation_recovers) {
    setup();
    azure_error_t err;
    const char *name = "test-transient-single.db";
    
    /* Create blob successfully */
    azure_err_t rc = g_ops->page_blob_create(g_ctx, name, 4096, &err);
    ASSERT_AZURE_OK(rc);
    
    /* Inject failure at the next call (total call #3: create blob, get_properties internally, then this read) */
    mock_set_fail_at(g_ctx, 3, AZURE_ERR_NETWORK);
    
    /* Next read should fail with network error */
    azure_buffer_t buf = {0};
    rc = g_ops->page_blob_read(g_ctx, name, 0, 512, &buf, &err);
    ASSERT_AZURE_ERR(rc, AZURE_ERR_NETWORK);
    
    /* Subsequent read should succeed (failure was one-shot) */
    memset(&err, 0, sizeof(err));
    rc = g_ops->page_blob_read(g_ctx, name, 0, 512, &buf, &err);
    ASSERT_AZURE_OK(rc);
    ASSERT_EQ(buf.size, 512);
    
    if (buf.data) free(buf.data);
}

TEST(chaos_transient_failure_write_recovers) {
    setup();
    azure_error_t err;
    const char *name = "test-transient-write.db";
    uint8_t data[512];
    memset(data, 0xAB, sizeof(data));
    
    /* Create blob */
    g_ops->page_blob_create(g_ctx, name, 4096, &err);
    
    /* Inject failure on next page_blob_write operation */
    mock_set_fail_operation_at(g_ctx, "page_blob_write", 1, AZURE_ERR_SERVER);
    
    /* First write should fail with server error */
    azure_err_t rc = g_ops->page_blob_write(g_ctx, name, 0, data, sizeof(data),
                                            NULL, NULL, &err);
    ASSERT_AZURE_ERR(rc, AZURE_ERR_SERVER);
    
    /* Retry write should succeed */
    memset(&err, 0, sizeof(err));
    rc = g_ops->page_blob_write(g_ctx, name, 0, data, sizeof(data),
                                NULL, NULL, &err);
    ASSERT_AZURE_OK(rc);
    ASSERT_NE(err.etag[0], '\0');
    
    /* Verify data was written */
    const uint8_t *blob_data = mock_get_page_blob_data(g_ctx, name);
    ASSERT_NE(blob_data, NULL);
    ASSERT_EQ(memcmp(blob_data, data, sizeof(data)), 0);
}

TEST(chaos_transient_failure_lease_acquire_recovers) {
    setup();
    azure_error_t err;
    const char *name = "test-transient-lease.db";
    char lease_id[128];
    
    /* Create blob */
    g_ops->page_blob_create(g_ctx, name, 4096, &err);
    
    /* Inject failure on next lease_acquire */
    mock_set_fail_operation_at(g_ctx, "lease_acquire", 1, AZURE_ERR_TIMEOUT);
    
    /* First acquire should fail with timeout */
    azure_err_t rc = g_ops->lease_acquire(g_ctx, name, 60,
                                          lease_id, sizeof(lease_id), &err);
    ASSERT_AZURE_ERR(rc, AZURE_ERR_TIMEOUT);
    
    /* Retry should succeed */
    memset(&err, 0, sizeof(err));
    rc = g_ops->lease_acquire(g_ctx, name, 60,
                              lease_id, sizeof(lease_id), &err);
    ASSERT_AZURE_OK(rc);
    ASSERT_NE(lease_id[0], '\0');
    ASSERT_EQ(mock_is_leased(g_ctx, name), 1);
}

TEST(chaos_multiple_transient_failures_recover) {
    setup();
    azure_error_t err;
    const char *name = "test-multi-transient.db";
    uint8_t data[512];
    memset(data, 0xCD, sizeof(data));
    
    /* Create blob */
    g_ops->page_blob_create(g_ctx, name, 4096, &err);
    
    /* Inject failures on 1st and 2nd page_blob_write */
    mock_set_fail_operation_at(g_ctx, "page_blob_write", 1, AZURE_ERR_NETWORK);
    mock_set_fail_operation_at(g_ctx, "page_blob_write", 2, AZURE_ERR_TIMEOUT);
    
    /* First write fails with network error */
    azure_err_t rc = g_ops->page_blob_write(g_ctx, name, 0, data, sizeof(data),
                                            NULL, NULL, &err);
    ASSERT_AZURE_ERR(rc, AZURE_ERR_NETWORK);
    
    /* Second write fails with timeout */
    memset(&err, 0, sizeof(err));
    rc = g_ops->page_blob_write(g_ctx, name, 0, data, sizeof(data),
                                NULL, NULL, &err);
    ASSERT_AZURE_ERR(rc, AZURE_ERR_TIMEOUT);
    
    /* Third write succeeds */
    memset(&err, 0, sizeof(err));
    rc = g_ops->page_blob_write(g_ctx, name, 0, data, sizeof(data),
                                NULL, NULL, &err);
    ASSERT_AZURE_OK(rc);
}

/* ══════════════════════════════════════════════════════════════════════
** SECTION 7: Phase 2 — Permanent Error / No-Retry Scenarios
** ══════════════════════════════════════════════════════════════════════ */

TEST(chaos_permanent_error_conflict_no_retry) {
    setup();
    azure_error_t err;
    const char *name = "test-permanent-conflict.db";
    char lease_id1[128], lease_id2[128];
    
    /* Create blob and acquire lease */
    g_ops->page_blob_create(g_ctx, name, 4096, &err);
    g_ops->lease_acquire(g_ctx, name, 60, lease_id1, sizeof(lease_id1), &err);
    
    /* Attempt to acquire again should fail with conflict (permanent until released) */
    for (int i = 0; i < 3; i++) {
        memset(&err, 0, sizeof(err));
        azure_err_t rc = g_ops->lease_acquire(g_ctx, name, 60,
                                              lease_id2, sizeof(lease_id2), &err);
        ASSERT_AZURE_ERR(rc, AZURE_ERR_CONFLICT);
        ASSERT_EQ(err.http_status, 409);
    }
}

TEST(chaos_permanent_error_not_found_no_retry) {
    setup();
    azure_error_t err;
    azure_buffer_t buf = {0};
    
    /* Repeatedly try to read non-existent blob — should always fail */
    for (int i = 0; i < 3; i++) {
        memset(&err, 0, sizeof(err));
        azure_err_t rc = g_ops->page_blob_read(g_ctx, "nonexistent.db", 0, 512,
                                               &buf, &err);
        ASSERT_AZURE_ERR(rc, AZURE_ERR_NOT_FOUND);
        ASSERT_EQ(err.http_status, 404);
    }
    
    if (buf.data) free(buf.data);
}

TEST(chaos_permanent_error_lease_mismatch_no_retry) {
    setup();
    azure_error_t err;
    const char *name = "test-lease-mismatch.db";
    char lease_id[128];
    uint8_t data[512];
    memset(data, 0xEF, sizeof(data));
    
    /* Create blob and acquire lease */
    g_ops->page_blob_create(g_ctx, name, 4096, &err);
    g_ops->lease_acquire(g_ctx, name, 60, lease_id, sizeof(lease_id), &err);
    
    /* Try to write with wrong lease ID — should fail permanently */
    for (int i = 0; i < 3; i++) {
        memset(&err, 0, sizeof(err));
        azure_err_t rc = g_ops->page_blob_write(g_ctx, name, 0, data, sizeof(data),
                                                "wrong-lease-id", NULL, &err);
        ASSERT_AZURE_ERR(rc, AZURE_ERR_CONFLICT);
        ASSERT_EQ(err.http_status, 409);
    }
}

TEST(chaos_permanent_all_operation_failures) {
    setup();
    azure_error_t err;
    const char *name = "test-all-fail.db";
    
    /* Create blob first */
    g_ops->page_blob_create(g_ctx, name, 4096, &err);
    
    /* Make ALL page_blob_write operations fail */
    mock_set_fail_operation(g_ctx, "page_blob_write", AZURE_ERR_AUTH);
    
    /* All subsequent writes should fail */
    uint8_t data[512];
    memset(data, 0x11, sizeof(data));
    
    for (int i = 0; i < 3; i++) {
        memset(&err, 0, sizeof(err));
        azure_err_t rc = g_ops->page_blob_write(g_ctx, name, 0, data, sizeof(data),
                                                NULL, NULL, &err);
        ASSERT_AZURE_ERR(rc, AZURE_ERR_AUTH);
    }
    
    /* Clear failures and verify operation succeeds */
    mock_clear_failures(g_ctx);
    memset(&err, 0, sizeof(err));
    azure_err_t rc = g_ops->page_blob_write(g_ctx, name, 0, data, sizeof(data),
                                            NULL, NULL, &err);
    ASSERT_AZURE_OK(rc);
}

/* ══════════════════════════════════════════════════════════════════════
** SECTION 8: Phase 2 — Extended ETag Behavior Validation
** ══════════════════════════════════════════════════════════════════════ */

TEST(chaos_etag_preserved_across_operations) {
    setup();
    azure_error_t err1, err2, err3;
    const char *name = "test-etag-preservation.db";
    
    /* Create blob and capture ETag */
    g_ops->page_blob_create(g_ctx, name, 4096, &err1);
    char etag_create[128];
    strcpy(etag_create, err1.etag);
    ASSERT_NE(etag_create[0], '\0');
    
    /* Get properties — ETag should match */
    int64_t size = 0;
    g_ops->blob_get_properties(g_ctx, name, &size, NULL, NULL, &err2);
    ASSERT_STR_EQ(err2.etag, etag_create);
    
    /* Write data — ETag should change */
    uint8_t data[512];
    memset(data, 0x22, sizeof(data));
    g_ops->page_blob_write(g_ctx, name, 0, data, sizeof(data), NULL, NULL, &err3);
    ASSERT_NE(err3.etag[0], '\0');
    ASSERT_STR_NE(err3.etag, etag_create);
}

TEST(chaos_cleared_etag_multiple_operations) {
    setup();
    azure_error_t err;
    const char *name = "test-etag-multi-clear.db";
    uint8_t data[512];
    memset(data, 0x33, sizeof(data));
    
    /* Create blob */
    g_ops->page_blob_create(g_ctx, name, 4096, &err);
    
    /* Clear ETag for next response */
    mock_chaos_clear_etag(g_ctx, name);
    
    /* Get properties should return empty ETag */
    memset(&err, 0, sizeof(err));
    int64_t size = 0;
    g_ops->blob_get_properties(g_ctx, name, &size, NULL, NULL, &err);
    ASSERT_EQ(err.etag[0], '\0');
    
    /* Next operation should have ETag again */
    memset(&err, 0, sizeof(err));
    g_ops->blob_get_properties(g_ctx, name, &size, NULL, NULL, &err);
    ASSERT_NE(err.etag[0], '\0');
    
    /* Clear again and write */
    mock_chaos_clear_etag(g_ctx, name);
    memset(&err, 0, sizeof(err));
    g_ops->page_blob_write(g_ctx, name, 0, data, sizeof(data), NULL, NULL, &err);
    ASSERT_EQ(err.etag[0], '\0');
    
    /* Subsequent write should have ETag */
    memset(&err, 0, sizeof(err));
    g_ops->page_blob_write(g_ctx, name, 512, data, sizeof(data), NULL, NULL, &err);
    ASSERT_NE(err.etag[0], '\0');
}

TEST(chaos_cleared_etag_does_not_affect_stored_etag) {
    setup();
    azure_error_t err;
    const char *name = "test-etag-stored.db";
    
    /* Create blob and capture initial ETag */
    g_ops->page_blob_create(g_ctx, name, 4096, &err);
    char etag_initial[128];
    strcpy(etag_initial, err.etag);
    
    /* Clear ETag for next response */
    mock_chaos_clear_etag(g_ctx, name);
    
    /* Get properties returns empty ETag */
    memset(&err, 0, sizeof(err));
    int64_t size = 0;
    g_ops->blob_get_properties(g_ctx, name, &size, NULL, NULL, &err);
    ASSERT_EQ(err.etag[0], '\0');
    
    /* But subsequent call returns the same stored ETag (unchanged) */
    memset(&err, 0, sizeof(err));
    g_ops->blob_get_properties(g_ctx, name, &size, NULL, NULL, &err);
    ASSERT_STR_EQ(err.etag, etag_initial);
}

/* ══════════════════════════════════════════════════════════════════════
** SECTION 9: Phase 2 — Lease Expiry Reflection in Properties/Helpers
** ══════════════════════════════════════════════════════════════════════ */

TEST(chaos_get_properties_reflects_expired_lease) {
    setup();
    azure_error_t err;
    const char *name = "test-props-expired.db";
    char lease_id[128];
    
    /* Create blob and acquire lease */
    g_ops->page_blob_create(g_ctx, name, 4096, &err);
    g_ops->lease_acquire(g_ctx, name, 30, lease_id, sizeof(lease_id), &err);
    
    /* Verify leased state via helper */
    ASSERT_EQ(mock_is_leased(g_ctx, name), 1);
    ASSERT_EQ(mock_get_lease_state(g_ctx, name), LEASE_LEASED);
    ASSERT_STR_EQ(mock_get_lease_id(g_ctx, name), lease_id);
    
    /* Advance time past lease expiry */
    mock_chaos_advance_time(g_ctx, 31);
    
    /* Properties check should clear expired lease */
    int64_t size = 0;
    memset(&err, 0, sizeof(err));
    g_ops->blob_get_properties(g_ctx, name, &size, NULL, NULL, &err);
    
    /* Helper accessors should now reflect AVAILABLE state */
    ASSERT_EQ(mock_is_leased(g_ctx, name), 0);
    ASSERT_EQ(mock_get_lease_state(g_ctx, name), LEASE_AVAILABLE);
    ASSERT_EQ(mock_get_lease_id(g_ctx, name), NULL);
}

TEST(chaos_helper_accessors_auto_clear_expired_lease) {
    setup();
    azure_error_t err;
    const char *name = "test-helper-expiry.db";
    char lease_id[128];
    
    /* Create blob and acquire lease */
    g_ops->page_blob_create(g_ctx, name, 4096, &err);
    g_ops->lease_acquire(g_ctx, name, 15, lease_id, sizeof(lease_id), &err);
    
    /* Verify lease is active */
    ASSERT_EQ(mock_is_leased(g_ctx, name), 1);
    
    /* Advance time to expire lease */
    mock_chaos_advance_time(g_ctx, 20);
    
    /* Calling mock_is_leased should auto-clear expired lease */
    ASSERT_EQ(mock_is_leased(g_ctx, name), 0);
    
    /* State and ID accessors should also reflect cleared state */
    ASSERT_EQ(mock_get_lease_state(g_ctx, name), LEASE_AVAILABLE);
    ASSERT_EQ(mock_get_lease_id(g_ctx, name), NULL);
}

TEST(chaos_lease_state_consistent_across_accessors) {
    setup();
    azure_error_t err;
    const char *name = "test-consistency.db";
    char lease_id[128];
    
    /* Create blob and acquire lease */
    g_ops->page_blob_create(g_ctx, name, 4096, &err);
    g_ops->lease_acquire(g_ctx, name, 60, lease_id, sizeof(lease_id), &err);
    
    /* All accessors should show LEASED */
    ASSERT_EQ(mock_is_leased(g_ctx, name), 1);
    ASSERT_EQ(mock_get_lease_state(g_ctx, name), LEASE_LEASED);
    ASSERT_NE(mock_get_lease_id(g_ctx, name), NULL);
    
    /* Release lease */
    g_ops->lease_release(g_ctx, name, lease_id, &err);
    
    /* All accessors should show AVAILABLE */
    ASSERT_EQ(mock_is_leased(g_ctx, name), 0);
    ASSERT_EQ(mock_get_lease_state(g_ctx, name), LEASE_AVAILABLE);
    ASSERT_EQ(mock_get_lease_id(g_ctx, name), NULL);
    
    /* Acquire again */
    g_ops->lease_acquire(g_ctx, name, 60, lease_id, sizeof(lease_id), &err);
    
    /* Back to LEASED */
    ASSERT_EQ(mock_is_leased(g_ctx, name), 1);
    ASSERT_EQ(mock_get_lease_state(g_ctx, name), LEASE_LEASED);
    ASSERT_NE(mock_get_lease_id(g_ctx, name), NULL);
}

/* ══════════════════════════════════════════════════════════════════════
** Test runner
** ══════════════════════════════════════════════════════════════════════ */

static void run_chaos_tests(void) {
    printf("\n%s=== Chaos Testing (Phase 1/P0 + Phase 2) ===%s\n", TH_BOLD, TH_RESET);
    
    /* ETag chaos */
    RUN_TEST(chaos_clear_etag_basic);
    RUN_TEST(chaos_clear_etag_nonexistent_blob);
    RUN_TEST(chaos_cleared_etag_write_preserves_clear);
    
    /* Time advancement */
    RUN_TEST(chaos_time_offset_basic);
    RUN_TEST(chaos_vfs_time_hook_links);
    RUN_TEST(chaos_lease_acquire_with_time_offset);
    RUN_TEST(chaos_lease_renew_tracks_advanced_time);
    RUN_TEST(chaos_lease_renew_after_expiry_fails);
    RUN_TEST(chaos_expired_lease_allows_new_acquire);
    RUN_TEST(chaos_write_with_expired_lease_fails);
    
    /* Lease race conditions */
    RUN_TEST(chaos_lease_race_double_acquire_fails);
    RUN_TEST(chaos_lease_race_renew_wrong_id_fails);
    RUN_TEST(chaos_lease_race_release_then_acquire_succeeds);
    
    /* Lease state transitions */
    RUN_TEST(chaos_lease_state_transitions);
    RUN_TEST(chaos_lease_break_transitions_to_breaking);
    
    /* Combined scenarios */
    RUN_TEST(chaos_combined_etag_and_time);
    RUN_TEST(chaos_reset_clears_all_chaos_state);
    
    /* Phase 2: Transient failure/recovery */
    RUN_TEST(chaos_transient_failure_single_operation_recovers);
    RUN_TEST(chaos_transient_failure_write_recovers);
    RUN_TEST(chaos_transient_failure_lease_acquire_recovers);
    RUN_TEST(chaos_multiple_transient_failures_recover);
    
    /* Phase 2: Permanent errors / no-retry */
    RUN_TEST(chaos_permanent_error_conflict_no_retry);
    RUN_TEST(chaos_permanent_error_not_found_no_retry);
    RUN_TEST(chaos_permanent_error_lease_mismatch_no_retry);
    RUN_TEST(chaos_permanent_all_operation_failures);
    
    /* Phase 2: Extended ETag behavior */
    RUN_TEST(chaos_etag_preserved_across_operations);
    RUN_TEST(chaos_cleared_etag_multiple_operations);
    RUN_TEST(chaos_cleared_etag_does_not_affect_stored_etag);
    
    /* Phase 2: Lease expiry reflection */
    RUN_TEST(chaos_get_properties_reflects_expired_lease);
    RUN_TEST(chaos_helper_accessors_auto_clear_expired_lease);
    RUN_TEST(chaos_lease_state_consistent_across_accessors);
}
