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
#include <errno.h>
#include <sys/stat.h>
#include <pthread.h>
#include <fcntl.h>
#include <pthread.h>

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
#define SQLITE_OBJS_PREFETCH_ALL       0       /* prefetch=all: full download at xOpen (default) */
#define SQLITE_OBJS_PREFETCH_NONE      1       /* prefetch=none: lazy cache filling */
#define SQLITE_OBJS_READAHEAD_PAGES    16      /* pages to fetch on a cache miss */
#define SQLITE_OBJS_PAGE1_BOOTSTRAP    65536   /* bytes to fetch at xOpen for page size detection */
#define SQLITE_OBJS_DIFF_THRESHOLD_PCT 50      /* >50% changed pages → full download */
#define SQLITE_OBJS_STATE_MAGIC        "SQOS"
#define SQLITE_OBJS_STATE_VERSION      1
#define SQLITE_OBJS_LEASE_DURATION     30      /* default lease duration (seconds) */
#define SQLITE_OBJS_LEASE_DURATION_LONG 60     /* extended lease for large flushes */
#define SQLITE_OBJS_LEASE_RENEW_AFTER  15      /* default renew threshold (unused — see leaseDuration) */
#define SQLITE_OBJS_DIRTY_PAGE_THRESHOLD 100   /* dirty pages triggering extended lease */
#define SQLITE_OBJS_MAX_PATHNAME       512
#define SQLITE_OBJS_INITIAL_ALLOC      (64*1024)  /* 64 KiB initial buffer */
#define SQLITE_OBJS_WAL_DEFAULT_CHUNK  (1024*1024)  /* 1 MiB default chunk */

/* ---------- Types ---------- */

/* Forward declaration for back-pointer in sqliteObjsFile */
typedef struct sqliteObjsVfsData sqliteObjsVfsData;

/*
** Generic bitmap — 1 bit per page with a running count of set bits.
** Used for both dirty-page tracking (which pages need flushing)
** and valid-page tracking (which pages are cached locally).
*/
typedef struct Bitmap {
    unsigned char *data;  /* Bit array: 1 bit per page */
    int nSet;             /* Count of set bits */
    int nAlloc;           /* Allocated size in bytes */
} Bitmap;

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
    Bitmap dirty;                       /* 1 bit per page, 1=dirty */
    int pageSize;                       /* Detected from header or default 4096 */
    int lastSyncDirtyCount;             /* Dirty pages at last xSync (lease heuristic) */

    /* Valid page tracking (lazy cache) */
    Bitmap valid;                       /* 1 bit per page, 1=cached */
    int prefetchMode;                   /* 0=all (default), 1=none (lazy) */

    /* Lock state */
    int eLock;                          /* Current SQLite lock level */
    char leaseId[64];                   /* Azure lease ID (empty = no lease) */
    time_t leaseAcquiredAt;             /* For renewal timing */
    int leaseDuration;                  /* Actual lease duration acquired (seconds) */

    /* R2: Skip redundant resize — track last synced blob size */
    sqlite3_int64 lastSyncedSize;       /* Blob size after last successful resize/open */

    /* ETag tracking for cache invalidation */
    char etag[128];                     /* Current blob ETag */
    char snapshot[128];                 /* Last blob snapshot datetime (for incremental diff) */
    int cacheReuse;                     /* 1 = persistent cache with ETag validation */

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

    /* Download counter — incremented each time a full blob GET occurs.
    ** ETag-based cache hits do NOT increment this. */
    int nDownloads;

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

    /* WAL parallel upload config (set via PRAGMA sqlite_objs_wal_parallel) */
    int walParallelUpload;              /* 1 = use parallel Put Block path */
    int walChunkSize;                   /* Chunk size in bytes (0 = default) */

    /* R1: Per-journal blob existence cache (one entry per database).
    ** Since we are the single writer, we know when a journal blob exists
    ** because we created (xSync) or deleted (xDelete) it ourselves.
    ** This eliminates ~4 HEAD requests per transaction (~110ms saved). */
    sqliteObjsJournalCacheEntry journalCache[SQLITE_OBJS_MAX_JOURNAL_CACHE];
    int nJournalCache;
    pthread_mutex_t journalCacheMutex;  /* Protects journalCache array */
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
** Bitmap operations — generic API for page-level bit tracking
**
** Both dirty and valid bitmaps use this common implementation.
** =================================================================== */

/* Number of bytes needed for a bitmap covering fileSize/pageSize pages. */
static int bitmapSize(sqlite3_int64 fileSize, int pageSize) {
    if (fileSize <= 0 || pageSize <= 0) return 0;
    int nPages = (int)((fileSize + pageSize - 1) / pageSize);
    return (nPages + 7) / 8;
}

/* Set a single bit by page index. */
static void bitmapSetBit(Bitmap *bm, int pageIdx) {
    if (!bm->data) return;
    int byteIdx = pageIdx / 8;
    int bitIdx = pageIdx % 8;
    if (byteIdx < 0 || byteIdx >= bm->nAlloc) return;
    if (!(bm->data[byteIdx] & (1 << bitIdx))) {
        bm->data[byteIdx] |= (1 << bitIdx);
        bm->nSet++;
    }
}

/* Test a single bit by page index. */
static int bitmapTestBit(const Bitmap *bm, int pageIdx) {
    if (!bm->data) return 0;
    int byteIdx = pageIdx / 8;
    int bitIdx = pageIdx % 8;
    if (byteIdx < 0 || byteIdx >= bm->nAlloc) return 0;
    return (bm->data[byteIdx] & (1 << bitIdx)) != 0;
}

/* Clear a single bit by page index. */
static void bitmapClearBit(Bitmap *bm, int pageIdx) {
    if (!bm->data) return;
    int byteIdx = pageIdx / 8;
    int bitIdx = pageIdx % 8;
    if (byteIdx < 0 || byteIdx >= bm->nAlloc) return;
    if (bm->data[byteIdx] & (1 << bitIdx)) {
        bm->data[byteIdx] &= ~(1 << bitIdx);
        bm->nSet--;
    }
}

/* Clear all bits. */
static void bitmapClearAll(Bitmap *bm) {
    if (bm->data && bm->nAlloc > 0) {
        memset(bm->data, 0, bm->nAlloc);
    }
    bm->nSet = 0;
}

/* Set all bits for the first nPages pages. */
static void bitmapSetAll(Bitmap *bm, int nPages) {
    if (!bm->data || bm->nAlloc <= 0) return;
    int fullBytes = nPages / 8;
    int remainBits = nPages % 8;
    if (fullBytes > bm->nAlloc) fullBytes = bm->nAlloc;
    if (fullBytes > 0) memset(bm->data, 0xFF, fullBytes);
    if (remainBits > 0 && fullBytes < bm->nAlloc) {
        bm->data[fullBytes] = (unsigned char)((1 << remainBits) - 1);
    }
    bm->nSet = nPages;
}

/* Set bits for page range [startPage, endPage). */
static void bitmapSetRange(Bitmap *bm, int startPage, int endPage) {
    if (!bm->data) return;
    for (int i = startPage; i < endPage; i++) {
        int byteIdx = i / 8;
        int bitIdx = i % 8;
        if (byteIdx < 0 || byteIdx >= bm->nAlloc) break;
        if (!(bm->data[byteIdx] & (1 << bitIdx))) {
            bm->data[byteIdx] |= (1 << bitIdx);
            bm->nSet++;
        }
    }
}

/* Returns 1 if any bit is set. */
static int bitmapHasAny(const Bitmap *bm) {
    if (!bm->data) return 0;
    for (int i = 0; i < bm->nAlloc; i++) {
        if (bm->data[i]) return 1;
    }
    return 0;
}

/* Grow bitmap to accommodate fileSize/pageSize pages. */
static int bitmapEnsureCapacity(Bitmap *bm, sqlite3_int64 fileSize,
                                int pageSize) {
    int needed = bitmapSize(fileSize, pageSize);
    if (needed <= 0) return SQLITE_OK;
    if (needed <= bm->nAlloc) return SQLITE_OK;
    unsigned char *pNew = (unsigned char *)realloc(bm->data, needed);
    if (!pNew) return SQLITE_NOMEM;
    memset(pNew + bm->nAlloc, 0, needed - bm->nAlloc);
    bm->data = pNew;
    bm->nAlloc = needed;
    return SQLITE_OK;
}

/* Free bitmap memory. */
static void bitmapFree(Bitmap *bm) {
    free(bm->data);
    bm->data = NULL;
    bm->nSet = 0;
    bm->nAlloc = 0;
}


/* ===================================================================
** Convenience wrappers — offset-based dirty/valid page operations
**
** These translate byte offsets to page indices and delegate to the
** generic Bitmap API, keeping call sites concise.
** =================================================================== */

/* Mark page dirty by byte offset. */
static void dirtyMarkPage(sqliteObjsFile *p, sqlite3_int64 offset) {
    if (p->pageSize <= 0) return;
    bitmapSetBit(&p->dirty, (int)(offset / p->pageSize));
}

/* Mark page valid by byte offset. */
static void validMarkPage(sqliteObjsFile *p, sqlite3_int64 offset) {
    if (p->pageSize <= 0) return;
    bitmapSetBit(&p->valid, (int)(offset / p->pageSize));
}

/* Mark byte range [startOffset, endOffset) as valid. */
static void validMarkRange(sqliteObjsFile *p, sqlite3_int64 startOffset,
                           sqlite3_int64 endOffset) {
    if (p->pageSize <= 0) return;
    int startPage = (int)(startOffset / p->pageSize);
    int endPage = (int)((endOffset + p->pageSize - 1) / p->pageSize);
    bitmapSetRange(&p->valid, startPage, endPage);
}

/* Mark all pages valid up to current file size. */
static void validMarkAll(sqliteObjsFile *p) {
    if (p->pageSize <= 0) return;
    int nPages = (int)((p->nData + p->pageSize - 1) / p->pageSize);
    bitmapSetAll(&p->valid, nPages);
}


/* ===================================================================
** Helper: cache file management for MAIN_DB
** =================================================================== */

/* Grow both dirty and valid bitmaps together. */
static int bitmapsEnsureCapacity(sqliteObjsFile *p) {
    int rc = bitmapEnsureCapacity(&p->dirty, p->nData, p->pageSize);
    if (rc != SQLITE_OK) return rc;
    if (p->prefetchMode == SQLITE_OBJS_PREFETCH_NONE) {
        rc = bitmapEnsureCapacity(&p->valid, p->nData, p->pageSize);
    }
    return rc;
}

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
    
    return bitmapsEnsureCapacity(p);
}

