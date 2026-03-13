# SQLite VFS API Deep Analysis

> Research by Aragorn (SQLite/C Dev) — sqlite-objs project
> Source: `sqlite-autoconf-3520000/sqlite3.h` and `sqlite3.c` (SQLite 3.52.0 amalgamation)

---

## Table of Contents

1. [VFS Architecture Overview](#1-vfs-architecture-overview)
2. [VFS API Surface — sqlite3_vfs](#2-vfs-api-surface--sqlite3_vfs)
3. [File API Surface — sqlite3_io_methods](#3-file-api-surface--sqlite3_io_methods)
4. [Locking Model](#4-locking-model)
5. [WAL Mode vs Journal Mode](#5-wal-mode-vs-journal-mode)
6. [File Types and I/O Patterns](#6-file-types-and-io-patterns)
7. [Page Size and Alignment](#7-page-size-and-alignment)
8. [VFS Registration and Chaining](#8-vfs-registration-and-chaining)
9. [Error Handling](#9-error-handling)
10. [Device Characteristics and Sector Size](#10-device-characteristics-and-sector-size)
11. [Memory-Mapped I/O (xFetch/xUnfetch)](#11-memory-mapped-io-xfetchxunfetch)
12. [Existing VFS Implementations](#12-existing-vfs-implementations)
13. [Limitations and Gotchas](#13-limitations-and-gotchas)
14. [Recommendations for Azure Blob VFS](#14-recommendations-for-azure-blob-vfs)

---

## 1. VFS Architecture Overview

SQLite's VFS (Virtual File System) is the primary extension point for custom storage backends. The architecture is a three-layer design:

```
Application
    │
    ▼
sqlite3_vfs          — VFS object: open files, delete, access, pathnames, randomness, time
    │
    ▼
sqlite3_file         — Base struct for an open file. Contains a single pointer to io_methods.
    │
    ▼
sqlite3_io_methods   — Per-file operations: read, write, lock, sync, etc.
```

**Key design principle:** SQLite allocates memory for `sqlite3_file` itself (at least `szOsFile` bytes). The VFS fills it in during `xOpen`. This means our custom struct must be a *subclass* (first member is `sqlite3_io_methods*`).

The VFS is selected at database open time via `sqlite3_open_v2()`:
```c
int sqlite3_open_v2(
  const char *filename,   /* Database filename (UTF-8) */
  sqlite3 **ppDb,         /* OUT: SQLite db handle */
  int flags,              /* Flags */
  const char *zVfs        /* Name of VFS module to use */
);
```

---

## 2. VFS API Surface — sqlite3_vfs

**Source:** `sqlite3.h:1507-1544`

```c
struct sqlite3_vfs {
  int iVersion;            /* Structure version number (currently 3) */
  int szOsFile;            /* Size of subclassed sqlite3_file */
  int mxPathname;          /* Maximum file pathname length */
  sqlite3_vfs *pNext;      /* Next registered VFS (managed by SQLite) */
  const char *zName;       /* Name of this virtual file system */
  void *pAppData;          /* Pointer to application-specific data */

  /* === Version 1 Methods (Required) === */
  int (*xOpen)(sqlite3_vfs*, sqlite3_filename zName, sqlite3_file*, int flags, int *pOutFlags);
  int (*xDelete)(sqlite3_vfs*, const char *zName, int syncDir);
  int (*xAccess)(sqlite3_vfs*, const char *zName, int flags, int *pResOut);
  int (*xFullPathname)(sqlite3_vfs*, const char *zName, int nOut, char *zOut);
  void *(*xDlOpen)(sqlite3_vfs*, const char *zFilename);
  void (*xDlError)(sqlite3_vfs*, int nByte, char *zErrMsg);
  void (*(*xDlSym)(sqlite3_vfs*,void*, const char *zSymbol))(void);
  void (*xDlClose)(sqlite3_vfs*, void*);
  int (*xRandomness)(sqlite3_vfs*, int nByte, char *zOut);
  int (*xSleep)(sqlite3_vfs*, int microseconds);
  int (*xCurrentTime)(sqlite3_vfs*, double*);
  int (*xGetLastError)(sqlite3_vfs*, int, char *);

  /* === Version 2 Methods === */
  int (*xCurrentTimeInt64)(sqlite3_vfs*, sqlite3_int64*);

  /* === Version 3 Methods (Optional, for testing) === */
  int (*xSetSystemCall)(sqlite3_vfs*, const char *zName, sqlite3_syscall_ptr);
  sqlite3_syscall_ptr (*xGetSystemCall)(sqlite3_vfs*, const char *zName);
  const char *(*xNextSystemCall)(sqlite3_vfs*, const char *zName);
};
```

### Method-by-Method Analysis

| Method | Required? | Purpose | Azure Blob Notes |
|--------|-----------|---------|-----------------|
| `xOpen` | **Yes** | Open a file. Must populate `sqlite3_file.pMethods` (or set NULL on failure). SQLite passes pre-allocated memory (>= `szOsFile` bytes). | Core implementation — maps filenames to blob operations. Must handle 8 file types (see §6). |
| `xDelete` | **Yes** | Delete a file. `syncDir` means fsync the directory after delete. | Delete blob from Azure. |
| `xAccess` | **Yes** | Check file existence/readability. Flags: `SQLITE_ACCESS_EXISTS` (0), `SQLITE_ACCESS_READWRITE` (1), `SQLITE_ACCESS_READ` (2). | HEAD request to blob. |
| `xFullPathname` | **Yes** | Convert relative path to absolute. Output buffer is `mxPathname+1` bytes. | Normalize blob container/path. Return `SQLITE_CANTOPEN` if buffer too small. |
| `xDlOpen` | **Yes** | Dynamic library loading. | **Can delegate to default VFS** or return NULL (no extension loading). |
| `xDlError` | **Yes** | Get dynamic library error message. | Can delegate or provide stub. |
| `xDlSym` | **Yes** | Lookup symbol in dynamic library. | Can delegate or return NULL. |
| `xDlClose` | **Yes** | Close dynamic library. | Can delegate or no-op. |
| `xRandomness` | **Yes** | Get random bytes. Return actual bytes obtained. | Delegate to default VFS. |
| `xSleep` | **Yes** | Sleep for N microseconds. | Delegate to default VFS. |
| `xCurrentTime` | **Yes** | Return Julian Day Number as double. | Delegate to default VFS. |
| `xGetLastError` | **Yes** | Get last error info. | Return our last Azure error. |
| `xCurrentTimeInt64` | Recommended | Higher precision time (Julian Day * 86400000). | Delegate to default VFS. Used if `iVersion >= 2`. |
| `xSetSystemCall` | Optional | Override system calls (testing). | Set to NULL or delegate. Not needed for production. |
| `xGetSystemCall` | Optional | Query system call overrides. | Set to NULL or delegate. |
| `xNextSystemCall` | Optional | Iterate system calls. | Set to NULL or delegate. |

**Critical xOpen details:**
- `zFilename` can be NULL — must create temp file name
- If NULL, `flags` will include `SQLITE_OPEN_DELETEONCLOSE`
- `pMethods` MUST be set even if xOpen fails (set to NULL to prevent xClose call)
- Filename is valid until xClose — can store pointer
- Filename may have a suffix: `"-" + up to 11 alphanumeric chars` (e.g., for journals)

---

## 3. File API Surface — sqlite3_io_methods

**Source:** `sqlite3.h:854-878`

```c
struct sqlite3_io_methods {
  int iVersion;
  /* Version 1 (Required) */
  int (*xClose)(sqlite3_file*);
  int (*xRead)(sqlite3_file*, void*, int iAmt, sqlite3_int64 iOfst);
  int (*xWrite)(sqlite3_file*, const void*, int iAmt, sqlite3_int64 iOfst);
  int (*xTruncate)(sqlite3_file*, sqlite3_int64 size);
  int (*xSync)(sqlite3_file*, int flags);
  int (*xFileSize)(sqlite3_file*, sqlite3_int64 *pSize);
  int (*xLock)(sqlite3_file*, int);
  int (*xUnlock)(sqlite3_file*, int);
  int (*xCheckReservedLock)(sqlite3_file*, int *pResOut);
  int (*xFileControl)(sqlite3_file*, int op, void *pArg);
  int (*xSectorSize)(sqlite3_file*);
  int (*xDeviceCharacteristics)(sqlite3_file*);
  /* Version 2 (WAL support) */
  int (*xShmMap)(sqlite3_file*, int iPg, int pgsz, int, void volatile**);
  int (*xShmLock)(sqlite3_file*, int offset, int n, int flags);
  void (*xShmBarrier)(sqlite3_file*);
  int (*xShmUnmap)(sqlite3_file*, int deleteFlag);
  /* Version 3 (Memory-mapped I/O) */
  int (*xFetch)(sqlite3_file*, sqlite3_int64 iOfst, int iAmt, void **pp);
  int (*xUnfetch)(sqlite3_file*, sqlite3_int64 iOfst, void *p);
};
```

### Method-by-Method Analysis

#### xClose
```c
int (*xClose)(sqlite3_file*);
```
- Called even if xOpen set pMethods to non-NULL but then returned an error
- Must free all resources associated with the file
- Return `SQLITE_OK` on success

#### xRead
```c
int (*xRead)(sqlite3_file*, void *pBuf, int iAmt, sqlite3_int64 iOfst);
```
- Read `iAmt` bytes starting at offset `iOfst` into `pBuf`
- Returns `SQLITE_OK` on success
- Returns `SQLITE_IOERR_SHORT_READ` if fewer bytes available — **MUST zero-fill the unread portion**
- **CRITICAL: Failure to zero-fill short reads WILL cause database corruption**
- The unix VFS uses `pread()` for positioned reads

#### xWrite
```c
int (*xWrite)(sqlite3_file*, const void *pBuf, int iAmt, sqlite3_int64 iOfst);
```
- Write `iAmt` bytes from `pBuf` at offset `iOfst`
- Returns `SQLITE_OK` on success, `SQLITE_IOERR_WRITE` on failure
- Writes can extend the file (implicit growth)
- The unix VFS uses `pwrite()` for positioned writes
- Write size is capped at 128KB internally: `assert( nBuf==(nBuf&0x1ffff) )`

#### xTruncate
```c
int (*xTruncate)(sqlite3_file*, sqlite3_int64 size);
```
- Truncate file to `size` bytes
- Used after journal rollback to restore original database size
- Used during VACUUM
- Returns `SQLITE_OK` or `SQLITE_IOERR_TRUNCATE`

#### xSync
```c
int (*xSync)(sqlite3_file*, int flags);
```
- Flush all writes to persistent storage
- `flags` is one of:
  - `SQLITE_SYNC_NORMAL` (0x02) — normal fsync
  - `SQLITE_SYNC_FULL` (0x03) — Mac OS X full fsync (F_FULLFSYNC)
  - Either can be OR'd with `SQLITE_SYNC_DATAONLY` (0x10) — sync data, not metadata
- **This is the durability guarantee.** After xSync returns OK, data must survive power loss.
- For Azure: this means ensuring the blob write is committed and durable.

#### xFileSize
```c
int (*xFileSize)(sqlite3_file*, sqlite3_int64 *pSize);
```
- Get current file size in bytes
- The unix VFS uses `fstat()` for this
- Called frequently — should be fast (consider caching)

#### xLock / xUnlock
```c
int (*xLock)(sqlite3_file*, int eLock);   /* SHARED, RESERVED, PENDING, EXCLUSIVE */
int (*xUnlock)(sqlite3_file*, int eLock); /* SHARED or NONE */
```
See §4 for full locking model analysis.

#### xCheckReservedLock
```c
int (*xCheckReservedLock)(sqlite3_file*, int *pResOut);
```
- Check if ANY connection (this process or other) holds RESERVED, PENDING, or EXCLUSIVE
- Set `*pResOut = 1` if such a lock exists, `0` otherwise
- Returns `SQLITE_OK` on success

#### xFileControl
```c
int (*xFileControl)(sqlite3_file*, int op, void *pArg);
```
- Generic escape hatch for VFS-specific operations
- Opcodes < 100 are reserved by SQLite core
- **Return `SQLITE_NOTFOUND` for unrecognized opcodes** (not SQLITE_ERROR)
- Important opcodes to handle:
  - `SQLITE_FCNTL_LOCKSTATE` — return current lock level (debug only)
  - `SQLITE_FCNTL_SIZE_HINT` — hint about upcoming file growth
  - `SQLITE_FCNTL_CHUNK_SIZE` — configure allocation chunk size
  - `SQLITE_FCNTL_PERSIST_WAL` — WAL persistence setting
  - `SQLITE_FCNTL_POWERSAFE_OVERWRITE` — toggle PSOW flag
  - `SQLITE_FCNTL_VFSNAME` — return VFS name (allocate with sqlite3_mprintf)
  - `SQLITE_FCNTL_SYNC` — sent before xSync or in place of xSync when synchronous=OFF
  - `SQLITE_FCNTL_COMMIT_PHASETWO` — sent after commit, before unlock
  - `SQLITE_FCNTL_HAS_MOVED` — detect if file was renamed/moved
  - `SQLITE_FCNTL_MMAP_SIZE` — configure memory-mapped I/O size
  - `SQLITE_FCNTL_TEMPFILENAME` — generate a temp filename

#### xSectorSize
```c
int (*xSectorSize)(sqlite3_file*);
```
- Returns sector size in bytes — minimum atomic write unit
- Default is `SQLITE_DEFAULT_SECTOR_SIZE` = 4096
- Must be a multiple of 512
- See §10 for recommendations.

#### xDeviceCharacteristics
```c
int (*xDeviceCharacteristics)(sqlite3_file*);
```
- Returns a bitmask of device properties
- See §10 for detailed flag analysis.

---

## 4. Locking Model

### The Five Lock Levels

```
NONE (0) → SHARED (1) → RESERVED (2) → PENDING (3) → EXCLUSIVE (4)
                                              ↑
                                    (never explicitly requested)
```

| Level | Value | Who Holds It | Purpose |
|-------|-------|-------------|---------|
| `SQLITE_LOCK_NONE` | 0 | Nobody | File not locked. Only valid as xUnlock target. |
| `SQLITE_LOCK_SHARED` | 1 | Readers | Multiple connections can hold SHARED simultaneously. Prevents writers from getting EXCLUSIVE. |
| `SQLITE_LOCK_RESERVED` | 2 | Single writer-in-progress | Only ONE connection can hold RESERVED. Signals intent to write. Other readers still allowed. |
| `SQLITE_LOCK_PENDING` | 3 | Writer upgrading to EXCLUSIVE | **Never explicitly requested** by SQLite (asserted in code). Blocks new SHARED locks but allows existing SHARED locks to drain. |
| `SQLITE_LOCK_EXCLUSIVE` | 4 | Active writer | Only ONE connection. No other locks of any kind allowed. Required for writing to the database file. |

### Lock Transition Rules

1. **NONE → SHARED** is the only valid first step. You CANNOT jump from NONE to anything higher.
2. **PENDING is never explicitly requested** — it's an internal transitional state.
3. **RESERVED requires SHARED** — you must hold SHARED to request RESERVED.
4. **xLock only upgrades** — argument is always SHARED or higher, never NONE.
5. **xUnlock only downgrades to SHARED or NONE** — you can't unlock to RESERVED.
6. If the file already has a lock >= requested, `xLock()` is a no-op (return SQLITE_OK).
7. If the file already has a lock <= requested, `xUnlock()` is a no-op (return SQLITE_OK).

### How the Unix VFS Implements Locking

**Source:** `sqlite3.c:41509-41738` (`unixLock`)

The unix VFS uses **POSIX advisory locks** (`fcntl(F_SETLK)`) on specific byte ranges within the database file:

```
Offset 0x40000000 (1GB):     PENDING_BYTE   (1 byte)
Offset 0x40000001:           RESERVED_BYTE  (1 byte)
Offset 0x40000002-0x400001FF: SHARED range   (510 bytes)
```

- **SHARED lock:** Read-lock on PENDING_BYTE (temporary, released after), then read-lock on SHARED range
- **RESERVED lock:** Write-lock on RESERVED_BYTE (while still holding SHARED)
- **PENDING lock:** Write-lock on PENDING_BYTE (blocks new SHARED acquisitions)
- **EXCLUSIVE lock:** Write-lock on entire SHARED range (waits for existing readers to release)

**Important:** Locking bytes are at offset 1GB in the file. The pager never allocates pages in this range. This means the locking range doesn't conflict with actual data.

### Mapping to Azure Blob Leases

**This is the hardest problem for our VFS.**

Azure Blob Storage offers:
- **Blob leases:** Exclusive lease on entire blob, 15-60s duration, renewable
- **No shared/read leases** — only exclusive
- **No byte-range locking**

**Gap analysis:**

| SQLite Lock | Requirement | Azure Capability | Gap |
|------------|-------------|-----------------|-----|
| SHARED | Multiple concurrent readers | No shared lock primitive | **Major gap** — need to implement via metadata or separate lock blob |
| RESERVED | Single writer intent, readers continue | Single lease | Partial — lease blocks all, not just writers |
| PENDING | Block new readers, existing continue | No equivalent | **Gap** — need custom implementation |
| EXCLUSIVE | Single writer, no readers | Blob lease | Works, but blocks ALL access, not just writes |

**Possible approaches:**
1. **Lock metadata blob:** A separate small blob that stores lock state as JSON/binary. Readers check state with conditional headers. Writers acquire lease on the lock blob.
2. **Lease-based with lock table blob:** Use a separate blob as a lock table. Atomic conditional writes using ETags for optimistic concurrency.
3. **Simplified single-writer model:** Only support `SQLITE_LOCK_NONE` and `SQLITE_LOCK_EXCLUSIVE`. Use blob lease for exclusive access. Significantly simpler but limits concurrency.
4. **No-lock VFS for read-only access:** Use `nolockLock` pattern (always return SQLITE_OK) for read-only replicas.

**Recommendation:** For MVP, implement a **single-writer VFS** using blob leases for EXCLUSIVE, with a simplified locking model. Readers operate without locks (read snapshot from blob). Concurrency control happens at the application layer. This avoids the distributed-lock complexity entirely.

### Busy Handling

When `xLock` returns `SQLITE_BUSY`, SQLite invokes the busy handler:
- `sqlite3_busy_handler()` — custom callback
- `sqlite3_busy_timeout()` — retry with backoff for N milliseconds
- If no busy handler, SQLite returns SQLITE_BUSY to the application immediately

For Azure, latency will be high. The busy handler retry pattern is important — we should support `SQLITE_ENABLE_SETLK_TIMEOUT` style blocking if possible.

---

## 5. WAL Mode vs Journal Mode

### Journal Mode (Rollback Journal)

**How it works:**
1. Before modifying a page, SQLite copies the original page to the journal file
2. Modified pages are written to the main database file
3. On commit: the journal file is deleted (or truncated)
4. On rollback: original pages are copied back from journal to database

**VFS operations used:**
- `xOpen` with `SQLITE_OPEN_MAIN_JOURNAL` flag
- Sequential writes to journal (append pattern)
- Random writes to main database file
- `xSync` on journal before writing to database
- `xSync` on database before deleting journal
- `xDelete` of journal file on commit
- `xTruncate` for journal truncation mode

**Files created:**
- `database-journal` — rollback journal
- `database-super-journal` — for multi-database transactions (rare)
- Subjournals for SAVEPOINT

**I/O pattern:** Journal writes are sequential (append). Database writes are random (page-aligned). Two syncs per transaction minimum.

### WAL Mode (Write-Ahead Log)

**How it works:**
1. Changes are appended to a WAL file instead of modifying the database
2. Readers see the database as it was when they started reading
3. A checkpoint operation transfers WAL content back to the database
4. Requires shared memory for WAL index coordination

**VFS operations used:**
- `xOpen` with `SQLITE_OPEN_WAL` flag
- Sequential append writes to WAL file
- Random reads from both WAL and main database
- **xShmMap, xShmLock, xShmBarrier, xShmUnmap** — shared memory for WAL index

**Files created:**
- `database-wal` — write-ahead log
- `database-shm` — shared memory (WAL index)

### Shared Memory Methods (WAL only, iVersion >= 2)

```c
int (*xShmMap)(sqlite3_file*, int iPg, int pgsz, int bExtend, void volatile **pp);
int (*xShmLock)(sqlite3_file*, int offset, int n, int flags);
void (*xShmBarrier)(sqlite3_file*);
int (*xShmUnmap)(sqlite3_file*, int deleteFlag);
```

**xShmMap:** Map a region of shared memory. The shared memory is divided into pages (typically 32KB each). `iPg` is the page number, `pgsz` is the page size, `bExtend` means create the region if it doesn't exist. Returns pointer to the mapped region via `*pp`.

**xShmLock:** Acquire or release locks on the shared memory. Uses 8 lock slots (SQLITE_SHM_NLOCK = 8):
- Slot 0: Write lock
- Slot 1: Checkpointer lock
- Slot 2: Recovery lock
- Slots 3-7: Read locks

Flags are combinations of:
- `SQLITE_SHM_LOCK | SQLITE_SHM_SHARED` — acquire shared lock
- `SQLITE_SHM_LOCK | SQLITE_SHM_EXCLUSIVE` — acquire exclusive lock
- `SQLITE_SHM_UNLOCK | SQLITE_SHM_SHARED` — release shared lock
- `SQLITE_SHM_UNLOCK | SQLITE_SHM_EXCLUSIVE` — release exclusive lock

**Cannot transition directly between SHARED and EXCLUSIVE** — must go through unlocked state.

**xShmBarrier:** Memory barrier. The unix VFS calls `sqlite3MemoryBarrier()` (compiler barrier) plus a mutex enter/leave for additional synchronization.

**xShmUnmap:** Disconnect from shared memory. If `deleteFlag` is true, delete the underlying shared memory file.

### Can WAL Work Over Remote Storage?

**Short answer: No, not practically.**

**Why not:**
1. **Shared memory requires local access:** The WAL index (shm file) must be accessible to all processes with sub-millisecond latency. It's used for lock coordination and to look up which WAL frames contain the latest version of each page. Network latency makes this impractical.
2. **xShmLock requires fine-grained, low-latency locking:** 8 independent lock slots with shared/exclusive semantics. This is essentially the distributed lock problem again, but with even more stringent latency requirements.
3. **Memory barriers assume shared address space:** `xShmBarrier` uses CPU memory barriers. These are meaningless across network boundaries.

**Setting xShmMap to NULL:** If our VFS sets `xShmMap` to NULL (or sets `iVersion` to 1), SQLite will **refuse to enter WAL mode** and fall back to journal mode. This is exactly what the `nolockIoMethods` does (sets `xShmMap` to 0).

### WAL2

No reference to WAL2 exists in the SQLite 3.52.0 source. WAL2 is a proposed extension (BEGIN CONCURRENT) that exists only in experimental branches. **Not relevant for our implementation.**

### Recommendation

**Use journal mode (DELETE or TRUNCATE) for MVP.** WAL mode is architecturally incompatible with remote blob storage. The shared memory requirement alone makes it impractical without a fundamental redesign (e.g., a local WAL with remote checkpointing, which is a much more complex architecture).

---

## 6. File Types and I/O Patterns

SQLite opens multiple file types, identified by flags passed to `xOpen`:

| Flag | Type | I/O Pattern | Remote? | Notes |
|------|------|-------------|---------|-------|
| `SQLITE_OPEN_MAIN_DB` (0x100) | Main database | Random read/write, page-aligned | **Yes — must be remote** | This is the primary database file. Page-aligned reads/writes. |
| `SQLITE_OPEN_MAIN_JOURNAL` (0x800) | Rollback journal | Sequential write (append), sequential read (rollback) | **Should be remote** (for crash safety) | Created on write transactions. Deleted on commit. |
| `SQLITE_OPEN_TEMP_DB` (0x200) | Temporary database | Random read/write | **No — local** | For temp tables. Has DELETEONCLOSE. |
| `SQLITE_OPEN_TEMP_JOURNAL` (0x1000) | Temp journal | Sequential | **No — local** | Journal for temp database. |
| `SQLITE_OPEN_TRANSIENT_DB` (0x400) | Transient/statement journal | Sequential | **No — local** | For complex queries. DELETEONCLOSE. |
| `SQLITE_OPEN_SUBJOURNAL` (0x2000) | Sub-journal | Sequential | **No — local** | For SAVEPOINT. DELETEONCLOSE. |
| `SQLITE_OPEN_SUPER_JOURNAL` (0x4000) | Super-journal | Small, sequential | Depends | For multi-database atomic commit. Rare. |
| `SQLITE_OPEN_WAL` (0x80000) | WAL file | Sequential append | N/A (not using WAL) | Only if WAL mode enabled. |

**Key insight:** Only MAIN_DB and MAIN_JOURNAL need to be on Azure. All temp/transient files should be local for performance. The VFS can detect the file type from the flags and route accordingly.

**Implementation strategy:**
- Check `flags & 0x0FFF00` to determine file type
- For `SQLITE_OPEN_MAIN_DB` → Azure blob-backed file
- For `SQLITE_OPEN_MAIN_JOURNAL` → Azure blob-backed file (for durability)
- For everything else → delegate to the default unix VFS (local temp files)

Additional flags that come with xOpen:
- `SQLITE_OPEN_DELETEONCLOSE` (0x08) — file should be deleted when closed
- `SQLITE_OPEN_EXCLUSIVE` (0x10) — file must be created, error if exists (always paired with CREATE)
- `SQLITE_OPEN_READONLY` (0x01) / `SQLITE_OPEN_READWRITE` (0x02) — access mode
- `SQLITE_OPEN_CREATE` (0x04) — create if doesn't exist

---

## 7. Page Size and Alignment

### SQLite Page Size

- **Default:** 4096 bytes (`SQLITE_DEFAULT_PAGE_SIZE`)
- **Range:** 512 to 65536 bytes (`SQLITE_MAX_PAGE_SIZE`)
- **Common values:** 4096, 8192, 16384, 32768
- Set via `PRAGMA page_size` (must be set before first write)
- Page size is stored in the database header (bytes 16-17)

### I/O Granularity

**SQLite always reads and writes in page-sized units** for the main database file, with these exceptions:
1. First page read may be a partial read to check the header (first 100 bytes)
2. The `SQLITE_IOCAP_SUBPAGE_READ` flag allows SQLite to do sub-page reads
3. Without `SQLITE_IOCAP_SUBPAGE_READ`, SQLite reads full pages on aligned boundaries

**For the journal file:** Writes are a page header (8 or 12 bytes) followed by the full page content, written sequentially.

**Actual read/write calls observed:**
- Main DB: `xRead(file, buf, pageSize, pageNumber * pageSize)` — always page-aligned
- Main DB: `xWrite(file, buf, pageSize, pageNumber * pageSize)` — always page-aligned
- Journal: `xWrite(file, buf, N, offset)` — sequential, N varies (header + page)
- Write sizes are capped at 128KB (`nBuf & 0x1ffff`)

### Azure Page Blob Compatibility

Azure Page Blobs require:
- Writes must be 512-byte aligned
- Write sizes must be multiples of 512 bytes

**This is compatible with SQLite's I/O patterns:**
- All standard SQLite page sizes (512, 1024, 2048, 4096, ..., 65536) are multiples of 512
- Page offsets are multiples of page size, which are multiples of 512
- We should set minimum page size to 512 (which is SQLite's minimum anyway)

**However,** Azure Page Blobs have a maximum size of 8TB and require pre-allocation or auto-growth. We need to handle file growth carefully.

**Alternative: Azure Block Blobs**
- No alignment requirements
- But no random write support — must rewrite entire blob or use staged blocks
- Better for journal files (sequential write)
- Could work for main DB with a read-cache + write-back strategy

**Alternative: Azure Append Blobs**
- Append-only
- Perfect for journal files
- Cannot be used for main database (requires random write)

---

## 8. VFS Registration and Chaining

### Registration

```c
SQLITE_API sqlite3_vfs *sqlite3_vfs_find(const char *zVfsName);
SQLITE_API int sqlite3_vfs_register(sqlite3_vfs*, int makeDflt);
SQLITE_API int sqlite3_vfs_unregister(sqlite3_vfs*);
```

- `sqlite3_vfs_register(pVfs, 1)` — register and make default
- `sqlite3_vfs_register(pVfs, 0)` — register without making default
- VFS names are case-sensitive, zero-terminated UTF-8
- The same VFS can be registered multiple times safely
- If two VFSes with the same name are registered, behavior is undefined

### Chaining / Wrapping

**A custom VFS can wrap the default VFS for operations it doesn't need to override:**

```c
sqlite3_vfs *pDefault = sqlite3_vfs_find(NULL);  /* Get default VFS */

/* For xDlOpen, xRandomness, xSleep, xCurrentTime, etc. — delegate */
myVfs.xRandomness = pDefault->xRandomness;
myVfs.xSleep = pDefault->xSleep;
```

**For temp files:** In `xOpen`, check the file type flags. If it's a temp file, delegate entirely to the default VFS's `xOpen`. The challenge is that `szOsFile` must be large enough for both our struct AND the default VFS's struct.

**UNIXVFS macro pattern** (from source):
```c
#define UNIXVFS(VFSNAME, FINDER) {       \
  3,                    /* iVersion */   \
  sizeof(unixFile),     /* szOsFile */   \
  MAX_PATHNAME,         /* mxPathname */ \
  0,                    /* pNext */      \
  VFSNAME,              /* zName */      \
  (void*)&FINDER,       /* pAppData */   \
  unixOpen,             /* xOpen */      \
  unixDelete,           /* xDelete */    \
  ...                                    \
}
```

### Selecting the VFS

Applications specify the VFS in three ways:
1. `sqlite3_open_v2(filename, &db, flags, "sqlite-objs")` — explicit VFS name
2. URI parameter: `sqlite3_open_v2("file:test.db?vfs=sqlite-objs", &db, SQLITE_OPEN_URI, NULL)`
3. Making it the default: `sqlite3_vfs_register(pVfs, 1)` — all opens use it

---

## 9. Error Handling

### VFS Error Codes

All VFS methods return `int`. The following error codes are relevant:

| Code | Value | When to Use |
|------|-------|-------------|
| `SQLITE_OK` | 0 | Success |
| `SQLITE_BUSY` | 5 | Lock contention — triggers busy handler |
| `SQLITE_NOMEM` | 7 | Memory allocation failed |
| `SQLITE_READONLY` | 8 | Write attempted on read-only file |
| `SQLITE_IOERR` | 10 | Generic I/O error (base code) |
| `SQLITE_FULL` | 13 | Disk/storage full |
| `SQLITE_CANTOPEN` | 14 | Cannot open file |
| `SQLITE_NOTFOUND` | 12 | **For xFileControl:** unrecognized opcode |

### Extended Error Codes (SQLITE_IOERR variants)

```c
SQLITE_IOERR_READ              (SQLITE_IOERR | (1<<8))   /* 266 */
SQLITE_IOERR_SHORT_READ        (SQLITE_IOERR | (2<<8))   /* 522 - not really an error */
SQLITE_IOERR_WRITE             (SQLITE_IOERR | (3<<8))   /* 778 */
SQLITE_IOERR_FSYNC             (SQLITE_IOERR | (4<<8))   /* 1034 */
SQLITE_IOERR_TRUNCATE          (SQLITE_IOERR | (6<<8))   /* 1546 */
SQLITE_IOERR_FSTAT             (SQLITE_IOERR | (7<<8))   /* 1802 */
SQLITE_IOERR_UNLOCK            (SQLITE_IOERR | (8<<8))   /* 2058 */
SQLITE_IOERR_LOCK              (SQLITE_IOERR | (15<<8))  /* 3850 */
SQLITE_IOERR_DELETE            (SQLITE_IOERR | (10<<8))  /* 2570 */
SQLITE_IOERR_ACCESS            (SQLITE_IOERR | (13<<8))  /* 3338 */
SQLITE_IOERR_CHECKRESERVEDLOCK (SQLITE_IOERR | (14<<8))  /* 3594 */
SQLITE_IOERR_CLOSE             (SQLITE_IOERR | (16<<8))  /* 4106 */
SQLITE_IOERR_SHMOPEN           (SQLITE_IOERR | (18<<8))
SQLITE_IOERR_SHMSIZE           (SQLITE_IOERR | (19<<8))
SQLITE_IOERR_SHMLOCK           (SQLITE_IOERR | (20<<8))
SQLITE_IOERR_SHMMAP            (SQLITE_IOERR | (21<<8))
```

### How SQLite Handles VFS Errors

- **SQLITE_BUSY from xLock:** Invokes busy handler. If busy handler returns non-zero, retries. If busy handler returns zero (or no handler), returns SQLITE_BUSY to application.
- **SQLITE_IOERR from any method:** Generally fatal to the current operation. SQLite will attempt to clean up (rollback) but the transaction is lost.
- **SQLITE_FULL:** Transaction fails, can be retried after freeing space.
- **SQLITE_IOERR_SHORT_READ from xRead:** Not actually an error — SQLite handles this by using the zero-filled buffer. Used when reading beyond end of file.
- **xFileControl returning SQLITE_NOTFOUND:** Not an error — just means the VFS doesn't handle that opcode.

**Important:** For Azure, network errors should generally map to `SQLITE_IOERR_*` variants. Timeout/throttling could potentially map to `SQLITE_BUSY` to trigger retry logic.

---

## 10. Device Characteristics and Sector Size

### xSectorSize

Returns the minimum write unit. For Azure blob storage:
- **Page blobs:** 512 bytes (natural alignment)
- **Block blobs:** Effectively the entire blob (no partial writes)

**Recommendation:** Return **4096** (matches SQLite default and common page size). This avoids any sub-page write issues.

### xDeviceCharacteristics Flags

| Flag | Value | Meaning | Set for Azure? | Rationale |
|------|-------|---------|----------------|-----------|
| `SQLITE_IOCAP_ATOMIC` | 0x0001 | All writes are atomic | **No** | Azure writes are not guaranteed atomic at arbitrary sizes |
| `SQLITE_IOCAP_ATOMIC4K` | 0x0010 | 4KB aligned writes are atomic | **Maybe** | Azure page blob 512-byte writes are atomic. A single 4KB page write maps to a single Azure API call. |
| `SQLITE_IOCAP_SAFE_APPEND` | 0x0200 | Appends extend atomically | **No** | Not guaranteed with blob storage |
| `SQLITE_IOCAP_SEQUENTIAL` | 0x0400 | Writes are ordered | **Yes** | Single-threaded writes to Azure are ordered by HTTP request/response |
| `SQLITE_IOCAP_POWERSAFE_OVERWRITE` | 0x1000 | Adjacent bytes unchanged on crash | **Yes** | Azure blob writes don't affect adjacent bytes |
| `SQLITE_IOCAP_IMMUTABLE` | 0x2000 | File never changes | **No** | Unless we have a read-only snapshot |
| `SQLITE_IOCAP_BATCH_ATOMIC` | 0x4000 | Batch atomic write support | **No** | Would require special Azure transaction support |
| `SQLITE_IOCAP_SUBPAGE_READ` | 0x8000 | OK to read less than a page | **Yes** | We can read any range from Azure |

**Recommended flags for Azure VFS:**
```c
SQLITE_IOCAP_SEQUENTIAL | SQLITE_IOCAP_POWERSAFE_OVERWRITE | SQLITE_IOCAP_SUBPAGE_READ
```

**Why POWERSAFE_OVERWRITE matters:** Without this flag, SQLite writes extra padding in the journal to protect against torn writes. With PSOW, this padding is eliminated, significantly reducing journal I/O. Since Azure blob writes are all-or-nothing at the API level, PSOW is safe to assert.

**Why NOT ATOMIC:** Setting `SQLITE_IOCAP_ATOMIC` would allow SQLite to skip writing to the journal entirely for single-page writes. While tempting (reduces latency), this is **dangerous** because Azure writes could fail mid-way from the client's perspective (network timeout with write actually succeeding). Journal provides crash safety.

---

## 11. Memory-Mapped I/O (xFetch/xUnfetch)

**Source:** `sqlite3.c:45357-45419`

```c
int (*xFetch)(sqlite3_file*, sqlite3_int64 iOfst, int iAmt, void **pp);
int (*xUnfetch)(sqlite3_file*, sqlite3_int64 iOfst, void *p);
```

**xFetch:** Returns a pointer to a memory-mapped region. If the VFS cannot provide memory-mapped access, set `*pp = 0` and return `SQLITE_OK`. SQLite will fall back to `xRead`.

**xUnfetch:** Release a reference from xFetch. If `p` is NULL, invalidate all mappings (the VFS should unmap).

**For Azure VFS: Set iVersion to 1 or return *pp = 0 from xFetch.**

Memory-mapped I/O is a performance optimization for local files. It makes no sense for remote storage. Setting `iVersion = 2` (for WAL support) without implementing xFetch/xUnfetch is fine — just set them to stubs that return `*pp = 0`.

Actually, since we're not supporting WAL mode either, we can set `iVersion = 1`, which makes xShmMap/xShmLock/xShmBarrier/xShmUnmap and xFetch/xUnfetch all irrelevant.

---

## 12. Existing VFS Implementations

### Implementations in the SQLite Source

The unix VFS provides multiple locking strategies, all sharing the same read/write/sync/filesize implementations:

1. **posixIoMethods** (default) — POSIX `fcntl(F_SETLK)` advisory locking. Full shared/exclusive support.
2. **nolockIoMethods** — No locking at all. `xLock`/`xUnlock`/`xCheckReservedLock` are all no-ops.
3. **dotlockIoMethods** — Uses lock files (directories) for locking. Works on any filesystem. Zero concurrency.
4. **flockIoMethods** — Uses `flock()` instead of `fcntl()`. Limited lock levels.
5. **semIoMethods** — Named semaphore locking (VxWorks).
6. **afpIoMethods** — AFP (Apple Filing Protocol) locking.
7. **nfsIoMethods** — NFS-aware locking with special unlock handling.

### nolockIoMethods — The Simplest Reference

**Source:** `sqlite3.c:42036-42055`

```c
static int nolockCheckReservedLock(sqlite3_file *NotUsed, int *pResOut){
  *pResOut = 0;
  return SQLITE_OK;
}
static int nolockLock(sqlite3_file *NotUsed, int NotUsed2){
  return SQLITE_OK;
}
static int nolockUnlock(sqlite3_file *NotUsed, int NotUsed2){
  return SQLITE_OK;
}
static int nolockClose(sqlite3_file *id) {
  return closeUnixFile(id);
}
```

This is the **minimum viable locking implementation.** It always succeeds, never blocks, and provides no concurrency protection. Registered as `unix-none`. This is our starting point for MVP — implement the Azure blob read/write/sync and use nolock semantics initially.

### Key Pattern: The IOMETHODS Macro

The unix VFS uses a macro to stamp out io_methods structs that share common methods (read, write, truncate, sync, filesize, filecontrol, sectorsize, devicechar) but differ in locking and close:

```c
#define IOMETHODS(FINDER, METHOD, VERSION, CLOSE, LOCK, UNLOCK, CKLOCK, SHMMAP) \
static const sqlite3_io_methods METHOD = {                                       \
   VERSION,              /* iVersion */                                          \
   CLOSE,                /* xClose */                                            \
   unixRead,             /* xRead */                                             \
   unixWrite,            /* xWrite */                                            \
   unixTruncate,         /* xTruncate */                                         \
   unixSync,             /* xSync */                                             \
   unixFileSize,         /* xFileSize */                                         \
   LOCK,                 /* xLock */                                             \
   UNLOCK,               /* xUnlock */                                           \
   CKLOCK,               /* xCheckReservedLock */                                \
   unixFileControl,      /* xFileControl */                                      \
   unixSectorSize,       /* xSectorSize */                                       \
   unixDeviceCharacteristics,  /* xDeviceCapabilities */                         \
   SHMMAP,               /* xShmMap */                                           \
   unixShmLock,          /* xShmLock */                                          \
   unixShmBarrier,       /* xShmBarrier */                                       \
   unixShmUnmap,         /* xShmUnmap */                                         \
   unixFetch,            /* xFetch */                                            \
   unixUnfetch,          /* xUnfetch */                                          \
};
```

We should follow this pattern — separate the locking strategy from the I/O strategy.

---

## 13. Limitations and Gotchas

### Synchronous Operations

**All VFS methods are synchronous.** SQLite calls `xRead`, expects data immediately. Calls `xWrite`, expects it's buffered. Calls `xSync`, expects durability. There is NO async VFS API.

This means Azure HTTP calls will block the calling thread. Mitigation:
- **Read caching:** Cache recently read pages in memory to avoid HTTP round-trips
- **Write buffering:** Buffer writes locally, flush on `xSync`
- **Connection pooling:** Reuse HTTP connections (keep-alive)

### Latency Assumptions

The unix VFS assumes microsecond-level I/O latency. Azure blob latency is typically 5-50ms per operation. This means:
- Each `xRead` call is 5-50ms (catastrophic for page-by-page reads without caching)
- Each `xSync` call is 5-50ms (acceptable — syncs are infrequent)
- Lock acquisition is 5-50ms (acceptable — locks are infrequent)

**Read cache is absolutely critical for usable performance.** Without it, reading a 100-page database requires 100 sequential HTTP requests (~5 seconds). With a cache that reads the entire blob on first access, it's one HTTP request.

### Thread Safety

SQLite's VFS must be thread-safe if SQLite is compiled with `SQLITE_THREADSAFE=1` (the default). Multiple threads may call VFS methods concurrently on different `sqlite3_file` objects. However, SQLite guarantees that **a single sqlite3_file is only accessed by one thread at a time** (the database connection mutex ensures this).

So: VFS-global state (e.g., cache) needs mutex protection. Per-file state does not.

### The Locking Range

The bytes at offset `PENDING_BYTE` (0x40000000 = 1GB) through `PENDING_BYTE + 511` are reserved for locking. The pager will never read or write these bytes. This means:
- Database files can be up to ~1GB without any issues
- Above 1GB, the pager skips the locking page
- For Azure, we don't need to worry about this unless we're emulating byte-range locking (we're not)

### PRAGMA Statements Affecting VFS

| PRAGMA | Effect on VFS |
|--------|--------------|
| `PRAGMA journal_mode=DELETE\|TRUNCATE\|PERSIST\|OFF\|MEMORY\|WAL` | Determines which files are opened and how |
| `PRAGMA synchronous=OFF\|NORMAL\|FULL\|EXTRA` | Controls how often xSync is called |
| `PRAGMA page_size=N` | Changes I/O granularity |
| `PRAGMA locking_mode=NORMAL\|EXCLUSIVE` | EXCLUSIVE holds locks for the connection lifetime |
| `PRAGMA mmap_size=N` | Controls memory-mapped I/O (xFetch/xUnfetch) |
| `PRAGMA temp_store=DEFAULT\|FILE\|MEMORY` | Where temp tables go |
| `PRAGMA cache_size=N` | SQLite's own page cache size (reduces xRead calls) |

**Recommended PRAGMAs for Azure VFS:**
```sql
PRAGMA journal_mode=DELETE;    -- or TRUNCATE (simpler for remote)
PRAGMA synchronous=NORMAL;     -- FULL is safer but slower
PRAGMA page_size=4096;         -- matches Azure 512-byte alignment
PRAGMA locking_mode=EXCLUSIVE; -- holds lock for connection lifetime (reduces lock chatter)
PRAGMA mmap_size=0;            -- disable mmap (useless for remote)
PRAGMA cache_size=-8192;       -- 8MB cache to reduce reads (negative = KB)
```

### xDelete and syncDir

`xDelete(vfs, zName, syncDir)` — the `syncDir` parameter tells the VFS to fsync the directory containing the deleted file. This ensures the delete is durable. For Azure, blob deletion is already durable when the API call returns, so we can ignore `syncDir`.

### NULL Filename in xOpen

If `zFilename` is NULL in xOpen, the VFS must create a temporary file with a unique name. The flags will include `SQLITE_OPEN_DELETEONCLOSE`. This should be handled by delegating to the local VFS.

### The pMethods Trap

If `xOpen` sets `pFile->pMethods` to a non-NULL value, `xClose` WILL be called even if `xOpen` returned an error. If xOpen fails, either:
1. Set `pMethods = NULL` to prevent xClose from being called, OR
2. Ensure your xClose can handle a partially-initialized file

---

## 14. Recommendations for Azure Blob VFS

### MVP Architecture

```
sqlite-objsVfs (sqlite3_vfs)
    ├── xOpen (MAIN_DB)     → sqlite-objsFile (our struct, Azure blob-backed)
    ├── xOpen (MAIN_JOURNAL) → sqlite-objsFile (Azure blob-backed)
    ├── xOpen (TEMP/*)       → delegate to default VFS (local files)
    ├── xDelete              → Azure blob delete
    ├── xAccess              → Azure blob exists check
    ├── xFullPathname        → normalize Azure path (container/blob)
    └── xRandomness/xSleep/xCurrentTime → delegate to default VFS

sqlite-objsFile (sqlite3_file subclass)
    ├── pMethod → sqlite-objsIoMethods (iVersion=1)
    │   ├── xRead         → read from local cache, fill from Azure on miss
    │   ├── xWrite        → write to local buffer
    │   ├── xSync         → flush local buffer to Azure blob
    │   ├── xTruncate     → update Azure blob size
    │   ├── xFileSize     → return cached size
    │   ├── xLock/xUnlock → nolock (MVP) or blob lease (later)
    │   ├── xCheckReservedLock → always return 0 (MVP)
    │   ├── xFileControl  → handle known opcodes, SQLITE_NOTFOUND for rest
    │   ├── xSectorSize   → return 4096
    │   └── xDeviceCharacteristics → SEQUENTIAL | POWERSAFE_OVERWRITE | SUBPAGE_READ
    └── xClose → flush + free resources
```

### Struct Layout

```c
typedef struct sqlite-objsFile sqlite-objsFile;
struct sqlite-objsFile {
    sqlite3_io_methods const *pMethod;  /* MUST be first */
    /* Azure connection state */
    char *zBlobUrl;                     /* Full Azure blob URL */
    /* Local buffer for write-back */
    unsigned char *aData;               /* Cached copy of blob content */
    sqlite3_int64 nData;                /* Current size */
    sqlite3_int64 nAlloc;               /* Allocated size */
    int isDirty;                        /* True if local changes need flushing */
    /* Lock state */
    int eLock;                          /* Current lock level */
    /* File type */
    int eFileType;                      /* SQLITE_OPEN_MAIN_DB, etc. */
};
```

### Implementation Priority

1. **Phase 1 (MVP Read-Only):** xOpen (download blob), xRead (from cache), xFileSize, xClose. No locking. Read-only access to existing databases.
2. **Phase 2 (MVP Read-Write):** xWrite (to local buffer), xSync (upload), xTruncate. Journal support. Single-writer.
3. **Phase 3 (Locking):** Blob lease-based locking for multi-client safety.
4. **Phase 4 (Performance):** Read cache with range requests, connection pooling, write batching.

### Hard Problems (in order of difficulty)

1. **Latency** — Read cache is mandatory. Without it, the VFS is unusable.
2. **Locking** — Azure has no shared lock primitive. Must build from primitives.
3. **Crash recovery** — Journal file must be durable on Azure before database writes begin.
4. **Concurrent access** — Multiple processes/machines need coordination.
5. **Large databases** — Downloading entire blob for cache is O(n). Need range-read strategy.

---

*End of analysis. The VFS API is well-documented, well-designed, and provides clean extension points. The hard part isn't the API — it's mapping Azure blob semantics onto SQLite's assumptions about local, low-latency, POSIX-like storage.*
