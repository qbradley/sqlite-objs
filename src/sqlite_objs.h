/*
** sqlite_objs.h — Public API for the sqliteObjs VFS
**
** This is the ONLY header users need to include.  It provides:
**   - VFS registration functions
**   - Configuration struct
**
** Usage:
**   #include "sqlite_objs.h"
**
**   // Option 1: Read config from environment variables
**   int rc = sqlite_objs_vfs_register(0);  // 0 = not the default VFS
**
**   // Option 2: Provide config explicitly
**   sqlite_objs_config_t cfg = {
**       .account   = "myaccount",
**       .container = "databases",
**       .sas_token = "sv=2024-08-04&...",
**   };
**   int rc = sqlite_objs_vfs_register_with_config(&cfg, 0);
**
**   // Then open databases with the "sqlite-objs" VFS:
**   sqlite3 *db;
**   sqlite3_open_v2("mydb.db", &db,
**       SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, "sqlite-objs");
*/
#ifndef SQLITE_OBJS_H
#define SQLITE_OBJS_H

#include "sqlite3.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration — defined in azure_client.h (internal) */
typedef struct azure_ops azure_ops_t;

/*
** Configuration for the sqliteObjs VFS.
*/
typedef struct sqlite_objs_config {
    const char *account;        /* Azure Storage account name */
    const char *container;      /* Blob container name */
    const char *sas_token;      /* SAS token (preferred), or NULL */
    const char *account_key;    /* Shared Key (fallback), or NULL */
    const char *endpoint;       /* Optional custom endpoint (for Azurite), or NULL for Azure */

    /*
    ** Optional: override the Azure operations vtable.
    ** If non-NULL, the VFS uses these ops instead of the production client.
    ** This is the test seam — pass a mock azure_ops_t for unit testing.
    ** ops_ctx is the opaque context pointer passed to each ops function.
    */
    const azure_ops_t *ops;
    void *ops_ctx;
} sqlite_objs_config_t;


/*
** Register the "sqlite-objs" VFS.  Reads configuration from environment
** variables:
**   AZURE_STORAGE_ACCOUNT    — Storage account name
**   AZURE_STORAGE_CONTAINER  — Container name
**   AZURE_STORAGE_SAS        — SAS token (checked first)
**   AZURE_STORAGE_KEY        — Shared Key (fallback)
**
** If makeDefault is non-zero, this VFS becomes the default.
** Returns SQLITE_OK on success, or an appropriate error code.
*/
int sqlite_objs_vfs_register(int makeDefault);

/*
** Register the "sqlite-objs" VFS with an explicit configuration.
** The config struct is copied — the caller may free it after this call.
**
** If makeDefault is non-zero, this VFS becomes the default.
** Returns SQLITE_OK on success, or an appropriate error code.
*/
int sqlite_objs_vfs_register_with_config(const sqlite_objs_config_t *config,
                                     int makeDefault);

/*
** Register the "sqlite-objs" VFS with an explicit ops vtable and context.
** Convenience wrapper for test code — equivalent to calling
** sqlite_objs_vfs_register_with_config with ops/ops_ctx fields set.
*/
int sqlite_objs_vfs_register_with_ops(azure_ops_t *ops, void *ctx,
                                  int makeDefault);

/*
** Register the "sqlite-objs" VFS with no global Azure client.
** All databases MUST provide Azure credentials via URI parameters:
**
**   file:mydb.db?azure_account=acct&azure_container=cont&azure_sas=token
**
** Supported URI parameters:
**   azure_account    — Storage account name (required)
**   azure_container  — Container name
**   azure_sas        — SAS token
**   azure_key        — Shared Key
**   azure_endpoint   — Custom endpoint (e.g. for Azurite)
**   cache_dir        — Directory for local cache files (default: /tmp)
**   cache_reuse      — If "1", persist cache on close and reuse via ETag (default: off)
**
** If azure_account is missing from the URI, xOpen returns SQLITE_CANTOPEN.
** If makeDefault is non-zero, this VFS becomes the default.
** Returns SQLITE_OK on success, or an appropriate error code.
*/
int sqlite_objs_vfs_register_uri(int makeDefault);

/*
** Custom file-control op code for querying the blob download counter.
** Usage:
**   int count = 0;
**   sqlite3_file_control(db, "main", SQLITE_OBJS_FCNTL_DOWNLOAD_COUNT, &count);
**
** Writes the number of full blob downloads performed on the MAIN_DB file
** into *pArg (int*).  ETag cache hits do not increment this counter.
** Op code chosen in the user-defined range (starting at 100+).
*/
#define SQLITE_OBJS_FCNTL_DOWNLOAD_COUNT 200

#ifdef __cplusplus
}
#endif

#endif /* SQLITE_OBJS_H */
