/*
** azqlite_cache.c — Local disk cache for Azure blob pages
**
** Implements the mmap-backed persistent page cache described in
** research/local-disk-cache-design.md.
**
** Two memory-mapped files per database:
**   .meta  — Header (256 bytes), valid bitmap, dirty bitmap, checksums
**   .pages — Raw page data, page N at offset N * pageSize
**
** No pointers are stored in mapped files. All references use page
** indices and byte offsets computed from the mmap base address.
*/

#include "azqlite_cache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>

/* ===================================================================
** Cache handle
** =================================================================== */

struct azqlite_cache {
    /* File descriptors */
    int meta_fd;
    int pages_fd;

    /* mmap pointers (recomputed after every remap) */
    unsigned char *meta_base;
    size_t meta_size;
    unsigned char *pages_base;
    size_t pages_size;

    /* Derived pointers into meta_base (recomputed after remap) */
    azqlite_cache_header_t *header;  /* At offset 0 */
    unsigned char *valid_bitmap;     /* At offset 256 */
    unsigned char *dirty_bitmap;     /* At offset 256 + bitmap_bytes */
    uint32_t *checksums;             /* At offset 256 + 2*bitmap_bytes */

    /* Cached values */
    int page_size;
    int page_count;
    int bitmap_bytes;                /* ceil(page_count / 8) */
    int checksums_enabled;

    /* Thread safety */
    pthread_mutex_t mutex;

    /* File paths (for cleanup / diagnostics) */
    char meta_path[512];
    char pages_path[512];
};

/* ===================================================================
** Internal helpers
** =================================================================== */

static int bitmap_bytes_for(int page_count) {
    return (page_count + 7) / 8;
}

static size_t meta_file_size(int page_count, int checksums_enabled) {
    int bm = bitmap_bytes_for(page_count);
    size_t sz = AZQLITE_CACHE_HEADER_SIZE + (size_t)bm * 2;
    if (checksums_enabled)
        sz += (size_t)page_count * sizeof(uint32_t);
    return sz;
}

static void recompute_pointers(azqlite_cache_t *c) {
    c->header = (azqlite_cache_header_t *)c->meta_base;
    c->bitmap_bytes = bitmap_bytes_for(c->page_count);
    c->valid_bitmap = c->meta_base + AZQLITE_CACHE_HEADER_SIZE;
    c->dirty_bitmap = c->valid_bitmap + c->bitmap_bytes;
    if (c->checksums_enabled) {
        c->checksums = (uint32_t *)(c->dirty_bitmap + c->bitmap_bytes);
    } else {
        c->checksums = NULL;
    }
}

static int bitmap_test(const unsigned char *bitmap, int bit) {
    return (bitmap[bit / 8] >> (bit % 8)) & 1;
}

static void bitmap_set(unsigned char *bitmap, int bit) {
    bitmap[bit / 8] |= (unsigned char)(1 << (bit % 8));
}

static void bitmap_clear(unsigned char *bitmap, int bit) {
    bitmap[bit / 8] &= (unsigned char)~(1 << (bit % 8));
}

/* ===================================================================
** CRC32C (Castagnoli) — software lookup-table implementation
** Polynomial: 0x82F63B78 (bit-reversed form of 0x1EDC6F41)
** =================================================================== */

static uint32_t crc32c_table[256];
static int crc32c_table_init = 0;

static void crc32c_init_table(void) {
    if (crc32c_table_init) return;
    for (int i = 0; i < 256; i++) {
        uint32_t crc = (uint32_t)i;
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (0x82F63B78 & (-(int32_t)(crc & 1)));
        }
        crc32c_table[i] = crc;
    }
    crc32c_table_init = 1;
}

static uint32_t crc32c(const unsigned char *data, size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc = crc32c_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFF;
}

