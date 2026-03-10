# Project Context

- **Owner:** Quetzal Bradley
- **Project:** Azure Blob-backed SQLite (azqlite) — a drop-in replacement for SQLite where all storage is backed by Azure Blob Storage, implemented as a custom VFS layer. MIT licensed.
- **Stack:** C, SQLite VFS API, Azure Blob Storage REST API, libcurl, OpenSSL
- **SQLite source:** `sqlite-autoconf-3520000/` (do not modify unless absolutely necessary)
- **Created:** 2026-03-10

## Key Context

- MVP 1: Drop-in replacement, single machine, remote storage, committed txns survive machine loss
- MVP 2: In-memory read cache
- MVP 3: Multi-machine reads (one writer, many readers on different machines)
- MVP 4: Multi-machine writes (correct but not necessarily performant)
- Dependencies: SQLite, OpenSSL (system), libcurl. No Azure SDK — direct REST API.
- License: MIT

## Learnings

<!-- Append new learnings below. Each entry is something lasting about the project. -->

### Prior Art Survey (2026-03-10)

- **Cloud Backed SQLite (CBS)** is the most directly relevant prior art — built by the SQLite team, supports Azure Blob Storage, uses block-based chunking (4MB blocks) with a manifest. However, it uses block blobs (not page blobs) and has no built-in locking. See `research/prior-art.md`.
- **Azure Page Blobs are our key differentiator.** Every S3-based project (sqlite-s3vfs, CBS, Litestream) struggles with immutable objects — either one object per page (too many requests) or chunked blocks (write amplification). Page blobs support 512-byte aligned random writes, meaning we can read/write individual SQLite pages directly at their offsets in a single blob. No prior project exploits this.
- **Every project is single-writer per database.** This is universal — CBS, Litestream, LiteFS, mvsqlite, rqlite, dqlite all enforce single writer. The only exception is cr-sqlite (CRDTs) which trades write performance (~2.5x overhead) for convergent multi-writer.
- **WAL mode is impossible across machines** without custom shared-memory implementation. Rollback journal is the safe default for MVP 1. WAL with EXCLUSIVE locking mode is viable for single-machine.
- **Locking must be built into the VFS** — projects that defer locking to "the application" (CBS, sqlite-s3vfs) see corruption in practice. Azure Blob Leases (60s renewable) are the right primitive.
- **Local caching is mandatory for usable performance.** Uncached cloud reads add 10-100ms per page. Every successful project caches aggressively. MVP 2 is correctly sequenced.
- **Don't fork SQLite.** libSQL/Turso shows both the power and maintenance burden. Our VFS-only approach is validated by CBS and mvsqlite.
- **License-compatible projects found:** rqlite (MIT), libSQL (MIT), sqlite-s3vfs (MIT), sqlite3vfshttp (MIT), gRPSQLite (MIT), go-cloud-sqlite-vfs (MIT), ncruces/go-sqlite3 (MIT). CBS is public domain. dqlite is LGPL (incompatible for static linking).

### Design Review — MVP 1 Architecture (2026-03-10)

- **Full design review at `research/design-review.md`.** 11 decisions covering blob types, locking, caching, WAL, testing, build system, error handling, auth, naming, VFS registration, and MVP scope.
- **Corrected my own prior-art recommendation:** Originally deferred caching to MVP 2. Aragorn proved uncached reads make VFS untestable (~5s for 100 pages). Cache (full blob download) is now mandatory for MVP 1.
- **Overrode Aragorn's nolock proposal:** Two-level lease-based locking from day 1 (SHARED=no lease, RESERVED+=acquire lease). Prior art shows deferred locking leads to corruption.
- **Key architecture: azure_ops_t vtable.** The swappable function pointer table between VFS and Azure client is both the production interface and the test seam. Defined in design-review.md Appendix A. This is the critical boundary between Aragorn and Frodo's code.
- **io_methods iVersion=1** — no WAL, no mmap. Eliminates 6 method implementations.
- **File type routing:** MAIN_DB and MAIN_JOURNAL → Azure. Everything else → default local VFS. Detected via flags in xOpen.
- **Journal handled as block blob** (sequential write/read pattern). DB as page blob (random R/W).
- **Write buffer with dirty page bitmap:** xWrite→memcpy+dirty bit, xSync→PUT Page per dirty page. Batches writes to sync time.
- **Error handling:** 429/lease-conflict → SQLITE_BUSY (retryable). All else after retry → SQLITE_IOERR_* (fatal). 5 retries, 500ms exponential backoff + jitter.
- **Device characteristics:** `SQLITE_IOCAP_SEQUENTIAL | SQLITE_IOCAP_POWERSAFE_OVERWRITE | SQLITE_IOCAP_SUBPAGE_READ`. NOT ATOMIC — journal safety is needed.
- **VFS name "azqlite"**, non-default registration. Usage: `sqlite3_open_v2(name, &db, flags, "azqlite")`.
- **Source layout:** `src/` (azqlite_vfs.c, azure_client.c/.h, azure_auth.c, azure_error.c, azqlite.h), `test/` (test_main.c, test_vfs.c, mock_azure_ops.c, test_integration.c).
- **Top risks:** (1) Lease expiry during long transactions, (2) Partial sync failure — both mitigated by journal-first ordering and inline lease renewal.
- **Decisions written to:** `.squad/decisions.md` — all 11 decisions (D1-D11) now approved and merged from inbox.

### MVP 1 Code Review — Reviewer Gate (2026-03-10)

- **Full review at `research/code-review.md`.** Reviewed all 7 source files against D1–D11.
- **Overall verdict: APPROVE WITH CONDITIONS** — 2 critical issues must fix before demo, 5 important issues for follow-up.
- **C1 (CRITICAL): Device characteristics in `azqlite_vfs.c` claim ATOMIC512 + SAFE_APPEND.** Design spec (D4) says NOT ATOMIC. ATOMIC512 tells SQLite it can skip journal entries for 512-byte writes, which is dangerous — our multi-page xSync is not atomic. Must change to `SEQUENTIAL | POWERSAFE_OVERWRITE | SUBPAGE_READ`.
- **C2 (CRITICAL): URL construction in `azure_client.c` uses unbounded `strcat` on a 2048-byte stack buffer.** SAS tokens + long paths can overflow. Must use snprintf with bounds checking.
- **Everything else is architecturally sound.** The azure_ops_t vtable is correctly implemented (all 13 operations, signatures match Appendix A). Lease-based locking has no race conditions. Journal workflow is correct. Error mapping follows D8. Both auth paths work.
- **Key learning:** ATOMIC in SQLite device characteristics is a loaded term — it's about whether the pager can skip journal entries, not about whether individual writes are reliable. Always cross-check io capabilities against pager behavior, not just the underlying storage semantics.

### Agent-11: Code Review Complete (2026-03-10 — 07:43:07Z)

Completed comprehensive design review (D1-D11). Verdict: **APPROVE WITH CONDITIONS**.

**Blockers (must fix before demo):**
- C1: Device flags (Aragorn) — change `ATOMIC512|SAFE_APPEND` to `SEQUENTIAL|POWERSAFE_OVERWRITE|SUBPAGE_READ`
- C2: URL buffer (Frodo) — replace `strcat` with bounds-checked `snprintf`

**Status:** SUCCESS. Awaiting C1/C2 fixes. Code review re-check not required — fixes are mechanical.
