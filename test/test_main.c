/*
** test_main.c — Test Runner for azqlite Layer 1 tests
**
** Runs all test suites, reports summary, returns 0 on all pass.
**
** Build:
**   cc -o test_runner test/test_main.c test/mock_azure_ops.c \
**      sqlite-autoconf-3520000/sqlite3.c \
**      -I sqlite-autoconf-3520000 -lpthread -ldl -lm
**
** Run:
**   ./test_runner
*/

#include "../sqlite-autoconf-3520000/sqlite3.h"
#include "mock_azure_ops.h"
#include "test_harness.h"

/*
** Include the test source files directly. Each defines a run_*_tests()
** function. This avoids needing separate compilation units for test
** files that share the test harness's static global counters.
*/
#include "test_vfs.c"
#include "test_azure_client.c"
#include "test_coalesce.c"
#include "test_wal.c"
#include "test_uri.c"

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    /* Enable URI filenames globally so ?vfs= works in ATTACH statements */
    sqlite3_config(SQLITE_CONFIG_URI, 1);

    fprintf(stdout,
        "%s%s╔══════════════════════════════════════╗%s\n"
        "%s%s║   azqlite Layer 1 Test Suite         ║%s\n"
        "%s%s╚══════════════════════════════════════╝%s\n",
        TH_BOLD, TH_YELLOW, TH_RESET,
        TH_BOLD, TH_YELLOW, TH_RESET,
        TH_BOLD, TH_YELLOW, TH_RESET);

    run_vfs_tests();
    run_azure_client_tests();
    run_coalesce_tests();
    run_wal_tests();
    run_uri_tests();

    return test_harness_summary();
}
