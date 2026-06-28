# sqlite-objs Implementation Notes

This document captures implementation techniques and operational details that are easy to forget during audits, tests, and feature work. It intentionally avoids duplicating code. Treat it as a checklist of concepts that must remain true even if files are refactored.

## VFS Implementation Techniques

### SQLite Subclassing Contract

The custom file object must satisfy SQLite's `sqlite3_file` subclass contract: SQLite receives an opaque file allocation and expects the VFS to install an `sqlite3_io_methods` table only when open has succeeded. On failed opens, avoid leaving a live method table that would cause SQLite to call close on partially initialized state.

### File-Type Routing

Always reason from SQLite file flags:

- Main DB files use page-blob behavior and local cache files.
- Main journals use in-memory journal buffers plus block-blob upload/download.
- WAL files use in-memory WAL buffers plus block-blob upload/download.
- Temp files and unrelated transient files should delegate to the platform VFS.

Do not infer durable-storage behavior from filename alone unless SQLite gives only a VFS-level method such as access/delete; in those cases suffix handling is a recovery-artifact heuristic and must fail closed when safety depends on it.

### Local Cache State

The main database cache is disk-backed. The implementation tracks:

- logical database size,
- dirty pages that need upload,
- valid pages that are present in the local cache for lazy mode,
- ETag/snapshot metadata for cache reuse,
- last synced size to avoid redundant remote resize.

When changing cache behavior, audit both bitmaps together. Growing or truncating the cache without keeping dirty/valid state coherent can drop writes or allow stale reads.

### Dirty Page Sync

Dirty pages are coalesced into contiguous ranges before upload. Those ranges must respect:

- SQLite page boundaries,
- Azure's 512-byte page alignment,
- Azure's maximum page-write size,
- the current logical size versus padded upload length.

Dirty state is cleared only after remote write success. If allocation, cache read, resize, lease renewal, or remote write fails, preserve dirty state where SQLite may retry.

### Recovery Artifact Handling

Rollback journals and WAL files are not ordinary optional side files. They are recovery inputs. The implementation technique is:

1. Use `blob_exists` to discover remote recovery artifacts.
2. Treat failure to check existence as a failure for recovery-artifact paths.
3. If an artifact exists, download it or fail the open/recovery path.
4. Populate the in-memory journal/WAL buffer only after a successful download.
5. On cleanup/truncate/delete, update remote state and cache state together.

Do not treat a failed download as an empty artifact. Do not treat a failed delete as a successful local truncate when stale remote state could be observed later.

### Journal Existence Cache

The journal cache is a performance aid and state record, not an authority. It can record that this process uploaded or deleted a journal, but it cannot prove what another process did after that. Recovery-sensitive access/open paths must perform an authoritative remote check.

### WAL Implementation

WAL uses block blobs, not append blobs. The VFS supports WAL only under exclusive locking assumptions, because shared-memory WAL cannot be shared through Azure Blob Storage. If SQLite asks for shared-memory WAL behavior, fail safely.

WAL recovery tests must prove data is recovered from WAL, not merely that WAL download was attempted. A robust test restores the main DB to a pre-WAL-only snapshot before reopening, so the row can appear only via WAL replay.

## Azure Client Implementation Techniques

### Request Execution and Mutex Ownership

The Azure client has shared libcurl state protected by a mutex. Normal request helpers lock that mutex. Batch page writes also hold the mutex while using a persistent multi handle.

Never call a normal lock-taking request helper from a path that already owns the client mutex. If a mutex-held path needs an Azure request, either:

- use a helper that is explicitly safe under existing mutex ownership, or
- release the mutex only after proving the multi/easy handle state is quiescent.

### Batch Writes

Batch writes run many page writes through libcurl multi. Important implementation constraints:

- request data pointers must stay valid for the entire multi operation,
- each easy handle owns its header list and response buffer,
- every early return after acquiring the client mutex must free request arrays and unlock,
- lease renewal during long batches must not stall indefinitely without a bounded policy,
- renewal failures should preserve the actionable cause when possible.

### Retry and Error Classification

