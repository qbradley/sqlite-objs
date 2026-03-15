/*
** testfixture_vfs_init.c — Register sqlite-objs VFS with mock Azure backend
**
** This file is compiled into the testfixture binary to register our
** VFS as the DEFAULT VFS before any TCL tests run.  It uses the mock
** Azure ops (in-memory storage) so no real Azure credentials are needed.
**
** Strategy:
**   The sqlite3TestInit() function in test_tclsh.c calls all the
**   Sqlitetest*_Init() functions.  We add our own init that registers
**   the VFS.  This is done via a constructor attribute so it runs
**   before main() — ensuring the VFS is default when tester.tcl opens
**   databases.
*/

#include "sqlite_objs.h"
#include "azure_client.h"
#include "mock_azure_ops.h"
#include <stdio.h>

static mock_azure_ctx_t *g_mockCtx = NULL;

/*
** Register our VFS as the default before main() runs.
** Called via __attribute__((constructor)).
*/
__attribute__((constructor))
static void register_sqlite_objs_vfs(void) {
    /* Create mock Azure context (in-memory blob storage) */
    g_mockCtx = mock_azure_create();
    if (!g_mockCtx) {
        fprintf(stderr, "FATAL: mock_azure_create() failed\n");
        return;
    }

    /* Register sqlite-objs VFS as the DEFAULT VFS */
    azure_ops_t *ops = mock_azure_get_ops();
    int rc = sqlite_objs_vfs_register_with_ops(ops, (void *)g_mockCtx, 1);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "FATAL: sqlite_objs_vfs_register_with_ops() returned %d\n", rc);
        return;
    }

    /* Only log if SQLITE_OBJS_VERBOSE is set — avoids polluting test output */
    if (getenv("SQLITE_OBJS_VERBOSE")) {
        fprintf(stderr, "[sqlite-objs] VFS registered as default (mock Azure backend)\n");
    }
}
