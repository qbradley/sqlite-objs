# Review Findings Remediation Implementation Plan

## Overview

This plan remediates the invariant audit findings across the C VFS/Azure client, Rust wrapper crates, tests, documentation, and release automation. The approach is to fix the data-safety issues first, then harden Rust and validation surfaces, then align documentation and workflow automation so maintainers can trust the resulting gates.

## Current State Analysis

The C VFS uses a global rollback-journal existence cache that can skip remote HEAD checks for cached-absent journals, while journal `xTruncate` only mutates the in-memory journal buffer and does not update remote block-blob state [CodeResearch.md:47-65](CodeResearch.md#L47-L65). WAL cleanup has a similar explicit-failure gap: `xTruncate(0)` calls `blob_delete` but ignores the result before clearing local state [CodeResearch.md:67-74](CodeResearch.md#L67-L74).

The production Azure batch-write path holds the client mutex while using the multi handle, then calls lease renewal through the request helper that locks the same mutex; it also has a per-attempt allocation failure path that returns without unlocking [CodeResearch.md:76-88](CodeResearch.md#L76-L88). Existing tests cover many lease and upload paths but not these exact production mutex/renewal failure modes.

Crash tests currently inject failures through hooks that run before the risky remote operation starts, so they prove clean pre-write error handling rather than behavior with representative partially persisted remote state [CodeResearch.md:90-97](CodeResearch.md#L90-L97). Release/test gates also have fidelity gaps: Azurite always starts with loose flags, property tests tolerate unexpected `SQLITE_ERROR`, and release gates/workflows can appear green while important coverage is skipped or unconfigured [CodeResearch.md:116-128](CodeResearch.md#L116-L128).

The Rust wrappers expose safe repeatable registration methods over a global C VFS state that is reset on every registration path, validate only NUL bytes in safe config APIs, declare Rust 1.70 while feature-gated helper syntax uses newer constructs, and one ignored integration helper ignores `sqlite3_file_control` return codes [CodeResearch.md:99-114](CodeResearch.md#L99-L114).

## Desired End State

Rollback-journal and WAL cleanup/recovery decisions use authoritative remote state when needed, and remote cleanup failures surface to SQLite callers. Large Azure batch writes do not self-deadlock and every failure path releases internal synchronization resources. Rust safe APIs are deterministic, validate required fields before FFI, and compile under the documented MSRV/feature policy. Tests include targeted coverage for every fixed invariant, including representative partial remote state beyond pre-operation hook failures. Documentation, scripts, and workflows point at real commands and clearly distinguish fast, extended, and live-Azure readiness.

## What We're NOT Doing

- No pull request creation, posting, or pushing.
- No new storage backend or broad VFS redesign.
- No live release publishing, crate publishing, or tag creation.
- No requirement to make default local validation depend on live Azure credentials.
- No performance optimization beyond what is needed to fix deadlocks, stale state, false success, and misleading gates.

## Phase Status

- [x] **Phase 1: Recovery Artifact Safety** - Fix rollback-journal/WAL cleanup and discovery invariants with targeted tests.
- [x] **Phase 2: Azure Batch Synchronization Safety** - Fix production batch-write lease renewal and mutex cleanup paths with targeted coverage.
- [x] **Phase 3: Rust Wrapper Safety and Compatibility** - Harden safe registration/configuration/MSRV/test helper behavior.
- [x] **Phase 4: Test Gate and Release Automation Integrity** - Make tests/gates/workflows fail or report accurately for critical coverage.
- [x] **Phase 5: Documentation and Final Validation** - Update project docs, create as-built Docs.md, and run final validation.

## Cross-Phase Verification Standard

For each targeted invariant test added in Phases 1-3, record evidence that it fails against the pre-fix behavior or otherwise demonstrates coverage of the previously untested invariant. Acceptable evidence includes a failing baseline run before the fix, a focused fault-injection/mutation check, or an implementation note explaining why the test necessarily exercises the audited failure mode.

## Phase Candidates

---

## Phase 1: Recovery Artifact Safety

### Changes Required:

- **`src/sqlite_objs_vfs.c`**: Rework rollback-journal existence caching so cached-absent state cannot suppress authoritative remote checks required for hot-journal discovery. Preserve useful cache updates from `xDelete` and journal `xSync`, but bypass cached-absent state for SQLite journal open/access checks that determine recovery so those paths perform an authoritative remote check.
- **`src/sqlite_objs_vfs.c`**: Implement an explicit rollback-journal mode policy. Supported rollback-journal cleanup should be data-safe: `DELETE` deletes the remote journal; `TRUNCATE` requests are normalized to safe remote deletion or authoritative zero-length state; `PERSIST` is either normalized/rejected to `DELETE` or must upload a zeroed-header state that tests prove cannot replay as hot. Unsupported modes must be rejected or documented rather than silently leaving stale remote data.
- **`src/sqlite_objs_vfs.c`**: Propagate WAL `blob_delete` failures from `xTruncate(0)` when stale WAL could remain visible; keep empty-WAL success behavior intact when delete is unnecessary or the blob is already absent.
- **`test/test_vfs.c`**: Add unit tests for stale cached-absent journal state followed by remote journal presence, journal-mode `TRUNCATE`/zero-length cleanup behavior, and cleanup failure propagation.
- **`test/test_wal.c`**: Add WAL checkpoint/truncate failure test using mock `blob_delete` injection.
- **`test/test_vfs.c` or `test/test_wal.c`**: Add at least one mock/fault-injection recovery test where representative remote journal/WAL state exists beyond the pre-write hook boundary and reopen behavior proves authoritative discovery. Add an Azurite integration variant only if the mechanism can create the same representative remote state reliably.

### Success Criteria:

#### Automated Verification:
- [x] Tests pass: `make test-unit`
- [x] Targeted integration passes: `make test-integration`
- [x] Targeted invariant tests have fail-first or equivalent coverage evidence recorded per the cross-phase verification standard.

#### Manual Verification:
- [x] Hot-journal discovery cannot be bypassed solely by stale cached absence.
- [x] Journal/WAL cleanup failures are observable to SQLite callers where stale remote recovery artifacts could remain.
- [x] Representative partial-state test covers behavior after remote recovery state exists, not only pre-operation hook aborts.

### Phase 1 Notes:

- Added mock/VFS coverage that first caches `test.db-journal` as absent, then creates remote journal blob state and verifies both direct `xAccess` and SQLite reopen paths perform authoritative journal discovery.
- Added recovery-artifact fail-closed coverage for `blob_exists` failures when no journal cache entry exists.
- Added `TRUNCATE` journal cleanup coverage proving remote journal deletion and delete-failure propagation.
- Added WAL checkpoint/truncate delete-failure coverage and strengthened existing WAL recovery coverage to assert recovered WAL-only data after reopen.
- Fail-first/equivalent evidence: the new stale-cached-absence test directly exercises the previously unsafe cache path; the new journal/WAL cleanup failure tests target code paths that previously ignored or skipped remote cleanup errors.

---

## Phase 2: Azure Batch Synchronization Safety

### Changes Required:

- **`src/azure_client.c`**: Refactor production `az_page_blob_write_batch` lease renewal so it does not re-enter `execute_with_retry` while holding `azure_client_t::mutex`. Viable options:
  - Release the batch mutex around renewal only if CURLM state and active handles are safely quiesced.
  - Introduce an internal no-lock lease-renew request path for use while the caller already owns the mutex.
  - Use a separate CURL easy handle for lease renewal outside the shared easy-handle lock.
- **`src/azure_client.c`**: Normalize batch-write cleanup with a single cleanup/unlock path so every allocation/setup/lease-loss/error branch releases `done`, request arrays, CURL handles, and the client mutex exactly once.
- **`src/azure_client.c` / test seam**: Add a testable hook or injectable failure path for batch per-attempt allocation/renewal failure if existing hooks cannot cover it without unsafe memory pressure.
- **`test/test_azure_client.c` or `test/test_coalesce.c`**: Add focused tests that exercise batch lease renewal without deadlock and verify client usability after injected setup failure.
- **`test/test_azure_client.c`, `test/test_coalesce.c`, or `test/test_vfs.c`**: Add or extend error-propagation coverage so representative remediated batch/write/lease/auth/retry failures still map to actionable SQLite-facing or Azure-client error codes after the refactor.
- **`test/test_chaos.c`**: Extend retry/lease tests if this is the most appropriate location for renewal failure classification.

### Success Criteria:

#### Automated Verification:
- [x] Tests pass: `make test-unit`
- [x] Sanitizer passes: `make sanitize`
- [x] Targeted invariant tests have fail-first or equivalent coverage evidence recorded per the cross-phase verification standard.

#### Manual Verification:
- [x] Batch-write lease renewal path has no recursive acquisition of the same non-recursive mutex.
- [x] Every batch-write return path after mutex acquisition has an auditable unlock/cleanup route.
- [x] Client remains usable after injected batch setup failure.

### Phase 2 Notes:

- Replaced recursive lease renewal inside production batch writes with a mutex-held renewal path using a temporary CURL easy handle instead of `execute_with_retry`.
- Fixed the per-attempt batch request allocation failure path to unlock the Azure client mutex before returning.
- Added integration coverage that injects batch request allocation failure and verifies the client mutex is released.
- Added deterministic integration coverage that forces the batch lease-renewal path to run and verifies it completes without deadlock or client mutex poisoning.
- Fail-first/equivalent evidence: the allocation-failure hook targets the previously leaked-mutex branch directly; the lease-renewal override drives the previously deadlocking renewal branch; existing batch partial-failure tests continue to verify VFS error propagation.

---

## Phase 3: Rust Wrapper Safety and Compatibility

### Changes Required:

- **`rust/sqlite-objs/src/lib.rs`**: Add process-wide registration coordination for safe wrapper APIs. Prefer deterministic idempotent behavior for repeated same-mode registration, and return a clear error for attempted incompatible reconfiguration through safe APIs. If true reconfiguration remains needed for tests, expose it only through an explicitly unsafe or clearly named API. Scope this phase to the Rust safe API boundary; direct C API re-registration over global VFS state should be documented unless a targeted C-level fix is added.
- **`rust/sqlite-objs/src/lib.rs`**: Validate non-empty account/container and presence of either SAS token or account key in `SqliteObjsConfig` before FFI; keep NUL-byte validation.
- **`rust/sqlite-objs/src/lib.rs`**: Add fallible URI-building or validation for required account/container/auth fields while preserving existing builder ergonomics where possible. If `build()` must remain infallible for compatibility, add `try_build()` and update docs/tests to recommend it for validated URIs.
- **`rust/sqlite-objs/src/pragmas.rs`**: Replace newer raw-reference/C-string literal syntax with Rust 1.70-compatible code, or raise documented MSRV consistently. Preferred for lower churn: keep Rust 1.70 and use compatible `CStr`/pointer construction.
- **`rust/sqlite-objs/tests/vfs_integration.rs`**: Make the local download-count helper assert/check `sqlite3_file_control` return code, or use the checked `sqlite_objs::pragmas::get_download_count` helper under the `rusqlite` feature.
- **Rust tests**: Add tests for invalid empty config, missing auth, repeated/concurrent safe registration behavior, validated URI construction, and feature-gated pragma helper compilation.

### Success Criteria:

#### Automated Verification:
- [x] Tests pass: `cd rust && cargo test --workspace`
- [x] Feature-gated tests pass: `cd rust && cargo test --workspace --all-features`
- [x] MSRV policy check passes: `cd rust && cargo +1.70.0 check --workspace --all-features` if Rust 1.70 remains documented; otherwise all MSRV documentation is updated and checked against the newly documented minimum.
- [x] Targeted invariant tests have fail-first or equivalent coverage evidence recorded per the cross-phase verification standard.

#### Manual Verification:
- [x] Safe Rust API cannot accidentally reset global C VFS state for active users.
- [x] Invalid required config values fail before unsafe FFI.
- [x] MSRV/documented feature behavior is internally consistent.

### Phase 3 Notes:

- Added a process-wide Rust registration gate so safe registration calls are idempotent or fail before reinitializing global C VFS state.
- Added Rust-side required config validation for account, container, and auth before FFI.
- Added `UriBuilder::try_build()` for validated URI construction while preserving `build()` compatibility.
- Replaced feature-gated pragma helper raw-reference/C-string literal usage with Rust 1.82-compatible code and raised documented Rust MSRV from 1.70 to 1.82 after dependency validation.
- Made the ignored Azure integration download-count helper assert `sqlite3_file_control` success instead of false-passing on the default count.
- Fail-first/equivalent evidence: new invalid config, `try_build`, concurrent registration, and file-control return-code assertions directly cover the audited failure modes. Rust 1.70 and 1.71 checks failed on dependency/toolchain requirements; Rust 1.82 all-features check passed and documentation was updated accordingly.

---

## Phase 4: Test Gate and Release Automation Integrity

### Changes Required:

- **`test/test_integration.c`**: Make deterministic property tests fail on unexpected `SQLITE_ERROR` for generated insert/update/delete operations unless a scenario has an explicitly documented expected constraint/error reason.
- **`test/run-integration.sh`**: Make Azurite loose/API-version behavior explicit and configurable. First verify whether the suite passes without `--loose`/`--skipApiVersionCheck`; if it does, default to strict mode with opt-in `AZURITE_LOOSE=1`. If strict mode exposes broader compatibility work, keep loose mode but print/flag the fidelity limitation and document it.
- **`scripts/release-gate.sh`**: Make skipped critical gates visible in the final result and prevent "alpha ready" language when important release-readiness gates are skipped. Preserve fast local usability by distinguishing "fast gate passed" from "full release gate passed".
- **`.github/workflows/squad-ci.yml`**: Keep CI local-fast by default, but ensure it reports fast-gate semantics accurately and installs/runs the available Azurite-backed gate intentionally.
- **`.github/workflows/squad-release.yml`, `squad-insider-release.yml`, `squad-preview.yml`, `squad-docs.yml`**: Replace placeholder echo commands with repository-appropriate build/test/docs validation, or convert them into explicit disabled/manual workflows that fail with actionable configuration messages rather than echoing success-shaped placeholders.
- **`.github/workflows/squad-promote.yml`**: Rework the functional preview-to-main promotion gates so they no longer depend on nonexistent Node `package.json` metadata or missing `CHANGELOG.md` assumptions; preserve intentional forbidden-path checks and promotion logic where still applicable.

### Success Criteria:

#### Automated Verification:
- [x] Tests pass: `make test-integration`
- [x] Gate smoke passes: `./scripts/release-gate.sh`
- [x] Workflow YAML remains syntactically valid by inspection and any available local YAML/tooling checks.

#### Manual Verification:
- [x] Fast gate output no longer implies full release readiness when extended/live-Azure gates are skipped.
- [x] Property tests no longer accept unexpected data-operation `SQLITE_ERROR` as success.
- [x] Azurite runner makes strict/loose fidelity explicit.
- [x] Workflows no longer rely on nonexistent `package.json` release metadata for this C/Rust repository.

### Phase 4 Notes:

- Property tests now fail on unexpected `SQLITE_ERROR` for generated data operations instead of treating it as acceptable.
- Azurite integration startup now defaults to strict mode, prints the selected fidelity mode, and supports opt-in `AZURITE_LOOSE=1` compatibility mode.
- Release gate output distinguishes fast/local pass-with-skips from full release readiness and exempts sanitizer execution from the `timeout` wrapper that conflicts with ASan signal handling.
- Preview/release/docs workflows now run repository-appropriate validation instead of placeholder echo commands; promotion now reads the Rust workspace version instead of nonexistent Node package metadata.
- Verification: `make test-integration` passed 57 tests; `./scripts/release-gate.sh` passed with explicit skipped-gate reporting; workflow YAML parsed with Python/PyYAML and shell scripts passed `bash -n`.

---

## Phase 5: Documentation and Final Validation

### Changes Required:

- **`.paw/work/review-findings-remediation/Docs.md`**: Create the as-built technical reference with implementation details, changed invariants, and validation commands. Load `paw-docs-guidance` before writing.
- **Final society-of-thought review**: Run the configured final PAW society-of-thought review after implementation and local validation, and resolve or document any findings before final milestone pause.
- **`README.md`**: Correct production build instructions, WAL support/limitations, validation tiers, and any changed behavior for journal/WAL cleanup or Rust APIs.
- **`benchmark/README.md`, `benchmark/tpcc/README.md`, `demo/README.md`, `demo/azure-demo.sh`**: Replace nonexistent `make all-production` references or add a compatible Makefile target if that is the chosen documentation-compatible fix.
- **`TEST_DOCS_INDEX.md`, `TEST_QUICK_REFERENCE.md`, and related test docs**: Update validation gate semantics, Azurite strict/loose mode notes, and newly added invariant tests.
- **`rust/README.md`, `rust/QUICKSTART.md`, `rust/sqlite-objs/README.md`, `rust/sqlite-objs-sys/README.md`**: Update Rust API validation, MSRV/feature notes, and registration semantics.
- **Validation**: Run local verification commands that cover all changed surfaces.

### Success Criteria:

#### Automated Verification:
- [x] Tests pass: `make test-unit`
- [x] Tests pass: `make sanitize`
- [x] Tests pass: `make test-integration`
- [x] Tests pass: `cd rust && cargo test --workspace`
- [x] Tests pass: `cd rust && cargo test --workspace --all-features`
- [x] Gate smoke passes: `./scripts/release-gate.sh`
- [ ] Final society-of-thought review completes with no unresolved high-severity correctness, safety, or validation gaps from the original audit.

#### Manual Verification:
- [x] Documentation references only existing targets or intentionally added targets.
- [x] README limitations match implemented WAL/journal behavior.
- [x] Docs.md captures final as-built state and validation results.
- [x] No pull request was created, pushed, or posted.

### Phase 5 Notes:

- Created `Docs.md` as the as-built technical reference.
- Updated README, benchmark/demo docs, test docs, and Rust docs for actual build targets, strict Azurite default, WAL/exclusive-locking support, Rust 1.82, and validated URI construction.
- Updated benchmark TPC-C runtime messages to reference `make all` instead of the nonexistent `make all-production`.
- Validation completed: `make test-unit`, `make sanitize`, `make test-integration`, `cd rust && cargo test --workspace`, `cd rust && cargo test --workspace --all-features`, `cd rust && cargo +1.82.0 check --workspace --all-features`, `cd rust && cargo doc --workspace --no-deps --quiet`, and `./scripts/release-gate.sh`.

---

## References

- Issue: none
- Spec: `.paw/work/review-findings-remediation/Spec.md`
- Research: `.paw/work/review-findings-remediation/CodeResearch.md`
