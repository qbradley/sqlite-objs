# sqlite-objs Design

sqlite-objs is a SQLite Virtual File System (VFS) that stores durable database state in Azure Blob Storage while preserving SQLite's transaction and recovery model as much as possible. The system is designed as a drop-in storage layer: callers use normal SQLite APIs, while the VFS translates SQLite file operations into Azure blob operations plus a local cache.

This document captures design knowledge that should remain true even if the implementation is refactored.

## Goals

- **SQLite semantics first**: preserve SQLite's expectations for reads, writes, locking, sync, rollback recovery, WAL recovery, and error reporting.
- **Remote durability**: committed data and recovery artifacts must be durable in Azure Blob Storage, not only in local cache.
- **Explicit failure over false success**: if remote state cannot be checked or cleaned up safely, report an error rather than pretend the operation succeeded.
- **Single-writer correctness**: distributed write exclusion is enforced with Azure leases; readers may run without leases but must not observe mixed snapshots.
- **Testable storage boundary**: Azure operations are abstracted behind a swappable operations table so unit tests can model failure, races, and recovery state without real Azure.
- **Rust safety boundary**: Rust wrappers should make common safe usage easy and prevent accidental global VFS reconfiguration.

## Non-Goals

- sqlite-objs is not a general distributed database.
- It does not provide multi-writer performance comparable to a server database.
- It does not support shared-memory WAL across machines.
- It does not make local cache files authoritative; Azure remains the source of durable truth.

## Major Pieces

### SQLite VFS Layer

The VFS is the semantic core. It implements SQLite file, lock, sync, access, open, truncate, and control operations. It owns the contract between SQLite's pager assumptions and Azure's blob APIs.

The VFS is responsible for:

- mapping SQLite file types to the correct Azure storage object,
- maintaining local cache and dirty/valid page state,
- acquiring and renewing leases for writer locks,
- synchronizing dirty pages and recovery artifacts,
- failing closed on recovery-critical uncertainty,
- translating Azure errors into SQLite-visible errors.

### Azure Operations Interface

The Azure operations table is the seam between SQLite semantics and storage transport. Production uses the real Azure REST client; tests use an in-memory mock with failure injection and state inspection.

This seam is essential: correctness tests must be able to model lost leases, stale ETags, missing blobs, partial remote state, and transient/permanent Azure failures without relying on live cloud behavior.

### Production Azure Client

The production client owns HTTP request construction, Shared Key/SAS authentication, retry policy, response parsing, ETag/snapshot capture, lease calls, batch writes, and parallel reads/uploads.

The client uses a mutex to protect shared libcurl handles. Any code path that runs while holding that mutex must not call helpers that lock it again.

### Rust Bindings

The Rust workspace has two layers:

- `sqlite-objs-sys`: raw FFI and constants.
- `sqlite-objs`: safe registration helpers, URI construction, typed metrics parsing, and optional rusqlite file-control helpers.

The Rust safe layer coordinates process-wide registration so safe APIs do not unexpectedly reset the global C VFS state.

### Validation and Release Gates

Validation is part of the design. The project relies on unit tests, Azurite-backed integration tests, sanitizer runs, Rust workspace tests, optional TCL/extended/live-Azure gates, and a release-gate script that distinguishes fast local validation from full release readiness.

## Storage Model

| SQLite file kind | Durable storage | Reason |
|---|---|---|
| Main database | Azure Page Blob | Supports random 512-byte-aligned page writes |
| Rollback journal | Azure Block Blob | Journal content is whole-object sequential state |
| WAL | Azure Block Blob | WAL content is uploaded/recovered as whole-object state; shared-memory WAL is not supported |
| Temp/sub-journal files | Default local VFS | These are transient SQLite implementation details |
| Local cache and sidecars | Local filesystem | Performance and cache reuse only; never the durable source of truth |

## Lifecycle and Data Flow

### Registration and Configuration

The C VFS has process-global registration state. It can be configured from environment variables, explicit C config, test operations, or URI-only mode. URI parameters take precedence at open time so different databases can supply per-file credentials.

Rust safe registration is intentionally conservative. Repeating the same safe registration mode is idempotent, but incompatible reconfiguration is rejected instead of silently resetting global C state.

### Opening a Database

Opening a main database resolves Azure credentials and operations, checks whether the page blob exists, captures blob metadata such as size and ETag, and creates a local cache file. By default, the database blob is downloaded on open. URI mode can request lazy page loading with `prefetch=none`.

