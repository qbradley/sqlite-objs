/*
** sqlite_objs_vfs.c — VFS implementation for Azure Blob Storage
**
** This is the core of sqliteObjs.  It implements sqlite3_vfs and
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

#include "sqlite_objs.h"
#include "azure_client.h"
#include "sqlite3.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include <assert.h>
#include <unistd.h>

/* ===================================================================
** Debug timing — opt-in via SQLITE_OBJS_DEBUG_TIMING=1 environment variable
** =================================================================== */

static int g_debug_timing = -1; /* -1 = unchecked, 0 = off, 1 = on */

static int sqlite_objs_debug_timing(void) {
    if (g_debug_timing < 0) {
        const char *val = getenv("SQLITE_OBJS_DEBUG_TIMING");
        g_debug_timing = (val && val[0] == '1') ? 1 : 0;
    }
    return g_debug_timing;
}

static double sqlite_objs_time_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

/* Cumulative stats reset each xSync */
static int g_xread_count = 0;
static int g_xread_journal_count = 0;

/* ---------- Forward declarations ---------- */

static int sqliteObjsOpen(sqlite3_vfs*, sqlite3_filename, sqlite3_file*, int, int*);
static int sqliteObjsDelete(sqlite3_vfs*, const char*, int);
static int sqliteObjsAccess(sqlite3_vfs*, const char*, int, int*);
static int sqliteObjsFullPathname(sqlite3_vfs*, const char*, int, char*);
static void *sqliteObjsDlOpen(sqlite3_vfs*, const char*);
static void sqliteObjsDlError(sqlite3_vfs*, int, char*);
static void (*sqliteObjsDlSym(sqlite3_vfs*, void*, const char*))(void);
static void sqliteObjsDlClose(sqlite3_vfs*, void*);
static int sqliteObjsRandomness(sqlite3_vfs*, int, char*);
static int sqliteObjsSleep(sqlite3_vfs*, int);
static int sqliteObjsCurrentTime(sqlite3_vfs*, double*);
static int sqliteObjsGetLastError(sqlite3_vfs*, int, char*);
static int sqliteObjsCurrentTimeInt64(sqlite3_vfs*, sqlite3_int64*);

/* io_methods */
static int sqliteObjsClose(sqlite3_file*);
static int sqliteObjsRead(sqlite3_file*, void*, int, sqlite3_int64);
static int sqliteObjsWrite(sqlite3_file*, const void*, int, sqlite3_int64);
static int sqliteObjsTruncate(sqlite3_file*, sqlite3_int64);
static int sqliteObjsSync(sqlite3_file*, int);
static int sqliteObjsFileSize(sqlite3_file*, sqlite3_int64*);
static int sqliteObjsLock(sqlite3_file*, int);
static int sqliteObjsUnlock(sqlite3_file*, int);
static int sqliteObjsCheckReservedLock(sqlite3_file*, int*);
static int sqliteObjsFileControl(sqlite3_file*, int, void*);
static int sqliteObjsSectorSize(sqlite3_file*);
static int sqliteObjsDeviceCharacteristics(sqlite3_file*);

/* io_methods v2 — shared memory stubs for WAL exclusive mode */
static int sqliteObjsShmMap(sqlite3_file*, int, int, int, void volatile**);
static int sqliteObjsShmLock(sqlite3_file*, int, int, int);
static void sqliteObjsShmBarrier(sqlite3_file*);
static int sqliteObjsShmUnmap(sqlite3_file*, int);

/* ---------- Constants ---------- */

#define SQLITE_OBJS_DEFAULT_PAGE_SIZE  4096
#define SQLITE_OBJS_LEASE_DURATION     30      /* default lease duration (seconds) */
#define SQLITE_OBJS_LEASE_DURATION_LONG 60     /* extended lease for large flushes */
#define SQLITE_OBJS_LEASE_RENEW_AFTER  15      /* default renew threshold (unused — see leaseDuration) */
#define SQLITE_OBJS_DIRTY_PAGE_THRESHOLD 100   /* dirty pages triggering extended lease */
#define SQLITE_OBJS_MAX_PATHNAME       512
#define SQLITE_OBJS_INITIAL_ALLOC      (64*1024)  /* 64 KiB initial buffer */

/* ---------- Types ---------- */

/* Forward declaration for back-pointer in sqliteObjsFile */
typedef struct sqliteObjsVfsData sqliteObjsVfsData;

/*
** Per-file state for Azure-backed files.
** The first member MUST be pMethod (sqlite3_io_methods*) to satisfy
** the sqlite3_file subclass contract.
*/
typedef struct sqliteObjsFile {
    const sqlite3_io_methods *pMethod;  /* MUST be first */

    /* Azure connection */
    azure_ops_t *ops;                   /* Swappable Azure operations vtable */
    void *ops_ctx;                      /* Context for ops */
    char *zBlobName;                    /* Blob name in container */

    /* Disk-backed cache for MAIN_DB */
    int cacheFd;                        /* File descriptor (-1 if not open) */
    char *zCachePath;                   /* Path to cache file (for cleanup) */
    sqlite3_int64 nData;                /* Current logical size */

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
    sqliteObjsVfsData *pVfsData;           /* VFS global data (for cache access) */

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

    /* Per-file Azure client (NULL = using global VFS client) */
    azure_client_t *ownClient;
} sqliteObjsFile;

/*
** Per-journal blob existence cache entry.
** Each open database has its own journal blob with independent state.
**   -1 = unknown (do real HEAD), 0 = does not exist, 1 = exists
*/
#define SQLITE_OBJS_MAX_JOURNAL_CACHE 16

typedef struct sqliteObjsJournalCacheEntry {
    int state;
    char zBlobName[512];
} sqliteObjsJournalCacheEntry;

/*
** Global VFS state — stored in sqlite3_vfs.pAppData.
*/
struct sqliteObjsVfsData {
    sqlite3_vfs *pDefaultVfs;           /* The platform default VFS */
    azure_ops_t *ops;                   /* Azure operations vtable */
    void *ops_ctx;                      /* Context for ops */
    azure_client_t *client;             /* Production client (may be NULL for tests) */
    char lastError[256];                /* Last error message for xGetLastError */

    /* R1: Per-journal blob existence cache (one entry per database).
    ** Since we are the single writer, we know when a journal blob exists
    ** because we created (xSync) or deleted (xDelete) it ourselves.
    ** This eliminates ~4 HEAD requests per transaction (~110ms saved). */
    sqliteObjsJournalCacheEntry journalCache[SQLITE_OBJS_MAX_JOURNAL_CACHE];
    int nJournalCache;
};

/* ---------- io_methods v2 ---------- */

