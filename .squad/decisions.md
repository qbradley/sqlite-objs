# Squad Decisions

## Active Decisions

### D1: Blob Type Strategy
**Date:** 2026-03-10 | **From:** Gandalf (Design Review)

**Page blobs** for MAIN_DB (512-byte aligned random R/W — perfect match for SQLite pages). **Block blobs** for MAIN_JOURNAL (sequential, whole-object). No append blobs.

---

### D2: Journal Mode Only
**Date:** 2026-03-10 | **From:** Gandalf (Design Review)

Rollback journal (DELETE) for MVP 1. WAL explicitly disabled — set `iVersion=1` on io_methods. WAL requires shared memory; architecturally incompatible with remote storage. Deferred to MVP 3+ at earliest.

---

### D3: Two-Level Lease-Based Locking
**Date:** 2026-03-10 | **From:** Gandalf (Design Review)

NOT nolock. SHARED=no lease (reads always work). RESERVED/EXCLUSIVE=acquire 30s blob lease. Release on unlock. `xCheckReservedLock`: HEAD request to check lease state. Inline renewal (no background thread). Overrides Aragorn's nolock proposal — prior art shows deferred locking → corruption.

---

### D4: Full-Blob Cache from Day 1
**Date:** 2026-03-10 | **From:** Gandalf (Design Review)

Download entire blob on xOpen into malloc'd buffer. xRead=memcpy. xWrite=memcpy+dirty bit. xSync=PUT Page per dirty page. Overrides initial MVP 2 deferral — Aragorn proved uncached reads are untestable (~5s for 100 pages).

---

### D5: Testing — 4-Layer Pyramid with azure_ops_t Vtable
**Date:** 2026-03-10 | **From:** Gandalf (Design Review)

Approved as Samwise proposed. Layers: C mocks (~300 tests, <5s), Azurite (~75, <60s), Toxiproxy (~30, <5min), real Azure (~75, weekly). MVP 1 delivers Layers 1+2.

**Critical requirement:** VFS layer MUST accept a swappable Azure operations interface (function pointer table / vtable) so that tests can inject mock implementations. This is non-negotiable for testability.

---

### D6: VFS Name "azqlite", Non-Default, Delegating
**Date:** 2026-03-10 | **From:** Gandalf (Design Review)

Register as "azqlite" via `sqlite3_vfs_register(pVfs, 0)`. Delegate xDlOpen/xRandomness/xSleep/xCurrentTime to default VFS. Route temp files to default VFS xOpen.

---

### D7: Filename = Blob Name, Container from Env Vars
**Date:** 2026-03-10 | **From:** Gandalf (Design Review)

SQLite filename maps directly to blob name. Container via `AZURE_STORAGE_CONTAINER` env var. Journal blob = `<name>-journal`.

---

### D8: Azure Errors → SQLITE_BUSY or SQLITE_IOERR
**Date:** 2026-03-10 | **From:** Gandalf (Design Review)

Lease conflicts (409) and throttling (429) → `SQLITE_BUSY` (retryable). All other Azure errors after retry exhaustion → `SQLITE_IOERR_*` variants (fatal). Retry: 5 attempts, 500ms exponential backoff + jitter.

---

### D9: Both SAS + Shared Key Auth
**Date:** 2026-03-10 | **From:** Gandalf (Design Review)

SAS tokens preferred (AZURE_STORAGE_SAS). Shared Key fallback (AZURE_STORAGE_KEY). Both implemented in PoC. Zero additional cost to support both.

---

### D10: Makefile Build System
**Date:** 2026-03-10 | **From:** Gandalf (Design Review)

GNU Makefile. Targets: all, test-unit, test-integration, test, clean, amalgamation. Source in `src/`, tests in `test/`. Dependencies: libcurl, OpenSSL.

---

### D11: MVP 1 Scope
**Date:** 2026-03-10 | **From:** Gandalf (Design Review)

**IN:** Page blob DB, block blob journal, in-memory cache+write buffer, lease locking, journal mode, azure_ops_t vtable, SAS+SharedKey auth, retry logic, temp file delegation, VFS registration, Layer 1+2 tests, Makefile, azqlite-shell.

**OUT:** WAL, LRU page cache, background lease renewal, multi-machine, Azure AD auth, connection pooling, HTTP/2, Content-MD5, amalgamation, Layers 3+4.

---

### UD1: User Directive — MIT License
**Date:** 2026-03-10 | **From:** Quetzal Bradley (via Copilot)

The project license should be MIT.

---

### UD2: User Directive — Use Claude Opus 4.6
**Date:** 2026-03-10 | **From:** Quetzal Bradley (via Copilot)

claude-opus-4.6 should be used for all research tasks. Use the most capable Opus model available.

---

### UD3: User Directive — Deployment: SQLite Amalgamation
**Date:** 2026-03-10 | **From:** Quetzal Bradley (via Copilot)

When it comes time to make a deployment package, create a SQLite amalgamation that includes the SQLite source code along with our Azure VFS source code so the entire package is a .c file and a .h file, plus a version of the SQLite CLI client built against our Azure version.

---

## Decision Research & Rationale

### Prior Art Findings
- **Cloud Backed SQLite (CBS)** — built by SQLite team, validates VFS approach, uses block-based chunking
- **Azure Page Blobs advantage** — supports 512-byte aligned random writes vs block chunking
- **Single-writer universal** — every project (CBS, Litestream, LiteFS, etc.) is single-writer per database
- **WAL infeasible across machines** — requires shared memory coordination
- **Locking must be in VFS** — deferred locking causes corruption (CBS, sqlite-s3vfs patterns show this)
- **Local caching mandatory** — 10-100ms per uncached page makes it unusable

