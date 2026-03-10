/*
** azqlite.h — Public API for the azqlite VFS
**
** This is the ONLY header users need to include.  It provides:
**   - VFS registration functions
**   - Configuration struct
**
** Usage:
**   #include "azqlite.h"
**
**   // Option 1: Read config from environment variables
**   int rc = azqlite_vfs_register(0);  // 0 = not the default VFS
**
**   // Option 2: Provide config explicitly
**   azqlite_config_t cfg = {
**       .account   = "myaccount",
**       .container = "databases",
**       .sas_token = "sv=2024-08-04&...",
**   };
**   int rc = azqlite_vfs_register_with_config(&cfg, 0);
**
**   // Then open databases with the "azqlite" VFS:
**   sqlite3 *db;
**   sqlite3_open_v2("mydb.db", &db,
**       SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, "azqlite");
*/
#ifndef AZQLITE_H
#define AZQLITE_H

#include "sqlite3.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration — defined in azure_client.h (internal) */
typedef struct azure_ops azure_ops_t;

/*
** Configuration for the azqlite VFS.
*/
typedef struct azqlite_config {
    const char *account;        /* Azure Storage account name */
    const char *container;      /* Blob container name */
    const char *sas_token;      /* SAS token (preferred), or NULL */
    const char *account_key;    /* Shared Key (fallback), or NULL */

    /*
    ** Optional: override the Azure operations vtable.
    ** If non-NULL, the VFS uses these ops instead of the production client.
    ** This is the test seam — pass a mock azure_ops_t for unit testing.
    ** ops_ctx is the opaque context pointer passed to each ops function.
    */
    const azure_ops_t *ops;
    void *ops_ctx;
} azqlite_config_t;


/*
** Register the "azqlite" VFS.  Reads configuration from environment
** variables:
**   AZURE_STORAGE_ACCOUNT    — Storage account name
**   AZURE_STORAGE_CONTAINER  — Container name
**   AZURE_STORAGE_SAS        — SAS token (checked first)
**   AZURE_STORAGE_KEY        — Shared Key (fallback)
**
** If makeDefault is non-zero, this VFS becomes the default.
** Returns SQLITE_OK on success, or an appropriate error code.
*/
int azqlite_vfs_register(int makeDefault);

/*
** Register the "azqlite" VFS with an explicit configuration.
** The config struct is copied — the caller may free it after this call.
**
** If makeDefault is non-zero, this VFS becomes the default.
** Returns SQLITE_OK on success, or an appropriate error code.
*/
int azqlite_vfs_register_with_config(const azqlite_config_t *config,
                                     int makeDefault);

/*
** Register the "azqlite" VFS with an explicit ops vtable and context.
** Convenience wrapper for test code — equivalent to calling
** azqlite_vfs_register_with_config with ops/ops_ctx fields set.
*/
int azqlite_vfs_register_with_ops(azure_ops_t *ops, void *ctx,
                                  int makeDefault);

#ifdef __cplusplus
}
#endif

#endif /* AZQLITE_H */