/* ===================================================================
** Helper: ETag sidecar file operations for cache reuse
** =================================================================== */

/*
** Build a sidecar path by replacing the .cache suffix with a new extension.
** E.g. buildSidecarPath("/tmp/foo.cache", ".etag") → "/tmp/foo.etag"
** Returns heap-allocated string.  Caller must free().
*/
static char *buildSidecarPath(const char *cachePath, const char *ext) {
    if (!cachePath || !ext) return NULL;
    size_t len = strlen(cachePath);
    size_t extLen = strlen(ext);
    const char *suffix = ".cache";
    size_t suffixLen = 6;
    if (len >= suffixLen && strcmp(cachePath + len - suffixLen, suffix) == 0) {
        size_t baseLen = len - suffixLen;
        char *path = (char *)malloc(baseLen + extLen + 1);
        if (!path) return NULL;
        memcpy(path, cachePath, baseLen);
        memcpy(path + baseLen, ext, extLen + 1);
        return path;
    }
    /* Fallback: append extension */
    char *path = (char *)malloc(len + extLen + 1);
    if (!path) return NULL;
    memcpy(path, cachePath, len);
    memcpy(path + len, ext, extLen + 1);
    return path;
}

static char *buildEtagPath(const char *cachePath) {
    return buildSidecarPath(cachePath, ".etag");
}

/*
** Read stored ETag from sidecar file.
** Returns 0 on success with buf filled, -1 on failure.
*/
static int readEtagFile(const char *path, char *buf, size_t bufSize) {
    if (!path || !buf || bufSize == 0) return -1;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, bufSize - 1);
    close(fd);
    if (n <= 0) return -1;
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r' || buf[n-1] == ' '))
        n--;
    buf[n] = '\0';
    return 0;
}

/*
** Write ETag to sidecar file derived from cachePath (.cache → .etag).
** Returns 0 on success, -1 on failure.
*/
static int writeEtagFile(const char *cachePath, const char *etag) {
    if (!cachePath || !etag || !etag[0]) return -1;
    char *etagPath = buildEtagPath(cachePath);
    if (!etagPath) return -1;
    int fd = open(etagPath, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { free(etagPath); return -1; }
    size_t len = strlen(etag);
    ssize_t nw = write(fd, etag, len);
    fsync(fd);
    close(fd);
    free(etagPath);
    return (nw == (ssize_t)len) ? 0 : -1;
}

/*
** Remove a sidecar file by extension.
*/
static void unlinkSidecarFile(const char *cachePath, const char *ext) {
    if (!cachePath) return;
    char *path = buildSidecarPath(cachePath, ext);
    if (path) {
        unlink(path);
        free(path);
    }
}

static void unlinkEtagFile(const char *cachePath) {
    unlinkSidecarFile(cachePath, ".etag");
}

static char *buildSnapshotPath(const char *cachePath) {
    return buildSidecarPath(cachePath, ".snapshot");
}

/*
** Read stored snapshot datetime from sidecar file.
** Returns 0 on success, -1 on failure.
*/
static int readSnapshotFile(const char *cachePath, char *buf, size_t bufSize) {
    if (!cachePath || !buf || bufSize == 0) return -1;
    char *snapPath = buildSnapshotPath(cachePath);
    if (!snapPath) return -1;
    int fd = open(snapPath, O_RDONLY);
    free(snapPath);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, bufSize - 1);
    close(fd);
    if (n <= 0) return -1;
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r' || buf[n-1] == ' '))
        n--;
    buf[n] = '\0';
    return 0;
}

/*
** Write snapshot datetime to sidecar file.
** Returns 0 on success, -1 on failure.
*/
static int writeSnapshotFile(const char *cachePath, const char *snapshot) {
    if (!cachePath || !snapshot || !snapshot[0]) return -1;
    char *snapPath = buildSnapshotPath(cachePath);
    if (!snapPath) return -1;
    int fd = open(snapPath, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { free(snapPath); return -1; }
    size_t len = strlen(snapshot);
    ssize_t nw = write(fd, snapshot, len);
    fsync(fd);
    close(fd);
    free(snapPath);
    return (nw == (ssize_t)len) ? 0 : -1;
}

static void unlinkSnapshotFile(const char *cachePath) {
    unlinkSidecarFile(cachePath, ".snapshot");
}


/* ===================================================================
** Helper: .state sidecar file — valid bitmap persistence
**
** Format: "SQOS" (4) | version (4, LE) | page_size (4, LE) |
**         file_size (8, LE) | bitmap_size (4, LE) | bitmap (N) |
**         crc32 (4, LE)
** =================================================================== */

static char *buildStatePath(const char *cachePath) {
    return buildSidecarPath(cachePath, ".state");
}

/* CRC32 (ISO 3309 / zlib polynomial) for .state file integrity */
static uint32_t stateCrc32(const unsigned char *data, size_t len) {
    static const uint32_t table[256] = {
        0x00000000,0x77073096,0xEE0E612C,0x990951BA,0x076DC419,0x706AF48F,
        0xE963A535,0x9E6495A3,0x0EDB8832,0x79DCB8A4,0xE0D5E91B,0x97D2D988,
        0x09B64C2B,0x7EB17CBB,0xE7B82D09,0x90BF1D91,0x1DB71064,0x6AB020F2,
        0xF3B97148,0x84BE41DE,0x1ADAD47D,0x6DDDE4EB,0xF4D4B551,0x83D385C7,
        0x136C9856,0x646BA8C0,0xFD62F97A,0x8A65C9EC,0x14015C4F,0x63066CD9,
        0xFA0F3D63,0x8D080DF5,0x3B6E20C8,0x4C69105E,0xD56041E4,0xA2677172,
        0x3C03E4D1,0x4B04D447,0xD20D85FD,0xA50AB56B,0x35B5A8FA,0x42B2986C,
        0xDBBDB8D6,0xACBCBEC8,0x32D86CE3,0x45DF5C75,0xDCD60DCF,0xABD13D59,
        0x26D930AC,0x51DE003A,0xC8D75180,0xBFD06116,0x21B4F4B5,0x56B3C423,
        0xCFBA9599,0xB8BDA50F,0x2802B89E,0x5F058808,0xC60CD9B2,0xB10BE924,
        0x2F6F7C87,0x58684C11,0xC1611DAB,0xB6662D3D,0x76DC4190,0x01DB7106,
        0x98D220BC,0xEFD5102A,0x71B18589,0x06B6B51F,0x9FBFE4A5,0xE8B8D433,
        0x7807C9A2,0x0F00F934,0x9609A88E,0xE10E9818,0x7F6A0D69,0x086D3D2B,
        0x91646C97,0xE6635C01,0x6B6B51F4,0x1C6C6162,0x856530D8,0xF262004E,
        0x6C0695ED,0x1B01A57B,0x8208F4C1,0xF50FC457,0x65B0D9C6,0x12B7E950,
        0x8BBEB8EA,0xFCB9887C,0x62DD1DDF,0x15DA2D49,0x8CD37CF3,0xFBD44C65,
        0x4DB26158,0x3AB551CE,0xA3BC0074,0xD4BB30E2,0x4ADFA541,0x3DD895D7,
        0xA4D1C46D,0xD3D6F4DB,0x4369E96A,0x346ED9FC,0xAD678846,0xDA60B8D0,
        0x44042D73,0x33031DE5,0xAA0A4C5F,0xDD0D7822,0x3B6E20C8,0x4C69105E,
        0xD56041E4,0xA2677172,0x3C03E4D1,0x4B04D447,0xD20D85FD,0xA50AB56B,
        0x35B5A8FA,0x42B2986C,0xDBBDB8D6,0xACBCBEC8,0x32D86CE3,0x45DF5C75,
        0xDCD60DCF,0xABD13D59,0x26D930AC,0x51DE003A,0xC8D75180,0xBFD06116,
        0x21B4F4B5,0x56B3C423,0xCFBA9599,0xB8BDA50F,0x2802B89E,0x5F058808,
        0xC60CD9B2,0xB10BE924,0x2F6F7C87,0x58684C11,0xC1611DAB,0xB6662D3D,
        0x76DC4190,0x01DB7106,0x98D220BC,0xEFD5102A,0x71B18589,0x06B6B51F,
        0x9FBFE4A5,0xE8B8D433,0x7807C9A2,0x0F00F934,0x9609A88E,0xE10E9818,
        0x7F6A0D69,0x086D3D2B,0x91646C97,0xE6635C01,0x6B6B51F4,0x1C6C6162,
        0x856530D8,0xF262004E,0x6C0695ED,0x1B01A57B,0x8208F4C1,0xF50FC457,
        0x65B0D9C6,0x12B7E950,0x8BBEB8EA,0xFCB9887C,0x62DD1DDF,0x15DA2D49,
        0x8CD37CF3,0xFBD44C65,0x4DB26158,0x3AB551CE,0xA3BC0074,0xD4BB30E2,
        0x4ADFA541,0x3DD895D7,0xA4D1C46D,0xD3D6F4DB,0x4369E96A,0x346ED9FC,
        0xAD678846,0xDA60B8D0,0x44042D73,0x33031DE5,0xAA0A4C5F,0xDD0D7822,
        0x9B64C2B0,0xEC63F226,0x756AA39C,0x026D930A,0x9C0906A9,0xEB0E363F,
        0x72076785,0x05005713,0x95BF4A82,0xE2B87A14,0x7BB12BAE,0x0CB61B38,
        0x92D28E9B,0xE5D5BE0D,0x7CDCEFB7,0x0BDBDF21,0x86D3D2D4,0xF1D4E242,
        0x68DDB3F6,0x1FDA836E,0x81BE16CD,0xF6B9265B,0x6FB077E1,0x18B74777,
        0x88085AE6,0xFF0F6B70,0x66063BCA,0x11010B5C,0x8F659EFF,0xF862AE69,
        0x616BFFD3,0x166CCF45,0xA00AE278,0xD70DD2EE,0x4E048354,0x3903B3C2,
        0xA7672661,0xD06016F7,0x4969474D,0x3E6E77DB,0xAED16A4A,0xD9D65ADC,
        0x40DF0B66,0x37D83BF0,0xA9BCAE53,0xDEBB9EC5,0x47B2CF7F,0x30B5FFE9,
        0xBDBDF21C,0xCABAC28A,0x53B39330,0x24B4A3A6,0xBAD03605,0xCDD706FF,
        0x54DE5729,0x23D967BF,0xB3667A2E,0xC4614AB8,0x5D681B02,0x2A6F2B94,
        0xB40BBE37,0xC30C8EA1,0x5A05DF1B,0x2D02EF8D
    };
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFF;
}

/* Little-endian read/write helpers */
static void writeLE32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)(v);
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}
static uint32_t readLE32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1]<<8) |
           ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24);
}
static void writeLE64(unsigned char *p, uint64_t v) {
    for (int i = 0; i < 8; i++) {
        p[i] = (unsigned char)(v >> (i*8));
    }
}
static uint64_t readLE64(const unsigned char *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v |= (uint64_t)p[i] << (i*8);
    }
    return v;
}