/* Ensure directory exists (creates parents as needed) */
static int ensure_directory(const char *path) {
    char tmp[512];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
                return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

/* Simple hash of blob identity string for cache file naming */
static void identity_hash(const char *identity, char *out, size_t out_size) {
    /* FNV-1a 64-bit hash */
    uint64_t hash = 14695981039346656037ULL;
    for (const char *p = identity; *p; p++) {
        hash ^= (uint64_t)(unsigned char)*p;
        hash *= 1099511628211ULL;
    }
    snprintf(out, out_size, "%016llx", (unsigned long long)hash);
}

/* Create or open and mmap a file to the given size */
static int open_and_mmap(const char *path, size_t size,
                         int *fd_out, unsigned char **base_out) {
    int fd = open(path, O_RDWR | O_CREAT, 0644);
    if (fd < 0) return -1;

    /* Check current size */
    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        return -1;
    }

    /* Grow if needed */
    if ((size_t)st.st_size < size) {
        if (ftruncate(fd, (off_t)size) != 0) {
            close(fd);
            return -1;
        }
    }

    unsigned char *base = (unsigned char *)mmap(NULL, size,
                                                 PROT_READ | PROT_WRITE,
                                                 MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        close(fd);
        return -1;
    }

    *fd_out = fd;
    *base_out = base;
    return 0;
}

/* Default cache directory */
static const char *default_cache_dir(void) {
    static char buf[512];
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    snprintf(buf, sizeof(buf), "%s/.azqlite/cache", home);
    return buf;
}

/* ===================================================================
** Lifecycle
** =================================================================== */