Retry policy belongs near the Azure client. VFS code should receive classified Azure errors and translate them into SQLite codes. Avoid spreading HTTP status interpretation into VFS code.

Preserve root causes in error messages. Operators need to distinguish auth, conflict, throttling, network, lease, timeout, and allocation failures.

### Authentication

SAS tokens are appended to URLs. Shared Key requests require canonicalized headers/resources and correct inclusion of relevant conditional/range/lease headers. If a new Azure operation is added, compare its auth/header shape with existing operations before writing a new ad hoc path.

## Rust Implementation Techniques

### Safe Registration Gate

The C VFS registration state is global. Rust safe APIs therefore use process-wide coordination:

- identical compatible registration may be idempotent,
- incompatible reconfiguration returns an error,
- URI-only mode is distinct from "URI parameters happen to work under global config."

Tests that mutate registration state can affect other tests in the same process. Prefer pure helper tests for compatibility logic and avoid public registration calls that change global state unless the expected order is deterministic.

### Config Validation

Safe Rust config validation should reject:

- empty account,
- empty container,
- missing auth,
- embedded NUL bytes.

`UriBuilder::try_build()` should validate the exact URI it emits. Empty preferred credentials must not suppress a usable fallback credential.

### File-Control Helpers

Rust pragmas/file-control helpers must check SQLite return codes. A default local output value is not evidence that the VFS wrote it. Always assert `SQLITE_OK` before trusting an out parameter.

### MSRV

The documented Rust minimum is 1.82. This is driven by current dependency and generated-binding requirements, not just code syntax. If dependencies change, re-run the MSRV check before changing the documented version.

## Testing Techniques

### Mock Azure Tests

Mock tests are the primary way to model precise remote states:

- create or delete page/block blobs directly,
- inject failures by operation or call count,
- inspect call counts and stored data,
- simulate lease state and time.

Use these for recovery artifacts, stale cache state, cleanup failure, and deterministic failure-path coverage.

### Azurite Integration Tests

Azurite tests validate real HTTP/Azure REST compatibility. They run strict by default. Use `AZURITE_LOOSE=1` only when debugging compatibility issues.

Azurite is not enough for every failure mode. It cannot easily simulate every partial write, lost response, or precise mid-operation crash. Pair it with mock/fault-injection tests for edge semantics.

### Sanitizer Tests

`make sanitize` runs the C unit suite under ASan/UBSan. Leak detection is disabled for this gate because the test framework and long-lived library initialization are not designed as leak-clean process teardown tests. ASan signal handling is configured so sanitizer execution works under the local release gate.

### Property Tests

Generated data operations should succeed or return expected contention errors. Unexpected `SQLITE_ERROR` for generated INSERT/UPDATE/DELETE operations is a regression, not an acceptable random outcome. Transaction-control operations may fail without corrupting state, but data operations should not silently drift from the model.

### Release Gate Semantics

The release gate has two different meanings:

- default mode: fast/local validation; may pass with explicit skipped-gate reporting,
- full mode: release readiness; must fail if required gates are skipped.

Do not add a new skipped gate without deciding which category it belongs to.

## Auditing Checklist

When auditing or implementing a feature, verify:

- Does this change affect any recovery artifact?
- Can any remote check failure be mistaken for absence?
- Can any remote cleanup failure leave stale state while reporting success?
- Does any code path hold `azure_client_t::mutex` and call a helper that also locks it?
- Are dirty/valid bitmaps updated with cache file size changes?
- Does a Rust safe API call mutate global VFS state?
- Do tests assert behavior or only call counts?
- Does the release gate still communicate skipped versus full validation correctly?
- Do docs and examples reference real build targets and supported modes?

## Known Areas for Future Improvement

- Extract a shared no-lock Azure request primitive to reduce duplication between normal requests and mutex-held batch lease renewal.
- Optimize authoritative journal checks without reintroducing cross-process recovery unsafety.
- Consider exact Rust registration descriptors instead of fingerprint identity if safe reconfiguration logic grows.
- Add live-Azure release validation in environments with scoped, short-lived credentials.
