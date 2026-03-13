# Design Review: sqlite-objs MVP 1

> **Facilitator:** Gandalf (Lead/Architect)
> **Date:** 2026-03-10
> **Status:** APPROVED — Gates all implementation work
> **Inputs:** Prior art survey (832 lines), Azure PoC (Frodo), VFS deep-dive (Aragorn), Testing strategy (Samwise), 7 decision proposals

---

## Executive Summary

After synthesizing research from all four agents across 2,700+ lines of analysis, the architecture for sqlite-objs MVP 1 is clear: **a single-file SQLite VFS backed by Azure Page Blobs, using blob leases for writer exclusion, an in-memory full-blob cache for reads, a local write buffer flushed on sync, rollback journal mode (DELETE), and a swappable `azure_ops_t` vtable for testability.** The key technical insight — that Azure Page Blobs support 512-byte aligned random writes, perfectly matching SQLite's page I/O model — is validated by both Frodo's PoC and Aragorn's VFS analysis, and has no precedent in the 18+ prior art projects surveyed. This gives us a genuine architectural advantage: zero write amplification, no block-chunking complexity, and direct page-to-offset mapping.

---

## Decision Log

### Decision 1: Blob Type Strategy

**Decision:** Page blobs for the main database file. Block blobs for journal files.

**Rationale:**

All three technical agents agree. Frodo's analysis proves the alignment math:

```
SQLite page 512  → 1 Azure page   (512 / 512)
SQLite page 4096 → 8 Azure pages  (4096 / 512)
```

All SQLite page sizes (512–65536) are powers of 2 ≥ 512, so they *always* align with Azure Page Blob's 512-byte page requirement. This is not a coincidence we're exploiting — it's a fundamental compatibility that makes the entire project viable.

Block blobs for journal files because journals are written sequentially and read entirely during recovery — a perfect fit for whole-object storage. Block blobs are cheaper ($0.055/10K writes vs $0.005/10K for page blobs, but page blob writes are individually useful while block blob writes replace the entire object).

Append blobs: **rejected.** Frodo correctly notes they can't be truncated, which journals require on checkpoint.

**What this means for each agent:**
- Aragorn: `xRead`/`xWrite` on MAIN_DB → page blob range read/write. `xRead`/`xWrite` on MAIN_JOURNAL → block blob download/upload.
- Frodo: Both blob types are already implemented in the PoC. No new Azure API work needed.
- Samwise: Test both blob types. Page blob alignment violations must be caught at the VFS layer, not by Azure returning 400.

---

### Decision 2: WAL vs Journal Mode

**Decision:** Rollback journal mode (DELETE) for MVP 1. WAL mode is explicitly disabled. No WAL support planned until MVP 3+ at the earliest.

**Rationale:**

Aragorn's VFS analysis is definitive: WAL requires `xShmMap`/`xShmLock`/`xShmBarrier`/`xShmUnmap` — shared memory methods that coordinate readers and writers through 8 fine-grained lock slots with sub-millisecond latency requirements. This is architecturally incompatible with remote blob storage. Every prior art project that attempted WAL over a network either failed or confined it to single-machine use with `PRAGMA locking_mode=EXCLUSIVE` (which defeats WAL's concurrency benefit).

**Implementation:**
- Set `sqlite3_io_methods.iVersion = 1` (excludes all shared memory and mmap methods)
- SQLite will automatically refuse `PRAGMA journal_mode=WAL` and remain in DELETE mode
- This eliminates 4 method implementations and the entire shared-memory subsystem

**Path to WAL in future MVPs:**
- MVP 3 (multi-machine reads): Still journal mode. Readers read snapshots.
- Future: A local WAL with remote checkpointing pattern (à la Litestream) could work, but it's a fundamentally different architecture — not a VFS extension. This is a strategic decision for post-MVP.

**Recommended PRAGMAs (set automatically by the VFS via xFileControl or documented for users):**
```sql
PRAGMA journal_mode=DELETE;
PRAGMA synchronous=NORMAL;
PRAGMA page_size=4096;
PRAGMA locking_mode=EXCLUSIVE;
PRAGMA mmap_size=0;
PRAGMA cache_size=-8192;  -- 8MB SQLite page cache
```

---

### Decision 3: Locking Strategy

**Decision:** Two-level lease-based locking from day 1. Not nolock.

**Rationale:**

This is where I override Aragorn's initial recommendation. He proposed starting with `nolockLock` (always return SQLITE_OK) for simplicity. I understand the appeal, but the prior art survey is clear: **projects that defer locking see corruption in practice** (CBS, sqlite-s3vfs). Locking must be built into the VFS from the first version that supports writes.

The good news: Frodo's lease analysis shows the mapping is simpler than it first appears.

**The key insight:** Azure blob leases are writer-exclusive but reader-permissive. Reads NEVER require a lease. This maps naturally to a two-level model:

