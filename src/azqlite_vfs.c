/*
** azqlite_vfs.c — VFS implementation for Azure Blob Storage
**
** This is the core of azqlite.  It implements sqlite3_vfs and
** sqlite3_io_methods v2, routing MAIN_DB operations through Azure
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
#include <sys/time.h>
#include <assert.h>

/* ===================================================================
** Debug timing — opt-in via AZQLITE_DEBUG_TIMING=1 environment variable
** =================================================================== */

static int g_debug_timing = -1; /* -1 = unchecked, 0 = off, 1 = on */

static int azqlite_debug_timing(void) {
    if (g_debug_timing < 0) {
        const char *val = getenv("AZQLITE_DEBUG_TIMING");
        g_debug_timing = (val && val[0] == '1') ? 1 : 0;
    }
    return g_debug_timing;
}

static double azqlite_time_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

/* Cumulative stats reset each xSync */
static int g_xread_count = 0;
static int g_xread_journal_count = 0;

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

/* io_methods v2 — shared memory stubs for WAL exclusive mode */
static int azqliteShmMap(sqlite3_file*, int, int, int, void volatile**);
static int azqliteShmLock(sqlite3_file*, int, int, int);
static void azqliteShmBarrier(sqlite3_file*);
static int azqliteShmUnmap(sqlite3_file*, int);

/* ---------- Constants ---------- */

#define AZQLITE_DEFAULT_PAGE_SIZE  4096
#define AZQLITE_LEASE_DURATION     30      /* default lease duration (seconds) */
#define AZQLITE_LEASE_DURATION_LONG 60     /* extended lease for large flushes */
#define AZQLITE_LEASE_RENEW_AFTER  15      /* default renew threshold (unused — see leaseDuration) */
#define AZQLITE_DIRTY_PAGE_THRESHOLD 100   /* dirty pages triggering extended lease */
#define AZQLITE_MAX_PATHNAME       512
#define AZQLITE_INITIAL_ALLOC      (64*1024)  /* 64 KiB initial buffer */

/* ---------- Types ---------- */

/* Forward declaration for back-pointer in azqliteFile */
typedef struct azqliteVfsData azqliteVfsData;

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
    int lastSyncDirtyCount;             /* Dirty pages at last xSync (lease heuristic) */

    /* Lock state */
    int eLock;                          /* Current SQLite lock level */
    char leaseId[64];                   /* Azure lease ID (empty = no lease) */
    time_t leaseAcquiredAt;             /* For renewal timing */
    int leaseDuration;                  /* Actual lease duration acquired (seconds) */

    /* R2: Skip redundant resize — track last synced blob size */
    sqlite3_int64 lastSyncedSize;       /* Blob size after last successful resize/open */

    /* ETag tracking (preparation for MVP 2 cache invalidation) */
    char etag[128];                     /* Current blob ETag */

    /* File type */
    int eFileType;                      /* SQLITE_OPEN_MAIN_DB, etc. */

    /* Back-pointer to VFS-level shared state */
    azqliteVfsData *pVfsData;           /* VFS global data (for cache access) */

    /* For journal files (block blob) */
    unsigned char *aJrnlData;           /* Journal buffer */
    sqlite3_int64 nJrnlData;           /* Journal logical size */
    sqlite3_int64 nJrnlAlloc;          /* Journal allocated size */

    /* For WAL files (append blob) */
    unsigned char *aWalData;            /* WAL buffer */
    sqlite3_int64 nWalData;            /* WAL logical size */
    sqlite3_int64 nWalAlloc;           /* WAL allocated size */
    sqlite3_int64 nWalSynced;          /* Bytes already synced to Azure append blob */
    int walNeedFullResync;              /* 1 if writes invalidated synced data */
} azqliteFile;

/*
** Global VFS state — stored in sqlite3_vfs.pAppData.
*/
struct azqliteVfsData {
    sqlite3_vfs *pDefaultVfs;           /* The platform default VFS */
    azure_ops_t *ops;                   /* Azure operations vtable */
    void *ops_ctx;                      /* Context for ops */
    azure_client_t *client;             /* Production client (may be NULL for tests) */
    char lastError[256];                /* Last error message for xGetLastError */

    /* R1: Journal blob existence cache.
    ** Since we are the single writer, we know when the journal blob exists
    ** because we created (xSync) or deleted (xDelete) it ourselves.
    ** This eliminates ~4 HEAD requests per transaction (~110ms saved).
    **   -1 = unknown (do real HEAD), 0 = does not exist, 1 = exists */
    int journalCacheState;
    char journalBlobName[512];          /* Tracked journal blob name */
};

/* ---------- io_methods v2 ---------- */

static const sqlite3_io_methods azqliteIoMethods = {
    2,                              /* iVersion = 2 (WAL via exclusive locking) */
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
    /* v2 methods (WAL shared memory stubs — exclusive mode only) */
    azqliteShmMap,
    azqliteShmLock,
    azqliteShmBarrier,
    azqliteShmUnmap,
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
    if (byteIdx < 0 || byteIdx >= p->nDirtyAlloc) return;
    if (!(p->aDirty[byteIdx] & (1 << bitIdx))) {
        p->aDirty[byteIdx] |= (1 << bitIdx);
        p->nDirtyPages++;
    }
}

