# Squad Decisions Archive

Decisions archived from decisions.md on 2026-06-27. These entries are older than 30 days (from 2026-03-10).

---

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
### D6: VFS Name "sqlite-objs", Non-Default, Delegating
**Date:** 2026-03-10 | **From:** Gandalf (Design Review)

Register as "sqlite-objs" via `sqlite3_vfs_register(pVfs, 0)`. Delegate xDlOpen/xRandomness/xSleep/xCurrentTime to default VFS. Route temp files to default VFS xOpen.
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

**IN:** Page blob DB, block blob journal, in-memory cache+write buffer, lease locking, journal mode, azure_ops_t vtable, SAS+SharedKey auth, retry logic, temp file delegation, VFS registration, Layer 1+2 tests, Makefile, sqlite-objs-shell.

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
