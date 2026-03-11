/*
** azqlite_vfs.c — VFS implementation for Azure Blob Storage
**
** This is the core of azqlite.  It implements sqlite3_vfs and
** sqlite3_io_methods v1, routing MAIN_DB operations through Azure
** Page Blobs, MAIN_JOURNAL through Block Blobs, and delegating
** all temporary/transient files to the platform's default VFS.
**
** Architecture (per design review):
**   - Full blob download on xOpen into aData buffer
**   - xRead/xWrite are pure memcpy against the buffer
**   - Dirty page bitmap tracks which pages need flushing
**   - xSync flushes dirty pages via page_blob_write
**   - Two-level lease locking: SHARED=no lease, RESERVED+=30s lease
**   - Journal files buffered in memory, uploaded on xSync as block blob
*/

#include "azqlite.h"
#include "azure_client.h"
#include "sqlite3.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <assert.h>

/* ---------- Forward declarations ---------- */

static int azqliteOpen(sqlite3_vfs*, sqlite3_filename, sqlite3_file*, int, int*);
static int azqliteDelete(sqlite3_vfs*, const char*, int);
static int azqliteAccess(sqlite3_vfs*, const char*, int, int*);
static int azqliteFullPathname(sqlite3_vfs*, const char*, int, char*);
static void *azqliteDlOpen(sqlite3_vfs*, const char*);
static void azqliteDlError(sqlite3_vfs*, int, char*);
static void (*azqliteDlSym(sqlite3_vfs*, void*, const char*))(void);
static void azqliteDlClose(sqlite3_vfs*, void*);
static int azqliteRandomness(sqlite3_vfs*, int, char*);
static int azqliteSleep(sqlite3_vfs*, int);
static int azqliteCurrentTime(sqlite3_vfs*, double*);
static int azqliteGetLastError(sqlite3_vfs*, int, char*);
static int azqliteCurrentTimeInt64(sqlite3_vfs*, sqlite3_int64*);

/* io_methods */
static int azqliteClose(sqlite3_file*);
static int azqliteRead(sqlite3_file*, void*, int, sqlite3_int64);
static int azqliteWrite(sqlite3_file*, const void*, int, sqlite3_int64);
static int azqliteTruncate(sqlite3_file*, sqlite3_int64);
static int azqliteSync(sqlite3_file*, int);
static int azqliteFileSize(sqlite3_file*, sqlite3_int64*);
static int azqliteLock(sqlite3_file*, int);
static int azqliteUnlock(sqlite3_file*, int);
static int azqliteCheckReservedLock(sqlite3_file*, int*);
static int azqliteFileControl(sqlite3_file*, int, void*);
static int azqliteSectorSize(sqlite3_file*);
static int azqliteDeviceCharacteristics(sqlite3_file*);

/* ---------- Constants ---------- */

#define AZQLITE_DEFAULT_PAGE_SIZE  4096
#define AZQLITE_LEASE_DURATION     30      /* seconds */
#define AZQLITE_LEASE_RENEW_AFTER  15      /* renew if older than this */
#define AZQLITE_MAX_PATHNAME       512
#define AZQLITE_INITIAL_ALLOC      (64*1024)  /* 64 KiB initial buffer */

/* ---------- Types ---------- */

/*
** Per-file state for Azure-backed files.
** The first member MUST be pMethod (sqlite3_io_methods*) to satisfy
** the sqlite3_file subclass contract.
*/
typedef struct azqliteFile {
    const sqlite3_io_methods *pMethod;  /* MUST be first */

    /* Azure connection */
    azure_ops_t *ops;                   /* Swappable Azure operations vtable */
    void *ops_ctx;                      /* Context for ops */
    char *zBlobName;                    /* Blob name in container */

    /* In-memory buffer (cache + write buffer) for MAIN_DB */
    unsigned char *aData;               /* Full blob content */
    sqlite3_int64 nData;                /* Current logical size */
    sqlite3_int64 nAlloc;               /* Allocated buffer size */

    /* Dirty page tracking */
    unsigned char *aDirty;              /* Bitmap: 1 bit per page, 1=dirty */
    int nDirtyPages;                    /* Count of dirty pages */
    int nDirtyAlloc;                    /* Allocated size of aDirty bitmap */
    int pageSize;                       /* Detected from header or default 4096 */

    /* Lock state */
    int eLock;                          /* Current SQLite lock level */
    char leaseId[64];                   /* Azure lease ID (empty = no lease) */
    time_t leaseAcquiredAt;             /* For renewal timing */

    /* File type */
    int eFileType;                      /* SQLITE_OPEN_MAIN_DB, etc. */

    /* For journal files (block blob) */
    unsigned char *aJrnlData;           /* Journal buffer */
    sqlite3_int64 nJrnlData;           /* Journal logical size */
    sqlite3_int64 nJrnlAlloc;          /* Journal allocated size */
} azqliteFile;

/*
** Global VFS state — stored in sqlite3_vfs.pAppData.
*/
typedef struct azqliteVfsData {
    sqlite3_vfs *pDefaultVfs;           /* The platform default VFS */
    azure_ops_t *ops;                   /* Azure operations vtable */
    void *ops_ctx;                      /* Context for ops */
    azure_client_t *client;             /* Production client (may be NULL for tests) */
    char lastError[256];                /* Last error message for xGetLastError */
} azqliteVfsData;

/* ---------- io_methods v1 ---------- */

static const sqlite3_io_methods azqliteIoMethods = {
    1,                              /* iVersion = 1 (no WAL/shm) */
    azqliteClose,
    azqliteRead,
    azqliteWrite,
    azqliteTruncate,
    azqliteSync,
    azqliteFileSize,
    azqliteLock,
    azqliteUnlock,
    azqliteCheckReservedLock,
    azqliteFileControl,
    azqliteSectorSize,
    azqliteDeviceCharacteristics,
    /* v2+ methods (WAL) — omitted; iVersion=1 */
    0, 0, 0, 0,
    /* v3 methods (mmap) — omitted */
    0, 0
};