| SQLite Lock Level | Azure Action | Notes |
|---|---|---|
| NONE | Release lease (if held) | Clean state |
| SHARED | No-op | Reads always work without a lease |
| RESERVED | Acquire 30-second lease on DB blob | Blocks other writers |
| PENDING | (same lease, transitional) | SQLite handles internally |
| EXCLUSIVE | (same lease from RESERVED) | Already have exclusive write access |
| Unlock to SHARED | Release lease | Allow other writers |
| Unlock to NONE | Release lease | Full release |

**Why this works:**
1. SQLite guarantees SHARED → RESERVED → EXCLUSIVE is the only upgrade path
2. We acquire the lease exactly once (at RESERVED), hold it through EXCLUSIVE
3. Readers never contend — reads don't need leases
4. `xCheckReservedLock`: HEAD request on blob, check `x-ms-lease-state` header. If "leased", set `*pResOut = 1`.
5. 30-second lease with proactive renewal. Renewal happens at xSync time (natural write boundary) and via a simple "renew if older than 15s" check in xWrite.

**Lease lifecycle:**
- Acquire: On `xLock(RESERVED)` — 30-second duration, get lease ID back
- Renew: At `xSync` and periodically during long write sequences
- Release: On `xUnlock(SHARED)` or `xUnlock(NONE)`
- Break: Recovery path — if we detect a stale lease (client crashed), break with 0-second break period

**What we're NOT doing (yet):**
- No reader counting/tracking (needed for strict PENDING semantics)
- No background renewal thread (too complex for MVP 1; inline renewal is sufficient)
- No multi-machine lock coordination beyond the lease itself

**Risk:** Lease expiry during a long transaction. Mitigation: renew proactively. If renewal fails (network partition), the next xWrite or xSync must detect the lost lease and return SQLITE_IOERR_LOCK. SQLite will rollback.

---

### Decision 4: Caching Strategy

**Decision:** Full blob download on open + in-memory write buffer. This is mandatory for MVP 1 — NOT deferred to MVP 2.

**Rationale:**

I'm correcting my own earlier recommendation from the prior art survey. I originally said "no read caching in MVP 1 — every read goes to Azure." Aragorn's analysis changed my mind:

> "Each xRead call is 5-50ms (catastrophic for page-by-page reads without caching). Reading a 100-page database requires 100 sequential HTTP requests (~5 seconds)."

He's right. Without caching, a simple `SELECT * FROM t` on a 100-page database would take 5 seconds. This makes the VFS not just slow but *untestable* — every test run would take minutes. The original plan to test MVP 1 without caching was unrealistic.

**Architecture:**

```c
struct sqlite-objsFile {
    sqlite3_io_methods const *pMethod;  /* MUST be first */
    /* Azure connection info */
    azure_ops_t *ops;                   /* Swappable Azure operations (vtable) */
    void *ops_ctx;                      /* Context for ops (azure_client_t* or mock) */
    char *zBlobName;                    /* Blob name in container */
    /* In-memory buffer (cache + write buffer) */
    unsigned char *aData;               /* Full blob content */
    sqlite3_int64 nData;                /* Current logical size */
    sqlite3_int64 nAlloc;               /* Allocated buffer size */
    /* Dirty page tracking */
    unsigned char *aDirty;              /* Bitmap: 1 bit per page, 1 = dirty */
    int nDirtyPages;                    /* Count of dirty pages for fast check */
    int pageSize;                       /* Detected from header or default 4096 */
    /* Lock state */
    int eLock;                          /* Current SQLite lock level */
    char leaseId[64];                   /* Azure lease ID (empty = no lease) */
    time_t leaseAcquiredAt;             /* For renewal timing */
    /* File type */
    int eFileType;                      /* SQLITE_OPEN_MAIN_DB, etc. */
};
```

**xOpen (MAIN_DB):**
1. HEAD request → get blob size
2. malloc(blob_size) for aData
3. GET entire blob → fill aData
4. Initialize dirty bitmap (all clean)

**xRead:** memcpy from aData. Pure memory operation. Zero Azure calls.

**xWrite:** memcpy into aData. Mark page dirty in aDirty bitmap.

**xSync:** For each dirty page, PUT Page to Azure. Clear dirty bitmap. This batches writes — a transaction that touches 10 pages makes 10 Azure PUT Page calls at sync time, not 10 scattered calls during the transaction.

**xTruncate:** Resize aData buffer. Call azure_page_blob_resize.