azqlite_cache_t *azqlite_cache_open(const azqlite_cache_config_t *config) {
    if (!config || !config->blob_identity ||
        config->page_count < 0) {
        return NULL;
    }

    if (config->checksums_enabled) crc32c_init_table();

    /* page_size==0 means "adopt from existing cache header, or use 4096" */
    int requested_page_size = config->page_size > 0 ? config->page_size : 4096;

    const char *dir = config->cache_dir ? config->cache_dir : default_cache_dir();
    if (ensure_directory(dir) != 0) return NULL;

    /* Compute file paths */
    char hash[32];
    identity_hash(config->blob_identity, hash, sizeof(hash));

    azqlite_cache_t *c = (azqlite_cache_t *)calloc(1, sizeof(azqlite_cache_t));
    if (!c) return NULL;

    c->page_size = requested_page_size;
    c->page_count = config->page_count > 0 ? config->page_count : 1;
    c->checksums_enabled = config->checksums_enabled;
    c->meta_fd = -1;
    c->pages_fd = -1;

    snprintf(c->meta_path, sizeof(c->meta_path), "%s/%s.meta", dir, hash);
    snprintf(c->pages_path, sizeof(c->pages_path), "%s/%s.pages", dir, hash);

    pthread_mutex_init(&c->mutex, NULL);

    /* Compute file sizes */
    c->meta_size = meta_file_size(c->page_count, c->checksums_enabled);
    c->pages_size = (size_t)c->page_count * (size_t)c->page_size;
    if (c->pages_size == 0) c->pages_size = (size_t)c->page_size;

    /* Open and mmap metadata file */
    if (open_and_mmap(c->meta_path, c->meta_size,
                      &c->meta_fd, &c->meta_base) != 0) {
        goto fail;
    }

    /* Try to lock metadata file (single-process access).
    ** Non-blocking: if another connection in the same process holds the
    ** lock (e.g., test harness) we proceed anyway — the mutex provides
    ** thread safety within a single process. */
    flock(c->meta_fd, LOCK_EX | LOCK_NB);  /* best-effort */

    /* Open and mmap page store */
    if (open_and_mmap(c->pages_path, c->pages_size,
                      &c->pages_fd, &c->pages_base) != 0) {
        goto fail;
    }

    recompute_pointers(c);

    /* Validate or initialize header */
    if (memcmp(c->header->magic, AZQLITE_CACHE_MAGIC,
               AZQLITE_CACHE_MAGIC_LEN) == 0 &&
        c->header->version == AZQLITE_CACHE_VERSION &&
        c->header->page_size > 0) {

        /* Adopt page_size from existing header if it differs */
        if ((int)c->header->page_size != c->page_size) {
            c->page_size = (int)c->header->page_size;
            /* Recompute sizes with adopted page_size */
            c->pages_size = (size_t)c->page_count * (size_t)c->page_size;
            if (c->pages_size == 0) c->pages_size = (size_t)c->page_size;
            /* Remap pages file with correct size */
            munmap(c->pages_base, c->pages_size);
            size_t new_pages_size = (size_t)c->page_count * (size_t)c->page_size;
            if (new_pages_size == 0) new_pages_size = (size_t)c->page_size;
            ftruncate(c->pages_fd, (off_t)new_pages_size);
            c->pages_size = new_pages_size;
            c->pages_base = (unsigned char *)mmap(NULL, c->pages_size,
                PROT_READ | PROT_WRITE, MAP_SHARED, c->pages_fd, 0);
            if (c->pages_base == MAP_FAILED) goto fail;
        }

        /* Existing valid cache — check if previous session crashed */
        if (c->header->flags & AZQLITE_CACHE_FLAG_SESSION_ACTIVE) {
            /* Unclean shutdown — invalidate all pages */
            memset(c->valid_bitmap, 0, (size_t)c->bitmap_bytes);
            memset(c->dirty_bitmap, 0, (size_t)c->bitmap_bytes);
        }
        /* May need to grow if page_count increased */
        if ((int)c->header->page_count < c->page_count) {
            /* Grow handled by caller via azqlite_cache_grow */
        } else if ((int)c->header->page_count > c->page_count) {
            /* Existing cache is larger — remap to cover all pages */
            c->page_count = (int)c->header->page_count;
            size_t new_meta_size = meta_file_size(c->page_count, c->checksums_enabled);
            size_t new_pages_size = (size_t)c->page_count * (size_t)c->page_size;

            /* Remap meta if it grew */
            if (new_meta_size > c->meta_size) {
                munmap(c->meta_base, c->meta_size);
                c->meta_size = new_meta_size;
                c->meta_base = (unsigned char *)mmap(NULL, c->meta_size,
                    PROT_READ | PROT_WRITE, MAP_SHARED, c->meta_fd, 0);
                if (c->meta_base == MAP_FAILED) goto fail;
            }
            /* Remap pages */
            if (new_pages_size > c->pages_size) {
                munmap(c->pages_base, c->pages_size);
                c->pages_size = new_pages_size;
                c->pages_base = (unsigned char *)mmap(NULL, c->pages_size,
                    PROT_READ | PROT_WRITE, MAP_SHARED, c->pages_fd, 0);
                if (c->pages_base == MAP_FAILED) goto fail;
            }
            recompute_pointers(c);
        }
    } else {
        /* New or corrupt cache — initialize */
        memset(c->meta_base, 0, c->meta_size);
        memcpy(c->header->magic, AZQLITE_CACHE_MAGIC, AZQLITE_CACHE_MAGIC_LEN);
        c->header->version = AZQLITE_CACHE_VERSION;
        c->header->page_size = (uint32_t)c->page_size;
        c->header->page_count = (uint32_t)c->page_count;
        c->header->flags = 0;
        c->header->etag[0] = '\0';
        c->header->blob_size = 0;
        recompute_pointers(c);
    }

    /* Mark session active */
    c->header->flags |= AZQLITE_CACHE_FLAG_SESSION_ACTIVE;

    return c;

fail:
    if (c->pages_base && c->pages_base != MAP_FAILED)
        munmap(c->pages_base, c->pages_size);
    if (c->meta_base && c->meta_base != MAP_FAILED)
        munmap(c->meta_base, c->meta_size);
    if (c->pages_fd >= 0) close(c->pages_fd);
    if (c->meta_fd >= 0) close(c->meta_fd);
    pthread_mutex_destroy(&c->mutex);
    free(c);
    return NULL;
}

void azqlite_cache_close(azqlite_cache_t *c) {
    if (!c) return;

    /* Clear session active flag */
    if (c->header) {
        c->header->flags &= ~AZQLITE_CACHE_FLAG_SESSION_ACTIVE;
    }

    /* msync both files */
    if (c->meta_base && c->meta_base != MAP_FAILED) {
        msync(c->meta_base, c->meta_size, MS_SYNC);
        munmap(c->meta_base, c->meta_size);
    }
    if (c->pages_base && c->pages_base != MAP_FAILED) {
        msync(c->pages_base, c->pages_size, MS_SYNC);
        munmap(c->pages_base, c->pages_size);
    }

    if (c->meta_fd >= 0) {
        flock(c->meta_fd, LOCK_UN);
        close(c->meta_fd);
    }
    if (c->pages_fd >= 0) close(c->pages_fd);

    pthread_mutex_destroy(&c->mutex);
    free(c);
}