Cache reuse is allowed only when the cached local file can be tied to the current blob identity and ETag. Cache reuse is an optimization; it must never allow stale pages to masquerade as current data.

### Reads

Reads are served from the local cache. In lazy mode, missing valid pages are fetched from Azure, written to cache, and marked valid. Reads beyond EOF follow SQLite's short-read/zero-fill expectations.

### Writes and Sync

Writes update the local cache and mark pages dirty. Sync coalesces dirty pages into Azure page-blob write ranges, aligns writes to Azure's 512-byte page requirement, and writes only changed ranges.

Extending a database may require resizing the page blob before writing pages. A successful sync means remote page state has been updated or an error has been reported.

### Locking

SQLite SHARED locks do not acquire Azure leases. RESERVED, PENDING, and EXCLUSIVE locks acquire a blob lease. The lease serializes writers across processes/machines. Long transactions and large flushes renew leases before they expire.

If a writer discovers that the blob changed after pages were already read in the current SQLite snapshot, it must fail with a retryable/busy outcome rather than refresh pages under SQLite's pager.

### Rollback Journal Recovery

Rollback journals are recovery artifacts. Their discovery must be authoritative. A cached "journal absent" state is not proof that another process did not create a journal and crash afterward.

If remote state says a journal exists, failing to download it is not equivalent to an empty journal. Recovery-critical download failures must be surfaced.

### WAL Mode

WAL is supported only with exclusive locking. Shared-memory WAL cannot work over Azure, so attempts to use WAL without the exclusive-mode assumptions must fail safely.

Existing WAL blobs are recovery artifacts. If WAL data exists remotely, opening/recovery must download it or fail; ignoring it can lose committed transactions. WAL cleanup must also propagate remote cleanup failures when stale WAL state could remain visible.

## Core Invariants

### Recovery Invariants

- Recovery artifacts are part of durable database state.
- Remote existence checks for journals/WAL must fail closed when recovery depends on them.
- Existing recovery artifacts must be downloaded successfully or reported as errors.
- Cleanup of journals/WAL must not report success if stale remote recovery state may remain.

### Page Blob Invariants

- Main database writes to Azure must be 512-byte aligned.
- Page writes must not exceed Azure's maximum page-write range size.
- Dirty page tracking must never drop writes beyond the old EOF.
- A successful sync must clear dirty state only after the relevant remote writes succeed.

### Snapshot and Cache Invariants

- A local cache is valid only for the blob identity and ETag/snapshot it represents.
- Readers must not see a mix of pages from old and new remote snapshots.
- If revalidation detects a stale snapshot after SQLite has already read pages, the operation must fail so SQLite can retry from a fresh snapshot.

### Locking Invariants

- At most one writer may hold RESERVED-or-higher lock state for a blob.
- Lease renewals must not deadlock the Azure client.
- Lease loss, renewal failure, or conflict must be visible to SQLite rather than silently ignored.

### Rust API Invariants

- Safe Rust registration must not accidentally reset global C VFS state.
- Safe Rust configuration and URI-building should reject missing required account/container/auth values before FFI/open.
- Rust MSRV must match actual dependency and feature requirements.

### Gate Invariants

- Fast local validation may pass with skipped optional gates, but must say so.
- Full release validation must not succeed with skipped required gates.
- Sanitizer validation must be bounded and must preserve useful ASan/UBSan behavior.
- Integration tests should run strict Azurite mode by default; loose compatibility mode is opt-in.

## Known Trade-offs

- Authoritative journal checks can add remote HEAD calls. Correct recovery takes priority over the old cached-absence optimization.
- Batch lease renewal uses a mutex-held renewal path to avoid recursive mutex acquisition. This intentionally prioritizes correctness and deadlock avoidance over request-execution deduplication.
- The Rust safe layer protects Rust callers; direct C registration APIs can still reinitialize global VFS state if used carelessly.
- Live Azure tests are opt-in so local development does not require cloud credentials.

## Design Principles for Future Changes

1. If SQLite thinks a file operation succeeded, Azure durable state must agree or the operation must be explicitly best-effort and safe.
2. Never optimize away recovery checks unless the proof remains valid across processes and crashes.
3. Treat local cache as derived state, not durable truth.
4. Keep Azure transport details behind `azure_ops_t` so correctness tests can model failures.
5. Preserve error causes whenever they are actionable.
6. Prefer adding explicit unsupported-mode failures over silently accepting modes with unclear recovery semantics.
7. When changing a workflow gate, define both human-readable output and machine-readable exit semantics.