/* ===================================================================
** Helper: dirty bitmap operations
** =================================================================== */

static int dirtyBitmapSize(sqlite3_int64 fileSize, int pageSize) {
    if (fileSize <= 0 || pageSize <= 0) return 0;
    int nPages = (int)((fileSize + pageSize - 1) / pageSize);
    return (nPages + 7) / 8;  /* bytes needed for bitmap */
}

static void dirtyMarkPage(azqliteFile *p, sqlite3_int64 offset) {
    if (!p->aDirty || p->pageSize <= 0) return;
    int pageIdx = (int)(offset / p->pageSize);
    int byteIdx = pageIdx / 8;
    int bitIdx = pageIdx % 8;
    if (!(p->aDirty[byteIdx] & (1 << bitIdx))) {
        p->aDirty[byteIdx] |= (1 << bitIdx);
        p->nDirtyPages++;
    }
}

static int dirtyIsPageDirty(azqliteFile *p, int pageIdx) {
    if (!p->aDirty) return 0;
    int byteIdx = pageIdx / 8;
    int bitIdx = pageIdx % 8;
    return (p->aDirty[byteIdx] & (1 << bitIdx)) != 0;
}

static void dirtyClearAll(azqliteFile *p) {
    if (p->aDirty) {
        int nBytes = dirtyBitmapSize(p->nAlloc, p->pageSize);
        memset(p->aDirty, 0, nBytes);
    }
    p->nDirtyPages = 0;
}

/*
** Ensure the dirty bitmap is large enough for the current allocation.
** Returns SQLITE_OK or SQLITE_NOMEM.
*/
static int dirtyEnsureCapacity(azqliteFile *p) {
    int needed = dirtyBitmapSize(p->nAlloc, p->pageSize);
    if (needed <= 0) return SQLITE_OK;
    if (needed <= p->nDirtyAlloc) return SQLITE_OK;
    unsigned char *pNew = (unsigned char *)realloc(p->aDirty, needed);
    if (!pNew) return SQLITE_NOMEM;
    /* Zero new bytes (realloc doesn't guarantee zeroed memory) */
    memset(pNew + p->nDirtyAlloc, 0, needed - p->nDirtyAlloc);
    p->aDirty = pNew;
    p->nDirtyAlloc = needed;
    return SQLITE_OK;
}


/* ===================================================================
** Helper: buffer management
** =================================================================== */

/*
** Ensure aData has room for at least newSize bytes.
** Grows geometrically.  Returns SQLITE_OK or SQLITE_NOMEM.
*/
static int bufferEnsure(azqliteFile *p, sqlite3_int64 newSize) {
    if (newSize <= p->nAlloc) return SQLITE_OK;
    sqlite3_int64 alloc = p->nAlloc;
    if (alloc == 0) alloc = AZQLITE_INITIAL_ALLOC;
    while (alloc < newSize) {
        alloc *= 2;
        if (alloc < 0) return SQLITE_NOMEM; /* overflow */
    }
    unsigned char *pNew = (unsigned char *)realloc(p->aData, (size_t)alloc);
    if (!pNew) return SQLITE_NOMEM;
    /* Zero the new region */
    memset(pNew + p->nAlloc, 0, (size_t)(alloc - p->nAlloc));
    p->aData = pNew;
    p->nAlloc = alloc;
    return dirtyEnsureCapacity(p);
}

/*
** Ensure journal buffer has room for at least newSize bytes.
*/
static int jrnlBufferEnsure(azqliteFile *p, sqlite3_int64 newSize) {
    if (newSize <= p->nJrnlAlloc) return SQLITE_OK;
    sqlite3_int64 alloc = p->nJrnlAlloc;
    if (alloc == 0) alloc = AZQLITE_INITIAL_ALLOC;
    while (alloc < newSize) {
        alloc *= 2;
        if (alloc < 0) return SQLITE_NOMEM;
    }
    unsigned char *pNew = (unsigned char *)realloc(p->aJrnlData, (size_t)alloc);
    if (!pNew) return SQLITE_NOMEM;
    memset(pNew + p->nJrnlAlloc, 0, (size_t)(alloc - p->nJrnlAlloc));
    p->aJrnlData = pNew;
    p->nJrnlAlloc = alloc;
    return SQLITE_OK;
}


/* ===================================================================
** Helper: lease management
** =================================================================== */

static int hasLease(azqliteFile *p) {
    return p->leaseId[0] != '\0';
}

/*
** Attempt to renew the lease if it's older than AZQLITE_LEASE_RENEW_AFTER
** seconds.  Returns SQLITE_OK or SQLITE_IOERR_LOCK.
*/
static int leaseRenewIfNeeded(azqliteFile *p) {
    if (!hasLease(p)) return SQLITE_OK;
    time_t now = time(NULL);
    if (difftime(now, p->leaseAcquiredAt) < AZQLITE_LEASE_RENEW_AFTER) {
        return SQLITE_OK;
    }
    azure_error_t aerr;
    azure_error_init(&aerr);
    azure_err_t rc = p->ops->lease_renew(p->ops_ctx, p->zBlobName,
                                          p->leaseId, &aerr);
    if (rc != AZURE_OK) {
        return SQLITE_IOERR_LOCK;
    }
    p->leaseAcquiredAt = now;
    return SQLITE_OK;
}