/* ===================================================================
** Helper: per-journal blob cache lookup
** =================================================================== */

/* Find a journal cache entry by blob name. Returns pointer or NULL. */
static sqliteObjsJournalCacheEntry *journalCacheFind(sqliteObjsVfsData *pData,
                                                   const char *zName) {
    for (int i = 0; i < pData->nJournalCache; i++) {
        if (strcmp(pData->journalCache[i].zBlobName, zName) == 0) {
            return &pData->journalCache[i];
        }
    }
    return NULL;
}

/* Find or create a journal cache entry. Returns pointer or NULL if full
** or name too long. New entries start with state = -1 (unknown). */
static sqliteObjsJournalCacheEntry *journalCacheGetOrCreate(sqliteObjsVfsData *pData,
                                                          const char *zName) {
    sqliteObjsJournalCacheEntry *e = journalCacheFind(pData, zName);
    if (e) return e;
    if (pData->nJournalCache >= SQLITE_OBJS_MAX_JOURNAL_CACHE) return NULL;
    size_t n = strlen(zName);
    if (n >= sizeof(pData->journalCache[0].zBlobName)) return NULL;
    e = &pData->journalCache[pData->nJournalCache++];
    e->state = -1;
    memcpy(e->zBlobName, zName, n + 1);
    return e;
}

