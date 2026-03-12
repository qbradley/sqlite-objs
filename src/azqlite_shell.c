/*
** azqlite_shell.c — SQLite CLI wrapper with azqlite VFS
**
** This is a thin wrapper that registers the azqlite VFS as the default
** VFS, then invokes the standard SQLite shell.  The result is a SQLite
** CLI that transparently reads/writes from Azure Blob Storage.
**
** Build:
**   cc -o azqlite-shell azqlite_shell.c azqlite_vfs.c azure_client_stub.c \
**      sqlite3.c -lpthread -ldl -lm
**
** Usage:
**   export AZURE_STORAGE_ACCOUNT=myaccount
**   export AZURE_STORAGE_CONTAINER=databases
**   export AZURE_STORAGE_SAS="sv=2024-08-04&..."
**   ./azqlite-shell mydb.db
**
** URI mode (no environment variables needed):
**   ./azqlite-shell --uri "file:mydb.db?azure_account=acct&azure_container=cont&azure_sas=tok"
*/

#include <stdio.h>
#include <string.h>
#include "sqlite3.h"
#include "azqlite.h"

/*
** The SQLite shell's entry point.  Declared in shell.c.
** We forward-declare it here since shell.c doesn't provide a header.
*/
extern int SQLITE_CDECL main(int argc, char **argv);

/*
** We rename the shell's main to shell_main via a #define before
** including shell.c, then call it from our own main().
*/
#define main shell_main
#include "shell.c"
#undef main

int main(int argc, char **argv) {
    int rc;
    int use_uri = 0;

    /* Scan for --uri flag (consume it before passing to shell) */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--uri") == 0) {
            use_uri = 1;
            /* Remove --uri from argv */
            for (int j = i; j < argc - 1; j++) {
                argv[j] = argv[j + 1];
            }
            argc--;
            i--;
        }
    }

    if (use_uri) {
        /*
        ** URI mode: register VFS with no global client.
        ** All Azure config comes from URI parameters in the filename.
        ** Enable URI filenames globally before any SQLite initialization.
        */
        sqlite3_config(SQLITE_CONFIG_URI, 1);
        rc = azqlite_vfs_register_uri(1);  /* 1 = make default */
        if (rc != SQLITE_OK) {
            fprintf(stderr,
                "azqlite: Failed to register URI-mode VFS (rc=%d).\n", rc);
            return 1;
        }
    } else {
        /*
        ** Default mode: register VFS using environment variables.
        */
        rc = azqlite_vfs_register(1);  /* 1 = make default */
        if (rc != SQLITE_OK) {
            fprintf(stderr,
                "azqlite: Failed to register VFS (rc=%d).\n"
                "Ensure these environment variables are set:\n"
                "  AZURE_STORAGE_ACCOUNT\n"
                "  AZURE_STORAGE_CONTAINER\n"
                "  AZURE_STORAGE_SAS or AZURE_STORAGE_KEY\n",
                rc);
            return 1;
        }
    }

    return shell_main(argc, argv);
}