/*
** Map azure_err_t to an appropriate SQLite error code.
*/
static int azureErrToSqlite(azure_err_t aerr, int ioerr_variant) {
    switch (aerr) {
        case AZURE_OK:           return SQLITE_OK;
        case AZURE_ERR_NOT_FOUND: return SQLITE_CANTOPEN;
        case AZURE_ERR_CONFLICT:  return SQLITE_BUSY;
        case AZURE_ERR_THROTTLED: return SQLITE_BUSY;
        case AZURE_ERR_LEASE_EXPIRED: return SQLITE_IOERR_LOCK;
        case AZURE_ERR_AUTH:      return SQLITE_IOERR;
        case AZURE_ERR_NOMEM:     return SQLITE_NOMEM;
        default:                  return ioerr_variant ? ioerr_variant : SQLITE_IOERR;
    }
}

/*
** Detect page size from the SQLite database header.
** The page size is stored as a 2-byte big-endian integer at offset 16.
** A value of 1 means 65536.  Returns 0 if the header is invalid.
*/
static int detectPageSize(const unsigned char *aData, sqlite3_int64 nData) {
    if (nData < 100) return 0;
    /* Check magic string */
    if (memcmp(aData, "SQLite format 3\000", 16) != 0) return 0;
    int ps = (aData[16] << 8) | aData[17];
    if (ps == 1) ps = 65536;
    /* Validate: must be power of 2 between 512 and 65536 */
    if (ps < 512 || ps > 65536) return 0;
    if ((ps & (ps - 1)) != 0) return 0;
    return ps;
}


/* ===================================================================
** sqlite3_io_methods implementation
** =================================================================== */

/*
** xClose — Release all resources.
*/
static int azqliteClose(sqlite3_file *pFile) {
    azqliteFile *p = (azqliteFile *)pFile;
    int rc = SQLITE_OK;

    /* Flush any remaining dirty pages before closing */
    if (p->nDirtyPages > 0 && p->ops && p->ops->page_blob_write) {
        rc = azqliteSync(pFile, 0);
    }

    /* Release lease if held */
    if (hasLease(p) && p->ops && p->ops->lease_release) {
        azure_error_t aerr;
        azure_error_init(&aerr);
        p->ops->lease_release(p->ops_ctx, p->zBlobName, p->leaseId, &aerr);
        p->leaseId[0] = '\0';
    }

    free(p->aData);
    p->aData = NULL;
    free(p->aDirty);
    p->aDirty = NULL;
    free(p->aJrnlData);
    p->aJrnlData = NULL;
    free(p->zBlobName);
    p->zBlobName = NULL;

    p->pMethod = NULL;
    return rc;
}

/*
** xRead — Read from the in-memory buffer.
** For MAIN_DB: read from aData.
** For MAIN_JOURNAL: read from aJrnlData.
** Must zero-fill on short read (SQLITE_IOERR_SHORT_READ).
*/
static int azqliteRead(sqlite3_file *pFile, void *pBuf, int iAmt,
                        sqlite3_int64 iOfst) {
    azqliteFile *p = (azqliteFile *)pFile;
    unsigned char *src;
    sqlite3_int64 srcLen;

    if (p->eFileType & SQLITE_OPEN_MAIN_JOURNAL) {
        src = p->aJrnlData;
        srcLen = p->nJrnlData;
    } else {
        src = p->aData;
        srcLen = p->nData;
    }

    if (iOfst >= srcLen) {
        /* Reading entirely past end — zero fill */
        memset(pBuf, 0, iAmt);
        return SQLITE_IOERR_SHORT_READ;
    }

    sqlite3_int64 avail = srcLen - iOfst;
    if (avail >= iAmt) {
        memcpy(pBuf, src + iOfst, iAmt);
        return SQLITE_OK;
    }

    /* Short read — copy what we have, zero the rest */
    memcpy(pBuf, src + iOfst, (size_t)avail);
    memset((unsigned char *)pBuf + avail, 0, iAmt - (size_t)avail);
    return SQLITE_IOERR_SHORT_READ;
}

/*
** xWrite — Write to the in-memory buffer.
** For MAIN_DB: write to aData, mark dirty bitmap.
** For MAIN_JOURNAL: append/write to aJrnlData.
*/
static int azqliteWrite(sqlite3_file *pFile, const void *pBuf, int iAmt,
                         sqlite3_int64 iOfst) {
    azqliteFile *p = (azqliteFile *)pFile;
    int rc;

    if (p->eFileType & SQLITE_OPEN_MAIN_JOURNAL) {
        /* Journal file — write to journal buffer */
        sqlite3_int64 end = iOfst + iAmt;
        rc = jrnlBufferEnsure(p, end);
        if (rc != SQLITE_OK) return rc;
        memcpy(p->aJrnlData + iOfst, pBuf, iAmt);
        if (end > p->nJrnlData) p->nJrnlData = end;
        return SQLITE_OK;
    }

    /* MAIN_DB — write to aData + mark dirty */
    sqlite3_int64 end = iOfst + iAmt;
    rc = bufferEnsure(p, end);
    if (rc != SQLITE_OK) return rc;

    memcpy(p->aData + iOfst, pBuf, iAmt);
    if (end > p->nData) p->nData = end;

    /* Renew lease if needed during long write sequences */
    if (hasLease(p)) {
        rc = leaseRenewIfNeeded(p);
        if (rc != SQLITE_OK) return rc;
    }

    /* Mark affected pages dirty */
    if (p->pageSize > 0) {
        sqlite3_int64 pageStart = (iOfst / p->pageSize) * p->pageSize;
        while (pageStart < end) {
            dirtyMarkPage(p, pageStart);
            pageStart += p->pageSize;
        }
    }

    return SQLITE_OK;
}

