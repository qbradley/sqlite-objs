# Review Findings Remediation

## Overview

This work remediates the 2026-06-28 invariant audit findings across sqlite-objs storage correctness, Azure client synchronization, Rust wrapper safety, test fidelity, release gates, and documentation. The main goal was to make data-safety failures explicit, prevent stale recovery artifacts from being ignored, and ensure contributor validation no longer reports success-shaped results when important coverage is skipped.

The implementation preserves the local-only workflow requested for this effort. All changes are committed locally on `feature/review-findings-remediation`; no pull request was created or pushed.

## Architecture and Design

### High-Level Architecture

sqlite-objs has three relevant layers:

- **SQLite VFS layer (`src/sqlite_objs_vfs.c`)**: maps main database files to Azure page blobs and rollback journals/WAL files to block blobs. This layer owns SQLite-visible semantics such as `xAccess`, `xOpen`, `xTruncate`, `xSync`, and locking.
- **Azure client layer (`src/azure_client.c`)**: implements Azure REST calls, retry behavior, leases, and parallel batch writes using libcurl.
- **Rust bindings (`rust/sqlite-objs*`)**: provide raw FFI plus a safe ergonomic API for registration, URI construction, metrics, and rusqlite helpers.

The remediation tightened the contracts between these layers. Recovery-artifact discovery now fails closed for rollback journal/WAL probes when remote state cannot be checked. Journal and WAL cleanup paths no longer report success when remote cleanup fails in a way that could leave stale recovery data visible. Batch page writes no longer recursively lock the Azure client mutex for lease renewal. Rust safe APIs now validate required Azure configuration before FFI and coordinate process-wide VFS registration.

### Design Decisions

**Recovery artifact checks are authoritative.** The prior journal existence cache treated cached absence as authoritative. That was unsafe across processes: another process could create a journal and crash after this process cached absence. The cache is now an optimization/state record only; recovery-artifact access paths perform remote checks and fail closed on remote check failures.

**Cleanup failure is visible.** A successful truncate/checkpoint must mean remote recovery state was also cleaned up or made safe. WAL truncate/delete and rollback-journal truncate cleanup now propagate meaningful SQLite-facing errors when remote cleanup fails unexpectedly.

**Batch lease renewal avoids recursive mutex acquisition.** The production batch write path holds `azure_client_t::mutex` while using the persistent CURLM handle. Calling the normal request helper from inside that critical section would re-lock the same non-recursive mutex. A dedicated locked renewal path uses a temporary CURL easy handle instead.

**Rust safe registration is conservative.** The C VFS still has global registration state. The Rust safe layer now makes repeated registration idempotent only for compatible requests and rejects incompatible safe reconfiguration rather than resetting global C state unexpectedly.

**Rust MSRV is raised to match dependencies.** The older Rust 1.70 policy no longer matches current dependencies and feature-gated code. The documented Rust minimum is now 1.82, validated with `cargo +1.82.0 check --workspace --all-features`.

**Fast gates are not full release readiness.** The release gate now explicitly reports "passed with skips" when extended, TCL, or live-Azure gates are skipped. This keeps local contributor feedback fast without claiming full release readiness.

## User Guide

### Prerequisites

- C build dependencies: C11 compiler, libcurl, OpenSSL, SQLite amalgamation included in the repo.
- Rust 1.82+ for Rust bindings.
- Azurite for local integration tests.

### Basic Usage

Build the project with:

```bash
make all
```

Run fast local validation with:

```bash
make test-unit
make sanitize
make test-integration
cd rust && cargo test --workspace
```

Run the fast release gate with:

```bash
./scripts/release-gate.sh
```

If optional gates are skipped, the report now says the fast/local gate passed with skips and explains that this is not full release readiness.

### Advanced Usage

Azurite integration tests run in strict mode by default. To opt into the old loose compatibility behavior:

```bash
AZURITE_LOOSE=1 make test-integration
```

For validated Rust URI construction, prefer:

```rust
let uri = sqlite_objs::UriBuilder::new("my.db", "account", "container")
    .sas_token("sv=...")
    .try_build()?;
```

`UriBuilder::build()` remains available for compatibility but does not validate required Azure fields before emitting a URI.

## API Reference

### Key Components

- `sqliteObjsAccess` / `sqliteObjsOpen`: recovery-artifact discovery now checks remote state rather than trusting stale cached absence.
- `sqliteObjsTruncate`: rollback journal and WAL truncate cleanup now propagates remote delete failures.
- `az_page_blob_write_batch`: batch write cleanup and lease renewal avoid recursive client mutex acquisition and unlock reliably on request-array allocation failure.
- `SqliteObjsVfs::register*`: safe Rust registration is process-coordinated and idempotent only for compatible registrations.
- `UriBuilder::try_build`: validates account, container, and auth fields before returning a URI.
- `sqlite_objs::pragmas`: feature-gated file-control helpers are compatible with the documented Rust 1.82 minimum.

### Configuration Options

- `AZURITE_LOOSE=1`: opt into Azurite loose/API-version compatibility mode.
- `AZURE_STORAGE_ACCOUNT`, `AZURE_STORAGE_CONTAINER`, `AZURE_STORAGE_SAS` / `AZURE_STORAGE_KEY`: configure Azure access for runtime or live integration tests.
- `Review workflow`: This PAW run used local strategy and did not push or create any PR.

## Testing

### How to Test

Primary validation commands used during this workflow:

```bash
make test-unit
make sanitize
make test-integration
cd rust && cargo test --workspace
cd rust && cargo test --workspace --all-features
cd rust && cargo +1.82.0 check --workspace --all-features
./scripts/release-gate.sh
```

The release gate intentionally reports skipped extended/live-Azure checks unless run with `--full` and the required Azure credentials/tooling.

### Edge Cases

- Cached rollback-journal absence no longer suppresses remote checks for recovery artifacts.
- Remote rollback-journal or WAL cleanup failures return errors instead of clearing only local buffers.
- Batch write request-array allocation failure releases the Azure client mutex before returning.
- Batch lease renewal is deterministically covered by integration tests and does not re-enter the normal request helper while holding the client mutex.
- Rust safe registration rejects incompatible default/non-default reconfiguration instead of silently no-oping.
- Rust integration download-count helpers now assert `sqlite3_file_control` success so they cannot false-pass on a default zero count.

## Limitations and Future Work

- The C API still exposes direct registration functions that can reinitialize global VFS state. Phase 3 scoped the hardening to the Rust safe API boundary and documents that direct C reconfiguration remains outside this workflow.
- Live Azure cloud validation remains opt-in because default local validation should not require cloud credentials.
- Full release readiness requires the extended/live-Azure gates to run without skips.
- No release was published and no PR was created as part of this work.
