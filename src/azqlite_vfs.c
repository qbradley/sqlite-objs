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
#include "azqlite_cache.h"
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

/* ===================================================================
** Demand-paging LRU page cache
** =================================================================== */

#define AZQLITE_DEFAULT_CACHE_PAGES 262144  /* 1 GB at 4K page size */
#define AZQLITE_DEFAULT_PREFETCH_PAGES 1024 /* 4 MB warmup at open */

/* Prefetch strategy codes (negative = named strategy, positive = page count) */
#define AZQLITE_PREFETCH_OFF    (-1)
#define AZQLITE_PREFETCH_ALL    (-2)
#define AZQLITE_PREFETCH_INDEX  (-3)
#define AZQLITE_PREFETCH_WARM   (-4)

/* ===================================================================
** Adaptive readahead state machine
** =================================================================== */

#define RA_INITIAL       0    /* No miss history          */
#define RA_SEQUENTIAL    1    /* Forward-sequential mode  */
#define RA_RANDOM        2    /* Demand-only mode         */

#define RA_INITIAL_WINDOW   4    /* Starting window for new sequences (pages) */
#define RA_MAX_WINDOW    1024    /* Maximum readahead window (pages = 4 MiB at 4K) */

/* Per-file adaptive readahead state */
typedef struct azqlite_readahead {
    int state;           /* RA_INITIAL, RA_SEQUENTIAL, or RA_RANDOM    */
    int lastMissPage;    /* Page number of last cache miss (-1 = none) */
    int window;          /* Current readahead window (pages)           */

    /* Observability counters */
    int nMisses;             /* Total cache misses */
    int nSequentialMisses;   /* Misses classified as sequential */
    int nRandomMisses;       /* Misses classified as random */
    int nWindowGrows;        /* Times the window doubled */
    int nWindowResets;       /* Times the window reset to 1 */
    int peakWindow;          /* High-water mark of window size */
} azqlite_readahead_t;

/* Readahead statistics — returned via FCNTL */
typedef struct azqlite_readahead_stats {
    int currentState;     /* RA_INITIAL, RA_SEQUENTIAL, RA_RANDOM */
    int currentWindow;    /* Current readahead window (pages) */
    int lastMissPage;     /* Last miss page number */
    int totalMisses;      /* Total cache misses since xOpen */
    int sequentialMisses; /* Misses classified as sequential */
    int randomMisses;     /* Misses classified as random */
    int windowGrows;      /* Times window doubled */
    int windowResets;     /* Times window reset to 1 */
    int peakWindow;       /* High-water mark */
} azqlite_readahead_stats_t;

#define AZQLITE_FCNTL_READAHEAD_MODE    1000
#define AZQLITE_FCNTL_READAHEAD_STATS   1001
#define AZQLITE_FCNTL_PREFETCH_STATUS   1002
#define AZQLITE_FCNTL_PREFETCH_WAIT     1003

/* Prefetch status — returned via FCNTL */
typedef struct azqlite_prefetch_status {
    int running;      /* 1 if prefetch thread is active */
    int progress;     /* Pages fetched so far */
    int total;        /* Total pages to fetch */
} azqlite_prefetch_status_t;

/*
** Determine readahead window for a cache miss at page P.
** Updates state in-place. Returns the number of pages to readahead.
*/
static int readaheadOnMiss(azqlite_readahead_t *ra, int P,
                           int maxWindow, int maxPages) {
    int L = ra->lastMissPage;
    int W = ra->window;
    int result;

    ra->nMisses++;

    /* Cap readahead to cache capacity */
    if (maxWindow > maxPages) maxWindow = maxPages;

    if (ra->state == RA_INITIAL) {
        /* First miss — begin with small probe window */
        result = RA_INITIAL_WINDOW;
        if (result > maxWindow) result = maxWindow;
        ra->state = RA_SEQUENTIAL;
        ra->lastMissPage = P;
        ra->window = result;
        ra->nSequentialMisses++;
        if (result > ra->peakWindow) ra->peakWindow = result;
        if (azqlite_debug_timing()) {
            fprintf(stderr, "[READAHEAD] miss page=%d state=INITIAL->SEQUENTIAL "
                    "window=0->%d (first miss)\n", P, result);
        }
        return result;
    }

    int distance = P - L;

    /* Case 1: Inside the last readahead window (eviction pressure) */
    if (distance > 0 && distance < W) {
        ra->lastMissPage = P;
        ra->nSequentialMisses++;
        result = W;
        if (result > maxWindow) result = maxWindow;
        if (azqlite_debug_timing()) {
            fprintf(stderr, "[READAHEAD] miss page=%d state=SEQUENTIAL "
                    "window=%d (inside-window, eviction pressure)\n", P, W);
        }
        return result;
    }

    /* Case 2: Sequential continuation — at or just past the window boundary */
    int tolerance = W / 4;
    if (tolerance < 4) tolerance = 4;

    if (distance >= W && distance <= W + tolerance) {
        int newW = W * 2;
        if (newW > maxWindow) newW = maxWindow;
        if (newW < RA_INITIAL_WINDOW) newW = RA_INITIAL_WINDOW;
        ra->state = RA_SEQUENTIAL;
        ra->lastMissPage = P;
        ra->window = newW;
        ra->nSequentialMisses++;
        if (newW > W) ra->nWindowGrows++;
        if (newW > ra->peakWindow) ra->peakWindow = newW;
        if (azqlite_debug_timing()) {
            fprintf(stderr, "[READAHEAD] miss page=%d state=SEQUENTIAL "
                    "window=%d->%d (sequential continuation)\n", P, W, newW);
        }
        return newW;
    }

    /* Case 3: Adjacent forward miss from RANDOM — new sequence detected */
    if (distance == 1 && ra->state == RA_RANDOM) {
        result = RA_INITIAL_WINDOW;
        if (result > maxWindow) result = maxWindow;
        ra->state = RA_SEQUENTIAL;
        ra->lastMissPage = P;
        ra->window = result;
        ra->nSequentialMisses++;
        ra->nWindowGrows++;
        if (result > ra->peakWindow) ra->peakWindow = result;
        if (azqlite_debug_timing()) {
            fprintf(stderr, "[READAHEAD] miss page=%d state=RANDOM->SEQUENTIAL "
                    "window=1->%d (new sequence detected)\n", P, result);
        }
        return result;
    }

    /* Case 4: Non-sequential — random access */
    ra->state = RA_RANDOM;
    ra->lastMissPage = P;
    ra->window = 1;
    ra->nRandomMisses++;
    ra->nWindowResets++;
    if (azqlite_debug_timing()) {
        fprintf(stderr, "[READAHEAD] miss page=%d state=RANDOM "
                "window=%d->1 (non-sequential jump from %d)\n", P, W, L);
    }
    return 1;
}

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

    /* Demand-paging mmap cache for MAIN_DB */
    azqlite_cache_t *diskCache;             /* mmap-backed page cache (NULL if cache=off) */
    int pageSize;                           /* SQLite page size */
    sqlite3_int64 blobSize;                 /* Current logical blob size */
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

    /* Per-file Azure client (NULL = using global VFS client) */
    azure_client_t *ownClient;

    /* Adaptive readahead state (per-file, per-connection) */
    azqlite_readahead_t readahead;

    /* Readahead configuration (parsed from URI at xOpen) */
    int readaheadMode;       /* 0=auto (adaptive), >0=fixed N pages */
    int readaheadMaxWindow;  /* Max window for adaptive mode */
    int cachePagesConfig;    /* Configured max cache pages */
    int prefetchPages;       /* Pages to fetch at open (warmup) */

    /* Background prefetch thread */
    pthread_t prefetchThread;        /* Thread handle */
    int prefetchRunning;             /* 1 = thread is alive */
    int prefetchCancel;              /* 1 = request cancellation */
    int prefetchProgress;            /* Pages fetched so far */
    int prefetchTotal;               /* Total pages to fetch */
    pthread_mutex_t prefetchMutex;   /* Protects progress/cancel */
} azqliteFile;