static const sqlite3_io_methods sqliteObjsIoMethods = {
    2,                              /* iVersion = 2 (WAL via exclusive locking) */
    sqliteObjsClose,
    sqliteObjsRead,
    sqliteObjsWrite,
    sqliteObjsTruncate,
    sqliteObjsSync,
    sqliteObjsFileSize,
    sqliteObjsLock,
    sqliteObjsUnlock,
    sqliteObjsCheckReservedLock,
    sqliteObjsFileControl,
    sqliteObjsSectorSize,
    sqliteObjsDeviceCharacteristics,
    /* v2 methods (WAL shared memory stubs — exclusive mode only) */
    sqliteObjsShmMap,
    sqliteObjsShmLock,
    sqliteObjsShmBarrier,
    sqliteObjsShmUnmap,
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

static void dirtyMarkPage(sqliteObjsFile *p, sqlite3_int64 offset) {
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

static int dirtyIsPageDirty(sqliteObjsFile *p, int pageIdx) {
    if (!p->aDirty) return 0;
    int byteIdx = pageIdx / 8;
    int bitIdx = pageIdx % 8;
    if (byteIdx < 0 || byteIdx >= p->nDirtyAlloc) return 0;
    return (p->aDirty[byteIdx] & (1 << bitIdx)) != 0;
}

static void dirtyClearAll(sqliteObjsFile *p) {
    if (p->aDirty && p->nDirtyAlloc > 0) {
        memset(p->aDirty, 0, p->nDirtyAlloc);
    }
    p->nDirtyPages = 0;
}

/*
** Ensure the dirty bitmap is large enough for the current allocation.
** Returns SQLITE_OK or SQLITE_NOMEM.
*/
static int dirtyEnsureCapacity(sqliteObjsFile *p) {
    int needed = dirtyBitmapSize(p->nData, p->pageSize);
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
** Helper: cache file management for MAIN_DB
** =================================================================== */

/*
** Ensure the cache file has room for at least newSize bytes.
** Returns SQLITE_OK or SQLITE_IOERR.
*/
static int cacheEnsureSize(sqliteObjsFile *p, sqlite3_int64 newSize) {
    if (p->cacheFd < 0) return SQLITE_IOERR;
    
    /* Get current file size */
    off_t currentSize = lseek(p->cacheFd, 0, SEEK_END);
    if (currentSize < 0) return SQLITE_IOERR;
    
    if (newSize <= currentSize) return SQLITE_OK;
    
    /* Extend cache file */
    if (ftruncate(p->cacheFd, (off_t)newSize) != 0) {
        return SQLITE_IOERR;
    }
    
    return dirtyEnsureCapacity(p);
}

/*
** Ensure journal buffer has room for at least newSize bytes.
*/
static int jrnlBufferEnsure(sqliteObjsFile *p, sqlite3_int64 newSize) {
    if (newSize <= p->nJrnlAlloc) return SQLITE_OK;
    sqlite3_int64 alloc = p->nJrnlAlloc;
    if (alloc == 0) alloc = SQLITE_OBJS_INITIAL_ALLOC;
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
static int walBufferEnsure(sqliteObjsFile *p, sqlite3_int64 newSize) {
    if (newSize <= p->nWalAlloc) return SQLITE_OK;
    sqlite3_int64 alloc = p->nWalAlloc;
    if (alloc == 0) alloc = SQLITE_OBJS_INITIAL_ALLOC;
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

static int hasLease(sqliteObjsFile *p) {
    return p->leaseId[0] != '\0';
}

/*
** Attempt to renew the lease if it's older than half the lease duration.
** Returns SQLITE_OK or SQLITE_IOERR_LOCK.
*/
static int leaseRenewIfNeeded(sqliteObjsFile *p) {
    if (!hasLease(p)) return SQLITE_OK;
    int renewAfter = p->leaseDuration > 0 ? p->leaseDuration / 2
                                          : SQLITE_OBJS_LEASE_DURATION / 2;
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
** Resolve Azure storage ops for VFS-level methods (xDelete, xAccess).
**
** These methods only have the VFS pointer (not a file pointer), so they
** normally use the global ops in pVfsData. In URI-only mode the global ops
** are NULL. For journal/WAL filenames we can recover the per-file ops from
** the main database file via sqlite3_database_file_object().
*/
static int resolveOps(sqliteObjsVfsData *pVfsData, const char *zName,
                      const azure_ops_t **ppOps, void **ppCtx) {
    if (pVfsData->ops) {
        *ppOps = pVfsData->ops;
        *ppCtx = pVfsData->ops_ctx;
        return 1;
    }
    /* URI-only mode: try to resolve per-file ops from the main DB file.
    ** sqlite3_database_file_object() is only valid for journal/WAL filenames
    ** (those that end with "-journal" or "-wal"). */
    if (zName) {
        size_t n = strlen(zName);
        int isJournalOrWal = (n >= 8 && strcmp(zName + n - 8, "-journal") == 0)
                          || (n >= 4 && strcmp(zName + n - 4, "-wal") == 0);
        if (isJournalOrWal) {
            sqlite3_file *pDbFile = sqlite3_database_file_object(zName);
            if (pDbFile) {
                sqliteObjsFile *pMain = (sqliteObjsFile *)pDbFile;
                if (pMain->ops) {
                    *ppOps = pMain->ops;
                    *ppCtx = pMain->ops_ctx;
                    return 1;
                }
            }
        }
    }
    *ppOps = NULL;
    *ppCtx = NULL;
    return 0;
}
static int azureErrToSqlite(azure_err_t aerr, int ioerr_variant) {
    int r;
    switch (aerr) {
        case AZURE_OK:           r = SQLITE_OK; break;
        case AZURE_ERR_NOT_FOUND: r = SQLITE_CANTOPEN; break;
        case AZURE_ERR_CONFLICT:  r = SQLITE_BUSY; break;
        case AZURE_ERR_THROTTLED: r = SQLITE_BUSY; break;
        case AZURE_ERR_LEASE_EXPIRED: r = SQLITE_IOERR_LOCK; break;
        case AZURE_ERR_AUTH:      r = SQLITE_IOERR; break;
        case AZURE_ERR_NOMEM:     r = SQLITE_NOMEM; break;
        default:                  r = ioerr_variant ? ioerr_variant : SQLITE_IOERR; break;
    }
    return r;
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
static int sqliteObjsClose(sqlite3_file *pFile) {
    sqliteObjsFile *p = (sqliteObjsFile *)pFile;
    int rc = SQLITE_OK;

    /* Flush any remaining dirty data before closing */
    if (p->nDirtyPages > 0 && p->ops && p->ops->page_blob_write) {
        rc = sqliteObjsSync(pFile, 0);
    } else if (p->eFileType == SQLITE_OPEN_WAL &&
               p->nWalData > p->nWalSynced && p->ops) {
        rc = sqliteObjsSync(pFile, 0);
    }

    /* Release lease if held.  Best-effort: lease auto-expires so we log
    ** but don't fail xClose on release errors. */
    if (hasLease(p) && p->ops && p->ops->lease_release) {
        azure_error_t aerr;
        azure_error_init(&aerr);
        azure_err_t larc = p->ops->lease_release(
            p->ops_ctx, p->zBlobName, p->leaseId, &aerr);
        if (larc != AZURE_OK) {
            fprintf(stderr, "sqliteObjs: lease_release failed (code=%d, blob=%s): %s\n",
                    larc, p->zBlobName ? p->zBlobName : "(null)",
                    aerr.error_message);
        }
        p->leaseId[0] = '\0';
    }

    /* Close and delete cache file for MAIN_DB */
    if (p->cacheFd >= 0) {
        close(p->cacheFd);
        p->cacheFd = -1;
    }
    if (p->zCachePath) {
        unlink(p->zCachePath);
        free(p->zCachePath);
        p->zCachePath = NULL;
    }
    
    free(p->aDirty);
    p->aDirty = NULL;
    free(p->aJrnlData);
    p->aJrnlData = NULL;
    free(p->aWalData);
    p->aWalData = NULL;
    sqlite3_free(p->zBlobName);
    p->zBlobName = NULL;

    /* Destroy per-file Azure client if we own one */
    if (p->ownClient) {
        azure_client_destroy(p->ownClient);
        p->ownClient = NULL;
    }

    p->pMethod = NULL;
    return rc;
}

/*
** xRead — Read from cache file or in-memory buffer.
** For MAIN_DB: read from cache file via pread.
** For MAIN_JOURNAL: read from aJrnlData.
** Must zero-fill on short read (SQLITE_IOERR_SHORT_READ).
*/
static int sqliteObjsRead(sqlite3_file *pFile, void *pBuf, int iAmt,
                        sqlite3_int64 iOfst) {
    sqliteObjsFile *p = (sqliteObjsFile *)pFile;
    
    if (p->eFileType == SQLITE_OPEN_WAL) {
        unsigned char *src = p->aWalData;
        sqlite3_int64 srcLen = p->nWalData;
        if (!src) {
            memset(pBuf, 0, iAmt);
            return SQLITE_IOERR_SHORT_READ;
        }
        if (iOfst >= srcLen) {
            memset(pBuf, 0, iAmt);
            return SQLITE_IOERR_SHORT_READ;
        }
        sqlite3_int64 avail = srcLen - iOfst;
        if (avail >= iAmt) {
            memcpy(pBuf, src + iOfst, iAmt);
            return SQLITE_OK;
        }
        memcpy(pBuf, src + iOfst, (size_t)avail);
        memset((unsigned char *)pBuf + avail, 0, iAmt - (size_t)avail);
        return SQLITE_IOERR_SHORT_READ;
    }
    
    if (p->eFileType & SQLITE_OPEN_MAIN_JOURNAL) {
        unsigned char *src = p->aJrnlData;
        sqlite3_int64 srcLen = p->nJrnlData;
        g_xread_journal_count++;
        if (!src) {
            memset(pBuf, 0, iAmt);
            return SQLITE_IOERR_SHORT_READ;
        }
        if (iOfst >= srcLen) {
            memset(pBuf, 0, iAmt);
            return SQLITE_IOERR_SHORT_READ;
        }
        sqlite3_int64 avail = srcLen - iOfst;
        if (avail >= iAmt) {
            memcpy(pBuf, src + iOfst, iAmt);
            return SQLITE_OK;
        }
        memcpy(pBuf, src + iOfst, (size_t)avail);
        memset((unsigned char *)pBuf + avail, 0, iAmt - (size_t)avail);
        return SQLITE_IOERR_SHORT_READ;
    }
    
    /* MAIN_DB — read from cache file */
    g_xread_count++;
    
    if (p->cacheFd < 0) {
        memset(pBuf, 0, iAmt);
        return SQLITE_IOERR_SHORT_READ;
    }
    
    if (iOfst >= p->nData) {
        /* Reading entirely past end — zero fill */
        memset(pBuf, 0, iAmt);
        return SQLITE_IOERR_SHORT_READ;
    }
    
    sqlite3_int64 avail = p->nData - iOfst;
    int toRead = (avail >= iAmt) ? iAmt : (int)avail;
    
    if (toRead > 0) {
        ssize_t nRead = pread(p->cacheFd, pBuf, toRead, (off_t)iOfst);
        if (nRead < 0) {
            memset(pBuf, 0, iAmt);
            return SQLITE_IOERR_READ;
        }
        if (nRead < toRead) {
            /* Short read from pread — zero rest */
            memset((unsigned char *)pBuf + nRead, 0, toRead - nRead);
        }
        if (toRead < iAmt) {
            /* Past EOF — zero remaining */
            memset((unsigned char *)pBuf + toRead, 0, iAmt - toRead);
            return SQLITE_IOERR_SHORT_READ;
        }
        if (nRead < iAmt) {
            return SQLITE_IOERR_SHORT_READ;
        }
        return SQLITE_OK;
    }
    
    memset(pBuf, 0, iAmt);
    return SQLITE_IOERR_SHORT_READ;
}

/*
** xWrite — Write to cache file or in-memory buffer.
** For MAIN_DB: write to cache file via pwrite, mark dirty bitmap.
** For MAIN_JOURNAL: append/write to aJrnlData.
*/
static int sqliteObjsWrite(sqlite3_file *pFile, const void *pBuf, int iAmt,
                         sqlite3_int64 iOfst) {
    sqliteObjsFile *p = (sqliteObjsFile *)pFile;
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

    /* MAIN_DB — write to cache file + mark dirty */
    if (p->cacheFd < 0) {
        return SQLITE_IOERR_WRITE;
    }
    
    sqlite3_int64 end = iOfst + iAmt;
    sqlite3_int64 prevNData = p->nData;
    
    /* Update nData BEFORE cacheEnsureSize so dirtyEnsureCapacity sizes the
    ** bitmap for the new file size, not the old one. Without this, dirty
    ** marks for pages beyond the old EOF are silently dropped. */
    if (end > p->nData) p->nData = end;
    
    /* Extend cache file if needed */
    if (end > prevNData) {
        rc = cacheEnsureSize(p, end);
        if (rc != SQLITE_OK) {
            p->nData = prevNData;
            return rc;
        }
    }
    
    /* Write to cache file */
    ssize_t nWritten = pwrite(p->cacheFd, pBuf, iAmt, (off_t)iOfst);
    if (nWritten != iAmt) {
        p->nData = prevNData;
        return SQLITE_IOERR_WRITE;
    }

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
static int sqliteObjsTruncate(sqlite3_file *pFile, sqlite3_int64 size) {
    sqliteObjsFile *p = (sqliteObjsFile *)pFile;

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

    /* Truncate the cache file */
    if (p->cacheFd >= 0) {
        if (ftruncate(p->cacheFd, (off_t)size) != 0) {
            return SQLITE_IOERR_TRUNCATE;
        }
    }
    
    if (size < p->nData) {
        p->nData = size;
    }
    return SQLITE_OK;
}

/* Maximum bytes per Azure Put Page request (4 MiB) */
#define SQLITE_OBJS_MAX_PUT_PAGE  (4 * 1024 * 1024)

/*
** coalesceDirtyRanges — Scan the dirty bitmap and merge contiguous dirty
** pages into azure_page_range_t entries.  Each range is capped at 4 MiB
** and the final range is 512-byte aligned upward.
**
** Returns the number of ranges written to `ranges`, or -1 if maxRanges
** is too small to hold all the coalesced ranges.
**
** NOTE: range.data is NOT set here — caller must read from cache file.
*/
static int coalesceDirtyRanges(
    sqliteObjsFile *p,
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
            if (runBytes >= SQLITE_OBJS_MAX_PUT_PAGE) break;
            i++;
        }

        /* If we stopped because of the 4 MiB cap, advance past this page */
        if (i < nPages && dirtyIsPageDirty(p, i)
            && (int64_t)runPages * p->pageSize >= SQLITE_OBJS_MAX_PUT_PAGE) {
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
        ranges[nRanges].data = NULL;  /* Caller must fill from cache file */
        ranges[nRanges].len = len;
        nRanges++;
    }

    return nRanges;
}

/* Stack-allocated range threshold (avoid heap for small flushes) */
#define SQLITE_OBJS_STACK_RANGES 64

/*
** xSync — Flush dirty pages to Azure.
** For MAIN_DB: coalesce dirty pages, then write via batch or sequential fallback.
** For MAIN_JOURNAL: upload entire journal via block_blob_upload.
** For WAL: append new data to Azure append blob.
*/
static int sqliteObjsSync(sqlite3_file *pFile, int flags) {
    sqliteObjsFile *p = (sqliteObjsFile *)pFile;
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
            if (sqlite_objs_debug_timing()) t0 = sqlite_objs_time_ms();

            azure_error_init(&aerr);
            azure_err_t arc = p->ops->block_blob_upload(
                p->ops_ctx, p->zBlobName,
                p->aJrnlData, (size_t)p->nJrnlData, &aerr);

            if (sqlite_objs_debug_timing()) {
                double elapsed = sqlite_objs_time_ms() - t0;
                fprintf(stderr, "[TIMING] xSync(journal): %.1fms (%lld bytes, blob=%s)\n",
                        elapsed, (long long)p->nJrnlData, p->zBlobName);
            }

            if (arc != AZURE_OK) {
                return SQLITE_IOERR_FSYNC;
            }

            /* R1: Journal blob now exists in Azure */
            {
                sqliteObjsJournalCacheEntry *jce =
                    journalCacheGetOrCreate(p->pVfsData, p->zBlobName);
                if (jce) jce->state = 1;
            }
        }
        return SQLITE_OK;
    }

    /* MAIN_DB — flush dirty pages */
    if (p->nDirtyPages == 0) return SQLITE_OK;
    if (!p->ops || !p->ops->page_blob_write) return SQLITE_IOERR_FSYNC;

    double sync_t0 = 0;
    if (sqlite_objs_debug_timing()) sync_t0 = sqlite_objs_time_ms();

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
        if (sqlite_objs_debug_timing()) rt0 = sqlite_objs_time_ms();

        azure_error_init(&aerr);
        azure_err_t arc = p->ops->page_blob_resize(p->ops_ctx, p->zBlobName,
                                                     alignedSize,
                                                     hasLease(p) ? p->leaseId : NULL,
                                                     &aerr);
        if (sqlite_objs_debug_timing()) resize_ms = sqlite_objs_time_ms() - rt0;

        if (arc != AZURE_OK) {
            return SQLITE_IOERR_FSYNC;
        }
        p->lastSyncedSize = p->nData;
    }

    /* Coalesce dirty pages into contiguous ranges */
    double coalesce_t0 = 0;
    if (sqlite_objs_debug_timing()) coalesce_t0 = sqlite_objs_time_ms();

    azure_page_range_t stackRanges[SQLITE_OBJS_STACK_RANGES];
    azure_page_range_t *ranges = stackRanges;
    int maxRanges = SQLITE_OBJS_STACK_RANGES;

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
    if (sqlite_objs_debug_timing()) coalesce_ms = sqlite_objs_time_ms() - coalesce_t0;

    /* Allocate buffer for dirty range data and read from cache file */
    unsigned char *rangeDataBuf = NULL;
    size_t totalRangeBytes = 0;
    for (int i = 0; i < nRanges; i++) {
        totalRangeBytes += ranges[i].len;
    }
    
    if (totalRangeBytes > 0) {
        /* Ensure cache file covers 512-byte aligned range ends.
        ** Ranges are padded to 512-byte boundaries for Azure, but the cache
        ** file may be shorter (nData may not be 512-aligned). Extend with
        ** zeros so pread returns full data. */
        if (nRanges > 0 && p->cacheFd >= 0) {
            sqlite3_int64 maxEnd = ranges[nRanges-1].offset + ranges[nRanges-1].len;
            off_t curSize = lseek(p->cacheFd, 0, SEEK_END);
            if (curSize >= 0 && maxEnd > curSize) {
                ftruncate(p->cacheFd, (off_t)maxEnd);
            }
        }

        rangeDataBuf = (unsigned char *)sqlite3_malloc64(totalRangeBytes);
        if (!rangeDataBuf) {
            if (ranges != stackRanges) sqlite3_free(ranges);
            return SQLITE_NOMEM;
        }
        
        unsigned char *bufPos = rangeDataBuf;
        for (int i = 0; i < nRanges; i++) {
            ssize_t nRead = pread(p->cacheFd, bufPos, ranges[i].len, (off_t)ranges[i].offset);
            if (nRead < 0) {
                sqlite3_free(rangeDataBuf);
                if (ranges != stackRanges) sqlite3_free(ranges);
                return SQLITE_IOERR_FSYNC;
            }
            if ((size_t)nRead < ranges[i].len) {
                /* Zero-fill tail — alignment padding beyond actual data */
                memset(bufPos + nRead, 0, ranges[i].len - (size_t)nRead);
            }
            ranges[i].data = bufPos;
            bufPos += ranges[i].len;
        }
    }

    /* Try batch write if available (Phase 2 — will be non-NULL with curl_multi) */
    if (p->ops->page_blob_write_batch) {
        double wt0 = 0;
        if (sqlite_objs_debug_timing()) wt0 = sqlite_objs_time_ms();

        azure_error_init(&aerr);
        azure_err_t arc = p->ops->page_blob_write_batch(
            p->ops_ctx, p->zBlobName,
            ranges, nRanges,
            hasLease(p) ? p->leaseId : NULL, &aerr);

        if (sqlite_objs_debug_timing()) {
            double write_ms = sqlite_objs_time_ms() - wt0;
            double total_ms = sqlite_objs_time_ms() - sync_t0;
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

        sqlite3_free(rangeDataBuf);
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
    if (sqlite_objs_debug_timing()) write_t0 = sqlite_objs_time_ms();

    for (int i = 0; i < nRanges; i++) {
        /* Renew lease periodically during large flushes */
        if (hasLease(p) && i > 0 && (i % 50 == 0)) {
            rc = leaseRenewIfNeeded(p);
            if (rc != SQLITE_OK) {
                sqlite3_free(rangeDataBuf);
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
            sqlite3_free(rangeDataBuf);
            if (ranges != stackRanges) sqlite3_free(ranges);
            return SQLITE_IOERR_FSYNC;
        }
    } {
        double write_ms = sqlite_objs_time_ms() - write_t0;
        double total_ms = sqlite_objs_time_ms() - sync_t0;
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

    sqlite3_free(rangeDataBuf);
    if (ranges != stackRanges) sqlite3_free(ranges);
    dirtyClearAll(p);
    return SQLITE_OK;
}

/*
** xFileSize — Return the current logical file size.
*/
static int sqliteObjsFileSize(sqlite3_file *pFile, sqlite3_int64 *pSize) {
    sqliteObjsFile *p = (sqliteObjsFile *)pFile;
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
static int sqliteObjsLock(sqlite3_file *pFile, int eLock) {
    sqliteObjsFile *p = (sqliteObjsFile *)pFile;

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
        int duration = (p->lastSyncDirtyCount > SQLITE_OBJS_DIRTY_PAGE_THRESHOLD)
                       ? SQLITE_OBJS_LEASE_DURATION_LONG
                       : SQLITE_OBJS_LEASE_DURATION;

        double t0 = 0;
        if (sqlite_objs_debug_timing()) t0 = sqlite_objs_time_ms();

        azure_error_t aerr;
        azure_error_init(&aerr);
        azure_err_t arc = p->ops->lease_acquire(
            p->ops_ctx, p->zBlobName,
            duration,
            p->leaseId, sizeof(p->leaseId), &aerr);

        if (sqlite_objs_debug_timing()) {
            double elapsed = sqlite_objs_time_ms() - t0;
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
static int sqliteObjsUnlock(sqlite3_file *pFile, int eLock) {
    sqliteObjsFile *p = (sqliteObjsFile *)pFile;

    if (p->eLock <= eLock) return SQLITE_OK;

    /* Release the lease when dropping below RESERVED */
    if (eLock <= SQLITE_LOCK_SHARED && hasLease(p)) {
        if (p->ops && p->ops->lease_release) {
            double t0 = 0;
            if (sqlite_objs_debug_timing()) t0 = sqlite_objs_time_ms();

            azure_error_t aerr;
            azure_error_init(&aerr);
            p->ops->lease_release(p->ops_ctx, p->zBlobName,
                                   p->leaseId, &aerr);

            if (sqlite_objs_debug_timing()) {
                double elapsed = sqlite_objs_time_ms() - t0;
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
static int sqliteObjsCheckReservedLock(sqlite3_file *pFile, int *pResOut) {
    sqliteObjsFile *p = (sqliteObjsFile *)pFile;

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
static int sqliteObjsFileControl(sqlite3_file *pFile, int op, void *pArg) {

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
                    sqliteObjsFile *p = (sqliteObjsFile *)pFile;
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
            *(char **)pArg = sqlite3_mprintf("sqlite-objs");
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
static int sqliteObjsSectorSize(sqlite3_file *pFile) {
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
static int sqliteObjsDeviceCharacteristics(sqlite3_file *pFile) {
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

static int sqliteObjsShmMap(sqlite3_file *pFile, int iRegion, int szRegion,
                          int bExtend, void volatile **pp) {
    (void)pFile; (void)iRegion; (void)szRegion; (void)bExtend;
    *pp = NULL;
    /* Never called in exclusive mode. Return SQLITE_IOERR to fail safely
    ** if the user forgot PRAGMA locking_mode=EXCLUSIVE. */
    fprintf(stderr,
        "sqliteObjs: ERROR — xShmMap called, but sqliteObjs WAL requires "
        "PRAGMA locking_mode=EXCLUSIVE. Shared-memory WAL is not supported.\n");
    return SQLITE_IOERR;
}

static int sqliteObjsShmLock(sqlite3_file *pFile, int offset, int n, int flags) {
    (void)pFile; (void)offset; (void)n; (void)flags;
    return SQLITE_OK;
}

static void sqliteObjsShmBarrier(sqlite3_file *pFile) {
    (void)pFile;
}

static int sqliteObjsShmUnmap(sqlite3_file *pFile, int deleteFlag) {
    (void)pFile; (void)deleteFlag;
    return SQLITE_OK;
}


/* ===================================================================
** sqlite3_vfs method implementations
** =================================================================== */

/*
** Parse Azure URI parameters from a sqlite3_filename.
** Returns 1 if azure_account is present (config populated), 0 otherwise.
*/
static int sqlite_objs_parse_uri_config(sqlite3_filename zName,
                                    azure_client_config_t *cfg) {
    const char *account = sqlite3_uri_parameter(zName, "azure_account");
    if (!account) return 0;
    memset(cfg, 0, sizeof(*cfg));
    cfg->account   = account;
    cfg->container = sqlite3_uri_parameter(zName, "azure_container");
    cfg->sas_token = sqlite3_uri_parameter(zName, "azure_sas");
    cfg->account_key = sqlite3_uri_parameter(zName, "azure_key");
    cfg->endpoint  = sqlite3_uri_parameter(zName, "azure_endpoint");
    return 1;
}

/*
** xOpen — Open a file.
** Routes by file type:
**   MAIN_DB       → Azure page blob (download full blob into aData)
**   MAIN_JOURNAL  → Azure block blob
**   Everything else → delegate to default VFS
*/
static int sqliteObjsOpen(sqlite3_vfs *pVfs, sqlite3_filename zName,
                        sqlite3_file *pFile, int flags, int *pOutFlags) {
    sqliteObjsVfsData *pVfsData = (sqliteObjsVfsData *)pVfs->pAppData;
    sqliteObjsFile *p = (sqliteObjsFile *)pFile;
    int isMainDb = (flags & SQLITE_OPEN_MAIN_DB) != 0;
    int isMainJournal = (flags & SQLITE_OPEN_MAIN_JOURNAL) != 0;
    int isWal = (flags & SQLITE_OPEN_WAL) != 0;

    /* Initialize pMethod to NULL — prevents xClose call on failure */
    memset(p, 0, sizeof(sqliteObjsFile));

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

    /* Try URI parameters for per-file Azure client */
    p->ownClient = NULL;
    azure_client_config_t uriCfg;
    if (sqlite_objs_parse_uri_config(zName, &uriCfg)) {
        azure_error_t aerr;
        azure_error_init(&aerr);
        azure_err_t arc = azure_client_create(&uriCfg, &p->ownClient, &aerr);
        if (arc != AZURE_OK) {
            sqlite3_free(p->zBlobName);
            p->zBlobName = NULL;
            p->ownClient = NULL;
            return SQLITE_CANTOPEN;
        }
        p->ops = (azure_ops_t *)azure_client_get_ops();
        p->ops_ctx = azure_client_get_ctx(p->ownClient);
    } else {
        /* Fall back to global VFS client */
        p->ops = pVfsData->ops;
        p->ops_ctx = pVfsData->ops_ctx;
    }

    /* Guard: if both per-file and global ops are NULL, we can't proceed */
    if (!p->ops) {
        sqlite3_free(p->zBlobName);
        p->zBlobName = NULL;
        return SQLITE_CANTOPEN;
    }
    p->pVfsData = pVfsData;             /* R1: back-pointer for cache access */
    if (isWal) p->eFileType = SQLITE_OPEN_WAL;
    else if (isMainJournal) p->eFileType = SQLITE_OPEN_MAIN_JOURNAL;
    else p->eFileType = SQLITE_OPEN_MAIN_DB;
    p->eLock = SQLITE_LOCK_NONE;
    p->leaseId[0] = '\0';
    p->leaseDuration = SQLITE_OBJS_LEASE_DURATION;
    p->lastSyncDirtyCount = 0;
    p->etag[0] = '\0';
    p->pageSize = SQLITE_OBJS_DEFAULT_PAGE_SIZE;
    p->cacheFd = -1;  /* Initialize cache fd */
    p->zCachePath = NULL;

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

            /* Create cache file */
            char cachePathTemplate[] = "/tmp/sqlite-objs-XXXXXX";
            p->cacheFd = mkstemp(cachePathTemplate);
            if (p->cacheFd < 0) {
                sqlite3_free(p->zBlobName);
                p->zBlobName = NULL;
                return SQLITE_CANTOPEN;
            }
            p->zCachePath = strdup(cachePathTemplate);
            if (!p->zCachePath) {
                close(p->cacheFd);
                unlink(cachePathTemplate);
                p->cacheFd = -1;
                sqlite3_free(p->zBlobName);
                p->zBlobName = NULL;
                return SQLITE_NOMEM;
            }

            if (blobSize > 0) {
                /* Download to temporary buffer, then write to cache file */
                unsigned char *tempBuf = (unsigned char *)malloc(blobSize);
                if (!tempBuf) {
                    close(p->cacheFd);
                    unlink(p->zCachePath);
                    free(p->zCachePath);
                    p->cacheFd = -1;
                    p->zCachePath = NULL;
                    sqlite3_free(p->zBlobName);
                    p->zBlobName = NULL;
                    return SQLITE_NOMEM;
                }

                azure_error_init(&aerr);

                /* Use parallel chunked download when available */
                if (p->ops->page_blob_read_multi) {
                    arc = p->ops->page_blob_read_multi(
                        p->ops_ctx, zName, blobSize,
                        tempBuf, &aerr);
                } else {
                    /* Fallback: single-stream download */
                    azure_buffer_t buf = {0};
                    arc = p->ops->page_blob_read(p->ops_ctx, zName,
                                                  0, (size_t)blobSize,
                                                  &buf, &aerr);
                    if (arc == AZURE_OK) {
                        if (!buf.data || buf.size == 0) {
                            /* Unexpected: blob properties said size>0 but read returned nothing */
                            arc = AZURE_ERR_INVALID_ARG;
                        } else {
                            sqlite3_int64 copyLen = (sqlite3_int64)buf.size;
                            if (copyLen > blobSize) copyLen = blobSize;
                            memcpy(tempBuf, buf.data, (size_t)copyLen);
                        }
                    }
                    free(buf.data);
                }

                if (arc != AZURE_OK) {
                    free(tempBuf);
                    close(p->cacheFd);
                    unlink(p->zCachePath);
                    free(p->zCachePath);
                    p->cacheFd = -1;
                    p->zCachePath = NULL;
                    free(p->aDirty);
                    p->aDirty = NULL;
                    sqlite3_free(p->zBlobName);
                    p->zBlobName = NULL;
                    return azureErrToSqlite(arc, SQLITE_CANTOPEN);
                }

                /* Write downloaded data to cache file */
                ssize_t totalWritten = 0;
                while (totalWritten < blobSize) {
                    ssize_t nWritten = write(p->cacheFd, tempBuf + totalWritten, 
                                            blobSize - totalWritten);
                    if (nWritten <= 0) {
                        free(tempBuf);
                        close(p->cacheFd);
                        unlink(p->zCachePath);
                        free(p->zCachePath);
                        p->cacheFd = -1;
                        p->zCachePath = NULL;
                        free(p->aDirty);
                        p->aDirty = NULL;
                        sqlite3_free(p->zBlobName);
                        p->zBlobName = NULL;
                        return SQLITE_IOERR;
                    }
                    totalWritten += nWritten;
                }
                
                /* Ensure data is written to disk */
                if (fsync(p->cacheFd) != 0) {
                    free(tempBuf);
                    close(p->cacheFd);
                    unlink(p->zCachePath);
                    free(p->zCachePath);
                    p->cacheFd = -1;
                    p->zCachePath = NULL;
                    free(p->aDirty);
                    p->aDirty = NULL;
                    sqlite3_free(p->zBlobName);
                    p->zBlobName = NULL;
                    return SQLITE_IOERR;
                }
                
                free(tempBuf);
                
                p->nData = blobSize;

                /* Detect page size from header */
                unsigned char header[100];
                ssize_t nRead = pread(p->cacheFd, header, sizeof(header), 0);
                if (nRead == sizeof(header)) {
                    int detected = detectPageSize(header, sizeof(header));
                    if (detected > 0) p->pageSize = detected;
                }

                /* Re-ensure dirty bitmap with correct page size */
                dirtyEnsureCapacity(p);

                /* R2: Record initial Azure blob size to skip redundant resizes */
                p->lastSyncedSize = p->nData;
            } else {
                /* Empty blob */
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
            
            /* Create cache file */
            char cachePathTemplate[] = "/tmp/sqlite-objs-XXXXXX";
            p->cacheFd = mkstemp(cachePathTemplate);
            if (p->cacheFd < 0) {
                sqlite3_free(p->zBlobName);
                p->zBlobName = NULL;
                return SQLITE_CANTOPEN;
            }
            p->zCachePath = strdup(cachePathTemplate);
            if (!p->zCachePath) {
                close(p->cacheFd);
                unlink(cachePathTemplate);
                p->cacheFd = -1;
                sqlite3_free(p->zBlobName);
                p->zBlobName = NULL;
                return SQLITE_NOMEM;
            }
            
            p->nData = 0;
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
        sqliteObjsJournalCacheEntry *jce =
            journalCacheGetOrCreate(pVfsData, zName);

        /* R1: If cache says journal doesn't exist, skip the HEAD request.
        ** Only do a real HEAD when cache is unknown (-1) or says it exists (1).
        ** Since we are the single writer, cache=0 is authoritative. */
        int exists = 0;
        if (jce && jce->state == 0) {
            exists = 0;  /* We deleted it — no HEAD needed */
        } else if (p->ops && p->ops->blob_exists) {
            azure_error_t aerr;
            azure_error_init(&aerr);
            p->ops->blob_exists(p->ops_ctx, zName, &exists, &aerr);
            if (jce) jce->state = exists ? 1 : 0;
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
    p->pMethod = &sqliteObjsIoMethods;
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
static int sqliteObjsIsAzurePath(const char *zPath) {
    if (!zPath || zPath[0] == '\0') return 0;
    /* Absolute filesystem paths go to the default VFS */
    if (zPath[0] == '/') return 0;
    /* Relative paths are Azure blob names */
    return 1;
}

/*
** xDelete — Delete a blob from Azure (or delegate for temp files).
*/
static int sqliteObjsDelete(sqlite3_vfs *pVfs, const char *zName, int syncDir) {
    sqliteObjsVfsData *pVfsData = (sqliteObjsVfsData *)pVfs->pAppData;

    if (!zName) return SQLITE_OK;

    /* Delegate non-Azure paths (absolute filesystem paths) to default VFS */
    if (!sqliteObjsIsAzurePath(zName)) {
        return pVfsData->pDefaultVfs->xDelete(pVfsData->pDefaultVfs, zName, syncDir);
    }

    const azure_ops_t *ops;
    void *ops_ctx;
    if (resolveOps(pVfsData, zName, &ops, &ops_ctx) && ops->blob_delete) {
        azure_error_t aerr;
        azure_error_init(&aerr);
        azure_err_t arc = ops->blob_delete(ops_ctx, zName, &aerr);
        if (arc == AZURE_ERR_NOT_FOUND) {
            /* R1: We know it doesn't exist now */
            sqliteObjsJournalCacheEntry *jce = journalCacheFind(pVfsData, zName);
            if (jce) jce->state = 0;
            return SQLITE_OK;  /* Already gone — not an error */
        }
        if (arc != AZURE_OK) {
            return SQLITE_IOERR_DELETE;
        }

        /* R1: After successful delete, update journal cache */
        {
            sqliteObjsJournalCacheEntry *jce = journalCacheFind(pVfsData, zName);
            if (jce) jce->state = 0;
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
static int sqliteObjsAccess(sqlite3_vfs *pVfs, const char *zName,
                          int flags, int *pResOut) {
    sqliteObjsVfsData *pVfsData = (sqliteObjsVfsData *)pVfs->pAppData;

    /* Delegate non-Azure paths (absolute filesystem paths) to default VFS */
    if (!sqliteObjsIsAzurePath(zName)) {
        return pVfsData->pDefaultVfs->xAccess(pVfsData->pDefaultVfs, zName,
                                               flags, pResOut);
    }

    const azure_ops_t *ops;
    void *ops_ctx;
    if (resolveOps(pVfsData, zName, &ops, &ops_ctx) && ops->blob_exists) {
        /* R1: Use cached journal existence when available */
        sqliteObjsJournalCacheEntry *jce = journalCacheFind(pVfsData, zName);
        if (jce && jce->state >= 0) {
            switch (flags) {
                case SQLITE_ACCESS_EXISTS:
                case SQLITE_ACCESS_READWRITE:
                case SQLITE_ACCESS_READ:
                    *pResOut = jce->state;
                    break;
                default:
                    *pResOut = 0;
            }
            return SQLITE_OK;
        }

        int exists = 0;
        azure_error_t aerr;
        azure_error_init(&aerr);
        azure_err_t arc = ops->blob_exists(ops_ctx, zName, &exists, &aerr);
        if (arc != AZURE_OK) {
            *pResOut = 0;
            return SQLITE_OK;  /* Access check should not fail fatally */
        }

        /* R1: Seed journal cache if this is a journal blob we haven't tracked yet.
        ** Detects journal names by "-journal" suffix (per D7). */
        if (zName && !jce) {
            size_t nlen = strlen(zName);
            if (nlen >= 8 && strcmp(zName + nlen - 8, "-journal") == 0) {
                jce = journalCacheGetOrCreate(pVfsData, zName);
                if (jce) jce->state = exists ? 1 : 0;
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
static int sqliteObjsFullPathname(sqlite3_vfs *pVfs, const char *zName,
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

static void *sqliteObjsDlOpen(sqlite3_vfs *pVfs, const char *zFilename) {
    sqliteObjsVfsData *d = (sqliteObjsVfsData *)pVfs->pAppData;
    return d->pDefaultVfs->xDlOpen(d->pDefaultVfs, zFilename);
}

static void sqliteObjsDlError(sqlite3_vfs *pVfs, int nByte, char *zErrMsg) {
    sqliteObjsVfsData *d = (sqliteObjsVfsData *)pVfs->pAppData;
    d->pDefaultVfs->xDlError(d->pDefaultVfs, nByte, zErrMsg);
}

static void (*sqliteObjsDlSym(sqlite3_vfs *pVfs, void *pH,
                             const char *zSym))(void) {
    sqliteObjsVfsData *d = (sqliteObjsVfsData *)pVfs->pAppData;
    return d->pDefaultVfs->xDlSym(d->pDefaultVfs, pH, zSym);
}

static void sqliteObjsDlClose(sqlite3_vfs *pVfs, void *pHandle) {
    sqliteObjsVfsData *d = (sqliteObjsVfsData *)pVfs->pAppData;
    d->pDefaultVfs->xDlClose(d->pDefaultVfs, pHandle);
}

static int sqliteObjsRandomness(sqlite3_vfs *pVfs, int nByte, char *zOut) {
    sqliteObjsVfsData *d = (sqliteObjsVfsData *)pVfs->pAppData;
    return d->pDefaultVfs->xRandomness(d->pDefaultVfs, nByte, zOut);
}

static int sqliteObjsSleep(sqlite3_vfs *pVfs, int microseconds) {
    sqliteObjsVfsData *d = (sqliteObjsVfsData *)pVfs->pAppData;
    return d->pDefaultVfs->xSleep(d->pDefaultVfs, microseconds);
}

static int sqliteObjsCurrentTime(sqlite3_vfs *pVfs, double *pTimeOut) {
    sqliteObjsVfsData *d = (sqliteObjsVfsData *)pVfs->pAppData;
    return d->pDefaultVfs->xCurrentTime(d->pDefaultVfs, pTimeOut);
}

static int sqliteObjsGetLastError(sqlite3_vfs *pVfs, int nByte, char *zErrMsg) {
    sqliteObjsVfsData *d = (sqliteObjsVfsData *)pVfs->pAppData;
    if (nByte > 0 && d->lastError[0]) {
        int len = (int)strlen(d->lastError);
        if (len >= nByte) len = nByte - 1;
        memcpy(zErrMsg, d->lastError, len);
        zErrMsg[len] = '\0';
        return SQLITE_OK;
    }
    return d->pDefaultVfs->xGetLastError(d->pDefaultVfs, nByte, zErrMsg);
}

static int sqliteObjsCurrentTimeInt64(sqlite3_vfs *pVfs, sqlite3_int64 *pTimeOut) {
    sqliteObjsVfsData *d = (sqliteObjsVfsData *)pVfs->pAppData;
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
static sqliteObjsVfsData g_vfsData;

static sqlite3_vfs g_sqliteObjsVfs = {
    2,                          /* iVersion = 2 for xCurrentTimeInt64 */
    0,                          /* szOsFile — set at registration time */
    SQLITE_OBJS_MAX_PATHNAME,       /* mxPathname */
    0,                          /* pNext (managed by SQLite) */
    "sqlite-objs",                  /* zName */
    0,                          /* pAppData — set at registration time */
    sqliteObjsOpen,
    sqliteObjsDelete,
    sqliteObjsAccess,
    sqliteObjsFullPathname,
    sqliteObjsDlOpen,
    sqliteObjsDlError,
    sqliteObjsDlSym,
    sqliteObjsDlClose,
    sqliteObjsRandomness,
    sqliteObjsSleep,
    sqliteObjsCurrentTime,
    sqliteObjsGetLastError,
    sqliteObjsCurrentTimeInt64,
    /* v3 methods — not implemented */
    0, 0, 0
};


/*
** sqlite_objs_vfs_register_with_config — Register with explicit config.
*/
int sqlite_objs_vfs_register_with_config(const sqlite_objs_config_t *config,
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
    int ourSize = (int)sizeof(sqliteObjsFile);
    int defaultSize = pDefault->szOsFile;
    g_sqliteObjsVfs.szOsFile = (ourSize > defaultSize) ? ourSize : defaultSize;
    g_sqliteObjsVfs.pAppData = &g_vfsData;

    return sqlite3_vfs_register(&g_sqliteObjsVfs, makeDefault);
}


/*
** sqlite_objs_vfs_register — Register reading config from environment.
*/
int sqlite_objs_vfs_register(int makeDefault) {
    sqlite_objs_config_t cfg;
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

    return sqlite_objs_vfs_register_with_config(&cfg, makeDefault);
}

/*
** sqlite_objs_vfs_register_with_ops — Convenience for tests.
** Registers the VFS with an explicit ops vtable and context pointer.
*/
int sqlite_objs_vfs_register_with_ops(azure_ops_t *ops, void *ctx,
                                  int makeDefault) {
    sqlite_objs_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.ops = ops;
    cfg.ops_ctx = ctx;
    return sqlite_objs_vfs_register_with_config(&cfg, makeDefault);
}

/*
** sqlite_objs_vfs_register_uri — Register VFS with no global Azure client.
** All databases must provide Azure config via URI parameters, or
** xOpen returns SQLITE_CANTOPEN.
*/
int sqlite_objs_vfs_register_uri(int makeDefault) {
    sqlite3_vfs *pDefault = sqlite3_vfs_find(0);
    if (!pDefault) return SQLITE_ERROR;

    memset(&g_vfsData, 0, sizeof(g_vfsData));
    g_vfsData.pDefaultVfs = pDefault;
    g_vfsData.ops = NULL;
    g_vfsData.ops_ctx = NULL;
    g_vfsData.client = NULL;

    /* szOsFile must accommodate both our struct and the default VFS's struct */
    int ourSize = (int)sizeof(sqliteObjsFile);
    int defaultSize = pDefault->szOsFile;
    g_sqliteObjsVfs.szOsFile = (ourSize > defaultSize) ? ourSize : defaultSize;
    g_sqliteObjsVfs.pAppData = &g_vfsData;

    return sqlite3_vfs_register(&g_sqliteObjsVfs, makeDefault);
}