/*
** xTruncate — Resize the file.
*/
static int azqliteTruncate(sqlite3_file *pFile, sqlite3_int64 size) {
    azqliteFile *p = (azqliteFile *)pFile;

    if (p->eFileType & SQLITE_OPEN_MAIN_JOURNAL) {
        if (size < p->nJrnlData) {
            p->nJrnlData = size;
            if (p->aJrnlData && size > 0) {
                memset(p->aJrnlData + size, 0,
                       (size_t)(p->nJrnlAlloc - size < 0 ? 0 : p->nJrnlAlloc - size));
            }
        }
        return SQLITE_OK;
    }

    /* MAIN_DB — resize the page blob on Azure */
    if (p->ops && p->ops->page_blob_resize) {
        /* Azure page blobs require 512-byte aligned sizes */
        sqlite3_int64 alignedSize = (size + 511) & ~(sqlite3_int64)511;
        azure_error_t aerr;
        azure_error_init(&aerr);
        azure_err_t arc = p->ops->page_blob_resize(p->ops_ctx, p->zBlobName,
                                                     alignedSize,
                                                     hasLease(p) ? p->leaseId : NULL,
                                                     &aerr);
        if (arc != AZURE_OK) {
            return azureErrToSqlite(arc, SQLITE_IOERR_TRUNCATE);
        }
    }

    if (size < p->nData) {
        p->nData = size;
    }
    return SQLITE_OK;
}

/*
** xSync — Flush dirty pages to Azure.
** For MAIN_DB: write each dirty page via page_blob_write, renew lease.
** For MAIN_JOURNAL: upload entire journal via block_blob_upload.
*/
static int azqliteSync(sqlite3_file *pFile, int flags) {
    azqliteFile *p = (azqliteFile *)pFile;
    azure_error_t aerr;
    (void)flags;

    if (p->eFileType & SQLITE_OPEN_MAIN_JOURNAL) {
        /* Upload journal as block blob */
        if (p->nJrnlData > 0 && p->ops && p->ops->block_blob_upload) {
            azure_error_init(&aerr);
            azure_err_t arc = p->ops->block_blob_upload(
                p->ops_ctx, p->zBlobName,
                p->aJrnlData, (size_t)p->nJrnlData, &aerr);
            if (arc != AZURE_OK) {
                return SQLITE_IOERR_FSYNC;
            }
        }
        return SQLITE_OK;
    }

    /* MAIN_DB — flush dirty pages */
    if (p->nDirtyPages == 0) return SQLITE_OK;
    if (!p->ops || !p->ops->page_blob_write) return SQLITE_IOERR_FSYNC;

    /* Renew lease before flushing */
    int rc = leaseRenewIfNeeded(p);
    if (rc != SQLITE_OK) return SQLITE_IOERR_FSYNC;

    /* If the blob needs to grow, resize first */
    if (p->nData > 0 && p->ops->page_blob_resize) {
        sqlite3_int64 alignedSize = (p->nData + 511) & ~(sqlite3_int64)511;
        azure_error_init(&aerr);
        azure_err_t arc = p->ops->page_blob_resize(p->ops_ctx, p->zBlobName,
                                                     alignedSize,
                                                     hasLease(p) ? p->leaseId : NULL,
                                                     &aerr);
        if (arc != AZURE_OK) {
            return SQLITE_IOERR_FSYNC;
        }
    }

    int nPages = (int)((p->nData + p->pageSize - 1) / p->pageSize);
    int flushedCount = 0;
    for (int i = 0; i < nPages; i++) {
        if (!dirtyIsPageDirty(p, i)) continue;

        /* Renew lease periodically during large flushes to prevent expiration */
        if (hasLease(p) && (flushedCount % 50 == 0) && flushedCount > 0) {
            rc = leaseRenewIfNeeded(p);
            if (rc != SQLITE_OK) return SQLITE_IOERR_FSYNC;
        }

        sqlite3_int64 offset = (sqlite3_int64)i * p->pageSize;
        size_t len = p->pageSize;

        /* Last page may be shorter */
        if (offset + (sqlite3_int64)len > p->nData) {
            len = (size_t)(p->nData - offset);
        }

        /* Azure requires 512-byte aligned writes */
        size_t alignedLen = (len + 511) & ~(size_t)511;

        azure_error_init(&aerr);
        azure_err_t arc = p->ops->page_blob_write(
            p->ops_ctx, p->zBlobName,
            offset, p->aData + offset, alignedLen,
            hasLease(p) ? p->leaseId : NULL, &aerr);

        if (arc != AZURE_OK) {
            return SQLITE_IOERR_FSYNC;
        }
        flushedCount++;
    }

    dirtyClearAll(p);
    return SQLITE_OK;
}

/*
** xFileSize — Return the current logical file size.
*/
static int azqliteFileSize(sqlite3_file *pFile, sqlite3_int64 *pSize) {
    azqliteFile *p = (azqliteFile *)pFile;
    if (p->eFileType & SQLITE_OPEN_MAIN_JOURNAL) {
        *pSize = p->nJrnlData;
    } else {
        *pSize = p->nData;
    }
    return SQLITE_OK;
}