/*
** Per-journal blob existence cache entry.
** Each open database has its own journal blob with independent state.
**   -1 = unknown (do real HEAD), 0 = does not exist, 1 = exists
*/
#define AZQLITE_MAX_JOURNAL_CACHE 16

typedef struct azqliteJournalCacheEntry {
    int state;
    char zBlobName[512];
} azqliteJournalCacheEntry;

/*
** Global VFS state — stored in sqlite3_vfs.pAppData.
*/
struct azqliteVfsData {
    sqlite3_vfs *pDefaultVfs;           /* The platform default VFS */
    azure_ops_t *ops;                   /* Azure operations vtable */
    void *ops_ctx;                      /* Context for ops */
    azure_client_t *client;             /* Production client (may be NULL for tests) */
    char lastError[256];                /* Last error message for xGetLastError */

    /* R1: Per-journal blob existence cache (one entry per database).
    ** Since we are the single writer, we know when a journal blob exists
    ** because we created (xSync) or deleted (xDelete) it ourselves.
    ** This eliminates ~4 HEAD requests per transaction (~110ms saved). */
    azqliteJournalCacheEntry journalCache[AZQLITE_MAX_JOURNAL_CACHE];
    int nJournalCache;
};

/* ---------- io_methods v2 ---------- */

/* ===================================================================
** Helper: per-journal blob cache lookup
** =================================================================== */

