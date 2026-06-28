# Feature Specification: Review Findings Remediation

**Branch**: feature/review-findings-remediation  |  **Created**: 2026-06-28  |  **Status**: Draft
**Input Brief**: Address all findings from the 2026-06-28 invariant audit without creating or pushing any pull request.

## Overview

The sqlite-objs project needs a coordinated remediation pass across its storage correctness, wrapper safety, tests, documentation, and release automation. The goal is to convert the invariant audit findings into concrete, verified fixes so users can rely on sqlite-objs for cloud-backed SQLite durability, predictable Rust APIs, and accurate project guidance.

The most important user value is data safety. A user who commits data, experiences contention, or recovers after interruption must not encounter missed recovery artifacts, stale journal/WAL state, silent success after a failed remote operation, or a hung synchronization path. Failures should be visible and recoverable rather than masked.

The second user value is trustworthy validation. Contributors and maintainers need tests and release gates that prove the important invariants rather than passing with major coverage skipped, loose emulator behavior, or test helpers that can false-pass. Documentation should describe what the project actually supports and how to build and validate it.

This workflow is local-only. It should produce code, tests, documentation, and local commits as needed, but it must not create, push, or post any pull request.

## Objectives

- Preserve database correctness across rollback-journal, WAL, cache-reuse, lease, and synchronization failure scenarios.
- Ensure large or long-running Azure operations fail or retry predictably without deadlocking, leaking synchronization primitives, or reporting false success.
- Make the Rust wrappers safe and honest about registration, configuration requirements, feature support, and minimum supported toolchain.
- Strengthen tests so critical invariants fail when behavior regresses, including partial-write/crash semantics, property-test failures, and file-control helpers.
- Align documentation, build targets, and release automation with real project behavior and validation expectations.

## User Scenarios & Testing

### User Story P1 – Durable Recovery
Narrative: As an application operator, I need committed database state to recover correctly after crashes, journal transitions, and WAL cleanup so that sqlite-objs does not lose or resurrect data.
Independent Test: Simulate crash/recovery and journal/WAL cleanup scenarios, then verify integrity and committed/uncommitted row expectations.
Acceptance Scenarios:
1. Given a stale or newly created rollback journal, When a database is reopened, Then recovery behavior is determined from authoritative remote state rather than an unsafe stale absence cache.
2. Given a database using supported journal modes, When a transaction commits and the journal is truncated or cleaned up, Then subsequent opens do not replay stale remote journal data.
3. Given WAL state cleanup encounters a remote delete failure, When SQLite requests the cleanup, Then the failure is surfaced rather than reported as success.

### User Story P1 – Reliable Remote Synchronization
Narrative: As a writer using Azure-backed storage, I need large syncs and error paths to complete, retry, or fail clearly without hanging the connection or poisoning the client.
Independent Test: Exercise batch page writes and injected allocation/failure paths, then verify no deadlock, no leaked lock, and correct error propagation.
Acceptance Scenarios:
1. Given a long-running batch write needs lease renewal, When renewal is triggered, Then the client does not self-deadlock and the write completes or fails explicitly.
2. Given allocation fails during batch-write setup, When the operation returns, Then the Azure client remains usable for later operations.
3. Given remote write/read/auth/retry operations return known errors, When they propagate through SQLite-facing operations, Then callers receive actionable failure codes.

### User Story P1 – Safe Rust API Surface
Narrative: As a Rust user, I need safe wrapper APIs that cannot accidentally invalidate live VFS state and that reject invalid configuration before crossing unsafe boundaries.
Independent Test: Call registration/configuration APIs under valid and invalid conditions and verify safe behavior, clear errors, and feature-gated compatibility.
Acceptance Scenarios:
1. Given registration is already established, When Rust code attempts repeated or concurrent registration, Then behavior is deterministic and cannot reset active global VFS state unsafely.
2. Given empty account/container or missing authentication, When constructing Rust configuration or URIs, Then the safe API rejects the input with a clear error.
3. Given documented Rust version and enabled features, When the workspace is checked, Then supported feature combinations compile under the documented policy.