/*
** xLock — Upgrade the lock level.
** Two-level lease model from Decision 3:
**   SHARED: no-op (reads don't need a lease)
**   RESERVED/PENDING/EXCLUSIVE: acquire 30s lease
*/
static int azqliteLock(sqlite3_file *pFile, int eLock) {
    azqliteFile *p = (azqliteFile *)pFile;

    /* Already at or above requested level */
    if (p->eLock >= eLock) return SQLITE_OK;

    /* SHARED — no Azure action needed */
    if (eLock == SQLITE_LOCK_SHARED) {
        p->eLock = eLock;
        return SQLITE_OK;
    }

    /* RESERVED, PENDING, or EXCLUSIVE — need a lease */
    if (!hasLease(p)) {
        if (!p->ops || !p->ops->lease_acquire) {
            /* No ops available — grant the lock anyway (test/stub mode) */
            p->eLock = eLock;
            return SQLITE_OK;
        }

        azure_error_t aerr;
        azure_error_init(&aerr);
        azure_err_t arc = p->ops->lease_acquire(
            p->ops_ctx, p->zBlobName,
            AZQLITE_LEASE_DURATION,
            p->leaseId, sizeof(p->leaseId), &aerr);

        if (arc == AZURE_ERR_CONFLICT) {
            return SQLITE_BUSY;
        }
        if (arc != AZURE_OK) {
            return azureErrToSqlite(arc, SQLITE_IOERR_LOCK);
        }
        p->leaseAcquiredAt = time(NULL);
    }

    p->eLock = eLock;
    return SQLITE_OK;
}

/*
** xUnlock — Downgrade the lock level.
** Release lease when going to SHARED or NONE.
*/
static int azqliteUnlock(sqlite3_file *pFile, int eLock) {
    azqliteFile *p = (azqliteFile *)pFile;

    if (p->eLock <= eLock) return SQLITE_OK;

    /* Release the lease when dropping below RESERVED */
    if (eLock <= SQLITE_LOCK_SHARED && hasLease(p)) {
        if (p->ops && p->ops->lease_release) {
            azure_error_t aerr;
            azure_error_init(&aerr);
            p->ops->lease_release(p->ops_ctx, p->zBlobName,
                                   p->leaseId, &aerr);
            /* Ignore errors on release — best effort */
        }
        p->leaseId[0] = '\0';
        p->leaseAcquiredAt = 0;
    }

    p->eLock = eLock;
    return SQLITE_OK;
}

/*
** xCheckReservedLock — Check if any connection holds RESERVED or higher.
** Uses HEAD request to check blob lease state.
*/
static int azqliteCheckReservedLock(sqlite3_file *pFile, int *pResOut) {
    azqliteFile *p = (azqliteFile *)pFile;

    /* If we hold the lock ourselves, report it */
    if (p->eLock >= SQLITE_LOCK_RESERVED) {
        *pResOut = 1;
        return SQLITE_OK;
    }

    /* Check Azure lease state */
    if (p->ops && p->ops->blob_get_properties) {
        char leaseState[32] = {0};
        char leaseStatus[32] = {0};
        int64_t blobSize = 0;
        azure_error_t aerr;
        azure_error_init(&aerr);

        azure_err_t arc = p->ops->blob_get_properties(
            p->ops_ctx, p->zBlobName,
            &blobSize, leaseState, leaseStatus, &aerr);

        if (arc == AZURE_OK && strcmp(leaseState, "leased") == 0) {
            *pResOut = 1;
            return SQLITE_OK;
        }
    }

    *pResOut = 0;
    return SQLITE_OK;
}

/*
** xFileControl — Handle pragmas and other file control operations.
*/
static int azqliteFileControl(sqlite3_file *pFile, int op, void *pArg) {
    (void)pFile;

    switch (op) {
        case SQLITE_FCNTL_PRAGMA: {
            /*
            ** pArg is char** where [1] is the pragma name, [2] is the value.
            ** Return SQLITE_NOTFOUND to let SQLite handle it normally.
            ** Return SQLITE_OK to override (set [0] to the result string).
            */
            char **aArg = (char **)pArg;
            if (aArg[1] && sqlite3_stricmp(aArg[1], "journal_mode") == 0) {
                if (aArg[2] && sqlite3_stricmp(aArg[2], "wal") == 0) {
                    /* Reject WAL mode — return "delete" instead */
                    aArg[0] = sqlite3_mprintf("delete");
                    return SQLITE_OK;
                }
            }
            return SQLITE_NOTFOUND;
        }
        case SQLITE_FCNTL_VFSNAME: {
            *(char **)pArg = sqlite3_mprintf("azqlite");
            return SQLITE_OK;
        }
        default:
            return SQLITE_NOTFOUND;
    }
    return SQLITE_NOTFOUND;
}

/*
** xSectorSize — Return the minimum write granularity.
** 512 matches Azure Page Blob's 512-byte page alignment.
*/
static int azqliteSectorSize(sqlite3_file *pFile) {
    (void)pFile;
    return 512;
}

/*
** xDeviceCharacteristics — Report device capabilities.
** ATOMIC512: 512-byte aligned writes are atomic on Azure Page Blobs.
** POWERSAFE_OVERWRITE: individual Azure PUT Page calls don't corrupt
** adjacent pages — each writes exactly the specified byte range.
** SUBPAGE_READ: reads from our in-memory cache can return any portion.
**
** NOT ATOMIC — multi-page sync to Azure is not atomic.
** NOT SAFE_APPEND — page_blob_resize + page_blob_write are separate
**   HTTP calls, so extension is not atomic.
** NOT SEQUENTIAL — Azure HTTP requests have no write ordering guarantee.
**   Claiming SEQUENTIAL would cause SQLite to skip xSync on the journal
**   file, which is where our VFS uploads the journal to Azure.
*/
static int azqliteDeviceCharacteristics(sqlite3_file *pFile) {
    (void)pFile;
    return SQLITE_IOCAP_POWERSAFE_OVERWRITE | SQLITE_IOCAP_SUBPAGE_READ;
}


/* ===================================================================
** sqlite3_vfs method implementations
** =================================================================== */