/* Find a journal cache entry by blob name. Returns pointer or NULL. */
static azqliteJournalCacheEntry *journalCacheFind(azqliteVfsData *pData,
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
static azqliteJournalCacheEntry *journalCacheGetOrCreate(azqliteVfsData *pData,
                                                          const char *zName) {
    azqliteJournalCacheEntry *e = journalCacheFind(pData, zName);
    if (e) return e;
    if (pData->nJournalCache >= AZQLITE_MAX_JOURNAL_CACHE) return NULL;
    size_t n = strlen(zName);
    if (n >= sizeof(pData->journalCache[0].zBlobName)) return NULL;
    e = &pData->journalCache[pData->nJournalCache++];
    e->state = -1;
    memcpy(e->zBlobName, zName, n + 1);
    return e;
}

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
** Resolve Azure storage ops for VFS-level methods (xDelete, xAccess).
**
** These methods only have the VFS pointer (not a file pointer), so they
** normally use the global ops in pVfsData. In URI-only mode the global ops
** are NULL. For journal/WAL filenames we can recover the per-file ops from
** the main database file via sqlite3_database_file_object().
*/
static int resolveOps(azqliteVfsData *pVfsData, const char *zName,
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
                azqliteFile *pMain = (azqliteFile *)pDbFile;
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
** Background prefetch thread
** =================================================================== */

#define PREFETCH_BATCH_PAGES 64

/*
** Cancel and join the background prefetch thread if it is running.
** Must be called before any Azure I/O from the main thread to prevent
** concurrent use of the shared curl_multi handle (not thread-safe).
*/
static void cancelPrefetchThread(azqliteFile *p) {
    if (!p->prefetchRunning) return;
    pthread_mutex_lock(&p->prefetchMutex);
    p->prefetchCancel = 1;
    pthread_mutex_unlock(&p->prefetchMutex);
    pthread_join(p->prefetchThread, NULL);
    p->prefetchRunning = 0;
}

static void *prefetchThreadFunc(void *arg) {
    azqliteFile *p = (azqliteFile *)arg;
    int pageSize = p->pageSize;
    int totalPages = (int)(p->blobSize / pageSize);
    if (p->blobSize % pageSize != 0) totalPages++;

    pthread_mutex_lock(&p->prefetchMutex);
    p->prefetchTotal = totalPages;
    pthread_mutex_unlock(&p->prefetchMutex);

    int fetched = 0;
    int pageNo = 0;

    while (pageNo < totalPages) {
        /* Check cancellation */
        pthread_mutex_lock(&p->prefetchMutex);
        int cancel = p->prefetchCancel;
        pthread_mutex_unlock(&p->prefetchMutex);
        if (cancel) break;

        /* Collect a batch of invalid pages */
        int batchPages[PREFETCH_BATCH_PAGES];
        int batchCount = 0;
        while (pageNo < totalPages && batchCount < PREFETCH_BATCH_PAGES) {
            if (!azqlite_cache_page_valid(p->diskCache, pageNo)) {
                batchPages[batchCount++] = pageNo;
            }
            pageNo++;
        }
        if (batchCount == 0) continue;

        /* Try batch read if available */
        if (p->ops->page_blob_read_batch) {
            azure_read_range_t ranges[PREFETCH_BATCH_PAGES];
            uint8_t *buffers[PREFETCH_BATCH_PAGES];
            for (int i = 0; i < batchCount; i++) {
                buffers[i] = malloc((size_t)pageSize);
                if (!buffers[i]) {
                    for (int j = 0; j < i; j++) free(buffers[j]);
                    goto done;
                }
                ranges[i].offset = (int64_t)batchPages[i] * pageSize;
                ranges[i].len = (size_t)pageSize;
                /* Clamp last page to blob boundary */
                if (ranges[i].offset + (int64_t)ranges[i].len > p->blobSize) {
                    ranges[i].len = (size_t)(p->blobSize - ranges[i].offset);
                }
                ranges[i].data = buffers[i];
                ranges[i].data_len = 0;
            }

            azure_error_t aerr;
            azure_error_init(&aerr);
            azure_err_t rc = p->ops->page_blob_read_batch(
                p->ops_ctx, p->zBlobName, ranges, batchCount, &aerr);

            if (rc == AZURE_OK) {
                for (int i = 0; i < batchCount; i++) {
                    /* Zero-pad short reads (partial last page) */
                    if (ranges[i].data_len < (size_t)pageSize) {
                        memset(buffers[i] + ranges[i].data_len, 0,
                               (size_t)pageSize - ranges[i].data_len);
                    }
                    if (batchPages[i] >= azqlite_cache_page_count(p->diskCache)) {
                        azqlite_cache_grow(p->diskCache, batchPages[i] + 64);
                    }
                    azqlite_cache_page_write_if_invalid(p->diskCache,
                                                         batchPages[i], buffers[i]);
                    fetched++;
                }
            }
            for (int i = 0; i < batchCount; i++) free(buffers[i]);
        } else {
            /* Fallback: sequential reads via page_blob_read */
            int firstPage = batchPages[0];
            int lastPage = batchPages[batchCount - 1];
            int64_t startOff = (int64_t)firstPage * pageSize;
            int64_t endOff = (int64_t)(lastPage + 1) * pageSize;
            if (endOff > p->blobSize) endOff = p->blobSize;
            size_t readLen = (size_t)(endOff - startOff);

            azure_buffer_t buf = {0};
            azure_error_t aerr;
            azure_error_init(&aerr);
            azure_err_t rc = p->ops->page_blob_read(
                p->ops_ctx, p->zBlobName, startOff, readLen, &buf, &aerr);

            if (rc == AZURE_OK && buf.data) {
                for (int i = 0; i < batchCount; i++) {
                    int pg = batchPages[i];
                    size_t off = (size_t)((int64_t)pg * pageSize - startOff);
                    if (off + (size_t)pageSize <= buf.size) {
                        if (pg >= azqlite_cache_page_count(p->diskCache)) {
                            azqlite_cache_grow(p->diskCache, pg + 64);
                        }
                        azqlite_cache_page_write_if_invalid(p->diskCache,
                                                             pg, buf.data + off);
                        fetched++;
                    } else if (off < buf.size) {
                        /* Partial last page — zero-pad */
                        unsigned char padded[65536];
                        size_t avail = buf.size - off;
                        memcpy(padded, buf.data + off, avail);
                        memset(padded + avail, 0, (size_t)pageSize - avail);
                        if (pg >= azqlite_cache_page_count(p->diskCache)) {
                            azqlite_cache_grow(p->diskCache, pg + 64);
                        }
                        azqlite_cache_page_write_if_invalid(p->diskCache,
                                                             pg, padded);
                        fetched++;
                    }
                }
            }
            free(buf.data);
        }

        /* Update progress */
        pthread_mutex_lock(&p->prefetchMutex);
        p->prefetchProgress = fetched;
        pthread_mutex_unlock(&p->prefetchMutex);
    }

done:
    pthread_mutex_lock(&p->prefetchMutex);
    p->prefetchProgress = fetched;
    p->prefetchRunning = 0;
    pthread_mutex_unlock(&p->prefetchMutex);

    if (azqlite_debug_timing()) {
        fprintf(stderr, "[PREFETCH-BG] done: fetched=%d/%d pages\n",
                fetched, totalPages);
    }
    return NULL;
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

    /* Cancel and join background prefetch thread if running */
    cancelPrefetchThread(p);
    pthread_mutex_destroy(&p->prefetchMutex);

    /* Flush any remaining dirty data before closing */
    if (p->diskCache && azqlite_cache_dirty_count(p->diskCache) > 0 &&
        p->ops && p->ops->page_blob_write) {
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

    /* Log readahead summary if there was any miss activity */
    if (azqlite_debug_timing() && p->readahead.nMisses > 0) {
        fprintf(stderr, "[READAHEAD] Summary: %d misses (%d sequential, %d random), "
                "peak window=%d, %d resets\n",
                p->readahead.nMisses, p->readahead.nSequentialMisses,
                p->readahead.nRandomMisses, p->readahead.peakWindow,
                p->readahead.nWindowResets);
    }

    azqlite_cache_close(p->diskCache);
    p->diskCache = NULL;
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
** xRead — Read from the page cache (MAIN_DB) or in-memory buffer.
** For MAIN_DB: demand-page through LRU cache with range GETs on miss.
** For MAIN_JOURNAL: read from aJrnlData.
** Must zero-fill on short read (SQLITE_IOERR_SHORT_READ).
*/
static int azqliteRead(sqlite3_file *pFile, void *pBuf, int iAmt,
                        sqlite3_int64 iOfst) {
    azqliteFile *p = (azqliteFile *)pFile;

    /* WAL and journal use existing buffer paths */
    if (p->eFileType == SQLITE_OPEN_WAL) {
        unsigned char *src = p->aWalData;
        sqlite3_int64 srcLen = p->nWalData;
        if (!src || iOfst >= srcLen) {
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
        if (!src || iOfst >= srcLen) {
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

    /* MAIN_DB: demand-page through mmap cache */
    g_xread_count++;
    int pageSize = p->pageSize;
    unsigned char *out = (unsigned char *)pBuf;
    sqlite3_int64 remaining = iAmt;
    sqlite3_int64 offset = iOfst;

    while (remaining > 0) {
        int pageNo = (int)(offset / pageSize);
        int pageOff = (int)(offset % pageSize);
        int copyLen = pageSize - pageOff;
        if (copyLen > remaining) copyLen = (int)remaining;

        /* Past logical end of file — short read */
        if (offset >= p->blobSize) {
            memset(out, 0, (size_t)remaining);
            return SQLITE_IOERR_SHORT_READ;
        }

        if (!azqlite_cache_page_valid(p->diskCache, pageNo)) {
            /* Cache miss — read-ahead: fetch a batch of contiguous pages in
             * one HTTP GET to amortize Azure round-trip latency (~22ms/GET) */
            int readaheadPages;
            if (p->readaheadMode > 0) {
                /* Fixed mode — user-specified via URI parameter */
                readaheadPages = p->readaheadMode;
            } else {
                /* Adaptive mode — consult state machine */
                readaheadPages = readaheadOnMiss(
                    &p->readahead, pageNo,
                    p->readaheadMaxWindow,
                    azqlite_cache_page_count(p->diskCache));
            }
            int totalPages = (int)((p->blobSize + pageSize - 1) / pageSize);
            int endPage = pageNo + readaheadPages;
            if (endPage > totalPages) endPage = totalPages;

            int64_t fetchOff = (int64_t)pageNo * pageSize;
            int64_t fetchEnd = (int64_t)endPage * pageSize;
            if (fetchEnd > p->blobSize) fetchEnd = p->blobSize;
            size_t fetchLen = (size_t)(fetchEnd - fetchOff);

            azure_buffer_t buf = {0};
            if (fetchLen > 0) {
                /* Stop prefetch thread before Azure I/O to avoid sharing
                 * the curl_multi handle concurrently. */
                cancelPrefetchThread(p);
                azure_error_t aerr;
                azure_error_init(&aerr);
                azure_err_t aec = p->ops->page_blob_read(
                    p->ops_ctx, p->zBlobName, fetchOff, fetchLen, &buf, &aerr);
                if (aec != AZURE_OK) {
                    int sqlRc = azureErrToSqlite(aec, SQLITE_IOERR_READ);
                    fprintf(stderr, "azqlite: page_blob_read failed at offset %lld len %zu: %s\n",
                            (long long)fetchOff, fetchLen, aerr.error_message);
                    azure_error_clear(&aerr);
                    return sqlRc;
                }
            }

            /* Split response into individual pages and write into cache */
            if (buf.data && buf.size > 0) {
                size_t bufOff = 0;
                for (int pg = pageNo; pg < endPage && bufOff < buf.size; pg++) {
                    size_t avail = buf.size - bufOff;
                    size_t toCopy = avail < (size_t)pageSize ? avail : (size_t)pageSize;
                    if (toCopy == (size_t)pageSize) {
                        azqlite_cache_page_write_if_invalid(
                            p->diskCache, pg, buf.data + bufOff);
                    } else {
                        /* Partial page at end of file — zero-pad */
                        unsigned char *tmp = (unsigned char *)calloc(1, (size_t)pageSize);
                        if (tmp) {
                            memcpy(tmp, buf.data + bufOff, toCopy);
                            azqlite_cache_page_write_if_invalid(p->diskCache, pg, tmp);
                            free(tmp);
                        }
                    }
                    bufOff += (size_t)pageSize;
                }
            }
            free(buf.data);

            /* Requested page should now be in cache */
            if (!azqlite_cache_page_valid(p->diskCache, pageNo)) {
                memset(out, 0, (size_t)remaining);
                return SQLITE_IOERR_SHORT_READ;
            }
        }

        /* Handle short read within partial page at end of file */
        sqlite3_int64 endOfPage = (sqlite3_int64)(pageNo + 1) * pageSize;
        if (endOfPage > p->blobSize) {
            int validInPage = (int)(p->blobSize - (sqlite3_int64)pageNo * pageSize);
            if (pageOff >= validInPage) {
                memset(out, 0, (size_t)remaining);
                return SQLITE_IOERR_SHORT_READ;
            }
            if (pageOff + copyLen > validInPage) {
                int validCopy = validInPage - pageOff;
                azqlite_cache_read_page_range(
                    p->diskCache, pageNo, pageOff, validCopy, out);
                memset(out + validCopy, 0, (size_t)(remaining - validCopy));
                return SQLITE_IOERR_SHORT_READ;
            }
        }

        /* Safe locked copy from mmap cache to output buffer */
        azqlite_cache_read_page_range(
            p->diskCache, pageNo, pageOff, copyLen, out);
        out += copyLen;
        offset += copyLen;
        remaining -= copyLen;
    }

    return SQLITE_OK;
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

    /* MAIN_DB — write through mmap cache */
    int pageSize = p->pageSize;
    const unsigned char *src = (const unsigned char *)pBuf;
    sqlite3_int64 remaining = iAmt;
    sqlite3_int64 offset = iOfst;

    /* Renew lease if needed during long write sequences */
    if (hasLease(p)) {
        rc = leaseRenewIfNeeded(p);
        if (rc != SQLITE_OK) return rc;
    }

    /* Ensure cache can hold pages we're about to write */
    int maxPageNeeded = (int)((iOfst + iAmt + pageSize - 1) / pageSize);
    if (p->diskCache && maxPageNeeded > azqlite_cache_page_count(p->diskCache)) {
        if (azqlite_cache_grow(p->diskCache, maxPageNeeded + 64) != 0) {
            return SQLITE_NOMEM;
        }
    }

    while (remaining > 0) {
        int pageNo = (int)(offset / pageSize);
        int pageOff = (int)(offset % pageSize);
        int copyLen = pageSize - pageOff;
        if (copyLen > remaining) copyLen = (int)remaining;

        /* Bounds check — page should be within cache after grow above */
        if (pageNo >= azqlite_cache_page_count(p->diskCache))
            return SQLITE_NOMEM;

        unsigned char *pagePtr;
        int isFullPage = (pageOff == 0 && copyLen == pageSize);
        int64_t pageStart = (int64_t)pageNo * pageSize;

        if (!azqlite_cache_page_valid(p->diskCache, pageNo)) {
            /* For partial writes, fetch existing page content from Azure first */
            if (!isFullPage && pageStart < p->blobSize) {
                size_t fetchLen = (size_t)pageSize;
                if (pageStart + (int64_t)fetchLen > p->blobSize) {
                    fetchLen = (size_t)(p->blobSize - pageStart);
                }
                /* Stop prefetch thread before Azure I/O */
                cancelPrefetchThread(p);
                azure_buffer_t buf = {0};
                azure_error_t aerr;
                azure_error_init(&aerr);
                azure_err_t aec = p->ops->page_blob_read(
                    p->ops_ctx, p->zBlobName, pageStart, fetchLen, &buf, &aerr);
                if (aec != AZURE_OK) {
                    int sqlRc = azureErrToSqlite(aec, SQLITE_IOERR_READ);
                    fprintf(stderr, "azqlite: page_blob_read failed at offset %lld: %s\n",
                            (long long)pageStart, aerr.error_message);
                    azure_error_clear(&aerr);
                    return sqlRc;
                }
                if (buf.data && buf.size > 0) {
                    /* Write fetched content as the base, then overlay our write */
                    unsigned char *tmp = (unsigned char *)calloc(1, (size_t)pageSize);
                    if (!tmp) { free(buf.data); return SQLITE_NOMEM; }
                    size_t toCopy = buf.size < (size_t)pageSize ? buf.size : (size_t)pageSize;
                    memcpy(tmp, buf.data, toCopy);
                    azqlite_cache_page_write(p->diskCache, pageNo, tmp, 0);
                    free(tmp);
                }
                free(buf.data);
            } else if (isFullPage) {
                /* Full page write — no need to fetch existing content */
            } else {
                /* New page beyond blob — zero-fill by writing zeros */
                unsigned char *tmp = (unsigned char *)calloc(1, (size_t)pageSize);
                if (!tmp) return SQLITE_NOMEM;
                azqlite_cache_page_write(p->diskCache, pageNo, tmp, 0);
                free(tmp);
            }
        }

        /* Write data directly to mmap and mark valid+dirty atomically */
        azqlite_cache_lock(p->diskCache);
        pagePtr = azqlite_cache_page_ptr(p->diskCache, pageNo);
        if (!pagePtr) {
            azqlite_cache_unlock(p->diskCache);
            return SQLITE_NOMEM;
        }
        memcpy(pagePtr + pageOff, src, copyLen);
        azqlite_cache_mark_written(p->diskCache, pageNo);
        azqlite_cache_unlock(p->diskCache);

        src += copyLen;
        offset += copyLen;
        remaining -= copyLen;
    }

    /* Extend logical size if write goes past current end */
    if (iOfst + iAmt > p->blobSize) {
        p->blobSize = iOfst + iAmt;
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

    if (size < p->blobSize) {
        /* Invalidate cache entries beyond new size */
        if (p->diskCache && p->pageSize > 0) {
            int maxPageNo = (size == 0) ? -1 : (int)((size - 1) / p->pageSize);
            azqlite_cache_invalidate_above(p->diskCache, maxPageNo);
        }
    }
    p->blobSize = size;
    return SQLITE_OK;
}

/* Maximum bytes per Azure Put Page request (4 MiB) */
#define AZQLITE_MAX_PUT_PAGE  (4 * 1024 * 1024)

/* Stack-allocated range threshold (avoid heap for small flushes) */
#define AZQLITE_STACK_RANGES 64

/*
** diskCacheCoalesceRanges — Given sorted dirty page numbers, coalesce
** consecutive pages into azure_page_range_t entries using mmap cache.
** Each range is capped at 4 MiB, uses a temp buffer, and is 512-byte aligned.
*/
static int diskCacheCoalesceRanges(
    azqlite_cache_t *cache, int *dirtyPages, int nDirty, int pageSize,
    sqlite3_int64 blobSize,
    azure_page_range_t *ranges, int maxRanges, int *pnRanges
){
    int nRanges = 0;
    int i = 0;

    while (i < nDirty) {
        int runStart = dirtyPages[i];
        int runCount = 1;
        while (i + runCount < nDirty
               && dirtyPages[i + runCount] == runStart + runCount
               && (int64_t)runCount * pageSize < AZQLITE_MAX_PUT_PAGE) {
            runCount++;
        }

        int64_t offset = (int64_t)runStart * pageSize;
        int64_t runEnd = offset + (int64_t)runCount * pageSize;
        if (runEnd > blobSize) runEnd = blobSize;
        size_t len = (size_t)(runEnd - offset);
        len = (len + 511) & ~(size_t)511;

        unsigned char *buf = (unsigned char *)calloc(1, len);
        if (!buf) {
            for (int r = 0; r < nRanges; r++) free((void *)ranges[r].data);
            *pnRanges = 0;
            return SQLITE_NOMEM;
        }
        /* Copy page data under lock to prevent stale pointers from grow() */
        azqlite_cache_lock(cache);
        for (int j = 0; j < runCount; j++) {
            unsigned char *pagePtr = azqlite_cache_page_ptr(cache, dirtyPages[i + j]);
            size_t copyLen = (size_t)pageSize;
            int64_t pageEnd = offset + (int64_t)(j + 1) * pageSize;
            if (pageEnd > blobSize) {
                copyLen = (size_t)(blobSize - (offset + (int64_t)j * pageSize));
            }
            if ((int64_t)copyLen > 0 && pagePtr) {
                memcpy(buf + (size_t)j * (size_t)pageSize, pagePtr, copyLen);
            }
        }
        azqlite_cache_unlock(cache);

        if (nRanges >= maxRanges) {
            free(buf);
            for (int r = 0; r < nRanges; r++) free((void *)ranges[r].data);
            *pnRanges = 0;
            return SQLITE_IOERR_FSYNC;
        }

        ranges[nRanges].offset = offset;
        ranges[nRanges].data = buf;
        ranges[nRanges].len = len;
        nRanges++;

        i += runCount;
    }

    *pnRanges = nRanges;
    return SQLITE_OK;
}

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

    /* Stop the prefetch thread before any Azure I/O to avoid concurrent
     * use of the shared curl_multi handle (not thread-safe). */
    cancelPrefetchThread(p);

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
            {
                azqliteJournalCacheEntry *jce =
                    journalCacheGetOrCreate(p->pVfsData, p->zBlobName);
                if (jce) jce->state = 1;
            }
        }
        return SQLITE_OK;
    }

    /* MAIN_DB — flush dirty pages from disk cache */
    if (!p->diskCache || azqlite_cache_dirty_count(p->diskCache) == 0) return SQLITE_OK;
    if (!p->ops || !p->ops->page_blob_write) return SQLITE_IOERR_FSYNC;

    double sync_t0 = 0;
    if (azqlite_debug_timing()) sync_t0 = azqlite_time_ms();

    /* Record dirty page count for lease duration heuristic */
    int dirtyCountBeforeSync = azqlite_cache_dirty_count(p->diskCache);

    /* Renew lease before flushing */
    int rc = leaseRenewIfNeeded(p);
    if (rc != SQLITE_OK) return SQLITE_IOERR_FSYNC;

    /* R2: Only resize if the blob has actually grown since last sync. */
    double resize_ms = 0;
    if (p->blobSize > p->lastSyncedSize && p->ops->page_blob_resize) {
        sqlite3_int64 alignedSize = (p->blobSize + 511) & ~(sqlite3_int64)511;
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
        p->lastSyncedSize = p->blobSize;
    }

    /* Collect and sort dirty page numbers */
    double coalesce_t0 = 0;
    if (azqlite_debug_timing()) coalesce_t0 = azqlite_time_ms();

    int *dirtyPages = NULL;
    int nDirtyEntries = azqlite_cache_collect_dirty(p->diskCache, &dirtyPages);
    if (nDirtyEntries > 0 && !dirtyPages) return SQLITE_NOMEM;

    /* Coalesce into ranges */
    azure_page_range_t stackRanges[AZQLITE_STACK_RANGES];
    azure_page_range_t *ranges = stackRanges;
    int maxRanges = AZQLITE_STACK_RANGES;
    int nRanges = 0;

    rc = diskCacheCoalesceRanges(p->diskCache, dirtyPages, nDirtyEntries,
                                  p->pageSize, p->blobSize,
                                  ranges, maxRanges, &nRanges);
    if (rc != SQLITE_OK && rc != SQLITE_IOERR_FSYNC) {
        free(dirtyPages);
        return rc;
    }
    if (rc == SQLITE_IOERR_FSYNC) {
        /* Stack too small — retry with heap */
        maxRanges = nDirtyEntries;
        ranges = (azure_page_range_t *)sqlite3_malloc64(
            (sqlite3_int64)maxRanges * (sqlite3_int64)sizeof(azure_page_range_t));
        if (!ranges) {
            free(dirtyPages);
            return SQLITE_NOMEM;
        }
        rc = diskCacheCoalesceRanges(p->diskCache, dirtyPages, nDirtyEntries,
                                      p->pageSize, p->blobSize,
                                      ranges, maxRanges, &nRanges);
        if (rc != SQLITE_OK) {
            sqlite3_free(ranges);
            free(dirtyPages);
            return rc;
        }
    }

    double coalesce_ms = 0;
    if (azqlite_debug_timing()) coalesce_ms = azqlite_time_ms() - coalesce_t0;

    /* Try batch write if available */
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

        for (int i = 0; i < nRanges; i++) free((void *)ranges[i].data);
        if (ranges != stackRanges) sqlite3_free(ranges);
        free(dirtyPages);

        if (arc != AZURE_OK) return SQLITE_IOERR_FSYNC;
        p->lastSyncDirtyCount = dirtyCountBeforeSync;
        if (aerr.etag[0] != '\0') {
            memcpy(p->etag, aerr.etag, sizeof(p->etag));
            azqlite_cache_set_etag(p->diskCache, aerr.etag);
        }
        azqlite_cache_clear_dirty(p->diskCache);
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
                for (int j = i; j < nRanges; j++) free((void *)ranges[j].data);
                if (ranges != stackRanges) sqlite3_free(ranges);
                free(dirtyPages);
                return SQLITE_IOERR_FSYNC;
            }
        }

        azure_error_init(&aerr);
        azure_err_t arc = p->ops->page_blob_write(
            p->ops_ctx, p->zBlobName,
            ranges[i].offset, ranges[i].data, ranges[i].len,
            hasLease(p) ? p->leaseId : NULL, &aerr);

        free((void *)ranges[i].data);

        if (arc != AZURE_OK) {
            for (int j = i + 1; j < nRanges; j++) free((void *)ranges[j].data);
            if (ranges != stackRanges) sqlite3_free(ranges);
            free(dirtyPages);
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
        azqlite_cache_set_etag(p->diskCache, aerr.etag);
    }

    if (ranges != stackRanges) sqlite3_free(ranges);
    free(dirtyPages);

    azqlite_cache_clear_dirty(p->diskCache);
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
        *pSize = p->blobSize;
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
        case AZQLITE_FCNTL_READAHEAD_MODE: {
            azqliteFile *p = (azqliteFile *)pFile;
            int mode = *(int *)pArg;
            p->readaheadMode = mode;
            if (mode == 0) {
                /* Reset adaptive state when switching back to auto */
                p->readahead.state = RA_INITIAL;
                p->readahead.lastMissPage = -1;
                p->readahead.window = 0;
            }
            return SQLITE_OK;
        }
        case AZQLITE_FCNTL_READAHEAD_STATS: {
            azqliteFile *p = (azqliteFile *)pFile;
            azqlite_readahead_stats_t *s = (azqlite_readahead_stats_t *)pArg;
            s->currentState = p->readahead.state;
            s->currentWindow = p->readahead.window;
            s->lastMissPage = p->readahead.lastMissPage;
            s->totalMisses = p->readahead.nMisses;
            s->sequentialMisses = p->readahead.nSequentialMisses;
            s->randomMisses = p->readahead.nRandomMisses;
            s->windowGrows = p->readahead.nWindowGrows;
            s->windowResets = p->readahead.nWindowResets;
            s->peakWindow = p->readahead.peakWindow;
            return SQLITE_OK;
        }
        case AZQLITE_FCNTL_PREFETCH_STATUS: {
            azqliteFile *p = (azqliteFile *)pFile;
            azqlite_prefetch_status_t *s = (azqlite_prefetch_status_t *)pArg;
            pthread_mutex_lock(&p->prefetchMutex);
            s->running = p->prefetchRunning;
            s->progress = p->prefetchProgress;
            s->total = p->prefetchTotal;
            pthread_mutex_unlock(&p->prefetchMutex);
            return SQLITE_OK;
        }
        case AZQLITE_FCNTL_PREFETCH_WAIT: {
            azqliteFile *p = (azqliteFile *)pFile;
            if (p->prefetchRunning) {
                pthread_join(p->prefetchThread, NULL);
                p->prefetchRunning = 0;
            }
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
** Parse Azure URI parameters from a sqlite3_filename.
** Returns 1 if azure_account is present (config populated), 0 otherwise.
*/
static int azqlite_parse_uri_config(sqlite3_filename zName,
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

    /* Initialize prefetch mutex for Azure-backed files */
    pthread_mutex_init(&p->prefetchMutex, NULL);

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
    if (azqlite_parse_uri_config(zName, &uriCfg)) {
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
    p->leaseDuration = AZQLITE_LEASE_DURATION;
    p->lastSyncDirtyCount = 0;
    p->etag[0] = '\0';

    /* Adaptive readahead defaults */
    p->readahead.state = RA_INITIAL;
    p->readahead.lastMissPage = -1;
    p->readahead.window = 0;
    memset(&p->readahead.nMisses, 0,
           sizeof(azqlite_readahead_t) - offsetof(azqlite_readahead_t, nMisses));
    p->readaheadMode = 0;              /* adaptive by default */
    p->readaheadMaxWindow = RA_MAX_WINDOW;
    p->cachePagesConfig = AZQLITE_DEFAULT_CACHE_PAGES;
    p->prefetchPages = AZQLITE_DEFAULT_PREFETCH_PAGES;

    /* Parse readahead URI parameters */
    const char *raParam = sqlite3_uri_parameter(zName, "readahead");
    if (raParam) {
        if (strcmp(raParam, "auto") == 0 || strcmp(raParam, "adaptive") == 0) {
            p->readaheadMode = 0;
        } else {
            int val = atoi(raParam);
            if (val >= 0) p->readaheadMode = val;
        }
    }
    const char *raMaxParam = sqlite3_uri_parameter(zName, "readahead_max");
    if (raMaxParam) {
        int val = atoi(raMaxParam);
        if (val > 0) p->readaheadMaxWindow = val;
    }
    const char *cpParam = sqlite3_uri_parameter(zName, "cache_pages");
    if (cpParam) {
        int val = atoi(cpParam);
        if (val > 0) p->cachePagesConfig = val;
    }
    const char *pfParam = sqlite3_uri_parameter(zName, "prefetch");
    if (pfParam) {
        if (strcmp(pfParam, "off") == 0) {
            p->prefetchPages = AZQLITE_PREFETCH_OFF;
        } else if (strcmp(pfParam, "all") == 0) {
            p->prefetchPages = AZQLITE_PREFETCH_ALL;
        } else if (strcmp(pfParam, "index") == 0) {
            p->prefetchPages = AZQLITE_PREFETCH_INDEX;
        } else if (strcmp(pfParam, "warm") == 0) {
            p->prefetchPages = AZQLITE_PREFETCH_WARM;
        } else {
            int val = atoi(pfParam);
            if (val >= 0) p->prefetchPages = val;
        }
    }

    if (isMainDb) {
        /* MAIN_DB: Initialize page cache and fetch first page for header.
        ** Use blob_get_properties as single HEAD request — returns NOT_FOUND
        ** if blob doesn't exist, eliminating the separate blob_exists call. */
        int blobExists = 0;
        int64_t blobSize = 0;

        if (p->ops && p->ops->blob_get_properties) {
            azure_error_t aerr;
            azure_error_init(&aerr);
            azure_err_t arc = p->ops->blob_get_properties(
                p->ops_ctx, zName, &blobSize, NULL, NULL, &aerr);
            if (arc == AZURE_OK) {
                blobExists = 1;
                /* Capture initial ETag from blob properties */
                if (aerr.etag[0] != '\0') {
                    memcpy(p->etag, aerr.etag, sizeof(p->etag));
                }
            } else if (arc == AZURE_ERR_NOT_FOUND) {
                blobExists = 0;
            } else {
                sqlite3_free(p->zBlobName);
                p->zBlobName = NULL;
                return azureErrToSqlite(arc, SQLITE_CANTOPEN);
            }
        } else if (p->ops && p->ops->blob_exists) {
            /* Fallback if blob_get_properties not available */
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

        if (blobExists) {
            p->blobSize = blobSize;

            if (blobSize > 0 && p->ops->page_blob_read) {
            /* Open disk cache first (page_size=0 adopts existing header's page_size) */
            const char *cacheDir = sqlite3_uri_parameter(zName, "cache_dir");
            const char *checksumsParam = sqlite3_uri_parameter(zName, "checksums");
            int checksumsEnabled = (checksumsParam && checksumsParam[0] == '1');
            int pageCount = (int)(blobSize / AZQLITE_DEFAULT_PAGE_SIZE) + 1;
            {
                azqlite_cache_config_t cfg;
                memset(&cfg, 0, sizeof(cfg));
                cfg.cache_dir = cacheDir;
                cfg.blob_identity = p->zBlobName;
                cfg.page_size = 0;  /* Adopt from existing cache, or 4096 */
                cfg.page_count = pageCount;
                cfg.checksums_enabled = checksumsEnabled;
                p->diskCache = azqlite_cache_open(&cfg);
            }
            if (!p->diskCache) {
                sqlite3_free(p->zBlobName);
                p->zBlobName = NULL;
                return SQLITE_NOMEM;
            }

            /* ETag check: if cached ETag matches remote, skip all downloads */
            int etagHit = 0;
            if (p->etag[0] != '\0' &&
                azqlite_cache_etag_matches(p->diskCache, p->etag)) {
                etagHit = 1;
                p->pageSize = azqlite_cache_page_size(p->diskCache);
                /* Ensure cache covers the full blob */
                int neededPages = (int)(blobSize / p->pageSize) + 1;
                if (neededPages > azqlite_cache_page_count(p->diskCache)) {
                    azqlite_cache_grow(p->diskCache, neededPages);
                }
            }

            if (!etagHit) {
                /* ETag mismatch or new cache — must download data */
                azqlite_cache_invalidate_all(p->diskCache);

                /* Determine how much to prefetch */
                size_t fetchLen = 0;
                size_t cacheMaxBytes = (size_t)p->cachePagesConfig * AZQLITE_DEFAULT_PAGE_SIZE;

                if (p->prefetchPages == AZQLITE_PREFETCH_OFF) {
                    fetchLen = AZQLITE_DEFAULT_PAGE_SIZE;
                } else if (p->prefetchPages == AZQLITE_PREFETCH_ALL) {
                    if ((size_t)blobSize <= cacheMaxBytes) {
                        fetchLen = (size_t)blobSize;
                    } else {
                        size_t fallback = (size_t)AZQLITE_DEFAULT_PREFETCH_PAGES * AZQLITE_DEFAULT_PAGE_SIZE;
                        fetchLen = (fallback <= (size_t)blobSize) ? fallback : (size_t)blobSize;
                    }
                } else if (p->prefetchPages == AZQLITE_PREFETCH_INDEX ||
                           p->prefetchPages == AZQLITE_PREFETCH_WARM) {
                    size_t fallback = (size_t)AZQLITE_DEFAULT_PREFETCH_PAGES * AZQLITE_DEFAULT_PAGE_SIZE;
                    fetchLen = (fallback <= (size_t)blobSize) ? fallback : (size_t)blobSize;
                } else {
                    int prefetchLimit = p->prefetchPages;
                    if (prefetchLimit > p->cachePagesConfig)
                        prefetchLimit = p->cachePagesConfig;
                    size_t maxPrefetch = (size_t)prefetchLimit * AZQLITE_DEFAULT_PAGE_SIZE;
                    fetchLen = ((int64_t)maxPrefetch <= blobSize)
                                      ? maxPrefetch : (size_t)blobSize;
                }

                if (fetchLen < AZQLITE_DEFAULT_PAGE_SIZE && blobSize > 0)
                    fetchLen = AZQLITE_DEFAULT_PAGE_SIZE;
                if ((int64_t)fetchLen > blobSize) fetchLen = (size_t)blobSize;

                double prefetch_t0 = 0;
                if (azqlite_debug_timing()) prefetch_t0 = azqlite_time_ms();

                azure_buffer_t buf = {0};
                azure_error_t aerr2;
                azure_error_init(&aerr2);
                azure_err_t rarc = p->ops->page_blob_read(p->ops_ctx, zName,
                                              0, fetchLen, &buf, &aerr2);
                if (rarc != AZURE_OK) {
                    azqlite_cache_close(p->diskCache);
                    p->diskCache = NULL;
                    sqlite3_free(p->zBlobName);
                    p->zBlobName = NULL;
                    return azureErrToSqlite(rarc, SQLITE_CANTOPEN);
                }

                /* Detect page size from downloaded header */
                int detectedPageSize = AZQLITE_DEFAULT_PAGE_SIZE;
                if (buf.data && buf.size >= 100) {
                    int detected = detectPageSize(buf.data, (sqlite3_int64)buf.size);
                    if (detected > 0) detectedPageSize = detected;
                }
                p->pageSize = detectedPageSize;

                /* If detected page size differs from cache's, close and reopen */
                if (detectedPageSize != azqlite_cache_page_size(p->diskCache)) {
                    azqlite_cache_close(p->diskCache);
                    pageCount = (int)(blobSize / detectedPageSize) + 1;
                    azqlite_cache_config_t cfg;
                    memset(&cfg, 0, sizeof(cfg));
                    cfg.cache_dir = cacheDir;
                    cfg.blob_identity = p->zBlobName;
                    cfg.page_size = detectedPageSize;
                    cfg.page_count = pageCount;
                    cfg.checksums_enabled = checksumsEnabled;
                    p->diskCache = azqlite_cache_open(&cfg);
                    if (!p->diskCache) {
                        free(buf.data);
                        sqlite3_free(p->zBlobName);
                        p->zBlobName = NULL;
                        return SQLITE_NOMEM;
                    }
                    azqlite_cache_invalidate_all(p->diskCache);
                }

                /* Insert fetched pages into cache */
                int pagesLoaded = 0;
                if (buf.data && buf.size > 0) {
                    size_t off = 0;
                    int pageNo = 0;
                    while (off < buf.size) {
                        if (pageNo >= azqlite_cache_page_count(p->diskCache)) {
                            azqlite_cache_grow(p->diskCache, pageNo + 64);
                        }
                        azqlite_cache_page_write_if_invalid(p->diskCache, pageNo,
                                                             buf.data + off);
                        off += (size_t)detectedPageSize;
                        pageNo++;
                        pagesLoaded++;
                    }
                }
                free(buf.data);

                if (azqlite_debug_timing()) {
                    double prefetch_ms = azqlite_time_ms() - prefetch_t0;
                    const char *stratName =
                        (p->prefetchPages == AZQLITE_PREFETCH_OFF) ? "off" :
                        (p->prefetchPages == AZQLITE_PREFETCH_ALL) ? "all" :
                        (p->prefetchPages == AZQLITE_PREFETCH_INDEX) ? "index" :
                        (p->prefetchPages == AZQLITE_PREFETCH_WARM) ? "warm" : "fixed";
                    fprintf(stderr, "[PREFETCH] strategy=%s fetched=%d pages (%.1fMB) "
                            "of %lld total (%.1fMB) in %.1fms\n",
                            stratName, pagesLoaded,
                            (double)fetchLen / (1024.0 * 1024.0),
                            (long long)blobSize,
                            (double)blobSize / (1024.0 * 1024.0),
                            prefetch_ms);
                }
            }

            /* Launch background prefetch thread for remaining pages */
            if (blobSize > 0 && p->pageSize > 0 && p->ops->page_blob_read) {
                int totalPages = (int)(blobSize / p->pageSize);
                if (blobSize % p->pageSize != 0) totalPages++;
                /* Count how many pages are still invalid */
                int invalidCount = 0;
                for (int i = 0; i < totalPages; i++) {
                    if (!azqlite_cache_page_valid(p->diskCache, i))
                        invalidCount++;
                }
                if (invalidCount > 0) {
                    p->prefetchRunning = 1;
                    p->prefetchCancel = 0;
                    p->prefetchProgress = 0;
                    p->prefetchTotal = totalPages;
                    if (pthread_create(&p->prefetchThread, NULL,
                                       prefetchThreadFunc, p) != 0) {
                        p->prefetchRunning = 0;
                    }
                }
            }

            /* Store ETag in cache for future sessions */
            if (p->etag[0] != '\0') {
                azqlite_cache_set_etag(p->diskCache, p->etag);
            }

            /* R2: Record initial Azure blob size to skip redundant resizes */
            p->lastSyncedSize = blobSize;
            } else {
                /* Empty blob or no read ops — open cache with default page size */
                const char *cacheDir = sqlite3_uri_parameter(zName, "cache_dir");
                azqlite_cache_config_t cfg;
                memset(&cfg, 0, sizeof(cfg));
                cfg.cache_dir = cacheDir;
                cfg.blob_identity = p->zBlobName;
                cfg.page_size = AZQLITE_DEFAULT_PAGE_SIZE;
                cfg.page_count = 64;
                p->diskCache = azqlite_cache_open(&cfg);
                if (!p->diskCache) {
                    sqlite3_free(p->zBlobName);
                    p->zBlobName = NULL;
                    return SQLITE_NOMEM;
                }
                p->pageSize = AZQLITE_DEFAULT_PAGE_SIZE;
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
            p->blobSize = 0;
            {
                const char *cacheDir = sqlite3_uri_parameter(zName, "cache_dir");
                azqlite_cache_config_t cfg;
                memset(&cfg, 0, sizeof(cfg));
                cfg.cache_dir = cacheDir;
                cfg.blob_identity = p->zBlobName;
                cfg.page_size = AZQLITE_DEFAULT_PAGE_SIZE;
                cfg.page_count = 64;
                p->diskCache = azqlite_cache_open(&cfg);
            }
            if (!p->diskCache) {
                sqlite3_free(p->zBlobName);
                p->zBlobName = NULL;
                return SQLITE_NOMEM;
            }
            /* Fresh blob — invalidate any stale pages from prior sessions */
            azqlite_cache_invalidate_all(p->diskCache);
            azqlite_cache_clear_dirty(p->diskCache);
            azqlite_cache_set_etag(p->diskCache, "");
            p->pageSize = AZQLITE_DEFAULT_PAGE_SIZE;
        } else if (!blobExists) {
            sqlite3_free(p->zBlobName);
            p->zBlobName = NULL;
            return SQLITE_CANTOPEN;
        }

    } else if (isMainJournal) {
        /* MAIN_JOURNAL → Azure block blob */
        p->nJrnlData = 0;
        p->nJrnlAlloc = 0;
        p->aJrnlData = NULL;

        /* R1: Track journal blob name and seed the cache from blob_exists check */
        azqliteJournalCacheEntry *jce =
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

    const azure_ops_t *ops;
    void *ops_ctx;
    if (resolveOps(pVfsData, zName, &ops, &ops_ctx) && ops->blob_delete) {
        azure_error_t aerr;
        azure_error_init(&aerr);
        azure_err_t arc = ops->blob_delete(ops_ctx, zName, &aerr);
        if (arc == AZURE_ERR_NOT_FOUND) {
            /* R1: We know it doesn't exist now */
            azqliteJournalCacheEntry *jce = journalCacheFind(pVfsData, zName);
            if (jce) jce->state = 0;
            return SQLITE_OK;  /* Already gone — not an error */
        }
        if (arc != AZURE_OK) {
            return SQLITE_IOERR_DELETE;
        }

        /* R1: After successful delete, update journal cache */
        {
            azqliteJournalCacheEntry *jce = journalCacheFind(pVfsData, zName);
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
static int azqliteAccess(sqlite3_vfs *pVfs, const char *zName,
                          int flags, int *pResOut) {
    azqliteVfsData *pVfsData = (azqliteVfsData *)pVfs->pAppData;

    /* Delegate non-Azure paths (absolute filesystem paths) to default VFS */
    if (!azqliteIsAzurePath(zName)) {
        return pVfsData->pDefaultVfs->xAccess(pVfsData->pDefaultVfs, zName,
                                               flags, pResOut);
    }

    const azure_ops_t *ops;
    void *ops_ctx;
    if (resolveOps(pVfsData, zName, &ops, &ops_ctx) && ops->blob_exists) {
        /* R1: Use cached journal existence when available */
        azqliteJournalCacheEntry *jce = journalCacheFind(pVfsData, zName);
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

/*
** azqlite_vfs_register_uri — Register VFS with no global Azure client.
** All databases must provide Azure config via URI parameters, or
** xOpen returns SQLITE_CANTOPEN.
*/
int azqlite_vfs_register_uri(int makeDefault) {
    sqlite3_vfs *pDefault = sqlite3_vfs_find(0);
    if (!pDefault) return SQLITE_ERROR;

    memset(&g_vfsData, 0, sizeof(g_vfsData));
    g_vfsData.pDefaultVfs = pDefault;
    g_vfsData.ops = NULL;
    g_vfsData.ops_ctx = NULL;
    g_vfsData.client = NULL;

    /* szOsFile must accommodate both our struct and the default VFS's struct */
    int ourSize = (int)sizeof(azqliteFile);
    int defaultSize = pDefault->szOsFile;
    g_azqliteVfs.szOsFile = (ourSize > defaultSize) ? ourSize : defaultSize;
    g_azqliteVfs.pAppData = &g_vfsData;

    return sqlite3_vfs_register(&g_azqliteVfs, makeDefault);
}