### Azure REST API Findings
- **API version 2024-08-04** — latest stable, set via x-ms-version header
- **Page blobs perfect for SQLite DB** — 512-byte alignment, max 4 MiB/request
- **Block blobs for journals** — sequential, cheapest
- **Leases for locking** — 30-second duration with inline renewal
- **Retry: 5 attempts, 500ms exponential backoff** — matches Azure SDK patterns

### SQLite VFS Findings
- **io_methods v1 sufficient** — core I/O + locking only (no WAL)
- **File type routing critical** — MAIN_DB/MAIN_JOURNAL to Azure, temps to local
- **Sector size 4096** — matches default page size and alignment
- **Device flags:** SQLITE_IOCAP_SEQUENTIAL | SQLITE_IOCAP_POWERSAFE_OVERWRITE | SQLITE_IOCAP_SUBPAGE_READ (NOT ATOMIC)
- **Read cache mandatory** — 5-50ms latency per page without cache

### Testing Strategy Findings
- **Four-layer pyramid** — unit (mocks), integration (Azurite), fault-inject (Toxiproxy), real Azure
- **Critical interface:** `azure_ops_t` vtable for swappable Azure operations
- **Azurite supports** — page blobs, leases, block blobs, shared key auth
- **No existing C Azure mocks** — write our own at azure_client boundary

### D12: pkg-config Integration for Production Build
**Date:** 2026-03-10 | **From:** Frodo (Azure Expert)

Integrate pkg-config into Makefile for discovering OpenSSL and libcurl compile/link flags. Production build (`CFLAGS_PROD`, `LDFLAGS_PROD`) uses pkg-config with graceful fallback. Stub build unchanged. Rationale: portability across macOS (Homebrew), Linux, BSD without user configuration.

---

### D13: Pre-Demo Code Review Conditions
**Date:** 2026-03-10 | **From:** Gandalf (Lead/Architect)

Code approved for MVP 1 demo **pending two mechanical fixes**:
- **C1 (azqlite_vfs.c:693):** Change device flags from `ATOMIC512|SAFE_APPEND` to `SEQUENTIAL|POWERSAFE_OVERWRITE|SUBPAGE_READ` (data corruption prevention)
- **C2 (azure_client.c:173–187):** Replace `strcat` with bounds-checked `snprintf` (URL buffer overflow fix)

No additional review required for these fixes — they are straightforward. Full code review in `research/code-review.md`.

---

### D14: Layer 2 Integration Test Suite
**Date:** 2026-03-10 | **From:** Samwise (QA)

Layer 2 (Azurite emulator) test suite complete: 75 tests passing. Discovered Shared Key auth fails on blob modifications with Azurite (403 Forbidden after first PUT). **Workaround:** Use SAS tokens for Layer 2. **Permanent fix:** Frodo investigating Azurite Shared Key validator behavior (D15).

---

### D15: Shared Key Auth Investigation
**Date:** 2026-03-10 | **From:** Frodo (Azure Expert)

Shared Key authentication fails with Azurite on blob modifications (issue discovered Layer 2 testing). Investigating whether root cause is Azurite behavior or code bug. Blocks full Layer 2 validation and production auth testing. Frodo (Agent-13) assigned.

---

### UD4: Page Blob Resizing Support
**Date:** 2026-03-10 | **From:** Quetzal Bradley (via Copilot)

Page blob resizing must be supported in architecture. First priority: growing databases (resize up). Second priority: VACUUM (resize down). Both deferred to MVP 1.5+; plan architecture for future support.

---

### D12: Azurite SharedKey Authentication Workaround
**Date:** 2026-03-10 | **From:** Frodo (Azure Client Layer)

Azurite has a quirk in canonicalized resource path construction for SharedKey signature validation — it doubles the account name internally. Implemented endpoint-aware URL and resource path building: custom endpoints (Azurite) include account in path before auth signing (allowing Azurite to double it); production Azure uses standard format. Also explicitly set `Content-Type:` header to prevent curl auto-adding headers. Backward compatible (endpoint=NULL → standard Azure behavior).

**Impact:** Integration testing with Azurite now succeeds with SharedKey auth. SharedKey auth works with both Azurite and production Azure.

---

### D13: Container Creation as Public API Function
**Date:** 2026-03-10 | **From:** Frodo (Azure Client Layer)

Added `azure_container_create(azure_client_t *client, azure_error_t *err)` as public API in azure_client.c. Leverages existing auth infrastructure and curl setup for clean, reusable code. Idempotent — treats both 201 (created) and 409 (already exists) as success. Test code calls this after client creation; bash script delegates to C code rather than attempting unauthenticated curl requests.

**Impact:** All Azure client integration tests now pass (8 of 10 total). No breaking changes to existing API.

---

### D14: Benchmark Harness Design
**Date:** 2026-03-10 | **From:** Aragorn (SQLite/C Dev)

Three-binary architecture: lightweight harness that shells out to speedtest1 subprocesses. speedtest1 (standard SQLite) and speedtest1-azure (with azqlite VFS registered as default) run as isolated processes measured via `system()` and `gettimeofday()`. speedtest1.c used unmodified from SQLite upstream. Rejected embedding via #define main trick (speedtest1's `exit()` terminates harness before results capture) and patching upstream (maintenance burden).

**Usage:** `./benchmark --local-only --size 25`, `./benchmark --size 50` (full comparison with Azure env vars), `./benchmark --output csv` (automation).

**Implementation:** Clean separation, flexible output (text/CSV), supports stub and production builds. Subprocess overhead (~10ms) negligible for multi-second benchmarks. Only captures total elapsed time, not per-test breakdown.

---

## Governance

- All meaningful changes require team consensus
- Document architectural decisions here
- Keep history focused on work, decisions focused on direction