### User Story P2 – Trustworthy Test and Release Gates
Narrative: As a maintainer, I need automated validation to cover the invariants it claims to cover so release decisions are based on meaningful evidence.
Independent Test: Run local gates and inspect failure behavior when critical prerequisites or generated operation failures occur.
Acceptance Scenarios:
1. Given a release gate skips critical coverage, When it reports the result, Then skipped gates are visible and cannot be mistaken for full release readiness.
2. Given generated property-test operations encounter unexpected ordinary SQL errors, When the property test runs, Then those errors fail the test rather than being silently accepted.
3. Given emulator fidelity matters, When integration tests run, Then the runner either uses stricter behavior by default or documents/flags loose-mode limitations.
4. Given file-control helpers fail, When tests query download counts, Then the test fails instead of treating the default value as success.

### User Story P2 – Accurate Documentation and Automation
Narrative: As a contributor or user, I need README, benchmark, demo, and workflow instructions that match the real build system, supported modes, and release process.
Independent Test: Follow documented build/validation commands and inspect configured CI/release workflows.
Acceptance Scenarios:
1. Given a documented production or benchmark build command, When a user runs it, Then the target exists or the documentation points to the correct command.
2. Given WAL support is documented, When users read limitations and tests, Then the docs match the actual supported/exclusive-locking behavior.
3. Given release workflows are present, When they run, Then they perform meaningful project build/test/release work or clearly stop as intentionally unconfigured.

### Edge Cases

- A remote journal existence cache must not suppress authoritative checks needed for crash recovery.
- Remote cleanup failure must not be converted into local success if stale recovery data can remain visible.
- Long-running batch writes must handle lease renewal without recursive client-lock acquisition.
- Allocation failure after acquiring an internal mutex must not leave future operations blocked.
- Rust API changes must preserve backwards compatibility where safe and produce explicit errors where previous behavior was unsafe or misleading.
- Validation improvements must avoid requiring live Azure credentials for default local development unless the gate explicitly requests live Azure coverage.

## Requirements

### Functional Requirements

- FR-001: Ensure rollback-journal discovery and recovery cannot be bypassed by stale in-process absence state. (Stories: P1 Durable Recovery)
- FR-002: Ensure supported journal cleanup modes do not leave stale non-empty remote journals after successful commits. (Stories: P1 Durable Recovery)
- FR-003: Surface WAL cleanup/delete failures when stale WAL state could affect later opens. (Stories: P1 Durable Recovery)
- FR-004: Remove or avoid batch-write self-deadlock during lease renewal. (Stories: P1 Reliable Remote Synchronization)
- FR-005: Ensure all batch-write failure paths release acquired client synchronization resources. (Stories: P1 Reliable Remote Synchronization)
- FR-006: Preserve clear error propagation across Azure client, VFS, and SQLite-facing operations for the remediated paths. (Stories: P1 Reliable Remote Synchronization)
- FR-007: Make Rust VFS registration safe against accidental repeat/concurrent global-state reinitialization, or explicitly mark unsafe/reconfiguration-only paths. (Stories: P1 Safe Rust API Surface)
- FR-008: Validate Rust configuration and URI required fields before unsafe FFI calls where the safe API can detect invalid input. (Stories: P1 Safe Rust API Surface)
- FR-009: Align Rust minimum supported version and feature-gated syntax so documented support matches actual compilation behavior. (Stories: P1 Safe Rust API Surface)
- FR-010: Strengthen tests for hot-journal, journal cleanup, WAL cleanup failure, batch renewal, mutex release, and Rust API safety. (Stories: P1 Durable Recovery, P1 Reliable Remote Synchronization, P1 Safe Rust API Surface)
- FR-011: Strengthen crash/partial-write tests so they prove behavior after representative partially persisted remote state, not only clean pre-write failure handling. (Stories: P1 Durable Recovery, P2 Trustworthy Test and Release Gates)
- FR-012: Strengthen release/test gates and helpers so skipped coverage, loose emulator mode, file-control failure, and property-test SQL errors are not mistaken for passing critical invariants. (Stories: P2 Trustworthy Test and Release Gates)
- FR-013: Update documentation and workflow automation references so build targets, WAL behavior, release gates, and validation commands match repository reality. (Stories: P2 Accurate Documentation and Automation)