/*
** xOpen — Open a file.
** Routes by file type:
**   MAIN_DB       → Azure page blob (download full blob into aData)
**   MAIN_JOURNAL  → Azure block blob
**   Everything else → delegate to default VFS
*/
static int azqliteOpen(sqlite3_vfs *pVfs, sqlite3_filename zName,
                        sqlite3_file *pFile, int flags, int *pOutFlags) {
    azqliteVfsData *pVfsData = (azqliteVfsData *)pVfs->pAppData;
    azqliteFile *p = (azqliteFile *)pFile;
    int isMainDb = (flags & SQLITE_OPEN_MAIN_DB) != 0;
    int isMainJournal = (flags & SQLITE_OPEN_MAIN_JOURNAL) != 0;

    /* Initialize pMethod to NULL — prevents xClose call on failure */
    memset(p, 0, sizeof(azqliteFile));

    /* Temp files, sub-journals, etc. → delegate to default VFS */
    if (!isMainDb && !isMainJournal) {
        return pVfsData->pDefaultVfs->xOpen(
            pVfsData->pDefaultVfs, zName, pFile, flags, pOutFlags);
    }

    /* Azure-backed file */
    if (!zName || zName[0] == '\0') {
        return SQLITE_CANTOPEN;
    }

    /* Copy blob name */
    p->zBlobName = sqlite3_mprintf("%s", zName);
    if (!p->zBlobName) return SQLITE_NOMEM;

    /* Wire up Azure ops from VFS global state */
    p->ops = pVfsData->ops;
    p->ops_ctx = pVfsData->ops_ctx;
    p->eFileType = flags & 0x0000FF00;  /* Extract type flags */
    p->eLock = SQLITE_LOCK_NONE;
    p->leaseId[0] = '\0';
    p->pageSize = AZQLITE_DEFAULT_PAGE_SIZE;

    if (isMainDb) {
        /* MAIN_DB → Azure page blob */
        int blobExists = 0;

        if (p->ops && p->ops->blob_exists) {
            azure_error_t aerr;
            azure_error_init(&aerr);
            azure_err_t arc = p->ops->blob_exists(p->ops_ctx, zName,
                                                    &blobExists, &aerr);
            if (arc != AZURE_OK && arc != AZURE_ERR_NOT_FOUND) {
                free(p->zBlobName);
                p->zBlobName = NULL;
                return azureErrToSqlite(arc, SQLITE_CANTOPEN);
            }
        }

        if (blobExists && p->ops && p->ops->blob_get_properties) {
            /* Download existing blob */
            int64_t blobSize = 0;
            azure_error_t aerr;
            azure_error_init(&aerr);
            azure_err_t arc = p->ops->blob_get_properties(
                p->ops_ctx, zName, &blobSize, NULL, NULL, &aerr);
            if (arc != AZURE_OK) {
                free(p->zBlobName);
                p->zBlobName = NULL;
                return azureErrToSqlite(arc, SQLITE_CANTOPEN);
            }

            if (blobSize > 0) {
                int rc = bufferEnsure(p, blobSize);
                if (rc != SQLITE_OK) {
                    free(p->zBlobName);
                    p->zBlobName = NULL;
                    return rc;
                }

                azure_buffer_t buf = {0};
                azure_error_init(&aerr);
                arc = p->ops->page_blob_read(p->ops_ctx, zName,
                                              0, (size_t)blobSize,
                                              &buf, &aerr);
                if (arc != AZURE_OK) {
                    free(p->aData);
                    p->aData = NULL;
                    free(p->aDirty);
                    p->aDirty = NULL;
                    free(p->zBlobName);
                    p->zBlobName = NULL;
                    return azureErrToSqlite(arc, SQLITE_CANTOPEN);
                }

                /* Copy downloaded data into our buffer */
                if (buf.data && buf.size > 0) {
                    sqlite3_int64 copyLen = (sqlite3_int64)buf.size;
                    if (copyLen > blobSize) copyLen = blobSize;
                    memcpy(p->aData, buf.data, (size_t)copyLen);
                    p->nData = copyLen;
                }
                free(buf.data);

                /* Detect page size from header */
                int detected = detectPageSize(p->aData, p->nData);
                if (detected > 0) p->pageSize = detected;

                /* Re-ensure dirty bitmap with correct page size */
                dirtyEnsureCapacity(p);
            } else {
                p->nData = 0;
            }
        } else if (!blobExists && (flags & SQLITE_OPEN_CREATE)) {
            /* Create new page blob */
            if (p->ops && p->ops->page_blob_create) {
                azure_error_t aerr;
                azure_error_init(&aerr);
                azure_err_t arc = p->ops->page_blob_create(
                    p->ops_ctx, zName, 0, &aerr);
                if (arc != AZURE_OK) {
                    free(p->zBlobName);
                    p->zBlobName = NULL;
                    return azureErrToSqlite(arc, SQLITE_CANTOPEN);
                }
            }
            p->nData = 0;
            /* Allocate initial buffer */
            int rc = bufferEnsure(p, AZQLITE_INITIAL_ALLOC);
            if (rc != SQLITE_OK) {
                free(p->zBlobName);
                p->zBlobName = NULL;
                return rc;
            }
        } else if (!blobExists) {
            free(p->zBlobName);
            p->zBlobName = NULL;
            return SQLITE_CANTOPEN;
        }

        dirtyClearAll(p);

    } else {
        /* MAIN_JOURNAL → Azure block blob */
        p->nJrnlData = 0;
        p->nJrnlAlloc = 0;
        p->aJrnlData = NULL;

        /* Check if journal blob already exists (crash recovery) */
        if (p->ops && p->ops->blob_exists) {
            int exists = 0;
            azure_error_t aerr;
            azure_error_init(&aerr);
            p->ops->blob_exists(p->ops_ctx, zName, &exists, &aerr);

            if (exists && p->ops->block_blob_download) {
                azure_buffer_t buf = {0};
                azure_error_init(&aerr);
                azure_err_t arc = p->ops->block_blob_download(
                    p->ops_ctx, zName, &buf, &aerr);
                if (arc == AZURE_OK && buf.data && buf.size > 0) {
                    int rc = jrnlBufferEnsure(p, (sqlite3_int64)buf.size);
                    if (rc == SQLITE_OK) {
                        memcpy(p->aJrnlData, buf.data, buf.size);
                        p->nJrnlData = (sqlite3_int64)buf.size;
                    }
                    free(buf.data);
                    if (rc != SQLITE_OK) {
                        free(p->zBlobName);
                        p->zBlobName = NULL;
                        return rc;
                    }
                } else {
                    free(buf.data);
                }
            }
        }
    }

    /* Set pMethod last — this tells SQLite to call xClose */
    p->pMethod = &azqliteIoMethods;
    if (pOutFlags) {
        *pOutFlags = flags;
    }
    return SQLITE_OK;
}

