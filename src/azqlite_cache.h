/*
** azqlite_cache.h — Local disk cache for Azure blob pages
**
** Provides mmap-backed persistent page cache with:
**   - Two files per database: metadata (.meta) + page store (.pages)
**   - Valid/dirty bitmaps for page-level tracking
**   - ETag-based staleness detection
**   - CRC32C checksums (optional) for corruption detection
**   - pthread_mutex for thread safety (prefetch thread + VFS)
**
** No pointers are stored in mapped files — only page indices and byte
** offsets. This makes remapping safe when files grow.
*/
#ifndef AZQLITE_CACHE_H
#define AZQLITE_CACHE_H

#include <stdint.h>
#include <stddef.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Magic bytes at start of metadata file */
#define AZQLITE_CACHE_MAGIC     "AZQCACHE"
#define AZQLITE_CACHE_MAGIC_LEN 8
#define AZQLITE_CACHE_VERSION   1

/* Header size — fixed at 256 bytes for alignment */
#define AZQLITE_CACHE_HEADER_SIZE 256

/* ===================================================================
** Cache metadata header (stored at offset 0 of .meta file)
** All multi-byte integers are host-endian (little on x86/ARM).
** =================================================================== */
typedef struct azqlite_cache_header {
    char     magic[8];          /* "AZQCACHE" */
    uint32_t version;           /* AZQLITE_CACHE_VERSION */
    uint32_t page_size;         /* SQLite page size (e.g. 4096) */
    uint32_t page_count;        /* Pages allocated in page store */
    uint32_t flags;             /* Bit 0: session_active */
    char     etag[128];         /* ETag from last successful sync/open */
    int64_t  blob_size;         /* Remote blob size at last sync */
    char     reserved[96];      /* Padding to 256 bytes */
} azqlite_cache_header_t;

/* Flag bits */
#define AZQLITE_CACHE_FLAG_SESSION_ACTIVE  0x01

/* ===================================================================
** Cache handle — opaque to callers
** =================================================================== */
typedef struct azqlite_cache azqlite_cache_t;

/* ===================================================================
** Cache configuration
** =================================================================== */
typedef struct azqlite_cache_config {
    const char *cache_dir;      /* Directory for cache files (NULL = default) */
    const char *blob_identity;  /* Unique string: "account/container/blobname" */
    int page_size;              /* SQLite page size */
    int page_count;             /* Initial page count (from blob size) */
    int checksums_enabled;      /* 1 = CRC32C per page */
} azqlite_cache_config_t;

/* ===================================================================
** Lifecycle
** =================================================================== */

/*
** Open or create a local disk cache.
** Returns NULL on failure (insufficient disk, permission error, etc).
** If an existing cache exists with matching magic/version, it is reused.
** The caller must call azqlite_cache_close() when done.
*/
azqlite_cache_t *azqlite_cache_open(const azqlite_cache_config_t *config);

/*
** Close the cache: msync, munmap, close fds, free handle.
** Safe to call with NULL.
*/
void azqlite_cache_close(azqlite_cache_t *cache);

/* ===================================================================
** ETag operations
** =================================================================== */

/* Get the stored ETag (from metadata file). Returns "" if none. */
const char *azqlite_cache_get_etag(azqlite_cache_t *cache);

/* Set the ETag (after successful blob operation). */
void azqlite_cache_set_etag(azqlite_cache_t *cache, const char *etag);

/* Check if stored ETag matches the given ETag. */
int azqlite_cache_etag_matches(azqlite_cache_t *cache, const char *remote_etag);

/*
** Invalidate the entire cache (clear valid bitmap).
** Called when ETag misses on reconnect.
*/
void azqlite_cache_invalidate_all(azqlite_cache_t *cache);

/*
** Invalidate pages above max_page_no (clear valid+dirty bits).
** Called on xTruncate to discard pages beyond the new file size.
** Pages 0..max_page_no are kept. If max_page_no < 0, all pages are invalidated.
*/
void azqlite_cache_invalidate_above(azqlite_cache_t *cache, int max_page_no);

/* ===================================================================
** Page operations (all thread-safe via internal mutex)
** =================================================================== */

/*
** Check if a page is valid in the cache.
** Returns 1 if valid, 0 if not.
*/
int azqlite_cache_page_valid(azqlite_cache_t *cache, int page_no);