/* ===================================================================
** ETag operations
** =================================================================== */

const char *azqlite_cache_get_etag(azqlite_cache_t *c) {
    if (!c || !c->header) return "";
    return c->header->etag;
}

void azqlite_cache_set_etag(azqlite_cache_t *c, const char *etag) {
    if (!c || !c->header) return;
    if (etag) {
        strncpy(c->header->etag, etag, sizeof(c->header->etag) - 1);
        c->header->etag[sizeof(c->header->etag) - 1] = '\0';
    } else {
        c->header->etag[0] = '\0';
    }
}

int azqlite_cache_etag_matches(azqlite_cache_t *c, const char *remote_etag) {
    if (!c || !c->header || !remote_etag) return 0;
    if (c->header->etag[0] == '\0') return 0;
    return strcmp(c->header->etag, remote_etag) == 0;
}

void azqlite_cache_invalidate_all(azqlite_cache_t *c) {
    if (!c) return;
    pthread_mutex_lock(&c->mutex);
    memset(c->valid_bitmap, 0, (size_t)c->bitmap_bytes);
    /* Don't clear dirty — those are local writes not yet synced */
    pthread_mutex_unlock(&c->mutex);
}

void azqlite_cache_invalidate_above(azqlite_cache_t *c, int max_page_no) {
    if (!c) return;
    pthread_mutex_lock(&c->mutex);
    if (max_page_no < 0) {
        /* Invalidate everything */
        memset(c->valid_bitmap, 0, (size_t)c->bitmap_bytes);
        memset(c->dirty_bitmap, 0, (size_t)c->bitmap_bytes);
    } else {
        for (int pg = max_page_no + 1; pg < c->page_count; pg++) {
            bitmap_clear(c->valid_bitmap, pg);
            bitmap_clear(c->dirty_bitmap, pg);
        }
    }
    pthread_mutex_unlock(&c->mutex);
}

/* ===================================================================
** Page operations
** =================================================================== */

int azqlite_cache_page_valid(azqlite_cache_t *c, int page_no) {
    if (!c || page_no < 0 || page_no >= c->page_count) return 0;
    return bitmap_test(c->valid_bitmap, page_no);
}

int azqlite_cache_page_read(azqlite_cache_t *c, int page_no,
                            unsigned char *buf) {
    if (!c || page_no < 0 || page_no >= c->page_count) return 0;
    if (!bitmap_test(c->valid_bitmap, page_no)) return 0;
    size_t offset = (size_t)page_no * (size_t)c->page_size;
    memcpy(buf, c->pages_base + offset, (size_t)c->page_size);
    return 1;
}

void azqlite_cache_page_write(azqlite_cache_t *c, int page_no,
                              const unsigned char *data, int mark_dirty) {
    if (!c || page_no < 0 || page_no >= c->page_count) return;
    pthread_mutex_lock(&c->mutex);
    size_t offset = (size_t)page_no * (size_t)c->page_size;
    memcpy(c->pages_base + offset, data, (size_t)c->page_size);
    bitmap_set(c->valid_bitmap, page_no);
    if (mark_dirty) {
        bitmap_set(c->dirty_bitmap, page_no);
    }
    if (c->checksums_enabled && c->checksums) {
        c->checksums[page_no] = crc32c(data, (size_t)c->page_size);
    }
    pthread_mutex_unlock(&c->mutex);
}