/*
** xDelete — Delete a blob from Azure (or delegate for temp files).
*/
static int azqliteDelete(sqlite3_vfs *pVfs, const char *zName, int syncDir) {
    azqliteVfsData *pVfsData = (azqliteVfsData *)pVfs->pAppData;

    if (!zName) return SQLITE_OK;

    if (pVfsData->ops && pVfsData->ops->blob_delete) {
        azure_error_t aerr;
        azure_error_init(&aerr);
        azure_err_t arc = pVfsData->ops->blob_delete(
            pVfsData->ops_ctx, zName, &aerr);
        if (arc == AZURE_ERR_NOT_FOUND) {
            return SQLITE_OK;  /* Already gone — not an error */
        }
        if (arc != AZURE_OK) {
            return SQLITE_IOERR_DELETE;
        }
        return SQLITE_OK;
    }

    /* Fallback to default VFS */
    return pVfsData->pDefaultVfs->xDelete(pVfsData->pDefaultVfs, zName, syncDir);
}

/*
** xAccess — Check blob existence or readability.
*/
static int azqliteAccess(sqlite3_vfs *pVfs, const char *zName,
                          int flags, int *pResOut) {
    azqliteVfsData *pVfsData = (azqliteVfsData *)pVfs->pAppData;

    if (pVfsData->ops && pVfsData->ops->blob_exists) {
        int exists = 0;
        azure_error_t aerr;
        azure_error_init(&aerr);
        azure_err_t arc = pVfsData->ops->blob_exists(
            pVfsData->ops_ctx, zName, &exists, &aerr);
        if (arc != AZURE_OK) {
            *pResOut = 0;
            return SQLITE_OK;  /* Access check should not fail fatally */
        }
        switch (flags) {
            case SQLITE_ACCESS_EXISTS:
            case SQLITE_ACCESS_READWRITE:
            case SQLITE_ACCESS_READ:
                *pResOut = exists;
                break;
            default:
                *pResOut = 0;
        }
        return SQLITE_OK;
    }

    /* Fallback to default VFS */
    return pVfsData->pDefaultVfs->xAccess(pVfsData->pDefaultVfs, zName,
                                           flags, pResOut);
}

/*
** xFullPathname — Normalize the blob name.
** Strip leading slashes, reject paths with "..".
*/
static int azqliteFullPathname(sqlite3_vfs *pVfs, const char *zName,
                                int nOut, char *zOut) {
    (void)pVfs;
    if (!zName || zName[0] == '\0') {
        if (nOut > 0) zOut[0] = '\0';
        return SQLITE_OK;
    }

    /* Reject paths containing ".." */
    if (strstr(zName, "..") != NULL) {
        if (nOut > 0) zOut[0] = '\0';
        return SQLITE_CANTOPEN;
    }

    /* Strip leading slashes */
    const char *p = zName;
    while (*p == '/') p++;

    int len = (int)strlen(p);
    if (len >= nOut) len = nOut - 1;
    memcpy(zOut, p, len);
    zOut[len] = '\0';
    return SQLITE_OK;
}

/* ---------- Delegated methods ---------- */

static void *azqliteDlOpen(sqlite3_vfs *pVfs, const char *zFilename) {
    azqliteVfsData *d = (azqliteVfsData *)pVfs->pAppData;
    return d->pDefaultVfs->xDlOpen(d->pDefaultVfs, zFilename);
}

static void azqliteDlError(sqlite3_vfs *pVfs, int nByte, char *zErrMsg) {
    azqliteVfsData *d = (azqliteVfsData *)pVfs->pAppData;
    d->pDefaultVfs->xDlError(d->pDefaultVfs, nByte, zErrMsg);
}

static void (*azqliteDlSym(sqlite3_vfs *pVfs, void *pH,
                             const char *zSym))(void) {
    azqliteVfsData *d = (azqliteVfsData *)pVfs->pAppData;
    return d->pDefaultVfs->xDlSym(d->pDefaultVfs, pH, zSym);
}

static void azqliteDlClose(sqlite3_vfs *pVfs, void *pHandle) {
    azqliteVfsData *d = (azqliteVfsData *)pVfs->pAppData;
    d->pDefaultVfs->xDlClose(d->pDefaultVfs, pHandle);
}

static int azqliteRandomness(sqlite3_vfs *pVfs, int nByte, char *zOut) {
    azqliteVfsData *d = (azqliteVfsData *)pVfs->pAppData;
    return d->pDefaultVfs->xRandomness(d->pDefaultVfs, nByte, zOut);
}

static int azqliteSleep(sqlite3_vfs *pVfs, int microseconds) {
    azqliteVfsData *d = (azqliteVfsData *)pVfs->pAppData;
    return d->pDefaultVfs->xSleep(d->pDefaultVfs, microseconds);
}