static int dirtyIsPageDirty(azqliteFile *p, int pageIdx) {
    if (!p->aDirty) return 0;
    int byteIdx = pageIdx / 8;
    int bitIdx = pageIdx % 8;
    if (byteIdx < 0 || byteIdx >= p->nDirtyAlloc) return 0;
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

/*
** Ensure WAL buffer has room for at least newSize bytes.
*/
static int walBufferEnsure(azqliteFile *p, sqlite3_int64 newSize) {
    if (newSize <= p->nWalAlloc) return SQLITE_OK;
    sqlite3_int64 alloc = p->nWalAlloc;
    if (alloc == 0) alloc = AZQLITE_INITIAL_ALLOC;
    while (alloc < newSize) {
        alloc *= 2;
        if (alloc < 0) return SQLITE_NOMEM;
    }
    unsigned char *pNew = (unsigned char *)realloc(p->aWalData, (size_t)alloc);
    if (!pNew) return SQLITE_NOMEM;
    memset(pNew + p->nWalAlloc, 0, (size_t)(alloc - p->nWalAlloc));
    p->aWalData = pNew;
    p->nWalAlloc = alloc;
    return SQLITE_OK;
}


/* ===================================================================
** Helper: lease management
** =================================================================== */

static int hasLease(azqliteFile *p) {
    return p->leaseId[0] != '\0';
}

/*
** Attempt to renew the lease if it's older than half the lease duration.
** Returns SQLITE_OK or SQLITE_IOERR_LOCK.
*/
static int leaseRenewIfNeeded(azqliteFile *p) {
    if (!hasLease(p)) return SQLITE_OK;
    int renewAfter = p->leaseDuration > 0 ? p->leaseDuration / 2
                                          : AZQLITE_LEASE_DURATION / 2;
    time_t now = time(NULL);
    if (difftime(now, p->leaseAcquiredAt) < renewAfter) {
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

    /* Flush any remaining dirty data before closing */
    if (p->nDirtyPages > 0 && p->ops && p->ops->page_blob_write) {
        rc = azqliteSync(pFile, 0);
    } else if (p->eFileType == SQLITE_OPEN_WAL &&
               p->nWalData > p->nWalSynced && p->ops) {
        rc = azqliteSync(pFile, 0);
    }

    /* Release lease if held.  Best-effort: lease auto-expires so we log
    ** but don't fail xClose on release errors. */
    if (hasLease(p) && p->ops && p->ops->lease_release) {
        azure_error_t aerr;
        azure_error_init(&aerr);
        azure_err_t larc = p->ops->lease_release(
            p->ops_ctx, p->zBlobName, p->leaseId, &aerr);
        if (larc != AZURE_OK) {
            fprintf(stderr, "azqlite: lease_release failed (code=%d, blob=%s): %s\n",
                    larc, p->zBlobName ? p->zBlobName : "(null)",
                    aerr.error_message);
        }
        p->leaseId[0] = '\0';
    }

    free(p->aData);
    p->aData = NULL;
    free(p->aDirty);
    p->aDirty = NULL;
    free(p->aJrnlData);
    p->aJrnlData = NULL;
    free(p->aWalData);
    p->aWalData = NULL;
    sqlite3_free(p->zBlobName);
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

    if (p->eFileType == SQLITE_OPEN_WAL) {
        src = p->aWalData;
        srcLen = p->nWalData;
    } else if (p->eFileType & SQLITE_OPEN_MAIN_JOURNAL) {
        src = p->aJrnlData;
        srcLen = p->nJrnlData;
        g_xread_journal_count++;
    } else {
        src = p->aData;
        srcLen = p->nData;
        g_xread_count++;
    }

    if (!src) {
        memset(pBuf, 0, iAmt);
        return SQLITE_IOERR_SHORT_READ;
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

    if (p->eFileType == SQLITE_OPEN_WAL) {
        /* WAL file — write to WAL buffer */
        sqlite3_int64 end = iOfst + iAmt;
        rc = walBufferEnsure(p, end);
        if (rc != SQLITE_OK) return rc;
        memcpy(p->aWalData + iOfst, pBuf, iAmt);
        if (end > p->nWalData) p->nWalData = end;
        /* If overwriting data already synced to Azure, need full re-upload */
        if (iOfst < p->nWalSynced) {
            p->walNeedFullResync = 1;
        }
        return SQLITE_OK;
    }

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

    if (p->eFileType == SQLITE_OPEN_WAL) {
        /* WAL truncate — delete and recreate append blob */
        if (size == 0) {
            if (p->ops && p->ops->append_blob_delete) {
                azure_error_t aerr;
                azure_error_init(&aerr);
                azure_err_t arc = p->ops->append_blob_delete(
                    p->ops_ctx, p->zBlobName, NULL, &aerr);
                if (arc != AZURE_OK && arc != AZURE_ERR_NOT_FOUND) {
                    return SQLITE_IOERR_TRUNCATE;
                }
            }
            if (p->ops && p->ops->append_blob_create) {
                azure_error_t aerr;
                azure_error_init(&aerr);
                azure_err_t arc = p->ops->append_blob_create(
                    p->ops_ctx, p->zBlobName, NULL, &aerr);
                if (arc != AZURE_OK) {
                    return SQLITE_IOERR_TRUNCATE;
                }
            }
            p->nWalData = 0;
            p->nWalSynced = 0;
            p->walNeedFullResync = 0;
            if (p->aWalData) {
                memset(p->aWalData, 0, (size_t)p->nWalAlloc);
            }
        } else if (size < p->nWalData) {
            p->nWalData = size;
            if (size < p->nWalSynced) {
                p->walNeedFullResync = 1;
            }
        }
        return SQLITE_OK;
    }

    if (p->eFileType & SQLITE_OPEN_MAIN_JOURNAL) {
        if (size < p->nJrnlData) {
            p->nJrnlData = size;
            if (p->aJrnlData && size < p->nJrnlAlloc) {
                memset(p->aJrnlData + size, 0,
                       (size_t)(p->nJrnlAlloc - size));
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
        /* R2: Track the new Azure blob size after truncate */
        p->lastSyncedSize = size;
    }

    if (size < p->nData) {
        p->nData = size;
    }
    return SQLITE_OK;
}

/* Maximum bytes per Azure Put Page request (4 MiB) */
#define AZQLITE_MAX_PUT_PAGE  (4 * 1024 * 1024)

/*
** coalesceDirtyRanges — Scan the dirty bitmap and merge contiguous dirty
** pages into azure_page_range_t entries.  Each range is capped at 4 MiB
** and the final range is 512-byte aligned upward.
**
** Returns the number of ranges written to `ranges`, or -1 if maxRanges
** is too small to hold all the coalesced ranges.
*/
static int coalesceDirtyRanges(
    azqliteFile *p,
    azure_page_range_t *ranges,
    int maxRanges
){
    if (p->nDirtyPages == 0) return 0;
    if (!p->aDirty || p->pageSize <= 0 || p->nData <= 0) return 0;

    int nPages = (int)((p->nData + p->pageSize - 1) / p->pageSize);
    int nRanges = 0;
    int i = 0;

    while (i < nPages) {
        /* Skip clean pages */
        if (!dirtyIsPageDirty(p, i)) { i++; continue; }

        /* Start of a dirty run */
        int64_t runStart = (int64_t)i * p->pageSize;
        int runPages = 0;

        while (i < nPages && dirtyIsPageDirty(p, i)) {
            runPages++;
            int64_t runBytes = (int64_t)runPages * p->pageSize;
            if (runBytes >= AZQLITE_MAX_PUT_PAGE) break;
            i++;
        }

        /* If we stopped because of the 4 MiB cap, advance past this page */
        if (i < nPages && dirtyIsPageDirty(p, i)
            && (int64_t)runPages * p->pageSize >= AZQLITE_MAX_PUT_PAGE) {
            i++;
        }

        /* Compute range length.  Last page may be shorter than pageSize. */
        int64_t runEnd = runStart + (int64_t)runPages * p->pageSize;
        if (runEnd > p->nData) runEnd = p->nData;
        size_t len = (size_t)(runEnd - runStart);

        /* 512-byte align up */
        len = (len + 511) & ~(size_t)511;

        if (nRanges >= maxRanges) return -1;

        ranges[nRanges].offset = runStart;
        ranges[nRanges].data = p->aData + runStart;
        ranges[nRanges].len = len;
        nRanges++;
    }

    return nRanges;
}

/* Stack-allocated range threshold (avoid heap for small flushes) */
#define AZQLITE_STACK_RANGES 64

/*
** xSync — Flush dirty pages to Azure.
** For MAIN_DB: coalesce dirty pages, then write via batch or sequential fallback.
** For MAIN_JOURNAL: upload entire journal via block_blob_upload.
** For WAL: append new data to Azure append blob.
*/
static int azqliteSync(sqlite3_file *pFile, int flags) {
    azqliteFile *p = (azqliteFile *)pFile;
    azure_error_t aerr;
    (void)flags;

    if (p->eFileType == SQLITE_OPEN_WAL) {
        /* WAL sync — append new data to Azure append blob */
        if (p->nWalData <= p->nWalSynced && !p->walNeedFullResync) {
            return SQLITE_OK;  /* Nothing new to sync */
        }

        if (!p->ops || !p->ops->append_blob_append) {
            return SQLITE_IOERR_FSYNC;
        }

        if (p->walNeedFullResync) {
            /* Writes invalidated previously synced data — recreate blob */
            if (p->nWalSynced > 0 && p->ops->append_blob_delete) {
                azure_error_init(&aerr);
                azure_err_t arc = p->ops->append_blob_delete(
                    p->ops_ctx, p->zBlobName, NULL, &aerr);
                if (arc != AZURE_OK && arc != AZURE_ERR_NOT_FOUND) {
                    return SQLITE_IOERR_FSYNC;
                }
            }
            if (p->ops->append_blob_create) {
                azure_error_init(&aerr);
                azure_err_t arc = p->ops->append_blob_create(
                    p->ops_ctx, p->zBlobName, NULL, &aerr);
                if (arc != AZURE_OK) {
                    return SQLITE_IOERR_FSYNC;
                }
            }
            /* Upload entire WAL buffer in chunks to respect 4 MiB limit */
            if (p->nWalData > 0) {
                sqlite3_int64 off = 0;
                while (off < p->nWalData) {
                    int chunk = (int)((p->nWalData - off > AZURE_MAX_APPEND_SIZE)
                                      ? AZURE_MAX_APPEND_SIZE : (p->nWalData - off));
                    azure_error_init(&aerr);
                    azure_err_t arc = p->ops->append_blob_append(
                        p->ops_ctx, p->zBlobName,
                        p->aWalData + off, chunk,
                        NULL, &aerr);
                    azure_error_clear(&aerr);
                    if (arc != AZURE_OK) {
                        /* Partial append — delete and recreate blob so next
                        ** sync re-uploads everything cleanly */
                        if (p->ops->append_blob_delete) {
                            azure_error_init(&aerr);
                            p->ops->append_blob_delete(
                                p->ops_ctx, p->zBlobName, NULL, &aerr);
                        }
                        if (p->ops->append_blob_create) {
                            azure_error_init(&aerr);
                            p->ops->append_blob_create(
                                p->ops_ctx, p->zBlobName, NULL, &aerr);
                        }
                        p->nWalSynced = 0;
                        p->walNeedFullResync = 1;
                        return SQLITE_IOERR_FSYNC;
                    }
                    off += chunk;
                }
            }
            p->nWalSynced = p->nWalData;
            p->walNeedFullResync = 0;
        } else {
            /* Incremental append — send only new data since last sync,
            ** chunked to respect 4 MiB limit */
            sqlite3_int64 pending = p->nWalData - p->nWalSynced;
            if (pending > 0) {
                sqlite3_int64 off = 0;
                while (off < pending) {
                    int chunk = (int)((pending - off > AZURE_MAX_APPEND_SIZE)
                                      ? AZURE_MAX_APPEND_SIZE : (pending - off));
                    azure_error_init(&aerr);
                    azure_err_t arc = p->ops->append_blob_append(
                        p->ops_ctx, p->zBlobName,
                        p->aWalData + p->nWalSynced + off, chunk,
                        NULL, &aerr);
                    azure_error_clear(&aerr);
                    if (arc != AZURE_OK) {
                        /* Partial append — delete and recreate blob so next
                        ** sync re-uploads everything cleanly */
                        if (p->ops->append_blob_delete) {
                            azure_error_init(&aerr);
                            p->ops->append_blob_delete(
                                p->ops_ctx, p->zBlobName, NULL, &aerr);
                        }
                        if (p->ops->append_blob_create) {
                            azure_error_init(&aerr);
                            p->ops->append_blob_create(
                                p->ops_ctx, p->zBlobName, NULL, &aerr);
                        }
                        p->nWalSynced = 0;
                        p->walNeedFullResync = 1;
                        return SQLITE_IOERR_FSYNC;
                    }
                    off += chunk;
                }
                p->nWalSynced = p->nWalData;
            }
        }
        return SQLITE_OK;
    }

    if (p->eFileType & SQLITE_OPEN_MAIN_JOURNAL) {
        /* Upload journal as block blob */
        if (p->nJrnlData > 0 && p->ops && p->ops->block_blob_upload) {
            double t0 = 0;
            if (azqlite_debug_timing()) t0 = azqlite_time_ms();

            azure_error_init(&aerr);
            azure_err_t arc = p->ops->block_blob_upload(
                p->ops_ctx, p->zBlobName,
                p->aJrnlData, (size_t)p->nJrnlData, &aerr);

            if (azqlite_debug_timing()) {
                double elapsed = azqlite_time_ms() - t0;
                fprintf(stderr, "[TIMING] xSync(journal): %.1fms (%lld bytes, blob=%s)\n",
                        elapsed, (long long)p->nJrnlData, p->zBlobName);
            }

            if (arc != AZURE_OK) {
                return SQLITE_IOERR_FSYNC;
            }

            /* R1: Journal blob now exists in Azure */
            p->pVfsData->journalCacheState = 1;
        }
        return SQLITE_OK;
    }

    /* MAIN_DB — flush dirty pages */
    if (p->nDirtyPages == 0) return SQLITE_OK;
    if (!p->ops || !p->ops->page_blob_write) return SQLITE_IOERR_FSYNC;

    double sync_t0 = 0;
    if (azqlite_debug_timing()) sync_t0 = azqlite_time_ms();

    /* Record dirty page count for lease duration heuristic */
    int dirtyCountBeforeSync = p->nDirtyPages;

    /* Renew lease before flushing */
    int rc = leaseRenewIfNeeded(p);
    if (rc != SQLITE_OK) return SQLITE_IOERR_FSYNC;

    /* R2: Only resize if the blob has actually grown since last sync.
    ** Most TPC-C transactions modify existing pages without growing the file,
    ** so this skips a ~45ms HTTP round-trip per transaction. */
    double resize_ms = 0;
    if (p->nData > p->lastSyncedSize && p->ops->page_blob_resize) {
        sqlite3_int64 alignedSize = (p->nData + 511) & ~(sqlite3_int64)511;
        double rt0 = 0;
        if (azqlite_debug_timing()) rt0 = azqlite_time_ms();

        azure_error_init(&aerr);
        azure_err_t arc = p->ops->page_blob_resize(p->ops_ctx, p->zBlobName,
                                                     alignedSize,
                                                     hasLease(p) ? p->leaseId : NULL,
                                                     &aerr);
        if (azqlite_debug_timing()) resize_ms = azqlite_time_ms() - rt0;

        if (arc != AZURE_OK) {
            return SQLITE_IOERR_FSYNC;
        }
        p->lastSyncedSize = p->nData;
    }

    /* Coalesce dirty pages into contiguous ranges */
    double coalesce_t0 = 0;
    if (azqlite_debug_timing()) coalesce_t0 = azqlite_time_ms();

    azure_page_range_t stackRanges[AZQLITE_STACK_RANGES];
    azure_page_range_t *ranges = stackRanges;
    int maxRanges = AZQLITE_STACK_RANGES;

    int nRanges = coalesceDirtyRanges(p, ranges, maxRanges);
    if (nRanges < 0) {
        maxRanges = p->nDirtyPages;
        ranges = (azure_page_range_t *)sqlite3_malloc64(
            (sqlite3_int64)maxRanges * (sqlite3_int64)sizeof(azure_page_range_t));
        if (!ranges) return SQLITE_NOMEM;
        nRanges = coalesceDirtyRanges(p, ranges, maxRanges);
        if (nRanges < 0) {
            sqlite3_free(ranges);
            return SQLITE_IOERR_FSYNC;
        }
    }

    double coalesce_ms = 0;
    if (azqlite_debug_timing()) coalesce_ms = azqlite_time_ms() - coalesce_t0;

    /* Try batch write if available (Phase 2 — will be non-NULL with curl_multi) */
    if (p->ops->page_blob_write_batch) {
        double wt0 = 0;
        if (azqlite_debug_timing()) wt0 = azqlite_time_ms();

        azure_error_init(&aerr);
        azure_err_t arc = p->ops->page_blob_write_batch(
            p->ops_ctx, p->zBlobName,
            ranges, nRanges,
            hasLease(p) ? p->leaseId : NULL, &aerr);

        if (azqlite_debug_timing()) {
            double write_ms = azqlite_time_ms() - wt0;
            double total_ms = azqlite_time_ms() - sync_t0;
            size_t total_bytes = 0;
            for (int i = 0; i < nRanges; i++) total_bytes += ranges[i].len;
            fprintf(stderr, "[TIMING] xSync(db): total=%.1fms | resize=%.1fms coalesce=%.1fms "
                    "write_batch=%.1fms | dirty_pages=%d ranges=%d bytes=%zu "
                    "reads_since_sync=%d jrnl_reads=%d\n",
                    total_ms, resize_ms, coalesce_ms, write_ms,
                    dirtyCountBeforeSync, nRanges, total_bytes,
                    g_xread_count, g_xread_journal_count);
            g_xread_count = 0;
            g_xread_journal_count = 0;
        }

        if (ranges != stackRanges) sqlite3_free(ranges);
        if (arc != AZURE_OK) return SQLITE_IOERR_FSYNC;
        p->lastSyncDirtyCount = dirtyCountBeforeSync;
        if (aerr.etag[0] != '\0') {
            memcpy(p->etag, aerr.etag, sizeof(p->etag));
        }
        dirtyClearAll(p);
        return SQLITE_OK;
    }

    /* Sequential fallback — write each coalesced range */
    double write_t0 = 0;
    if (azqlite_debug_timing()) write_t0 = azqlite_time_ms();

    for (int i = 0; i < nRanges; i++) {
        /* Renew lease periodically during large flushes */
        if (hasLease(p) && i > 0 && (i % 50 == 0)) {
            rc = leaseRenewIfNeeded(p);
            if (rc != SQLITE_OK) {
                if (ranges != stackRanges) sqlite3_free(ranges);
                return SQLITE_IOERR_FSYNC;
            }
        }

        azure_error_init(&aerr);
        azure_err_t arc = p->ops->page_blob_write(
            p->ops_ctx, p->zBlobName,
            ranges[i].offset, ranges[i].data, ranges[i].len,
            hasLease(p) ? p->leaseId : NULL, &aerr);

        if (arc != AZURE_OK) {
            if (ranges != stackRanges) sqlite3_free(ranges);
            return SQLITE_IOERR_FSYNC;
        }
    }

    if (azqlite_debug_timing()) {
        double write_ms = azqlite_time_ms() - write_t0;
        double total_ms = azqlite_time_ms() - sync_t0;
        size_t total_bytes = 0;
        for (int i = 0; i < nRanges; i++) total_bytes += ranges[i].len;
        fprintf(stderr, "[TIMING] xSync(db): total=%.1fms | resize=%.1fms coalesce=%.1fms "
                "write_seq=%.1fms | dirty_pages=%d ranges=%d bytes=%zu "
                "reads_since_sync=%d jrnl_reads=%d\n",
                total_ms, resize_ms, coalesce_ms, write_ms,
                dirtyCountBeforeSync, nRanges, total_bytes,
                g_xread_count, g_xread_journal_count);
        g_xread_count = 0;
        g_xread_journal_count = 0;
    }

    /* Capture ETag from last successful write and update dirty count */
    p->lastSyncDirtyCount = dirtyCountBeforeSync;
    if (aerr.etag[0] != '\0') {
        memcpy(p->etag, aerr.etag, sizeof(p->etag));
    }

    if (ranges != stackRanges) sqlite3_free(ranges);
    dirtyClearAll(p);
    return SQLITE_OK;
}

/*
** xFileSize — Return the current logical file size.
*/
static int azqliteFileSize(sqlite3_file *pFile, sqlite3_int64 *pSize) {
    azqliteFile *p = (azqliteFile *)pFile;
    if (p->eFileType == SQLITE_OPEN_WAL) {
        *pSize = p->nWalData;
    } else if (p->eFileType & SQLITE_OPEN_MAIN_JOURNAL) {
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

        /* Choose lease duration based on last sync workload */
        int duration = (p->lastSyncDirtyCount > AZQLITE_DIRTY_PAGE_THRESHOLD)
                       ? AZQLITE_LEASE_DURATION_LONG
                       : AZQLITE_LEASE_DURATION;

        double t0 = 0;
        if (azqlite_debug_timing()) t0 = azqlite_time_ms();

        azure_error_t aerr;
        azure_error_init(&aerr);
        azure_err_t arc = p->ops->lease_acquire(
            p->ops_ctx, p->zBlobName,
            duration,
            p->leaseId, sizeof(p->leaseId), &aerr);

        if (azqlite_debug_timing()) {
            double elapsed = azqlite_time_ms() - t0;
            fprintf(stderr, "[TIMING] lease_acquire: %.1fms (lock=%d, blob=%s)\n",
                    elapsed, eLock, p->zBlobName);
        }

        if (arc == AZURE_ERR_CONFLICT) {
            return SQLITE_BUSY;
        }
        if (arc != AZURE_OK) {
            return azureErrToSqlite(arc, SQLITE_IOERR_LOCK);
        }
        p->leaseAcquiredAt = time(NULL);
        p->leaseDuration = duration;
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
            double t0 = 0;
            if (azqlite_debug_timing()) t0 = azqlite_time_ms();

            azure_error_t aerr;
            azure_error_init(&aerr);
            p->ops->lease_release(p->ops_ctx, p->zBlobName,
                                   p->leaseId, &aerr);

            if (azqlite_debug_timing()) {
                double elapsed = azqlite_time_ms() - t0;
                fprintf(stderr, "[TIMING] lease_release: %.1fms (blob=%s)\n",
                        elapsed, p->zBlobName);
            }
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
                    /* WAL mode requires append blob operations.
                    ** If unavailable, reject and force "delete" mode. */
                    azqliteFile *p = (azqliteFile *)pFile;
                    if (!p->ops || !p->ops->append_blob_create ||
                        !p->ops->append_blob_append ||
                        !p->ops->append_blob_delete) {
                        aArg[0] = sqlite3_mprintf("delete");
                        return SQLITE_OK;
                    }
                    /* Append blob ops available — let SQLite set WAL mode.
                    ** If locking_mode=EXCLUSIVE is not set, xShmMap will
                    ** return SQLITE_IOERR and fail safely. */
                    return SQLITE_NOTFOUND;
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
** Shared memory stubs — WAL exclusive locking mode
**
** In exclusive locking mode (PRAGMA locking_mode=EXCLUSIVE), SQLite
** never calls these methods. They exist only to satisfy iVersion=2
** requirements. If called outside exclusive mode, xShmMap returns
** SQLITE_IOERR to prevent unsafe operation.
** =================================================================== */

static int azqliteShmMap(sqlite3_file *pFile, int iRegion, int szRegion,
                          int bExtend, void volatile **pp) {
    (void)pFile; (void)iRegion; (void)szRegion; (void)bExtend;
    *pp = NULL;
    /* Never called in exclusive mode. Return SQLITE_IOERR to fail safely
    ** if the user forgot PRAGMA locking_mode=EXCLUSIVE. */
    fprintf(stderr,
        "azqlite: ERROR — xShmMap called, but azqlite WAL requires "
        "PRAGMA locking_mode=EXCLUSIVE. Shared-memory WAL is not supported.\n");
    return SQLITE_IOERR;
}

static int azqliteShmLock(sqlite3_file *pFile, int offset, int n, int flags) {
    (void)pFile; (void)offset; (void)n; (void)flags;
    return SQLITE_OK;
}

static void azqliteShmBarrier(sqlite3_file *pFile) {
    (void)pFile;
}

static int azqliteShmUnmap(sqlite3_file *pFile, int deleteFlag) {
    (void)pFile; (void)deleteFlag;
    return SQLITE_OK;
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
    int isWal = (flags & SQLITE_OPEN_WAL) != 0;

    /* Initialize pMethod to NULL — prevents xClose call on failure */
    memset(p, 0, sizeof(azqliteFile));

    /* Temp files, sub-journals, etc. → delegate to default VFS */
    if (!isMainDb && !isMainJournal && !isWal) {
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
    p->pVfsData = pVfsData;             /* R1: back-pointer for cache access */
    if (isWal) p->eFileType = SQLITE_OPEN_WAL;
    else if (isMainJournal) p->eFileType = SQLITE_OPEN_MAIN_JOURNAL;
    else p->eFileType = SQLITE_OPEN_MAIN_DB;
    p->eLock = SQLITE_LOCK_NONE;
    p->leaseId[0] = '\0';
    p->leaseDuration = AZQLITE_LEASE_DURATION;
    p->lastSyncDirtyCount = 0;
    p->etag[0] = '\0';
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
                sqlite3_free(p->zBlobName);
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
                sqlite3_free(p->zBlobName);
                p->zBlobName = NULL;
                return azureErrToSqlite(arc, SQLITE_CANTOPEN);
            }
            /* Capture initial ETag from blob properties */
            if (aerr.etag[0] != '\0') {
                memcpy(p->etag, aerr.etag, sizeof(p->etag));
            }

            if (blobSize > 0) {
                int rc = bufferEnsure(p, blobSize);
                if (rc != SQLITE_OK) {
                    sqlite3_free(p->zBlobName);
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
                    sqlite3_free(p->zBlobName);
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

                /* R2: Record initial Azure blob size to skip redundant resizes */
                p->lastSyncedSize = p->nData;
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
                    sqlite3_free(p->zBlobName);
                    p->zBlobName = NULL;
                    return azureErrToSqlite(arc, SQLITE_CANTOPEN);
                }
            }
            p->nData = 0;
            /* Allocate initial buffer */
            int rc = bufferEnsure(p, AZQLITE_INITIAL_ALLOC);
            if (rc != SQLITE_OK) {
                sqlite3_free(p->zBlobName);
                p->zBlobName = NULL;
                return rc;
            }
        } else if (!blobExists) {
            sqlite3_free(p->zBlobName);
            p->zBlobName = NULL;
            return SQLITE_CANTOPEN;
        }

        dirtyClearAll(p);

    } else if (isMainJournal) {
        /* MAIN_JOURNAL → Azure block blob */
        p->nJrnlData = 0;
        p->nJrnlAlloc = 0;
        p->aJrnlData = NULL;

        /* R1: Track journal blob name and seed the cache from blob_exists check */
        size_t nameLen = strlen(zName);
        if (nameLen < sizeof(pVfsData->journalBlobName)) {
            memcpy(pVfsData->journalBlobName, zName, nameLen + 1);
        }

        /* R1: If cache says journal doesn't exist, skip the HEAD request.
        ** Only do a real HEAD when cache is unknown (-1) or says it exists (1).
        ** Since we are the single writer, cache=0 is authoritative. */
        int exists = 0;
        if (pVfsData->journalCacheState == 0) {
            exists = 0;  /* We deleted it — no HEAD needed */
        } else if (p->ops && p->ops->blob_exists) {
            azure_error_t aerr;
            azure_error_init(&aerr);
            p->ops->blob_exists(p->ops_ctx, zName, &exists, &aerr);
            pVfsData->journalCacheState = exists ? 1 : 0;
        }

        /* Check if journal blob already exists (crash recovery) */
        if (exists && p->ops && p->ops->block_blob_download) {
            azure_buffer_t buf = {0};
            azure_error_t aerr;
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
                    sqlite3_free(p->zBlobName);
                    p->zBlobName = NULL;
                    return rc;
                }
            } else {
                free(buf.data);
            }
        }

    } else if (isWal) {
        /* WAL → Azure append blob */

        /* Verify append blob operations are available */
        if (!p->ops || !p->ops->append_blob_create ||
            !p->ops->append_blob_append || !p->ops->append_blob_delete) {
            sqlite3_free(p->zBlobName);
            p->zBlobName = NULL;
            return SQLITE_CANTOPEN;
        }

        /* Check if WAL blob exists (for crash recovery) */
        int walExists = 0;
        if (p->ops->blob_exists) {
            azure_error_t aerr;
            azure_error_init(&aerr);
            azure_err_t arc = p->ops->blob_exists(p->ops_ctx, zName,
                                                    &walExists, &aerr);
            if (arc != AZURE_OK && arc != AZURE_ERR_NOT_FOUND) {
                sqlite3_free(p->zBlobName);
                p->zBlobName = NULL;
                return azureErrToSqlite(arc, SQLITE_CANTOPEN);
            }
        }

        if (walExists && p->ops->block_blob_download) {
            /* Download existing WAL for crash recovery.
            ** block_blob_download uses GET Blob which works on all blob types. */
            azure_buffer_t buf = {0};
            azure_error_t aerr;
            azure_error_init(&aerr);
            azure_err_t arc = p->ops->block_blob_download(
                p->ops_ctx, zName, &buf, &aerr);
            if (arc == AZURE_OK && buf.data && buf.size > 0) {
                int rc = walBufferEnsure(p, (sqlite3_int64)buf.size);
                if (rc == SQLITE_OK) {
                    memcpy(p->aWalData, buf.data, buf.size);
                    p->nWalData = (sqlite3_int64)buf.size;
                    p->nWalSynced = p->nWalData;
                }
                free(buf.data);
                if (rc != SQLITE_OK) {
                    free(p->aWalData);
                    p->aWalData = NULL;
                    sqlite3_free(p->zBlobName);
                    p->zBlobName = NULL;
                    return rc;
                }
            } else if (arc != AZURE_OK) {
                /* Download failed (transient network error etc.) —
                ** do NOT proceed without WAL replay or committed
                ** transactions will be silently lost. */
                free(buf.data);
                sqlite3_free(p->zBlobName);
                p->zBlobName = NULL;
                return azureErrToSqlite(arc, SQLITE_CANTOPEN);
            } else {
                /* Download succeeded but WAL is empty — safe to proceed */
                free(buf.data);
            }
        } else if (!walExists) {
            /* Create new empty append blob */
            azure_error_t aerr;
            azure_error_init(&aerr);
            azure_err_t arc = p->ops->append_blob_create(
                p->ops_ctx, zName, NULL, &aerr);
            if (arc != AZURE_OK) {
                sqlite3_free(p->zBlobName);
                p->zBlobName = NULL;
                return azureErrToSqlite(arc, SQLITE_CANTOPEN);
            }
        }

        p->walNeedFullResync = 0;
    }

    /* Set pMethod last — this tells SQLite to call xClose */
    p->pMethod = &azqliteIoMethods;
    if (pOutFlags) {
        *pOutFlags = flags;
    }
    return SQLITE_OK;
}

/*
** Helper: determine if a path should be routed to Azure.
** Azure blob names are relative (no leading '/').
** Absolute paths (starting with '/') are local filesystem paths
** (temp files, sub-journals, etc.) and should go to the default VFS.
*/
static int azqliteIsAzurePath(const char *zPath) {
    if (!zPath || zPath[0] == '\0') return 0;
    /* Absolute filesystem paths go to the default VFS */
    if (zPath[0] == '/') return 0;
    /* Relative paths are Azure blob names */
    return 1;
}

/*
** xDelete — Delete a blob from Azure (or delegate for temp files).
*/
static int azqliteDelete(sqlite3_vfs *pVfs, const char *zName, int syncDir) {
    azqliteVfsData *pVfsData = (azqliteVfsData *)pVfs->pAppData;

    if (!zName) return SQLITE_OK;

    /* Delegate non-Azure paths (absolute filesystem paths) to default VFS */
    if (!azqliteIsAzurePath(zName)) {
        return pVfsData->pDefaultVfs->xDelete(pVfsData->pDefaultVfs, zName, syncDir);
    }

    if (pVfsData->ops && pVfsData->ops->blob_delete) {
        azure_error_t aerr;
        azure_error_init(&aerr);
        azure_err_t arc = pVfsData->ops->blob_delete(
            pVfsData->ops_ctx, zName, &aerr);
        if (arc == AZURE_ERR_NOT_FOUND) {
            /* R1: We know it doesn't exist now */
            if (pVfsData->journalBlobName[0] &&
                strcmp(zName, pVfsData->journalBlobName) == 0) {
                pVfsData->journalCacheState = 0;
            }
            return SQLITE_OK;  /* Already gone — not an error */
        }
        if (arc != AZURE_OK) {
            return SQLITE_IOERR_DELETE;
        }

        /* R1: After successful delete, update journal cache */
        if (pVfsData->journalBlobName[0] &&
            strcmp(zName, pVfsData->journalBlobName) == 0) {
            pVfsData->journalCacheState = 0;
        }
        return SQLITE_OK;
    }

    /* Fallback to default VFS */
    return pVfsData->pDefaultVfs->xDelete(pVfsData->pDefaultVfs, zName, syncDir);
}

/*
** xAccess — Check blob existence or readability.
**
** R1 optimization: We cache journal blob existence state. Since we are the
** single writer, we know when the journal exists (we uploaded it in xSync)
** or doesn't exist (we deleted it in xDelete). This eliminates ~4 HEAD
** requests per transaction (~110ms saved).
*/
static int azqliteAccess(sqlite3_vfs *pVfs, const char *zName,
                          int flags, int *pResOut) {
    azqliteVfsData *pVfsData = (azqliteVfsData *)pVfs->pAppData;

    /* Delegate non-Azure paths (absolute filesystem paths) to default VFS */
    if (!azqliteIsAzurePath(zName)) {
        return pVfsData->pDefaultVfs->xAccess(pVfsData->pDefaultVfs, zName,
                                               flags, pResOut);
    }

    if (pVfsData->ops && pVfsData->ops->blob_exists) {
        /* R1: Use cached journal existence when available */
        if (zName && pVfsData->journalCacheState >= 0 &&
            pVfsData->journalBlobName[0] &&
            strcmp(zName, pVfsData->journalBlobName) == 0) {
            switch (flags) {
                case SQLITE_ACCESS_EXISTS:
                case SQLITE_ACCESS_READWRITE:
                case SQLITE_ACCESS_READ:
                    *pResOut = pVfsData->journalCacheState;
                    break;
                default:
                    *pResOut = 0;
            }
            return SQLITE_OK;
        }

        int exists = 0;
        azure_error_t aerr;
        azure_error_init(&aerr);
        azure_err_t arc = pVfsData->ops->blob_exists(
            pVfsData->ops_ctx, zName, &exists, &aerr);
        if (arc != AZURE_OK) {
            *pResOut = 0;
            return SQLITE_OK;  /* Access check should not fail fatally */
        }

        /* R1: Seed journal cache if this is a journal blob we haven't tracked yet.
        ** Detects journal names by "-journal" suffix (per D7). */
        if (zName && !pVfsData->journalBlobName[0]) {
            size_t nlen = strlen(zName);
            if (nlen >= 8 && strcmp(zName + nlen - 8, "-journal") == 0 &&
                nlen < sizeof(pVfsData->journalBlobName)) {
                memcpy(pVfsData->journalBlobName, zName, nlen + 1);
                pVfsData->journalCacheState = exists ? 1 : 0;
            }
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
    g_vfsData.journalCacheState = -1;  /* R1: unknown until first journal open */

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