### Key Entities

- Recovery Artifact: Any journal or WAL state used by SQLite to recover committed or uncommitted changes after interruption.
- Azure Client Operation: A remote storage action whose retry, locking, and error behavior can affect SQLite-visible correctness.
- Rust Safe API: The Rust wrapper layer intended to prevent misuse of unsafe FFI and provide clear configuration errors.
- Validation Gate: A documented or automated test/build/release step used to decide readiness.

### Cross-Cutting / Non-Functional

- Data-safety remediations must prefer explicit failure over silent success whenever remote state may remain inconsistent.
- Default local validation should remain runnable without live Azure credentials; live-Azure validation may remain opt-in but must be clearly distinguished.
- Documentation changes must be consistent across README, test docs, benchmark/demo instructions, and release automation where applicable.

## Success Criteria

- SC-001: Local unit, sanitizer, Azurite integration, and Rust workspace tests pass after remediation. (FR-001 through FR-012)
- SC-002: Targeted tests fail before the relevant correctness fixes or demonstrate coverage of the previously untested invariant, then pass after remediation. (FR-001, FR-002, FR-003, FR-004, FR-005, FR-010)
- SC-003: Rust safe wrapper tests cover invalid configuration and repeat/concurrent registration behavior with deterministic results. (FR-007, FR-008, FR-009)
- SC-004: Crash/partial-write validation includes at least one scenario where recovery is checked after representative remote state exists past the pre-write hook boundary. (FR-011)
- SC-005: Release/test gate behavior clearly distinguishes full readiness from skipped or optional coverage. (FR-012)
- SC-006: Documented build and validation commands can be resolved to existing repository targets or commands. (FR-013)
- SC-007: Final society-of-thought review finds no unresolved high-severity correctness, safety, or validation gaps from the original audit. (FR-001 through FR-013)

## Assumptions

- The 2026-06-28 invariant audit findings in WorkflowContext.md are authoritative scope for this workflow.
- Remediation should use local commits and artifacts only; no branch push or pull request creation is allowed.
- Where a finding can be fixed either by behavior change or by explicitly rejecting unsupported mode, the chosen outcome should prioritize data safety and clear user-facing semantics.
- Live Azure cloud validation may remain optional unless a specific fix cannot be meaningfully validated against Azurite or mocks.

## Scope

In Scope:
- C VFS rollback journal, WAL, cache/revalidation, locking, sync, and Azure client fixes needed by the audit findings.
- Rust wrapper safety, configuration validation, MSRV/feature consistency, and test helper fixes.
- C and Rust tests for each remediated invariant where practical.
- Documentation and release/CI workflow updates that directly address the audit findings.
- Final society-of-thought review of the completed local implementation.

Out of Scope:
- New storage backends unrelated to Azure Blob Storage.
- Performance optimization beyond what is required to avoid deadlocks, stale state, or misleading validation.
- Publishing crates, tagging releases, or pushing/creating any pull request.
- Broad redesign of SQLite VFS architecture beyond the targeted invariant remediation.

## Dependencies

- Existing C build and test targets.
- Existing Rust workspace and feature flags.
- Azurite for emulator-backed integration tests.
- GitHub workflow files for release/preview/promotion gate corrections.

## Risks & Mitigations

- Risk: Tightening journal/WAL behavior could alter SQLite mode compatibility. Mitigation: add explicit tests and document supported/unsupported modes.
- Risk: Rust registration hardening could break callers that relied on repeat registration. Mitigation: provide deterministic idempotent behavior or explicit reconfiguration API semantics.
- Risk: Stronger gates could lengthen validation. Mitigation: separate fast local gates from extended/live-Azure gates while making skip status explicit.
- Risk: Partial-write behavior may be hard to simulate with Azurite. Mitigation: add mock/fault-injection tests that create remote state representative of partial persistence, and document live-Azure limitations where needed.

## References

- Workflow context: .paw/work/review-findings-remediation/WorkflowContext.md