**xClose:** If any dirty pages remain (shouldn't happen — SQLite syncs before close), flush them. Free aData. Release lease if held.

**Size limits:** For MVP 1, we impose a soft limit of 100MB on blob download at open. Databases larger than this will still work but performance degrades. MVP 2 introduces range-read caching with LRU eviction for large databases.

**Why dirty page tracking instead of full-blob upload:**
A full-blob upload on every sync would mean uploading the entire database for every transaction. Dirty page tracking means we only upload the pages that changed. For a 10MB database where a transaction modifies 3 pages, we upload 12KB instead of 10MB.

---

### Decision 5: Testing Stack

**Decision:** Approved as proposed. Four-layer pyramid with `azure_ops_t` vtable.

**Rationale:**

Samwise's research is thorough and the architecture is sound. The vtable pattern is validated by Azure SDK for C's own testing approach, by SQLite's system call override pattern, and by the memdb/kvvfs internal test VFS implementations.

**One clarification:** The `azure_ops_t` vtable is not just a testing concern — it's the production architecture. The VFS layer calls through function pointers, period. In production, those pointers go to Frodo's libcurl-based implementation. In tests, they go to mocks. This isn't a test seam bolted on later; it's how the code works.

**Confirmed layers:**

| Layer | Tool | Tests | Frequency | Time |
|---|---|---|---|---|
| 1: Unit | C mocks via `azure_ops_t` | ~300 | Every commit | <5s |
| 2: Integration | Azurite (Docker, in-memory) | ~75 | Every PR | <60s |
| 3: Fault injection | Toxiproxy + Azurite | ~30 | Nightly | <5min |
| 4: Real Azure | Azure Storage (test account) | ~75 | Weekly CI | <5min |

**Build targets:**
```
make test-unit          # Layer 1
make test-integration   # Layer 2 (requires Azurite)
make test-faults        # Layer 3 (requires Toxiproxy + Azurite)
make test-azure         # Layer 4 (requires real Azure creds)
make test               # Layers 1+2 (default dev workflow)
```

**MVP 1 testing scope:** Layers 1 and 2. Layers 3 and 4 are added when we have the infrastructure but are not required for MVP 1 functionality sign-off.

---

### Decision 6: VFS Registration Strategy

**Decision:** Named VFS "sqlite-objs", registered as non-default, with delegation to the default VFS for non-essential methods and temp file routing.

**Rationale:**

The VFS should be opt-in, not forced. Users select it explicitly via `sqlite3_open_v2()` or URI parameters. This prevents accidentally routing all SQLite operations through Azure.

**Registration API:**
```c
/* Public API — call once at startup */
int sqlite_objs_vfs_register(int makeDefault);

/* Or let the user pass config */
int sqlite_objs_vfs_register_with_config(const sqlite_objs_config_t *config, int makeDefault);
```

**VFS struct:**
```c
static sqlite3_vfs sqlite-objsVfs = {
    .iVersion    = 2,            /* Version 2 for xCurrentTimeInt64 */
    .szOsFile    = sizeof(sqlite-objsFile),
    .mxPathname  = 512,          /* Azure blob paths can be long */
    .zName       = "sqlite-objs",
    /* Methods we implement */
    .xOpen       = sqlite-objsOpen,
    .xDelete     = sqlite-objsDelete,
    .xAccess     = sqlite-objsAccess,
    .xFullPathname = sqlite-objsFullPathname,
    .xGetLastError = sqlite-objsGetLastError,
    /* Methods delegated to default VFS */
    .xDlOpen     = /* delegate */,
    .xDlError    = /* delegate */,
    .xDlSym      = /* delegate */,
    .xDlClose    = /* delegate */,
    .xRandomness = /* delegate */,
    .xSleep      = /* delegate */,
    .xCurrentTime = /* delegate */,
    .xCurrentTimeInt64 = /* delegate */,
};
```

**Temp file routing:**
In `xOpen`, check file type flags:
- `SQLITE_OPEN_MAIN_DB` → Azure-backed `sqlite-objsFile`
- `SQLITE_OPEN_MAIN_JOURNAL` → Azure-backed `sqlite-objsFile` (block blob)
- Everything else (`TEMP_DB`, `TEMP_JOURNAL`, `SUBJOURNAL`, `TRANSIENT_DB`) → delegate to default VFS's `xOpen`

**The szOsFile challenge:** Our `sqlite-objsFile` struct and the default VFS's `unixFile` may differ in size. `szOsFile` must be `max(sizeof(sqlite-objsFile), default->szOsFile)` to accommodate both paths. Aragorn should handle this.

**Usage:**
```c
// Explicit VFS name
sqlite3_open_v2("mydb.db", &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, "sqlite-objs");

// URI parameter
sqlite3_open_v2("file:mydb.db?vfs=sqlite-objs", &db, SQLITE_OPEN_URI | SQLITE_OPEN_READWRITE, NULL);
```

---

### Decision 7: File Naming / Blob Naming Convention

**Decision:** SQLite filenames map directly to Azure blob names within a configured container. Journal blobs use the standard `-journal` suffix.

**Rationale:**

Keep it simple. The Azure container is configured out-of-band (environment variable or config struct). The filename passed to `sqlite3_open_v2()` becomes the blob name.

**Mapping:**
```
sqlite3_open("mydb.db")      → blob: "mydb.db"      (page blob)
journal for mydb.db           → blob: "mydb.db-journal" (block blob)
```

**Container configuration:**
```bash
export AZURE_STORAGE_ACCOUNT=myaccount
export AZURE_STORAGE_CONTAINER=databases
export AZURE_STORAGE_SAS="sv=2024-08-04&ss=b&srt=o&sp=rwdlac&se=..."
# or
export AZURE_STORAGE_KEY="base64key..."
```

**xFullPathname normalization:**
- Strip leading slashes
- Reject paths containing `..` or absolute paths
- Return the normalized blob name
- Maximum path: 512 chars (well within Azure's 1024-char blob name limit)

**Future (MVP 2+):**
- Support for subdirectory-like blob name prefixes (e.g., `tenant1/mydb.db`)
- Possible URI scheme: `sqlite-objs://account/container/blob` (but not for MVP 1 — adds parser complexity)

---

### Decision 8: Error Handling Philosophy

**Decision:** Azure errors go through retry, then map to specific SQLite error codes. Lease conflicts and throttling map to SQLITE_BUSY (retryable). Everything else maps to SQLITE_IOERR variants (fatal to the current operation).

**Rationale:**

The distinction between SQLITE_BUSY (retryable — SQLite has a built-in busy handler mechanism) and SQLITE_IOERR (fatal — transaction is lost) is the most important mapping decision. Getting this wrong either causes data loss (treating fatal as retryable) or unnecessary failures (treating retryable as fatal).

**Error mapping table:**

| Azure Error | HTTP | After Retry? | SQLite Code | Why |
|---|---|---|---|---|
| Transient (5xx, timeout) | 500-504, 408 | Retry exhausted | `SQLITE_IOERR_*` | Network is down, can't recover in VFS |
| Throttled | 429 | After retry | `SQLITE_BUSY` | Temporary overload, app can retry |
| Auth failure | 401, 403 | No retry | `SQLITE_IOERR` | Credentials wrong, can't fix in VFS |
| Not found | 404 | No retry | `SQLITE_CANTOPEN` | Blob doesn't exist |
| Lease conflict | 409 | No retry | `SQLITE_BUSY` | Another writer has the lease |
| Lease expired | (detected locally) | N/A | `SQLITE_IOERR_LOCK` | Our lease expired mid-transaction |
| Precondition fail | 412 | No retry | `SQLITE_IOERR` | ETag mismatch (future) |
| Client error | 400 | No retry | `SQLITE_IOERR` | Bug in our code |

**Retry policy (Frodo's, confirmed):**
- Max 5 retries
- Base delay: 500ms, exponential (500, 1000, 2000, 4000, 8000)
- Jitter: 0-500ms added to each delay
- Max delay cap: 30 seconds
- `Retry-After` header: respect it (add to PoC — Frodo's TODO item)

**xGetLastError:**
Store the most recent `azure_error_t` in the VFS global state. Return the `error_message` and `request_id` via `xGetLastError` for debugging.

---

### Decision 9: Authentication Strategy

**Decision:** Support both SAS tokens and Shared Key from day 1. SAS is the recommended default.

**Rationale:**

Frodo already implemented both. The cost of supporting both is near-zero (one `if` branch in the auth code). SAS tokens are preferred for production (scoped permissions, no key exposure) but Shared Key is essential for:
1. Azurite testing (uses well-known dev account key)
2. Server-side tools where generating SAS tokens is impractical
3. Development workflows where a SAS token hasn't been provisioned

**Configuration precedence:**
1. If `AZURE_STORAGE_SAS` is set → use SAS token auth (append `?<token>` to URLs)
2. Else if `AZURE_STORAGE_KEY` is set → use Shared Key HMAC-SHA256 auth
3. Else → `SQLITE_CANTOPEN` with descriptive error message

**Future (MVP 2+):**
- Azure AD / Managed Identity support (important for production Azure deployments)
- Config via `sqlite3_file_control` for programmatic credential injection

---

### Decision 10: Build System

**Decision:** GNU Makefile with standard targets. Amalgamation target for releases.

**Rationale:**

SQLite itself uses a Makefile. Our users are C developers. Keep it familiar.

**Directory structure:**
```
sqlite-objs/
├── Makefile
├── src/
│   ├── sqlite_objs.h              # Public API (VFS registration, config)
│   ├── sqlite_objs_vfs.c          # VFS implementation (xOpen, xRead, xWrite, ...)
│   ├── azure_client.c         # Azure REST client (evolved from PoC)
│   ├── azure_client.h         # Internal Azure client header
│   ├── azure_auth.c           # HMAC-SHA256 auth signing
│   └── azure_error.c          # Error parsing, retry logic
├── test/
│   ├── test_main.c            # Test harness + runner
│   ├── test_vfs.c             # VFS unit tests (Layer 1)
│   ├── test_azure_client.c    # Azure client unit tests (Layer 1)
│   ├── test_integration.c     # Azurite integration tests (Layer 2)
│   └── mock_azure_ops.c       # Mock azure_ops_t implementation
├── sqlite-autoconf-3520000/   # Unmodified SQLite source (existing)
└── research/                  # Research artifacts (existing)
```

**Makefile targets:**
```makefile
all:              # Build libsqlite_objs.a + sqlite-objs-shell (SQLite CLI with our VFS)
test-unit:        # Build and run Layer 1 tests
test-integration: # Build and run Layer 2 tests (requires Azurite)
test:             # test-unit + test-integration
clean:            # Remove build artifacts
amalgamation:     # Produce sqlite_objs.c + sqlite_objs.h (single-file distribution)
```

**Compilation flags:**
```makefile
CFLAGS = -Wall -Wextra -Wpedantic -std=c11 -O2
CFLAGS += -DSQLITE_THREADSAFE=1
LDFLAGS = -lcurl -lssl -lcrypto
```

**The amalgamation target** (per Quetzal's directive): concatenate SQLite amalgamation + our source files into a single `sqlite_objs.c` with a unified `sqlite_objs.h`. Plus a `sqlite-objs-shell.c` that builds a SQLite CLI with our VFS auto-registered. This is a release artifact, not a development workflow.

---

### Decision 11: MVP 1 Scope Finalization

**IN scope for MVP 1:**

| Feature | Owner | Notes |
|---|---|---|
| Page blob read/write for MAIN_DB | Aragorn + Frodo | Core data path |
| Block blob upload/download for MAIN_JOURNAL | Aragorn + Frodo | Crash safety |
| Full-blob in-memory cache + write buffer | Aragorn | Download on open, dirty page flush on sync |
| Two-level lease-based locking | Aragorn + Frodo | SHARED=no lease, RESERVED+=lease |
| Journal mode DELETE | Aragorn | iVersion=1, no WAL |
| `azure_ops_t` vtable | Aragorn + Frodo | Swappable interface for testing |
| SAS token + Shared Key auth | Frodo | Both paths from PoC |
| Error handling + retry | Frodo | 5 retries, exponential backoff |
| Temp file delegation to default VFS | Aragorn | Route non-main files locally |
| VFS registration (`sqlite_objs_vfs_register`) | Aragorn | Named "sqlite-objs", non-default |
| Layer 1 tests (C mocks) | Samwise | ~300 tests target |
| Layer 2 tests (Azurite) | Samwise | ~75 tests target |
| Makefile build system | Aragorn | all, test-unit, test-integration |
| `sqlite-objs-shell` binary | Aragorn | SQLite CLI with our VFS |
| 30-second lease with inline renewal | Aragorn | Renew at sync and during long writes |

**OUT of scope (deferred):**

| Feature | Deferred To | Reason |
|---|---|---|
| WAL mode | MVP 3+ | Requires shared memory — architecturally incompatible |
| LRU page cache for large DBs | MVP 2 | Full-blob download is sufficient for MVP 1 databases |
| Background lease renewal thread | MVP 2 | Inline renewal is simpler and sufficient |
| Multi-machine reads | MVP 3 | Requires cache invalidation protocol |
| Multi-machine writes | MVP 4 | Requires distributed coordination |
| Azure AD / Managed Identity auth | MVP 2 | SAS + Shared Key covers MVP 1 |
| Connection pooling (HTTP keep-alive) | MVP 2 | Performance optimization |
| HTTP/2 support | MVP 2 | Performance optimization |
| Content-MD5 integrity verification | MVP 2 | Important but not blocking |
| Retry-After header support | MVP 2 | Enhancement to retry policy |
| Amalgamation build | Post-MVP 1 | Release packaging, not development |
| Layer 3 tests (Toxiproxy) | MVP 2 | Infrastructure can be set up in parallel |
| Layer 4 tests (real Azure) | MVP 2 | Nice to have earlier but not blocking |
| URI scheme (`sqlite-objs://`) | MVP 2 | Env vars are sufficient for MVP 1 |

---

## Architecture Diagram

```
┌─────────────────────────────────────────────────────────────┐
│                    Application Layer                         │
│              sqlite3_open_v2("mydb.db", ..., "sqlite-objs")     │
└────────────────────────┬────────────────────────────────────┘
                         │
┌────────────────────────▼────────────────────────────────────┐
│                  SQLite Core (Unmodified)                     │
│            Pager → B-tree → Page Cache → VFS calls           │
└────────────────────────┬────────────────────────────────────┘
                         │ sqlite3_vfs / sqlite3_io_methods
┌────────────────────────▼────────────────────────────────────┐
│                    sqlite-objs VFS Layer                          │
│                                                              │
│  ┌──────────────┐  ┌──────────────┐  ┌───────────────────┐  │
│  │  sqlite-objsOpen  │  │ sqlite-objsFile  │  │  File Type Router │  │
│  │  sqlite-objsDelete│  │  ┌─────────┐ │  │                   │  │
│  │  sqlite-objsAccess│  │  │ aData[] │ │  │ MAIN_DB → Azure   │  │
│  │  ...          │  │  │ (cache) │ │  │ MAIN_JOURNAL →    │  │
│  │               │  │  ├─────────┤ │  │   Azure           │  │
│  │ Delegates to  │  │  │aDirty[] │ │  │ TEMP_* → local    │  │
│  │ default VFS:  │  │  │(bitmap) │ │  │   default VFS     │  │
│  │ xRandomness   │  │  ├─────────┤ │  │                   │  │
│  │ xSleep        │  │  │ eLock   │ │  └───────────────────┘  │
│  │ xCurrentTime  │  │  │ leaseId │ │                         │
│  │ xDlOpen/...   │  │  └─────────┘ │                         │
│  └──────────────┘  └──────┬───────┘                         │
│                           │ azure_ops_t* (vtable)            │
└───────────────────────────┼─────────────────────────────────┘
                            │
┌───────────────────────────▼─────────────────────────────────┐
│              azure_ops_t Interface (Swappable)                │
│                                                              │
│  Production:                    Testing:                     │
│  ┌────────────────────┐        ┌─────────────────────┐      │
│  │ azure_client.c     │        │ mock_azure_ops.c    │      │
│  │ (libcurl + OpenSSL)│        │ (in-memory buffers) │      │
│  └────────┬───────────┘        └─────────────────────┘      │
│           │                                                  │
└───────────┼──────────────────────────────────────────────────┘
            │ HTTPS
┌───────────▼──────────────────────────────────────────────────┐
│                Azure Blob Storage                             │
│                                                              │
│  ┌──────────────────┐  ┌──────────────────┐                  │
│  │  mydb.db          │  │  mydb.db-journal  │                 │
│  │  (Page Blob)      │  │  (Block Blob)     │                 │
│  │  Random R/W       │  │  Sequential R/W   │                 │
│  │  512-byte aligned │  │  Whole-object     │                 │
│  └──────────────────┘  └──────────────────┘                  │
│                                                              │
│  Lease: 30s renewable, writer-exclusive, reader-permissive   │
│  Auth: SAS token (preferred) or Shared Key (HMAC-SHA256)     │
└──────────────────────────────────────────────────────────────┘
```

**Data flow for a write transaction:**

```
1. App: INSERT INTO t VALUES (...)
2. SQLite: xLock(RESERVED)        → sqlite-objs: acquire 30s lease on mydb.db blob
3. SQLite: xOpen(MAIN_JOURNAL)    → sqlite-objs: prepare journal (block blob)
4. SQLite: xWrite(journal, ...)   → sqlite-objs: buffer journal data in memory
5. SQLite: xSync(journal)         → sqlite-objs: upload journal as block blob
6. SQLite: xWrite(db, page N)     → sqlite-objs: memcpy into aData, mark dirty
7. SQLite: xWrite(db, page M)     → sqlite-objs: memcpy into aData, mark dirty
8. SQLite: xSync(db)              → sqlite-objs: PUT Page for each dirty page,
                                              renew lease, clear dirty bitmap
9. SQLite: xDelete(journal)       → sqlite-objs: delete journal block blob
10.SQLite: xUnlock(NONE)          → sqlite-objs: release lease
```

---

## Risk Register

### Risk 1: Lease Expiry During Long Transactions (HIGH)

**Description:** A transaction that takes longer than 30 seconds (many writes, slow network) could see its lease expire before xSync completes. Another client could acquire the lease and start writing, leading to data corruption.

**Probability:** Medium (long transactions + Azure latency)

**Impact:** Critical (data corruption)

**Mitigation:**
1. Renew lease proactively at each `xSync` call and every N writes (e.g., every 10 seconds worth of writes)
2. Before every PUT Page in the sync flush, verify the lease is still valid by checking `leaseAcquiredAt + 30s > now`
3. If lease renewal fails, abort the sync with `SQLITE_IOERR_LOCK` — SQLite will rollback using the journal
4. Document: for very long transactions, recommend `PRAGMA busy_timeout=60000` and consider transaction chunking

### Risk 2: Partial Sync Failure (HIGH)

**Description:** During `xSync`, we flush N dirty pages. If page 5 of 10 fails (network error), the database on Azure is now in an inconsistent state — some pages from the new transaction, some from the old.

**Probability:** Low-Medium (network errors during multi-page writes)

**Impact:** Critical (database corruption without journal)

**Mitigation:**
1. The journal is uploaded BEFORE any database page writes (SQLite ensures this via sync ordering)
2. If sync fails partway, the journal blob is still intact on Azure
3. On next open, SQLite detects the journal and rolls back automatically
4. This is the same crash recovery model as local SQLite — the journal IS the mitigation
5. xSync must return `SQLITE_IOERR_FSYNC` on partial failure, NOT `SQLITE_OK`

### Risk 3: Full-Blob Download Scaling (MEDIUM)

**Description:** Downloading the entire database on `xOpen` works for small databases but becomes impractical for large ones (a 1GB database = 1GB download before any query runs).

**Probability:** Low for MVP 1 (most early databases will be small)

**Impact:** High (unusable for large databases)

**Mitigation:**
1. MVP 1: Document the limitation. Target databases <100MB.
2. MVP 2: Implement range-read caching — download pages on demand, LRU eviction.
3. The `aData` buffer architecture supports future lazy loading (read from Azure on cache miss instead of failing).

### Risk 4: Azurite Fidelity Gaps (MEDIUM)

**Description:** Samwise identified several areas where Azurite may differ from real Azure (range header behavior, lease timing, error code formats). Tests that pass against Azurite might fail against real Azure.

**Probability:** Medium (known gaps exist)

**Impact:** Medium (bugs discovered late)

**Mitigation:**
1. Layer 4 testing (real Azure) catches these gaps
2. Strict mode for Azurite (no `--loose` flag)
3. Document known Azurite vs Azure differences in test comments
4. Add real Azure CI before first release

### Risk 5: libcurl/OpenSSL Dependency Management (LOW-MEDIUM)

**Description:** libcurl and OpenSSL are system dependencies with version-specific behavior. Different platforms ship different versions. Build failures or runtime differences across macOS, Linux, CI environments.

**Probability:** Medium (platform differences are real)

**Impact:** Low-Medium (build/compatibility issues, not data issues)

**Mitigation:**
1. Pin minimum versions in Makefile/documentation (libcurl ≥ 7.68, OpenSSL ≥ 1.1.1)
2. CI tests on both macOS and Ubuntu
3. The amalgamation distributes our code only — users provide curl/ssl (standard practice)
4. Future: consider mbedTLS as an OpenSSL alternative for embedded targets

---

## MVP 1 Scope Definition

### Definition of Done for MVP 1

A user can:

1. **Build** `sqlite-objs-shell` from source with `make` on macOS or Linux
2. **Create** a new SQLite database backed by Azure Page Blobs:
   ```bash
   export AZURE_STORAGE_ACCOUNT=myaccount
   export AZURE_STORAGE_CONTAINER=mycontainer
   export AZURE_STORAGE_SAS="..."
   ./sqlite-objs-shell mydb.db
   sqlite> CREATE TABLE t(id INTEGER PRIMARY KEY, name TEXT);
   sqlite> INSERT INTO t VALUES (1, 'hello');
   sqlite> SELECT * FROM t;
   1|hello
   ```
3. **Close and reopen** the database — data persists on Azure:
   ```bash
   ./sqlite-objs-shell mydb.db
   sqlite> SELECT * FROM t;
   1|hello
   ```
4. **Survive machine loss** — data is recoverable from Azure on any other machine with the same credentials
5. **See write contention handled** — if two processes try to write simultaneously, one gets SQLITE_BUSY (not corruption)
6. **Run the test suite** — `make test` passes (Layers 1+2)

### What MVP 1 Is NOT

- Not fast (full blob download, no connection pooling, synchronous Azure calls)
- Not scalable (single writer, full blob in memory, <100MB practical limit)
- Not multi-machine (single machine only — locking works via leases, but no cache invalidation for remote readers)
- Not WAL-compatible (journal mode only)
- Not a production-grade system (but it IS correct and crash-safe)

---

## Action Items

### Aragorn (SQLite/C Dev) — VFS Implementation

1. **Create `src/sqlite_objs_vfs.c`** — the complete VFS implementation
   - sqlite-objsFile struct (as specified in Decision 4)
   - All sqlite3_io_methods v1: xClose, xRead, xWrite, xTruncate, xSync, xFileSize, xLock, xUnlock, xCheckReservedLock, xFileControl, xSectorSize, xDeviceCharacteristics
   - File type router in xOpen (Azure for MAIN_DB/MAIN_JOURNAL, delegate for temp files)
   - Dirty page tracking with bitmap
   - Lease-based locking (acquire at RESERVED, renew at sync, release at unlock)

2. **Create `src/sqlite_objs.h`** — public API header
   - `sqlite_objs_vfs_register()` / `sqlite_objs_vfs_register_with_config()`
   - Config struct (or document env var approach)

3. **Create `Makefile`** — build system (targets: all, test-unit, test-integration, test, clean)

4. **Create `sqlite-objs-shell.c`** — SQLite CLI wrapper that registers our VFS

### Frodo (Azure Expert) — Azure Client Layer

1. **Refactor PoC into `src/azure_client.c` + `src/azure_client.h`**
   - Evolve `azure_blob.h` types and functions into production code
   - Implement the `azure_ops_t` vtable (function pointer table)
   - Both SAS and Shared Key auth paths

2. **Add `Retry-After` header support** to retry logic

3. **Add lease renewal function** — simple wrapper that checks elapsed time and calls `azure_lease_renew` if needed

4. **Connection string parsing** — read AZURE_STORAGE_ACCOUNT, AZURE_STORAGE_CONTAINER, AZURE_STORAGE_SAS/KEY from environment

### Samwise (QA) — Testing Infrastructure

1. **Create `test/mock_azure_ops.c`** — the in-memory mock implementing `azure_ops_t`
   - malloc'd buffer simulating a page blob
   - Lease state machine (available/leased/expired)
   - Configurable failure injection (fail at call N, specific error code)
   - Call counting for verification

2. **Create `test/test_main.c`** — test harness with assert macros and runner

3. **Create `test/test_vfs.c`** — VFS unit tests (~300 tests target)
   - Read/write roundtrip, lock escalation, error handling, crash recovery

4. **Create `test/test_integration.c`** — Azurite integration tests (~75 tests target)
   - End-to-end SQLite operations through our VFS against Azurite

5. **Document CI pipeline** — GitHub Actions workflow for test-unit + test-integration

### Gandalf (Lead/Architect) — Oversight

1. Review all PRs before merge — verify design decisions are respected
2. Write `decisions.md` entries for each approved decision
3. Monitor Risk 1 (lease expiry) and Risk 2 (partial sync) during implementation
4. Define the `azure_ops_t` vtable contract (interface specification) — this is the critical boundary between Aragorn and Frodo's code

---

## Appendix A: The `azure_ops_t` Interface Contract

This is the single most important interface in the system. It sits between the VFS layer (Aragorn) and the Azure client layer (Frodo).

```c
typedef struct azure_ops azure_ops_t;
struct azure_ops {
    /* Page Blob Operations (for MAIN_DB) */
    azure_err_t (*page_blob_create)(void *ctx, const char *name,
                                     int64_t size, azure_error_t *err);
    azure_err_t (*page_blob_write)(void *ctx, const char *name,
                                    int64_t offset, const uint8_t *data,
                                    size_t len, azure_error_t *err);
    azure_err_t (*page_blob_read)(void *ctx, const char *name,
                                   int64_t offset, size_t len,
                                   azure_buffer_t *out, azure_error_t *err);
    azure_err_t (*page_blob_resize)(void *ctx, const char *name,
                                     int64_t new_size, azure_error_t *err);

    /* Block Blob Operations (for MAIN_JOURNAL) */
    azure_err_t (*block_blob_upload)(void *ctx, const char *name,
                                      const uint8_t *data, size_t len,
                                      azure_error_t *err);
    azure_err_t (*block_blob_download)(void *ctx, const char *name,
                                        azure_buffer_t *out,
                                        azure_error_t *err);

    /* Common Blob Operations */
    azure_err_t (*blob_get_properties)(void *ctx, const char *name,
                                        int64_t *size,
                                        char *lease_state,
                                        char *lease_status,
                                        azure_error_t *err);
    azure_err_t (*blob_delete)(void *ctx, const char *name,
                                azure_error_t *err);
    azure_err_t (*blob_exists)(void *ctx, const char *name,
                                int *exists, azure_error_t *err);

    /* Lease Operations (for Locking) */
    azure_err_t (*lease_acquire)(void *ctx, const char *name,
                                  int duration_secs, char *lease_id_out,
                                  size_t lease_id_size,
                                  azure_error_t *err);
    azure_err_t (*lease_renew)(void *ctx, const char *name,
                                const char *lease_id, azure_error_t *err);
    azure_err_t (*lease_release)(void *ctx, const char *name,
                                  const char *lease_id, azure_error_t *err);
    azure_err_t (*lease_break)(void *ctx, const char *name,
                                int break_period_secs, int *remaining_secs,
                                azure_error_t *err);
};
```

**Contract rules:**
1. All functions return `azure_err_t`. On error, `azure_error_t *err` is populated.
2. The `ctx` pointer is opaque — in production it's `azure_client_t*`, in tests it's `azure_mock_t*`.
3. Retry logic lives INSIDE the production implementation (Frodo's code). The VFS layer does NOT retry.
4. The mock DOES NOT implement retry — it returns errors immediately for testing.
5. Buffer management: `azure_buffer_t` is allocated by the caller, filled by the callee. Caller frees.
6. `blob_exists` is separate from `blob_get_properties` — exists is a lightweight HEAD check.

---

## Appendix B: Journal File Handling

Journal files have different semantics than the main database:

| Operation | MAIN_DB (Page Blob) | MAIN_JOURNAL (Block Blob) |
|---|---|---|
| xOpen (create) | `page_blob_create(size=0)` | No-op (journal doesn't exist yet) |
| xOpen (existing) | Download entire blob | Download entire blob |
| xWrite | memcpy to buffer + dirty bit | Append to in-memory buffer |
| xSync | PUT Page per dirty page | Upload entire journal as block blob |
| xRead | memcpy from buffer | memcpy from downloaded buffer |
| xTruncate | `page_blob_resize` + truncate buffer | Delete + recreate (or just truncate buffer) |
| xDelete | `blob_delete` | `blob_delete` |
| xFileSize | Return cached `nData` | Return cached buffer size |
| xLock/xUnlock | Lease-based (see Decision 3) | No-op (journal is tied to DB lock) |

**Critical ordering:** SQLite guarantees journal-sync-before-db-write. Our VFS MUST NOT reorder these — the journal block blob must be fully uploaded before any dirty DB pages are written to Azure. The xSync implementation enforces this naturally (each xSync flushes only its own file).

---

*This document gates all implementation work. No code should be written that contradicts these decisions. If implementation reveals a flaw in any decision, bring it back to design review — do not work around it silently.*

— Gandalf