/*
** Read page data from cache into output buffer.
** Caller must ensure buf has page_size bytes.
** Returns 1 if page was valid and copied, 0 if page was not valid.
** Does NOT acquire mutex — caller must hold it or ensure single-threaded access.
*/
int azqlite_cache_page_read(azqlite_cache_t *cache, int page_no,
                            unsigned char *buf);

/*
** Write page data into the cache page store.
** Marks page as valid. If mark_dirty is true, also marks as dirty.
** Thread-safe (acquires mutex internally).
*/
void azqlite_cache_page_write(azqlite_cache_t *cache, int page_no,
                              const unsigned char *data, int mark_dirty);

/*
** Write page data into cache only if the page is currently invalid.
** Used by the prefetch thread to avoid clobbering foreground writes.
** Thread-safe (acquires mutex internally).
** Returns 1 if page was written, 0 if skipped (already valid).
*/
int azqlite_cache_page_write_if_invalid(azqlite_cache_t *cache, int page_no,
                                        const unsigned char *data);

/*
** Get a direct pointer to page data in the mmap.
** The pointer is valid until the next cache_grow() call.
** Caller must hold the mutex if accessing concurrently.
** Returns NULL if page_no is out of range.
**
** WARNING: Prefer azqlite_cache_read_page_range() for safe access.
** Raw pointers become invalid after grow().
*/
unsigned char *azqlite_cache_page_ptr(azqlite_cache_t *cache, int page_no);

/*
** Read a sub-range of a cached page into dest.
** Thread-safe: acquires mutex internally.
** Returns 1 if the page was valid and data was copied, 0 otherwise.
** page_off + len must not exceed page_size.
*/
int azqlite_cache_read_page_range(azqlite_cache_t *cache, int page_no,
                                  int page_off, int len,
                                  unsigned char *dest);

/*
** Mark a page as valid + dirty, computing CRC32C from in-place data.
** Caller MUST hold the cache mutex. Used after writing data directly
** into the mmap via page_ptr() to atomically update metadata.
*/
void azqlite_cache_mark_written(azqlite_cache_t *cache, int page_no);

/*
** Verify the CRC32C checksum of a cached page.
** Returns 1 if the checksum matches (or checksums are disabled), 0 if corrupt.
** Returns 0 if page_no is out of range or the page is not valid.
*/
int azqlite_cache_page_verify(azqlite_cache_t *cache, int page_no);

/* ===================================================================
** Dirty page tracking
** =================================================================== */

/* Check if a page is dirty. */
int azqlite_cache_page_dirty(azqlite_cache_t *cache, int page_no);

/* Mark a page as dirty. */
void azqlite_cache_page_set_dirty(azqlite_cache_t *cache, int page_no);

/*
** Collect all dirty page numbers into *out_pages (caller frees).
** Returns the count of dirty pages.
*/
int azqlite_cache_collect_dirty(azqlite_cache_t *cache,
                                int **out_pages);

/* Clear all dirty bits (after successful sync). */
void azqlite_cache_clear_dirty(azqlite_cache_t *cache);

/* Count of dirty pages. */
int azqlite_cache_dirty_count(azqlite_cache_t *cache);

/* ===================================================================
** Cache growth (when remote blob grows)
** =================================================================== */

/*
** Grow the cache to accommodate new_page_count pages.
** Remaps both files. Existing page data is preserved.
** New pages are marked as invalid.
** Returns 0 on success, -1 on failure.
*/
int azqlite_cache_grow(azqlite_cache_t *cache, int new_page_count);

/* ===================================================================
** Sync to disk
** =================================================================== */

/* msync both files to ensure persistence. */
void azqlite_cache_msync(azqlite_cache_t *cache);

/* ===================================================================
** Mutex — exposed for callers who need multi-page atomic operations
** =================================================================== */

void azqlite_cache_lock(azqlite_cache_t *cache);
void azqlite_cache_unlock(azqlite_cache_t *cache);

/* ===================================================================
** Accessors
** =================================================================== */

int azqlite_cache_page_size(azqlite_cache_t *cache);
int azqlite_cache_page_count(azqlite_cache_t *cache);
int64_t azqlite_cache_blob_size(azqlite_cache_t *cache);
void azqlite_cache_set_blob_size(azqlite_cache_t *cache, int64_t size);

#ifdef __cplusplus
}
#endif

#endif /* AZQLITE_CACHE_H */