static int azqliteCurrentTime(sqlite3_vfs *pVfs, double *pTimeOut) {
    azqliteVfsData *d = (azqliteVfsData *)pVfs->pAppData;
    return d->pDefaultVfs->xCurrentTime(d->pDefaultVfs, pTimeOut);
}

static int azqliteGetLastError(sqlite3_vfs *pVfs, int nByte, char *zErrMsg) {
    azqliteVfsData *d = (azqliteVfsData *)pVfs->pAppData;
    if (nByte > 0 && d->lastError[0]) {
        int len = (int)strlen(d->lastError);
        if (len >= nByte) len = nByte - 1;
        memcpy(zErrMsg, d->lastError, len);
        zErrMsg[len] = '\0';
        return SQLITE_OK;
    }
    return d->pDefaultVfs->xGetLastError(d->pDefaultVfs, nByte, zErrMsg);
}

static int azqliteCurrentTimeInt64(sqlite3_vfs *pVfs, sqlite3_int64 *pTimeOut) {
    azqliteVfsData *d = (azqliteVfsData *)pVfs->pAppData;
    if (d->pDefaultVfs->iVersion >= 2 && d->pDefaultVfs->xCurrentTimeInt64) {
        return d->pDefaultVfs->xCurrentTimeInt64(d->pDefaultVfs, pTimeOut);
    }
    /* Fallback: convert from double */
    double t;
    int rc = d->pDefaultVfs->xCurrentTime(d->pDefaultVfs, &t);
    if (rc == SQLITE_OK) {
        *pTimeOut = (sqlite3_int64)(t * 86400000.0);
    }
    return rc;
}


/* ===================================================================
** VFS object and registration
** =================================================================== */

/* Static VFS data — one instance, set up at registration time */
static azqliteVfsData g_vfsData;

static sqlite3_vfs g_azqliteVfs = {
    2,                          /* iVersion = 2 for xCurrentTimeInt64 */
    0,                          /* szOsFile — set at registration time */
    AZQLITE_MAX_PATHNAME,       /* mxPathname */
    0,                          /* pNext (managed by SQLite) */
    "azqlite",                  /* zName */
    0,                          /* pAppData — set at registration time */
    azqliteOpen,
    azqliteDelete,
    azqliteAccess,
    azqliteFullPathname,
    azqliteDlOpen,
    azqliteDlError,
    azqliteDlSym,
    azqliteDlClose,
    azqliteRandomness,
    azqliteSleep,
    azqliteCurrentTime,
    azqliteGetLastError,
    azqliteCurrentTimeInt64,
    /* v3 methods — not implemented */
    0, 0, 0
};


/*
** azqlite_vfs_register_with_config — Register with explicit config.
*/
int azqlite_vfs_register_with_config(const azqlite_config_t *config,
                                     int makeDefault) {
    sqlite3_vfs *pDefault = sqlite3_vfs_find(0);
    if (!pDefault) return SQLITE_ERROR;

    memset(&g_vfsData, 0, sizeof(g_vfsData));
    g_vfsData.pDefaultVfs = pDefault;

    if (config->ops) {
        /* Use caller-provided ops (test mode) */
        g_vfsData.ops = (azure_ops_t *)config->ops;
        g_vfsData.ops_ctx = config->ops_ctx;
    } else {
        /* Production mode — create Azure client */
        azure_client_config_t clientCfg;
        clientCfg.account = config->account;
        clientCfg.container = config->container;
        clientCfg.sas_token = config->sas_token;
        clientCfg.account_key = config->account_key;
        clientCfg.endpoint = config->endpoint;

        azure_error_t aerr;
        azure_error_init(&aerr);
        azure_err_t arc = azure_client_create(&clientCfg,
                                               &g_vfsData.client, &aerr);
        if (arc != AZURE_OK) {
            snprintf(g_vfsData.lastError, sizeof(g_vfsData.lastError),
                     "Failed to create Azure client: %s", aerr.error_message);
            return SQLITE_ERROR;
        }

        g_vfsData.ops = (azure_ops_t *)azure_client_get_ops();
        g_vfsData.ops_ctx = azure_client_get_ctx(g_vfsData.client);
    }

    /* szOsFile must accommodate both our struct and the default VFS's struct */
    int ourSize = (int)sizeof(azqliteFile);
    int defaultSize = pDefault->szOsFile;
    g_azqliteVfs.szOsFile = (ourSize > defaultSize) ? ourSize : defaultSize;
    g_azqliteVfs.pAppData = &g_vfsData;

    return sqlite3_vfs_register(&g_azqliteVfs, makeDefault);
}


/*
** azqlite_vfs_register — Register reading config from environment.
*/
int azqlite_vfs_register(int makeDefault) {
    azqlite_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    cfg.account = getenv("AZURE_STORAGE_ACCOUNT");
    cfg.container = getenv("AZURE_STORAGE_CONTAINER");
    cfg.sas_token = getenv("AZURE_STORAGE_SAS");
    cfg.account_key = getenv("AZURE_STORAGE_KEY");

    if (!cfg.account || !cfg.container) {
        return SQLITE_ERROR;
    }
    if (!cfg.sas_token && !cfg.account_key) {
        return SQLITE_ERROR;
    }

    return azqlite_vfs_register_with_config(&cfg, makeDefault);
}

/*
** azqlite_vfs_register_with_ops — Convenience for tests.
** Registers the VFS with an explicit ops vtable and context pointer.
*/
int azqlite_vfs_register_with_ops(azure_ops_t *ops, void *ctx,
                                  int makeDefault) {
    azqlite_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.ops = ops;
    cfg.ops_ctx = ctx;
    return azqlite_vfs_register_with_config(&cfg, makeDefault);
}