/*
** Write .state sidecar file with valid bitmap.
** Uses atomic rename: write to .state.tmp, fsync, rename to .state.
** Returns 0 on success, -1 on failure.
*/
static int writeStateFile(const char *cachePath, int pageSize,
                          sqlite3_int64 fileSize,
                          const unsigned char *aValid, int nValidBytes) {
    if (!cachePath || !aValid || nValidBytes <= 0) return -1;
    char *statePath = buildStatePath(cachePath);
    if (!statePath) return -1;

    /* Build temp path for atomic rename */
    size_t spLen = strlen(statePath);
    char *tmpPath = (char *)malloc(spLen + 5);  /* ".tmp\0" */
    if (!tmpPath) { free(statePath); return -1; }
    memcpy(tmpPath, statePath, spLen);
    memcpy(tmpPath + spLen, ".tmp", 5);

    /* Header: magic(4) + version(4) + pageSize(4) + fileSize(8) + bitmapSize(4) = 24 */
    size_t headerSize = 24;
    size_t totalSize = headerSize + (size_t)nValidBytes + 4; /* +4 for CRC32 */

    unsigned char *buf = (unsigned char *)malloc(totalSize);
    if (!buf) { free(tmpPath); free(statePath); return -1; }

    /* Write header */
    memcpy(buf, SQLITE_OBJS_STATE_MAGIC, 4);
    writeLE32(buf + 4, SQLITE_OBJS_STATE_VERSION);
    writeLE32(buf + 8, (uint32_t)pageSize);
    writeLE64(buf + 12, (uint64_t)fileSize);
    writeLE32(buf + 20, (uint32_t)nValidBytes);

    /* Write bitmap */
    memcpy(buf + headerSize, aValid, (size_t)nValidBytes);

    /* Write CRC32 over everything before the CRC field */
    uint32_t crc = stateCrc32(buf, headerSize + (size_t)nValidBytes);
    writeLE32(buf + headerSize + (size_t)nValidBytes, crc);

    int fd = open(tmpPath, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { free(buf); free(tmpPath); free(statePath); return -1; }

    ssize_t nw = write(fd, buf, totalSize);
    free(buf);
    if (nw != (ssize_t)totalSize) {
        close(fd);
        unlink(tmpPath);
        free(tmpPath);
        free(statePath);
        return -1;
    }
    if (fsync(fd) != 0) {
        close(fd);
        unlink(tmpPath);
        free(tmpPath);
        free(statePath);
        return -1;
    }
    close(fd);

    /* Atomic rename */
    if (rename(tmpPath, statePath) != 0) {
        unlink(tmpPath);
        free(tmpPath);
        free(statePath);
        return -1;
    }

    free(tmpPath);
    free(statePath);
    return 0;
}

/*
** Read .state sidecar file.
** On success, allocates *aValidOut (caller must free) and fills *nValidOut,
** *pageSizeOut, *fileSizeOut.
** Returns 0 on success, -1 on failure (missing, corrupt, version mismatch).
*/
static int readStateFile(const char *cachePath, unsigned char **aValidOut,
                         int *nValidOut, int *pageSizeOut,
                         sqlite3_int64 *fileSizeOut) {
    if (!cachePath) return -1;
    char *statePath = buildStatePath(cachePath);
    if (!statePath) return -1;

    int fd = open(statePath, O_RDONLY);
    free(statePath);
    if (fd < 0) return -1;

    /* Read header (24 bytes) */
    unsigned char header[24];
    ssize_t nr = read(fd, header, 24);
    if (nr != 24) { close(fd); return -1; }

    /* Validate magic */
    if (memcmp(header, SQLITE_OBJS_STATE_MAGIC, 4) != 0) { close(fd); return -1; }

    /* Validate version */
    uint32_t version = readLE32(header + 4);
    if (version != SQLITE_OBJS_STATE_VERSION) { close(fd); return -1; }

    uint32_t ps = readLE32(header + 8);
    uint64_t fs = readLE64(header + 12);
    uint32_t bitmapSize = readLE32(header + 20);

    /* Sanity checks */
    if (ps < 512 || ps > 65536 || (ps & (ps-1)) != 0) { close(fd); return -1; }
    if (bitmapSize == 0 || bitmapSize > (1024*1024)) { close(fd); return -1; }

    /* Read bitmap + CRC */
    unsigned char *payload = (unsigned char *)malloc(bitmapSize + 4);
    if (!payload) { close(fd); return -1; }
    nr = read(fd, payload, bitmapSize + 4);
    close(fd);
    if (nr != (ssize_t)(bitmapSize + 4)) { free(payload); return -1; }

    /* Verify CRC32 */
    uint32_t storedCrc = readLE32(payload + bitmapSize);
    /* CRC is over header + bitmap */
    unsigned char *crcBuf = (unsigned char *)malloc(24 + bitmapSize);
    if (!crcBuf) { free(payload); return -1; }
    memcpy(crcBuf, header, 24);
    memcpy(crcBuf + 24, payload, bitmapSize);
    uint32_t computedCrc = stateCrc32(crcBuf, 24 + bitmapSize);
    free(crcBuf);

    if (storedCrc != computedCrc) { free(payload); return -1; }

    /* Success — extract bitmap */
    unsigned char *bitmap = (unsigned char *)malloc(bitmapSize);
    if (!bitmap) { free(payload); return -1; }
    memcpy(bitmap, payload, bitmapSize);
    free(payload);

    *aValidOut = bitmap;
    *nValidOut = (int)bitmapSize;
    *pageSizeOut = (int)ps;
    *fileSizeOut = (sqlite3_int64)fs;
    return 0;
}

static void unlinkStateFile(const char *cachePath) {
    unlinkSidecarFile(cachePath, ".state");
}

/* Forward declaration */
static int detectPageSize(const unsigned char *aData, sqlite3_int64 nData);

/*
** Fetch a window of pages from Azure into the local cache file.
** Starting at pageIdx, fetches up to SQLITE_OBJS_READAHEAD_PAGES pages
** (the readahead window).  Writes data to cache file and marks pages valid.
**
** Returns SQLITE_OK on success, or SQLITE_IOERR_READ on failure.
*/
static int fetchPagesFromAzure(sqliteObjsFile *p, int pageIdx) {
    if (!p->ops || !p->ops->page_blob_read) return SQLITE_IOERR_READ;
    if (p->pageSize <= 0 || p->cacheFd < 0) return SQLITE_IOERR_READ;

    int64_t startOffset = (int64_t)pageIdx * p->pageSize;
    int64_t endOffset = startOffset + (int64_t)SQLITE_OBJS_READAHEAD_PAGES * p->pageSize;
    if (endOffset > p->nData) endOffset = p->nData;
    if (startOffset >= p->nData) return SQLITE_OK;  /* Past EOF — nothing to fetch */

    size_t fetchLen = (size_t)(endOffset - startOffset);
    if (fetchLen == 0) return SQLITE_OK;

    double t0 = 0;
    if (sqlite_objs_debug_timing()) t0 = sqlite_objs_time_ms();

    azure_buffer_t rbuf;
    azure_buffer_init(&rbuf);
    azure_error_t aerr;
    azure_error_init(&aerr);

    azure_err_t arc = p->ops->page_blob_read(
        p->ops_ctx, p->zBlobName,
        startOffset, fetchLen, &rbuf, &aerr);

    if (arc != AZURE_OK) {
        azure_buffer_free(&rbuf);
        return SQLITE_IOERR_READ;
    }

    if (!rbuf.data || rbuf.size == 0) {
        azure_buffer_free(&rbuf);
        return SQLITE_IOERR_READ;
    }

    /* Write to cache file */
    size_t writeLen = rbuf.size < fetchLen ? rbuf.size : fetchLen;
    ssize_t nw = pwrite(p->cacheFd, rbuf.data, writeLen, (off_t)startOffset);
    azure_buffer_free(&rbuf);

    if (nw < 0 || (size_t)nw != writeLen) {
        return SQLITE_IOERR_WRITE;
    }

    /* Mark fetched pages valid */
    validMarkRange(p, startOffset, startOffset + (sqlite3_int64)writeLen);

    if (sqlite_objs_debug_timing()) {
        double elapsed = sqlite_objs_time_ms() - t0;
        fprintf(stderr, "[TIMING] fetchPagesFromAzure: %.1fms "
                "(page=%d, %zu bytes, blob=%s)\n",
                elapsed, pageIdx, writeLen, p->zBlobName);
    }

    return SQLITE_OK;
}

/*
** Download all invalid pages in the valid bitmap.
** Coalesces contiguous invalid ranges for efficiency.
** Used by PRAGMA sqlite_objs_prefetch.
**
** Returns SQLITE_OK on success, or an error code on failure.
*/
static int prefetchInvalidPages(sqliteObjsFile *p) {
    if (!p->valid.data || p->pageSize <= 0 || p->nData <= 0) return SQLITE_OK;
    if (!p->ops || !p->ops->page_blob_read) return SQLITE_IOERR;

    int nPages = (int)((p->nData + p->pageSize - 1) / p->pageSize);
    int fetchCount = 0;
    int i = 0;

    while (i < nPages) {
        /* Skip valid pages */
        if (bitmapTestBit(&p->valid, i)) { i++; continue; }

        /* Found an invalid page — start a contiguous range */
        int rangeStart = i;
        while (i < nPages && !bitmapTestBit(&p->valid, i)) i++;
        int rangeEnd = i;

        /* Fetch this range */
        int64_t startOffset = (int64_t)rangeStart * p->pageSize;
        int64_t endOffset = (int64_t)rangeEnd * p->pageSize;
        if (endOffset > p->nData) endOffset = p->nData;
        size_t fetchLen = (size_t)(endOffset - startOffset);
        if (fetchLen == 0) continue;

        azure_buffer_t rbuf;
        azure_buffer_init(&rbuf);
        azure_error_t aerr;
        azure_error_init(&aerr);

        azure_err_t arc = p->ops->page_blob_read(
            p->ops_ctx, p->zBlobName,
            startOffset, fetchLen, &rbuf, &aerr);

        if (arc != AZURE_OK) {
            azure_buffer_free(&rbuf);
            return SQLITE_IOERR_READ;
        }
        if (!rbuf.data || rbuf.size == 0) {
            azure_buffer_free(&rbuf);
            return SQLITE_IOERR_READ;
        }

        size_t writeLen = rbuf.size < fetchLen ? rbuf.size : fetchLen;
        ssize_t nw = pwrite(p->cacheFd, rbuf.data, writeLen, (off_t)startOffset);
        azure_buffer_free(&rbuf);

        if (nw < 0 || (size_t)nw != writeLen) {
            return SQLITE_IOERR_WRITE;
        }

        validMarkRange(p, startOffset, startOffset + (sqlite3_int64)writeLen);
        fetchCount++;
    }

    if (sqlite_objs_debug_timing() && fetchCount > 0) {
        fprintf(stderr, "[TIMING] prefetchInvalidPages: %d ranges fetched (blob=%s)\n",
                fetchCount, p->zBlobName);
    }

    return SQLITE_OK;
}

/*
** Apply incremental diff from a previous snapshot to the local cache file.
** Downloads only the changed page ranges rather than re-downloading the full blob.
**
** Parameters:
**   p          — the open MAIN_DB file (must have cacheFd, ops, snapshot set)
**   blobSize   — the current blob size from Azure
**   label      — label for timing messages (e.g. "xOpen" or "revalidateAfterLease")
**
** Returns SQLITE_OK on success, or negative on failure (caller should fall back
** to full re-download).
*/
static int applyIncrementalDiff(sqliteObjsFile *p, int64_t blobSize, const char *label) {
    if (p->snapshot[0] == '\0') return -1;
    if (!p->ops || !p->ops->blob_get_page_ranges_diff || !p->ops->page_blob_read) return -1;

    double t0 = 0;
    if (sqlite_objs_debug_timing()) t0 = sqlite_objs_time_ms();

    azure_diff_range_t *diffRanges = NULL;
    int diffCount = 0;
    azure_error_t aerr;
    azure_error_init(&aerr);

    azure_err_t arc = p->ops->blob_get_page_ranges_diff(
        p->ops_ctx, p->zBlobName, p->snapshot,
        &diffRanges, &diffCount, &aerr);

    if (arc != AZURE_OK) {
        free(diffRanges);
        if (sqlite_objs_debug_timing()) {
            fprintf(stderr, "[TIMING] %s: page ranges diff failed (code=%d) "
                    "— falling back to full re-download (blob=%s)\n",
                    label, arc, p->zBlobName);
        }
        return -1;
    }

    int diffOk = 1;
    int64_t totalDiffBytes = 0;

    /* Resize cache file if blob grew */
    if (blobSize > p->nData) {
        if (ftruncate(p->cacheFd, (off_t)blobSize) != 0) {
            diffOk = 0;
        }
    }

    /* Apply each changed/cleared range to the cache file */
    for (int i = 0; i < diffCount && diffOk; i++) {
        int64_t rangeStart = diffRanges[i].start;
        int64_t rangeLen = diffRanges[i].end - rangeStart + 1;
        if (rangeLen <= 0) continue;
        totalDiffBytes += rangeLen;

        if (diffRanges[i].is_clear) {
            /* ClearRange: write zeros */
            unsigned char *zeros = (unsigned char *)calloc(1, (size_t)rangeLen);
            if (!zeros) { diffOk = 0; break; }
            ssize_t nw = pwrite(p->cacheFd, zeros, (size_t)rangeLen,
                                (off_t)rangeStart);
            free(zeros);
            if (nw != (ssize_t)rangeLen) { diffOk = 0; break; }
            /* Mark these pages valid (they contain the correct zeros) */
            if (p->valid.data) {
                validMarkRange(p, rangeStart, rangeStart + rangeLen);
            }
        } else {
            /* PageRange: download and patch */
            azure_buffer_t rbuf;
            azure_buffer_init(&rbuf);
            azure_error_t rerr;
            azure_error_init(&rerr);

            azure_err_t rrc = p->ops->page_blob_read(
                p->ops_ctx, p->zBlobName,
                rangeStart, (size_t)rangeLen, &rbuf, &rerr);

            if (rrc != AZURE_OK || !rbuf.data || (int64_t)rbuf.size < rangeLen) {
                azure_buffer_free(&rbuf);
                diffOk = 0;
                break;
            }

            ssize_t nw = pwrite(p->cacheFd, rbuf.data, (size_t)rangeLen,
                                (off_t)rangeStart);
            azure_buffer_free(&rbuf);
            if (nw != (ssize_t)rangeLen) { diffOk = 0; break; }
            /* Mark these pages valid (freshly downloaded) */
            if (p->valid.data) {
                validMarkRange(p, rangeStart, rangeStart + rangeLen);
            }
        }
    }

    free(diffRanges);

    if (!diffOk) {
        if (sqlite_objs_debug_timing()) {
            fprintf(stderr, "[TIMING] %s: incremental diff failed "
                    "— falling back to full re-download (blob=%s)\n",
                    label, p->zBlobName);
        }
        return -1;
    }

    /* Truncate if blob shrank */
    if (blobSize < p->nData) {
        ftruncate(p->cacheFd, (off_t)blobSize);
    }
    fsync(p->cacheFd);
    p->nData = blobSize;
    p->nDownloads++;

    /* Re-detect page size */
    unsigned char header[100];
    ssize_t nRead = pread(p->cacheFd, header, sizeof(header), 0);
    if (nRead == (ssize_t)sizeof(header)) {
        int detected = detectPageSize(header, sizeof(header));
        if (detected > 0) p->pageSize = detected;
    }

    bitmapsEnsureCapacity(p);
    bitmapClearAll(&p->dirty);
    p->lastSyncedSize = p->nData;

    if (sqlite_objs_debug_timing()) {
        double elapsed = sqlite_objs_time_ms() - t0;
        fprintf(stderr, "[TIMING] %s: incremental diff "
                "%.1fms (%d ranges, %lld diff bytes, %lld total bytes, blob=%s)\n",
                label, elapsed, diffCount, (long long)totalDiffBytes,
                (long long)blobSize, p->zBlobName);
    }
    return SQLITE_OK;
}

/*
** Grow a heap buffer with exponential doubling.
** On success, *ppBuf is updated and the new region is zeroed.
*/
static int bufferEnsure(unsigned char **ppBuf, sqlite3_int64 *pAlloc,
                        sqlite3_int64 newSize) {
    if (newSize <= *pAlloc) return SQLITE_OK;
    sqlite3_int64 alloc = *pAlloc;
    if (alloc == 0) alloc = SQLITE_OBJS_INITIAL_ALLOC;
    while (alloc < newSize) {
        alloc *= 2;
        if (alloc < 0) return SQLITE_NOMEM;
    }
    unsigned char *pNew = (unsigned char *)realloc(*ppBuf, (size_t)alloc);
    if (!pNew) return SQLITE_NOMEM;
    memset(pNew + *pAlloc, 0, (size_t)(alloc - *pAlloc));
    *ppBuf = pNew;
    *pAlloc = alloc;
    return SQLITE_OK;
}

static int jrnlBufferEnsure(sqliteObjsFile *p, sqlite3_int64 newSize) {
    return bufferEnsure(&p->aJrnlData, &p->nJrnlAlloc, newSize);
}

static int walBufferEnsure(sqliteObjsFile *p, sqlite3_int64 newSize) {
    return bufferEnsure(&p->aWalData, &p->nWalAlloc, newSize);
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
    if (p->dirty.nSet > 0 && p->ops && p->ops->page_blob_write) {
        rc = sqliteObjsSync(pFile, 0);
    } else if (p->eFileType == SQLITE_OPEN_WAL &&
               p->nWalData > 0 && p->ops) {
        rc = sqliteObjsSync(pFile, 0);
    }

    /* Refresh ETag from Azure before persisting the sidecar.
    ** The batch write path may not reliably capture the final ETag
    ** (concurrent PUTs return ETags in indeterminate order), so a
    ** HEAD request gives us the definitive current ETag while we
    ** still hold the lease. */
    if (p->cacheReuse && p->ops && p->ops->blob_get_properties
        && p->zBlobName && rc == SQLITE_OK) {
        azure_error_t aerr;
        azure_error_init(&aerr);
        azure_err_t arc = p->ops->blob_get_properties(
            p->ops_ctx, p->zBlobName, NULL, NULL, NULL, &aerr);
        if (arc == AZURE_OK && aerr.etag[0] != '\0') {
            memcpy(p->etag, aerr.etag, sizeof(p->etag));
        }
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

    /* Close and conditionally delete cache file for MAIN_DB */
    if (p->cacheFd >= 0) {
        fsync(p->cacheFd);  /* cache fsync before .state write */
        close(p->cacheFd);
        p->cacheFd = -1;
    }
    if (p->zCachePath) {
        if (p->cacheReuse && p->etag[0] != '\0' && !bitmapHasAny(&p->dirty)) {
            /* Write order: .state → .etag (per design decision) */
            /* Persist valid bitmap for lazy cache reuse */
            if (p->valid.data && p->valid.nAlloc > 0) {
                writeStateFile(p->zCachePath, p->pageSize, p->nData,
                               p->valid.data, p->valid.nAlloc);
            }
            /* Cache is clean and we have a valid ETag — persist for reuse */
            writeEtagFile(p->zCachePath, p->etag);
            /* Persist snapshot for incremental diff on reconnect */
            if (p->snapshot[0] != '\0') {
                writeSnapshotFile(p->zCachePath, p->snapshot);
            }
            /* DON'T unlink the cache file */
        } else {
            /* Default behavior: clean up */
            unlink(p->zCachePath);
            unlinkEtagFile(p->zCachePath);
            unlinkSnapshotFile(p->zCachePath);
            unlinkStateFile(p->zCachePath);
        }
        free(p->zCachePath);
        p->zCachePath = NULL;
    }
    
    bitmapFree(&p->dirty);
    bitmapFree(&p->valid);
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
** Read from an in-memory buffer with zero-fill on short reads.
** Used by xRead for WAL and journal files.
*/
static int bufferRead(const unsigned char *src, sqlite3_int64 srcLen,
                      void *pBuf, int iAmt, sqlite3_int64 iOfst) {
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
        return bufferRead(p->aWalData, p->nWalData, pBuf, iAmt, iOfst);
    }
    
    if (p->eFileType & SQLITE_OPEN_MAIN_JOURNAL) {
        g_xread_journal_count++;
        return bufferRead(p->aJrnlData, p->nJrnlData, pBuf, iAmt, iOfst);
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

    /* Lazy cache: check validity bitmap and fetch on miss */
    if (p->prefetchMode == SQLITE_OBJS_PREFETCH_NONE && p->valid.data
        && p->pageSize > 0) {
        sqlite3_int64 readEnd = iOfst + iAmt;
        if (readEnd > p->nData) readEnd = p->nData;
        int startPage = (int)(iOfst / p->pageSize);
        int endPage = (int)((readEnd + p->pageSize - 1) / p->pageSize);
        for (int pg = startPage; pg < endPage; pg++) {
            if (!bitmapTestBit(&p->valid, pg)) {
                int rc = fetchPagesFromAzure(p, pg);
                if (rc != SQLITE_OK) {
                    memset(pBuf, 0, iAmt);
                    return rc;
                }
            }
        }
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
    
    /* Update nData BEFORE cacheEnsureSize so bitmapsEnsureCapacity sizes the
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

    /* Mark affected pages dirty + valid */
    if (p->pageSize > 0) {
        sqlite3_int64 pageStart = (iOfst / p->pageSize) * p->pageSize;
        while (pageStart < end) {
            dirtyMarkPage(p, pageStart);
            if (p->valid.data) validMarkPage(p, pageStart);
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
        /* WAL truncate — reset buffer; next xSync uploads fresh */
        if (size == 0) {
            if (p->ops && p->ops->blob_delete) {
                azure_error_t aerr;
                azure_error_init(&aerr);
                p->ops->blob_delete(p->ops_ctx, p->zBlobName, &aerr);
            }
            p->nWalData = 0;
            if (p->aWalData) {
                memset(p->aWalData, 0, (size_t)p->nWalAlloc);
            }
        } else if (size < p->nWalData) {
            p->nWalData = size;
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
        /* Clear valid bits for pages beyond new size */
        if (p->valid.data && p->pageSize > 0) {
            int firstGone = (int)((size + p->pageSize - 1) / p->pageSize);
            int nPages = (int)((p->nData + p->pageSize - 1) / p->pageSize);
            for (int pg = firstGone; pg < nPages; pg++) {
                bitmapClearBit(&p->valid, pg);
            }
        }
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
    if (p->dirty.nSet == 0) return 0;
    if (!p->dirty.data || p->pageSize <= 0 || p->nData <= 0) return 0;

    int nPages = (int)((p->nData + p->pageSize - 1) / p->pageSize);
    int nRanges = 0;
    int i = 0;

    while (i < nPages) {
        /* Skip clean pages */
        if (!bitmapTestBit(&p->dirty, i)) { i++; continue; }

        /* Start of a dirty run */
        int64_t runStart = (int64_t)i * p->pageSize;
        int runPages = 0;

        while (i < nPages && bitmapTestBit(&p->dirty, i)) {
            runPages++;
            int64_t runBytes = (int64_t)runPages * p->pageSize;
            if (runBytes >= SQLITE_OBJS_MAX_PUT_PAGE) break;
            i++;
        }

        /* If we stopped because of the 4 MiB cap, advance past this page */
        if (i < nPages && bitmapTestBit(&p->dirty, i)
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
** For WAL: upload entire buffer as block blob.
*/
static int sqliteObjsSync(sqlite3_file *pFile, int flags) {
    sqliteObjsFile *p = (sqliteObjsFile *)pFile;
    azure_error_t aerr;
    (void)flags;

    if (p->eFileType == SQLITE_OPEN_WAL) {
        /* WAL sync — upload entire WAL as block blob */
        if (p->nWalData <= 0) {
            return SQLITE_OK;  /* Nothing to sync */
        }

        if (!p->ops || !p->ops->block_blob_upload) {
            return SQLITE_IOERR_FSYNC;
        }

        double t0 = 0;
        if (sqlite_objs_debug_timing()) t0 = sqlite_objs_time_ms();

        azure_error_init(&aerr);
        azure_err_t arc;

        if (p->pVfsData && p->pVfsData->walParallelUpload &&
            p->ops->block_blob_upload_parallel) {
            /* Parallel path: Put Block + Put Block List */
            int chunkSz = (p->pVfsData->walChunkSize > 0)
                        ? p->pVfsData->walChunkSize
                        : SQLITE_OBJS_WAL_DEFAULT_CHUNK;
            arc = p->ops->block_blob_upload_parallel(
                p->ops_ctx, p->zBlobName,
                p->aWalData, (size_t)p->nWalData,
                (size_t)chunkSz, &aerr);

            if (sqlite_objs_debug_timing()) {
                double elapsed = sqlite_objs_time_ms() - t0;
                int nChunks = ((int)p->nWalData + chunkSz - 1) / chunkSz;
                fprintf(stderr, "[TIMING] xSync(wal-parallel): %.1fms "
                        "(%lld bytes, chunks=%d, chunk_size=%d, blob=%s)\n",
                        elapsed, (long long)p->nWalData, nChunks,
                        chunkSz, p->zBlobName);
            }
        } else {
            /* Single PUT path */
            arc = p->ops->block_blob_upload(
                p->ops_ctx, p->zBlobName,
                p->aWalData, (size_t)p->nWalData, &aerr);

            if (sqlite_objs_debug_timing()) {
                double elapsed = sqlite_objs_time_ms() - t0;
                fprintf(stderr, "[TIMING] xSync(wal): %.1fms "
                        "(%lld bytes, blob=%s)\n",
                        elapsed, (long long)p->nWalData, p->zBlobName);
            }
        }

        if (arc != AZURE_OK) {
            return SQLITE_IOERR_FSYNC;
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
                pthread_mutex_lock(&p->pVfsData->journalCacheMutex);
                sqliteObjsJournalCacheEntry *jce =
                    journalCacheGetOrCreate(p->pVfsData, p->zBlobName);
                if (jce) jce->state = 1;
                pthread_mutex_unlock(&p->pVfsData->journalCacheMutex);
            }
        }
        return SQLITE_OK;
    }

    /* MAIN_DB — flush dirty pages */
    if (p->dirty.nSet == 0) return SQLITE_OK;
    if (!p->ops || !p->ops->page_blob_write) return SQLITE_IOERR_FSYNC;

    double sync_t0 = 0;
    if (sqlite_objs_debug_timing()) sync_t0 = sqlite_objs_time_ms();

    /* Record dirty page count for lease duration heuristic */
    int dirtyCountBeforeSync = p->dirty.nSet;

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
        maxRanges = p->dirty.nSet;
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
        bitmapClearAll(&p->dirty);
        goto sync_create_snapshot;
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
    bitmapClearAll(&p->dirty);

sync_create_snapshot:
    /* Create a blob snapshot after successful upload for incremental diff.
    ** This is best-effort — failure does not fail the sync. */
    if (p->cacheReuse && p->ops && p->ops->blob_snapshot_create) {
        char snap[128];
        azure_error_t snap_err;
        azure_error_init(&snap_err);
        azure_err_t snap_rc = p->ops->blob_snapshot_create(
            p->ops_ctx, p->zBlobName, snap, sizeof(snap), &snap_err);
        if (snap_rc == AZURE_OK && snap[0] != '\0') {
            memcpy(p->snapshot, snap, sizeof(p->snapshot));
            if (sqlite_objs_debug_timing()) {
                fprintf(stderr, "[TIMING] xSync: created snapshot %s for blob=%s\n",
                        p->snapshot, p->zBlobName);
            }
        } else if (sqlite_objs_debug_timing()) {
            fprintf(stderr, "[TIMING] xSync: snapshot creation failed (code=%d) "
                    "for blob=%s — incremental diff unavailable\n",
                    snap_rc, p->zBlobName);
        }
    }

    /* Persist .state sidecar after successful upload (best-effort) */
    if (p->cacheReuse && p->valid.data && p->valid.nAlloc > 0 && p->zCachePath) {
        writeStateFile(p->zCachePath, p->pageSize, p->nData,
                       p->valid.data, p->valid.nAlloc);
    }

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
** Re-validate the local cache after acquiring an Azure lease.
**
** Between xOpen (which downloads the blob without a lease) and xLock
** (which acquires the lease for writes), another client may have modified
** the blob.  If the blob's ETag has changed, re-download the entire blob
** into the cache file so that subsequent reads see the latest data.
**
** This prevents the stale-read race condition where a client reads
** data that was overwritten by a concurrent writer.
**
** Parameters:
**   p          — the open MAIN_DB file
**   leaseEtag  — ETag returned by the lease_acquire response (may be empty)
**
** Returns SQLITE_OK if cache is valid (or successfully refreshed),
** or an appropriate error code on failure.
*/
static int revalidateAfterLease(sqliteObjsFile *p, const char *leaseEtag) {
    /* Only applies to MAIN_DB files with cache and a stored ETag */
    if (p->eFileType != SQLITE_OPEN_MAIN_DB) return SQLITE_OK;
    if (p->cacheFd < 0) return SQLITE_OK;
    if (p->etag[0] == '\0') return SQLITE_OK;
    if (!p->ops) return SQLITE_OK;

    /* Fast path: compare ETag from the lease response */
    if (leaseEtag && leaseEtag[0] != '\0') {
        if (strcmp(p->etag, leaseEtag) == 0) {
            return SQLITE_OK;  /* Cache is still valid */
        }
    }

    /* Either no ETag from lease, or ETag mismatch — do a HEAD to confirm
    ** and get the current blob size (needed for re-download). */
    if (!p->ops->blob_get_properties) return SQLITE_OK;

    int64_t blobSize = 0;
    azure_error_t aerr;
    azure_error_init(&aerr);
    azure_err_t arc = p->ops->blob_get_properties(
        p->ops_ctx, p->zBlobName, &blobSize, NULL, NULL, &aerr);
    if (arc != AZURE_OK) {
        return azureErrToSqlite(arc, SQLITE_IOERR_READ);
    }

    /* Compare ETags */
    if (aerr.etag[0] == '\0' || strcmp(p->etag, aerr.etag) == 0) {
        return SQLITE_OK;  /* Cache is still valid */
    }

    /* ETag mismatch — blob has changed since xOpen.  Re-download. */
    double t0 = 0;
    if (sqlite_objs_debug_timing()) {
        t0 = sqlite_objs_time_ms();
        fprintf(stderr, "[TIMING] revalidateAfterLease: ETag mismatch "
                "(cached=%.32s, current=%.32s, blob=%s) — re-downloading\n",
                p->etag, aerr.etag, p->zBlobName);
    }

    /* Defensive: if dirty pages exist something is very wrong — SQLite should
    ** not write without RESERVED, and we just acquired RESERVED now. */
    if (bitmapHasAny(&p->dirty)) {
        return SQLITE_IOERR_READ;
    }

    /* Update stored ETag */
    memcpy(p->etag, aerr.etag, sizeof(p->etag));

    if (blobSize <= 0) {
        /* Blob was truncated to empty */
        if (ftruncate(p->cacheFd, 0) != 0) return SQLITE_IOERR;
        p->nData = 0;
        p->snapshot[0] = '\0';
        bitmapClearAll(&p->dirty);
        bitmapClearAll(&p->valid);
        p->lastSyncedSize = 0;
        return SQLITE_OK;
    }

    /* ---- Lazy mode: try to mark changed pages invalid instead of downloading ---- */
    if (p->prefetchMode == SQLITE_OBJS_PREFETCH_NONE
        && p->snapshot[0] != '\0'
        && p->ops->blob_get_page_ranges_diff && p->ops->page_blob_read) {
        azure_diff_range_t *diffRanges = NULL;
        int diffCount = 0;
        azure_error_t derr;
        azure_error_init(&derr);
        azure_err_t darc = p->ops->blob_get_page_ranges_diff(
            p->ops_ctx, p->zBlobName, p->snapshot,
            &diffRanges, &diffCount, &derr);
        if (darc == AZURE_OK && diffCount >= 0) {
            /* Calculate how many pages changed */
            int nTotalPages = (int)((blobSize + p->pageSize - 1) / p->pageSize);
            int nChangedPages = 0;
            for (int i = 0; i < diffCount; i++) {
                int64_t rangeLen = diffRanges[i].end - diffRanges[i].start + 1;
                nChangedPages += (int)((rangeLen + p->pageSize - 1) / p->pageSize);
            }

            if (nTotalPages > 0 &&
                (nChangedPages * 100 / nTotalPages) <= SQLITE_OBJS_DIFF_THRESHOLD_PCT) {
                /* Small diff — mark changed pages invalid, don't download */
                /* Resize cache if blob grew */
                if (blobSize > p->nData) {
                    ftruncate(p->cacheFd, (off_t)blobSize);
                }
                p->nData = blobSize;
                bitmapEnsureCapacity(&p->valid, p->nData, p->pageSize);
                for (int i = 0; i < diffCount; i++) {
                    int64_t rs = diffRanges[i].start;
                    int64_t re = diffRanges[i].end + 1;
                    int startPg = (int)(rs / p->pageSize);
                    int endPg = (int)((re + p->pageSize - 1) / p->pageSize);
                    for (int pg = startPg; pg < endPg; pg++) {
                        bitmapClearBit(&p->valid, pg);
                    }
                }
                /* Truncate if blob shrank */
                if (blobSize < p->nData) {
                    ftruncate(p->cacheFd, (off_t)blobSize);
                    p->nData = blobSize;
                }
                free(diffRanges);
                p->snapshot[0] = '\0';
                bitmapsEnsureCapacity(p);
                bitmapClearAll(&p->dirty);
                p->lastSyncedSize = p->nData;
                if (sqlite_objs_debug_timing()) {
                    double elapsed = sqlite_objs_time_ms() - t0;
                    fprintf(stderr, "[TIMING] revalidateAfterLease(lazy): "
                            "invalidated %d/%d pages in %.1fms (blob=%s)\n",
                            nChangedPages, nTotalPages, elapsed, p->zBlobName);
                }
                return SQLITE_OK;
            }
            /* >50% changed — fall through to full download */
        }
        free(diffRanges);
        p->snapshot[0] = '\0';
    }

    /* ---- Incremental diff path (prefetch=all): download changed pages ---- */
    if (p->prefetchMode == SQLITE_OBJS_PREFETCH_ALL) {
        if (applyIncrementalDiff(p, blobSize, "revalidateAfterLease") == SQLITE_OK) {
            /* All pages now valid after full diff apply */
            if (p->valid.data) {
                bitmapEnsureCapacity(&p->valid, p->nData, p->pageSize);
                validMarkAll(p);
            }
            return SQLITE_OK;
        }
        p->snapshot[0] = '\0';
    }

    /* ---- Full re-download path (original behavior) ---- */
    unsigned char *tempBuf = (unsigned char *)malloc(blobSize);
    if (!tempBuf) return SQLITE_NOMEM;

    azure_error_init(&aerr);
    if (p->ops->page_blob_read_multi) {
        arc = p->ops->page_blob_read_multi(
            p->ops_ctx, p->zBlobName, blobSize, tempBuf, &aerr);
    } else if (p->ops->page_blob_read) {
        azure_buffer_t buf = {0};
        arc = p->ops->page_blob_read(p->ops_ctx, p->zBlobName,
                                      0, (size_t)blobSize, &buf, &aerr);
        if (arc == AZURE_OK) {
            if (!buf.data || buf.size == 0) {
                arc = AZURE_ERR_INVALID_ARG;
            } else {
                int64_t copyLen = (int64_t)buf.size;
                if (copyLen > blobSize) copyLen = blobSize;
                memcpy(tempBuf, buf.data, (size_t)copyLen);
            }
        }
        free(buf.data);
    } else {
        free(tempBuf);
        return SQLITE_IOERR;
    }

    if (arc != AZURE_OK) {
        free(tempBuf);
        return azureErrToSqlite(arc, SQLITE_IOERR_READ);
    }

    /* Rewrite cache file with fresh data */
    if (lseek(p->cacheFd, 0, SEEK_SET) != 0) {
        free(tempBuf);
        return SQLITE_IOERR;
    }
    if (ftruncate(p->cacheFd, 0) != 0) {
        free(tempBuf);
        return SQLITE_IOERR;
    }

    ssize_t totalWritten = 0;
    while (totalWritten < blobSize) {
        ssize_t nWritten = write(p->cacheFd, tempBuf + totalWritten,
                                blobSize - totalWritten);
        if (nWritten <= 0) {
            free(tempBuf);
            return SQLITE_IOERR;
        }
        totalWritten += nWritten;
    }

    if (fsync(p->cacheFd) != 0) {
        free(tempBuf);
        return SQLITE_IOERR;
    }

    free(tempBuf);

    p->nData = blobSize;
    p->nDownloads++;

    /* Re-detect page size from fresh data */
    unsigned char header[100];
    ssize_t nRead = pread(p->cacheFd, header, sizeof(header), 0);
    if (nRead == (ssize_t)sizeof(header)) {
        int detected = detectPageSize(header, sizeof(header));
        if (detected > 0) p->pageSize = detected;
    }

    /* Reset dirty bitmap and size tracking */
    bitmapsEnsureCapacity(p);
    bitmapClearAll(&p->dirty);
    p->lastSyncedSize = p->nData;

    /* Full download = all pages valid */
    if (p->valid.data) {
        bitmapEnsureCapacity(&p->valid, p->nData, p->pageSize);
        validMarkAll(p);
    }

    if (sqlite_objs_debug_timing()) {
        double elapsed = sqlite_objs_time_ms() - t0;
        fprintf(stderr, "[TIMING] revalidateAfterLease: re-download complete "
                "%.1fms (%lld bytes, blob=%s)\n",
                elapsed, (long long)blobSize, p->zBlobName);
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

        /* Re-validate: if the blob changed since xOpen, re-download
        ** before SQLite reads any data under this new lease. */
        int rv = revalidateAfterLease(p, aerr.etag);
        if (rv != SQLITE_OK) {
            /* Re-validation failed — release the lease we just acquired
            ** so we don't hold it while returning an error. */
            if (p->ops->lease_release) {
                azure_error_t releaseErr;
                azure_error_init(&releaseErr);
                p->ops->lease_release(p->ops_ctx, p->zBlobName,
                                       p->leaseId, &releaseErr);
            }
            p->leaseId[0] = '\0';
            p->leaseAcquiredAt = 0;
            return rv;
        }
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
                    if (!p->ops || !p->ops->block_blob_upload ||
                        !p->ops->block_blob_download) {
                        aArg[0] = sqlite3_mprintf("delete");
                        return SQLITE_OK;
                    }
                    /* Append blob ops available — let SQLite set WAL mode.
                    ** If locking_mode=EXCLUSIVE is not set, xShmMap will
                    ** return SQLITE_IOERR and fail safely. */
                    return SQLITE_NOTFOUND;
                }
            }
            /* PRAGMA sqlite_objs_wal_parallel = ON|OFF|1|0 */
            if (aArg[1] && sqlite3_stricmp(aArg[1],
                    "sqlite_objs_wal_parallel") == 0) {
                sqliteObjsFile *p = (sqliteObjsFile *)pFile;
                if (aArg[2]) {
                    /* SET: enable/disable parallel WAL upload */
                    int val = (sqlite3_stricmp(aArg[2], "on") == 0 ||
                               sqlite3_stricmp(aArg[2], "1") == 0);
                    if (p->pVfsData) p->pVfsData->walParallelUpload = val;
                    aArg[0] = sqlite3_mprintf(val ? "on" : "off");
                } else {
                    /* GET: query current setting */
                    int val = p->pVfsData ? p->pVfsData->walParallelUpload : 0;
                    aArg[0] = sqlite3_mprintf(val ? "on" : "off");
                }
                return SQLITE_OK;
            }
            /* PRAGMA sqlite_objs_wal_chunk_size = <bytes> */
            if (aArg[1] && sqlite3_stricmp(aArg[1],
                    "sqlite_objs_wal_chunk_size") == 0) {
                sqliteObjsFile *p = (sqliteObjsFile *)pFile;
                if (aArg[2]) {
                    int val = atoi(aArg[2]);
                    if (val < 0) val = 0;
                    if (p->pVfsData) p->pVfsData->walChunkSize = val;
                    aArg[0] = sqlite3_mprintf("%d", val);
                } else {
                    int val = p->pVfsData ? p->pVfsData->walChunkSize : 0;
                    aArg[0] = sqlite3_mprintf("%d", val);
                }
                return SQLITE_OK;
            }
            /* PRAGMA sqlite_objs_prefetch — download all invalid pages */
            if (aArg[1] && sqlite3_stricmp(aArg[1],
                    "sqlite_objs_prefetch") == 0) {
                sqliteObjsFile *p = (sqliteObjsFile *)pFile;
                if (p->prefetchMode == SQLITE_OBJS_PREFETCH_NONE && p->valid.data) {
                    int rc = prefetchInvalidPages(p);
                    if (rc != SQLITE_OK) {
                        aArg[0] = sqlite3_mprintf("error");
                        return rc;
                    }
                    aArg[0] = sqlite3_mprintf("ok");
                } else {
                    aArg[0] = sqlite3_mprintf("noop");
                }
                return SQLITE_OK;
            }
            return SQLITE_NOTFOUND;
        }
        case SQLITE_FCNTL_VFSNAME: {
            *(char **)pArg = sqlite3_mprintf("sqlite-objs");
            return SQLITE_OK;
        }
        case SQLITE_OBJS_FCNTL_DOWNLOAD_COUNT: {
            sqliteObjsFile *p = (sqliteObjsFile *)pFile;
            *(int *)pArg = p->nDownloads;
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
** Build a heap-allocated mkstemp template for the cache file.
** If cacheDir is non-NULL and non-empty, the template is placed under
** that directory (which is created with mkdir if it doesn't exist).
** Otherwise falls back to /tmp.  Returns NULL on allocation failure.
** The caller must free() the returned string.
*/
static char *buildCacheTemplate(const char *cacheDir) {
    if (cacheDir && cacheDir[0]) {
        mkdir(cacheDir, 0700);  /* ignore EEXIST */
        size_t dirLen = strlen(cacheDir);
        /* Strip trailing slash */
        if (dirLen > 0 && cacheDir[dirLen - 1] == '/') dirLen--;
        char *tmpl = (char *)malloc(dirLen + sizeof("/sqlite-objs-XXXXXX"));
        if (!tmpl) return NULL;
        memcpy(tmpl, cacheDir, dirLen);
        memcpy(tmpl + dirLen, "/sqlite-objs-XXXXXX", sizeof("/sqlite-objs-XXXXXX"));
        return tmpl;
    }
    return strdup("/tmp/sqlite-objs-XXXXXX");
}

/* ===================================================================
** ETag-based cache reuse helpers
** =================================================================== */

/*
** FNV-1a hash — fast non-cryptographic hash for cache path naming.
*/
static uint64_t fnv1a_hash(const char *data, size_t len) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < len; i++) {
        h ^= (uint64_t)(unsigned char)data[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

/*
** Build a deterministic cache file path from blob identity.
** Returns heap-allocated string: {cacheDir}/sqlite-objs-{hex}.cache
** Caller must free().  Returns NULL on allocation failure.
*/
static char *buildCachePath(const char *cacheDir, const char *account,
                            const char *container, const char *blobName) {
    if (!cacheDir || !cacheDir[0]) cacheDir = "/tmp";
    mkdir(cacheDir, 0700);  /* ignore EEXIST */

    /* Hash "account:container:blobName" */
    size_t aLen = account ? strlen(account) : 0;
    size_t cLen = container ? strlen(container) : 0;
    size_t bLen = blobName ? strlen(blobName) : 0;
    size_t keyLen = aLen + 1 + cLen + 1 + bLen;
    char *key = (char *)malloc(keyLen + 1);
    if (!key) return NULL;
    snprintf(key, keyLen + 1, "%s:%s:%s",
             account ? account : "",
             container ? container : "",
             blobName ? blobName : "");
    uint64_t h = fnv1a_hash(key, keyLen);
    free(key);

    char hex[17];
    snprintf(hex, sizeof(hex), "%016llx", (unsigned long long)h);

    size_t dirLen = strlen(cacheDir);
    if (dirLen > 0 && cacheDir[dirLen - 1] == '/') dirLen--;

    /* "{dir}/sqlite-objs-{16hex}.cache\0" */
    size_t pathLen = dirLen + 1 + 12 + 16 + 6 + 1;
    char *path = (char *)malloc(pathLen);
    if (!path) return NULL;
    snprintf(path, pathLen, "%.*s/sqlite-objs-%s.cache", (int)dirLen, cacheDir, hex);
    return path;
}

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
    p->cacheReuse = 0;

    /* Read optional cache_dir and cache_reuse URI parameters */
    const char *cacheDir = sqlite3_uri_parameter(zName, "cache_dir");
    int cacheReuse = sqlite3_uri_boolean(zName, "cache_reuse", 0);

    /* Parse prefetch mode: "all" (default) or "none" (lazy cache) */
    const char *prefetchParam = sqlite3_uri_parameter(zName, "prefetch");
    p->prefetchMode = SQLITE_OBJS_PREFETCH_ALL;  /* default */
    if (prefetchParam && sqlite3_stricmp(prefetchParam, "none") == 0) {
        p->prefetchMode = SQLITE_OBJS_PREFETCH_NONE;
    }

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

            /* ETag-based cache reuse: try to reuse existing cache file */
            if (cacheReuse && blobSize > 0) {
                const char *acct = sqlite3_uri_parameter(zName, "azure_account");
                const char *cont = sqlite3_uri_parameter(zName, "azure_container");
                char *cachePath = buildCachePath(cacheDir, acct, cont, zName);
                if (cachePath) {
                    char *etagPath = buildEtagPath(cachePath);
                    char storedEtag[128] = {0};
                    if (etagPath &&
                        readEtagFile(etagPath, storedEtag, sizeof(storedEtag)) == 0 &&
                        p->etag[0] != '\0' &&
                        strcmp(storedEtag, p->etag) == 0) {
                        /* ETag match — try to reuse cache file */
                        int fd = open(cachePath, O_RDWR);
                        if (fd >= 0) {
                            /* Verify cached file size matches blob */
                            off_t cachedSize = lseek(fd, 0, SEEK_END);
                            if (cachedSize == (off_t)blobSize) {
                                p->cacheFd = fd;
                                p->zCachePath = cachePath;
                                p->cacheReuse = 1;
                                p->nData = blobSize;

                                /* Detect page size from header */
                                unsigned char header[100];
                                ssize_t nRead = pread(p->cacheFd, header, sizeof(header), 0);
                                if (nRead == (ssize_t)sizeof(header)) {
                                    int detected = detectPageSize(header, sizeof(header));
                                    if (detected > 0) p->pageSize = detected;
                                }
                                bitmapsEnsureCapacity(p);
                                p->lastSyncedSize = p->nData;
                                /* Load stored snapshot for incremental diff */
                                readSnapshotFile(cachePath, p->snapshot,
                                                 sizeof(p->snapshot));
                                /* Load valid bitmap from .state sidecar (lazy mode) */
                                if (p->prefetchMode == SQLITE_OBJS_PREFETCH_NONE) {
                                    unsigned char *savedValid = NULL;
                                    int savedValidSize = 0;
                                    int savedPageSize = 0;
                                    sqlite3_int64 savedFileSize = 0;
                                    if (readStateFile(cachePath, &savedValid,
                                                      &savedValidSize, &savedPageSize,
                                                      &savedFileSize) == 0
                                        && savedPageSize == p->pageSize
                                        && savedFileSize == p->nData) {
                                        p->valid.data = savedValid;
                                        p->valid.nAlloc = savedValidSize;
                                        /* Recount valid pages */
                                        p->valid.nSet = 0;
                                        int nPg = (int)((p->nData + p->pageSize - 1) / p->pageSize);
                                        for (int vi = 0; vi < nPg; vi++) {
                                            if (bitmapTestBit(&p->valid, vi)) p->valid.nSet++;
                                        }
                                    } else {
                                        free(savedValid);
                                        /* No valid .state — treat all as invalid,
                                        ** but page 1 was cached so mark it valid */
                                        bitmapEnsureCapacity(&p->valid, p->nData, p->pageSize);
                                        bitmapClearAll(&p->valid);
                                    }
                                }
                                free(etagPath);
                                goto cache_ready;
                            }
                            close(fd);
                        }
                    }
                    free(etagPath);
                    /* Cache miss (ETag mismatch) — try incremental diff first
                    ** if we have a stored snapshot from a previous session. */
                    {
                        char storedSnapshot[128] = {0};
                        int hasSnapshot = (readSnapshotFile(cachePath, storedSnapshot,
                                                            sizeof(storedSnapshot)) == 0
                                           && storedSnapshot[0] != '\0');
                        if (hasSnapshot) {
                            /* Open existing cache file WITHOUT truncating */
                            int fd = open(cachePath, O_RDWR);
                            if (fd >= 0) {
                                off_t cachedSize = lseek(fd, 0, SEEK_END);
                                p->cacheFd = fd;
                                p->zCachePath = cachePath;
                                p->cacheReuse = 1;
                                p->nData = (sqlite3_int64)cachedSize;
                                memcpy(p->snapshot, storedSnapshot, sizeof(p->snapshot));
                                bitmapsEnsureCapacity(p);
                                p->lastSyncedSize = p->nData;
                                if (applyIncrementalDiff(p, blobSize, "xOpen") == SQLITE_OK) {
                                    /* Success — cache is now up-to-date */
                                    goto cache_ready;
                                }
                                /* Incremental diff failed — fall through to full download.
                                ** Truncate the cache file and proceed. */
                                ftruncate(p->cacheFd, 0);
                                lseek(p->cacheFd, 0, SEEK_SET);
                                p->nData = 0;
                                p->snapshot[0] = '\0';
                                goto cache_download;
                            }
                        }
                    }
                    /* No snapshot or couldn't open — full download with fresh file */
                    p->cacheFd = open(cachePath, O_RDWR | O_CREAT | O_TRUNC, 0600);
                    if (p->cacheFd < 0) {
                        free(cachePath);
                        sqlite3_free(p->zBlobName);
                        p->zBlobName = NULL;
                        return SQLITE_CANTOPEN;
                    }
                    p->zCachePath = cachePath;
                    p->cacheReuse = 1;
                    goto cache_download;
                }
                /* buildCachePath failed — fall through to mkstemp path */
            }

            /* Create cache file (default: random mkstemp name) */
            {
                char *cacheTemplate = buildCacheTemplate(cacheDir);
                if (!cacheTemplate) {
                    sqlite3_free(p->zBlobName);
                    p->zBlobName = NULL;
                    return SQLITE_NOMEM;
                }
                p->cacheFd = mkstemp(cacheTemplate);
                if (p->cacheFd < 0) {
                    free(cacheTemplate);
                    sqlite3_free(p->zBlobName);
                    p->zBlobName = NULL;
                    return SQLITE_CANTOPEN;
                }
                p->zCachePath = cacheTemplate;
            }

            cache_download:
            p->nDownloads++;
            if (blobSize > 0 && p->prefetchMode == SQLITE_OBJS_PREFETCH_NONE) {
                /* Lazy cache: create sparse cache, fetch page 1 only */
                if (ftruncate(p->cacheFd, (off_t)blobSize) != 0) {
                    close(p->cacheFd);
                    if (p->zCachePath) { unlink(p->zCachePath); free(p->zCachePath); }
                    p->cacheFd = -1;
                    p->zCachePath = NULL;
                    sqlite3_free(p->zBlobName);
                    p->zBlobName = NULL;
                    return SQLITE_IOERR;
                }
                p->nData = blobSize;

                /* Bootstrap: fetch first 65536 bytes (covers any legal page size) */
                size_t bootstrapLen = SQLITE_OBJS_PAGE1_BOOTSTRAP;
                if ((int64_t)bootstrapLen > blobSize) bootstrapLen = (size_t)blobSize;

                azure_buffer_t bbuf;
                azure_buffer_init(&bbuf);
                azure_error_t berr;
                azure_error_init(&berr);
                arc = p->ops->page_blob_read(
                    p->ops_ctx, zName, 0, bootstrapLen, &bbuf, &berr);
                if (arc != AZURE_OK || !bbuf.data || bbuf.size == 0) {
                    azure_buffer_free(&bbuf);
                    close(p->cacheFd);
                    if (p->zCachePath) { unlink(p->zCachePath); free(p->zCachePath); }
                    p->cacheFd = -1;
                    p->zCachePath = NULL;
                    sqlite3_free(p->zBlobName);
                    p->zBlobName = NULL;
                    return azureErrToSqlite(arc, SQLITE_CANTOPEN);
                }

                size_t writeLen = bbuf.size < bootstrapLen ? bbuf.size : bootstrapLen;
                ssize_t nw = pwrite(p->cacheFd, bbuf.data, writeLen, 0);
                azure_buffer_free(&bbuf);
                if (nw < 0 || (size_t)nw != writeLen) {
                    close(p->cacheFd);
                    if (p->zCachePath) { unlink(p->zCachePath); free(p->zCachePath); }
                    p->cacheFd = -1;
                    p->zCachePath = NULL;
                    sqlite3_free(p->zBlobName);
                    p->zBlobName = NULL;
                    return SQLITE_IOERR;
                }

                /* Detect page size from bootstrap data */
                {
                    unsigned char header[100];
                    ssize_t nRead = pread(p->cacheFd, header, sizeof(header), 0);
                    if (nRead == (ssize_t)sizeof(header)) {
                        int detected = detectPageSize(header, sizeof(header));
                        if (detected > 0) p->pageSize = detected;
                    }
                }

                /* Initialize bitmaps */
                bitmapsEnsureCapacity(p);
                bitmapClearAll(&p->valid);
                /* Mark bootstrapped pages valid */
                validMarkRange(p, 0, (sqlite3_int64)writeLen);

                p->lastSyncedSize = p->nData;

            } else if (blobSize > 0) {
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
                    bitmapFree(&p->dirty);
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
                        bitmapFree(&p->dirty);
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
                    bitmapFree(&p->dirty);
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
                bitmapsEnsureCapacity(p);

                /* Full download — all pages are valid */
                if (p->prefetchMode == SQLITE_OBJS_PREFETCH_NONE) {
                    bitmapEnsureCapacity(&p->valid, p->nData, p->pageSize);
                    validMarkAll(p);
                }

                /* R2: Record initial Azure blob size to skip redundant resizes */
                p->lastSyncedSize = p->nData;
            } else {
                /* Empty blob */
                p->nData = 0;
            }
            cache_ready: ;  /* ETag cache hit jumps here */
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
            
            /* Create cache file — use deterministic path when cache_reuse
            ** is requested so the ETag sidecar can be persisted on close. */
            if (cacheReuse) {
                const char *acct = sqlite3_uri_parameter(zName, "azure_account");
                const char *cont = sqlite3_uri_parameter(zName, "azure_container");
                char *cachePath = buildCachePath(cacheDir, acct, cont, zName);
                if (cachePath) {
                    p->cacheFd = open(cachePath, O_RDWR | O_CREAT | O_TRUNC, 0600);
                    if (p->cacheFd < 0) {
                        free(cachePath);
                        sqlite3_free(p->zBlobName);
                        p->zBlobName = NULL;
                        return SQLITE_CANTOPEN;
                    }
                    p->zCachePath = cachePath;
                    p->cacheReuse = 1;
                } else {
                    /* buildCachePath failed — fall through to mkstemp */
                    goto create_mkstemp;
                }
            } else {
                create_mkstemp: ;
                char *cacheTemplate2 = buildCacheTemplate(cacheDir);
                if (!cacheTemplate2) {
                    sqlite3_free(p->zBlobName);
                    p->zBlobName = NULL;
                    return SQLITE_NOMEM;
                }
                p->cacheFd = mkstemp(cacheTemplate2);
                if (p->cacheFd < 0) {
                    free(cacheTemplate2);
                    sqlite3_free(p->zBlobName);
                    p->zBlobName = NULL;
                    return SQLITE_CANTOPEN;
                }
                p->zCachePath = cacheTemplate2;
            }
            
            p->nData = 0;
        } else if (!blobExists) {
            sqlite3_free(p->zBlobName);
            p->zBlobName = NULL;
            return SQLITE_CANTOPEN;
        }

        bitmapClearAll(&p->dirty);

    } else if (isMainJournal) {
        /* MAIN_JOURNAL → Azure block blob */
        p->nJrnlData = 0;
        p->nJrnlAlloc = 0;
        p->aJrnlData = NULL;

        /* R1: Track journal blob name and seed the cache from blob_exists check */
        pthread_mutex_lock(&pVfsData->journalCacheMutex);
        sqliteObjsJournalCacheEntry *jce =
            journalCacheGetOrCreate(pVfsData, zName);
        int cached_state = jce ? jce->state : -1;
        pthread_mutex_unlock(&pVfsData->journalCacheMutex);

        /* R1: If cache says journal doesn't exist, skip the HEAD request.
        ** Only do a real HEAD when cache is unknown (-1) or says it exists (1).
        ** Since we are the single writer, cache=0 is authoritative. */
        int exists = 0;
        if (cached_state == 0) {
            exists = 0;  /* We deleted it — no HEAD needed */
        } else if (p->ops && p->ops->blob_exists) {
            azure_error_t aerr;
            azure_error_init(&aerr);
            p->ops->blob_exists(p->ops_ctx, zName, &exists, &aerr);
            pthread_mutex_lock(&pVfsData->journalCacheMutex);
            if (jce) jce->state = exists ? 1 : 0;
            pthread_mutex_unlock(&pVfsData->journalCacheMutex);
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
        /* WAL → Azure block blob */

        /* Verify block blob operations are available */
        if (!p->ops || !p->ops->block_blob_upload ||
            !p->ops->block_blob_download) {
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
        }
        /* No need to create blob on open — first xSync uploads via PUT */
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
            pthread_mutex_lock(&pVfsData->journalCacheMutex);
            sqliteObjsJournalCacheEntry *jce = journalCacheFind(pVfsData, zName);
            if (jce) jce->state = 0;
            pthread_mutex_unlock(&pVfsData->journalCacheMutex);
            return SQLITE_OK;  /* Already gone — not an error */
        }
        if (arc != AZURE_OK) {
            return SQLITE_IOERR_DELETE;
        }

        /* R1: After successful delete, update journal cache */
        {
            pthread_mutex_lock(&pVfsData->journalCacheMutex);
            sqliteObjsJournalCacheEntry *jce = journalCacheFind(pVfsData, zName);
            if (jce) jce->state = 0;
            pthread_mutex_unlock(&pVfsData->journalCacheMutex);
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
        pthread_mutex_lock(&pVfsData->journalCacheMutex);
        sqliteObjsJournalCacheEntry *jce = journalCacheFind(pVfsData, zName);
        if (jce && jce->state >= 0) {
            int cached_result = jce->state;
            pthread_mutex_unlock(&pVfsData->journalCacheMutex);
            switch (flags) {
                case SQLITE_ACCESS_EXISTS:
                case SQLITE_ACCESS_READWRITE:
                case SQLITE_ACCESS_READ:
                    *pResOut = cached_result;
                    break;
                default:
                    *pResOut = 0;
            }
            return SQLITE_OK;
        }
        pthread_mutex_unlock(&pVfsData->journalCacheMutex);

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
        if (zName) {
            size_t nlen = strlen(zName);
            if (nlen >= 8 && strcmp(zName + nlen - 8, "-journal") == 0) {
                pthread_mutex_lock(&pVfsData->journalCacheMutex);
                jce = journalCacheGetOrCreate(pVfsData, zName);
                if (jce) jce->state = exists ? 1 : 0;
                pthread_mutex_unlock(&pVfsData->journalCacheMutex);
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

    /* Initialize journal cache mutex */
    if (pthread_mutex_init(&g_vfsData.journalCacheMutex, NULL) != 0) {
        return SQLITE_ERROR;
    }

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

    /* Initialize journal cache mutex */
    if (pthread_mutex_init(&g_vfsData.journalCacheMutex, NULL) != 0) {
        return SQLITE_ERROR;
    }

    /* szOsFile must accommodate both our struct and the default VFS's struct */
    int ourSize = (int)sizeof(sqliteObjsFile);
    int defaultSize = pDefault->szOsFile;
    g_sqliteObjsVfs.szOsFile = (ourSize > defaultSize) ? ourSize : defaultSize;
    g_sqliteObjsVfs.pAppData = &g_vfsData;

    return sqlite3_vfs_register(&g_sqliteObjsVfs, makeDefault);
}