int azqlite_cache_page_write_if_invalid(azqlite_cache_t *c, int page_no,
                                        const unsigned char *data) {
    if (!c || page_no < 0 || page_no >= c->page_count) return 0;
    pthread_mutex_lock(&c->mutex);
    if (bitmap_test(c->valid_bitmap, page_no)) {
        pthread_mutex_unlock(&c->mutex);
        return 0;  /* Already valid — don't clobber */
    }
    size_t offset = (size_t)page_no * (size_t)c->page_size;
    memcpy(c->pages_base + offset, data, (size_t)c->page_size);
    bitmap_set(c->valid_bitmap, page_no);
    if (c->checksums_enabled && c->checksums) {
        c->checksums[page_no] = crc32c(data, (size_t)c->page_size);
    }
    pthread_mutex_unlock(&c->mutex);
    return 1;
}

unsigned char *azqlite_cache_page_ptr(azqlite_cache_t *c, int page_no) {
    if (!c || page_no < 0 || page_no >= c->page_count) return NULL;
    return c->pages_base + (size_t)page_no * (size_t)c->page_size;
}

int azqlite_cache_page_verify(azqlite_cache_t *c, int page_no) {
    if (!c || page_no < 0 || page_no >= c->page_count) return 0;
    if (!c->checksums_enabled || !c->checksums) return 1;
    if (!bitmap_test(c->valid_bitmap, page_no)) return 0;
    size_t offset = (size_t)page_no * (size_t)c->page_size;
    uint32_t actual = crc32c(c->pages_base + offset, (size_t)c->page_size);
    return actual == c->checksums[page_no];
}

/* ===================================================================
** Dirty page tracking
** =================================================================== */

int azqlite_cache_page_dirty(azqlite_cache_t *c, int page_no) {
    if (!c || page_no < 0 || page_no >= c->page_count) return 0;
    return bitmap_test(c->dirty_bitmap, page_no);
}

void azqlite_cache_page_set_dirty(azqlite_cache_t *c, int page_no) {
    if (!c || page_no < 0 || page_no >= c->page_count) return;
    bitmap_set(c->dirty_bitmap, page_no);
}

int azqlite_cache_collect_dirty(azqlite_cache_t *c, int **out_pages) {
    if (!c || !out_pages) return 0;
    *out_pages = NULL;

    /* Count dirty pages first */
    int count = 0;
    for (int i = 0; i < c->page_count; i++) {
        if (bitmap_test(c->dirty_bitmap, i)) count++;
    }
    if (count == 0) return 0;

    int *pages = (int *)malloc((size_t)count * sizeof(int));
    if (!pages) return 0;

    int idx = 0;
    for (int i = 0; i < c->page_count; i++) {
        if (bitmap_test(c->dirty_bitmap, i)) {
            pages[idx++] = i;
        }
    }
    *out_pages = pages;
    return count;
}

void azqlite_cache_clear_dirty(azqlite_cache_t *c) {
    if (!c) return;
    memset(c->dirty_bitmap, 0, (size_t)c->bitmap_bytes);
}

int azqlite_cache_dirty_count(azqlite_cache_t *c) {
    if (!c) return 0;
    int count = 0;
    for (int i = 0; i < c->page_count; i++) {
        if (bitmap_test(c->dirty_bitmap, i)) count++;
    }
    return count;
}

/* ===================================================================
** Cache growth
** =================================================================== */

int azqlite_cache_grow(azqlite_cache_t *c, int new_page_count) {
    if (!c || new_page_count <= c->page_count) return 0;

    int old_bitmap_bytes = c->bitmap_bytes;
    int old_page_count = c->page_count;

    /* Save old bitmaps BEFORE unmapping (they live in the mmap'd region) */
    unsigned char *old_valid = NULL;
    unsigned char *old_dirty = NULL;
    uint32_t *old_checksums = NULL;
    if (old_bitmap_bytes > 0) {
        old_valid = (unsigned char *)malloc((size_t)old_bitmap_bytes);
        old_dirty = (unsigned char *)malloc((size_t)old_bitmap_bytes);
        if (old_valid && c->valid_bitmap)
            memcpy(old_valid, c->valid_bitmap, (size_t)old_bitmap_bytes);
        if (old_dirty && c->dirty_bitmap)
            memcpy(old_dirty, c->dirty_bitmap, (size_t)old_bitmap_bytes);
    }
    if (c->checksums_enabled && c->checksums && old_page_count > 0) {
        size_t cksum_bytes = (size_t)old_page_count * sizeof(uint32_t);
        old_checksums = (uint32_t *)malloc(cksum_bytes);
        if (old_checksums)
            memcpy(old_checksums, c->checksums, cksum_bytes);
    }

    /* Unmap existing */
    if (c->meta_base) munmap(c->meta_base, c->meta_size);
    if (c->pages_base) munmap(c->pages_base, c->pages_size);

    c->page_count = new_page_count;
    c->meta_size = meta_file_size(new_page_count, c->checksums_enabled);
    c->pages_size = (size_t)new_page_count * (size_t)c->page_size;

    /* Resize and remap metadata */
    if (ftruncate(c->meta_fd, (off_t)c->meta_size) != 0) {
        free(old_valid);
        free(old_dirty);
        free(old_checksums);
        return -1;
    }
    c->meta_base = (unsigned char *)mmap(NULL, c->meta_size,
                                          PROT_READ | PROT_WRITE,
                                          MAP_SHARED, c->meta_fd, 0);
    if (c->meta_base == MAP_FAILED) {
        free(old_valid);
        free(old_dirty);
        free(old_checksums);
        return -1;
    }

    /* Resize and remap page store */
    if (ftruncate(c->pages_fd, (off_t)c->pages_size) != 0) {
        free(old_valid);
        free(old_dirty);
        free(old_checksums);
        return -1;
    }
    c->pages_base = (unsigned char *)mmap(NULL, c->pages_size,
                                           PROT_READ | PROT_WRITE,
                                           MAP_SHARED, c->pages_fd, 0);
    if (c->pages_base == MAP_FAILED) {
        free(old_valid);
        free(old_dirty);
        free(old_checksums);
        return -1;
    }

    recompute_pointers(c);

    /* Clear new bitmap regions then restore old bits */
    memset(c->valid_bitmap, 0, (size_t)c->bitmap_bytes);
    memset(c->dirty_bitmap, 0, (size_t)c->bitmap_bytes);
    if (old_valid && old_bitmap_bytes > 0)
        memcpy(c->valid_bitmap, old_valid, (size_t)old_bitmap_bytes);
    if (old_dirty && old_bitmap_bytes > 0)
        memcpy(c->dirty_bitmap, old_dirty, (size_t)old_bitmap_bytes);
    if (c->checksums_enabled && c->checksums && old_checksums && old_page_count > 0) {
        memset(c->checksums, 0, (size_t)new_page_count * sizeof(uint32_t));
        memcpy(c->checksums, old_checksums, (size_t)old_page_count * sizeof(uint32_t));
    }

    free(old_valid);
    free(old_dirty);
    free(old_checksums);

    /* Update header */
    c->header->page_count = (uint32_t)new_page_count;

    return 0;
}

/* ===================================================================
** Sync
** =================================================================== */

void azqlite_cache_msync(azqlite_cache_t *c) {
    if (!c) return;
    if (c->meta_base && c->meta_base != MAP_FAILED)
        msync(c->meta_base, c->meta_size, MS_ASYNC);
    if (c->pages_base && c->pages_base != MAP_FAILED)
        msync(c->pages_base, c->pages_size, MS_ASYNC);
}

/* ===================================================================
** Mutex
** =================================================================== */

void azqlite_cache_lock(azqlite_cache_t *c) {
    if (c) pthread_mutex_lock(&c->mutex);
}

void azqlite_cache_unlock(azqlite_cache_t *c) {
    if (c) pthread_mutex_unlock(&c->mutex);
}

/* ===================================================================
** Accessors
** =================================================================== */

int azqlite_cache_page_size(azqlite_cache_t *c) {
    return c ? c->page_size : 0;
}

int azqlite_cache_page_count(azqlite_cache_t *c) {
    return c ? c->page_count : 0;
}

int64_t azqlite_cache_blob_size(azqlite_cache_t *c) {
    if (!c || !c->header) return 0;
    return c->header->blob_size;
}

void azqlite_cache_set_blob_size(azqlite_cache_t *c, int64_t size) {
    if (c && c->header) c->header->blob_size = size;
}
