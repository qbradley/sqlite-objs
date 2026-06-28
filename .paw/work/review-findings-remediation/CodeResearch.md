---
date: 2026-06-28T08:10:33+00:00
git_commit: 6cebf37b4142d78a3d2fc850d4a5262032fd980d
branch: feature/review-findings-remediation
repository: qbradley/sqlite-objs
topic: "Review findings remediation implementation surfaces"
tags: [research, codebase, c-vfs, azure-client, rust, tests, release-gates]
status: complete
last_updated: 2026-06-28
---

# Research: Review Findings Remediation Implementation Surfaces

## Research Question

Research all implementation surfaces needed to address the 2026-06-28 invariant audit findings described in `.paw/work/review-findings-remediation/Spec.md`, covering C VFS journal/WAL correctness, Azure client lease/mutex paths, crash/partial-write tests, Rust wrapper safety/MSRV/configuration, test/release gates, documentation, and existing validation targets.

## Summary

- Rollback journals are Azure block blobs backed by an in-memory `aJrnlData` buffer; discovery is mediated by a global per-journal existence cache that can return cached answers in `xAccess` and can skip HEAD checks in `xOpen` when state is cached absent (`src/sqlite_objs_vfs.c:247-279`, `src/sqlite_objs_vfs.c:3422-3445`, `src/sqlite_objs_vfs.c:3598-3656`).
- Journal cleanup paths differ by SQLite operation: `xDelete` calls remote `blob_delete` and updates the cache, while `xTruncate` for rollback journals only shrinks/zeros the in-memory buffer and returns success (`src/sqlite_objs_vfs.c:1635-1644`, `src/sqlite_objs_vfs.c:3554-3596`).
- WAL `xTruncate(0)` calls `blob_delete` but ignores the Azure return value before clearing the local buffer and returning `SQLITE_OK`; WAL upload/download failures are otherwise propagated in `xSync` and WAL recovery-open paths (`src/sqlite_objs_vfs.c:1617-1633`, `src/sqlite_objs_vfs.c:1820-1823`, `src/sqlite_objs_vfs.c:3515-3522`).
- Production batch page writes hold `azure_client_t::mutex` for the whole multi-handle operation, and the lease-renewal branch calls `az_lease_renew`, which reaches `execute_single` and locks the same mutex (`src/azure_client.c:1921-1922`, `src/azure_client.c:2041-2048`, `src/azure_client.c:1566-1591`, `src/azure_client.c:311-327`).
- Crash tests currently use pre-operation sync hooks that abort before resize/page-write/journal/WAL upload calls, while integration tests verify recovery after those injected pre-write failures (`src/sqlite_objs_vfs.c:48-72`, `src/sqlite_objs_vfs.c:1773-1779`, `src/sqlite_objs_vfs.c:1832-1840`, `src/sqlite_objs_vfs.c:1901-1906`, `src/sqlite_objs_vfs.c:1998-2006`, `test/test_integration.c:4045-4126`, `test/test_integration.c:4129-4199`).
- Rust safe registration methods call global C registration functions; the C VFS stores one static `g_vfsData` and resets it on each registration path (`rust/sqlite-objs/src/lib.rs:194-204`, `rust/sqlite-objs/src/lib.rs:217-273`, `rust/sqlite-objs/src/lib.rs:297-307`, `src/sqlite_objs_vfs.c:3774-3851`, `src/sqlite_objs_vfs.c:3895-3916`).
- The repository uses plain Markdown documentation and Make/Cargo validation commands; release and preview/doc workflows include placeholder build/release/doc commands, while `squad-ci.yml` invokes `./scripts/release-gate.sh` (`README.md:36-85`, `TEST_DOCS_INDEX.md:72-98`, `.github/workflows/squad-release.yml:19-34`, `.github/workflows/squad-preview.yml:17-30`, `.github/workflows/squad-docs.yml:23-27`, `.github/workflows/squad-ci.yml:51-53`).

## Documentation System

- **Framework**: Plain Markdown. No mkdocs/docusaurus/sphinx config was found; root docs are Markdown files such as `README.md`, `TEST_DOCS_INDEX.md`, `TEST_INFRASTRUCTURE_DETAILED.md`, `VFS_TEST_INFRASTRUCTURE.md`, plus `research/*.md`.
- **Docs Directory**: N/A. There is no `docs/` tree; docs are stored at repo root, `research/`, `test/`, `benchmark/`, `demo/`, and `rust/`.
- **Navigation Config**: N/A. `TEST_DOCS_INDEX.md` acts as a manual index for test documentation and validation commands (`TEST_DOCS_INDEX.md:72-98`, `TEST_DOCS_INDEX.md:313-337`).
- **Style Conventions**: Markdown headings with fenced shell/code blocks for commands in README/testing docs (`README.md:36-85`, `TEST_DOCS_INDEX.md:72-98`); Rust docs use crate READMEs and code snippets (`rust/README.md:12-41`, `rust/sqlite-objs/README.md:95-115`).
- **Build Command**: No documentation-site build command found. Rust docs can be checked with `cargo doc` per Rust development docs (`rust/DEVELOPMENT.md:113-119`).
- **Standard Files**: `README.md`, `LICENSE`, `TEST_DOCS_INDEX.md`, `TEST_QUICK_REFERENCE.md`, `TEST_INFRASTRUCTURE_DETAILED.md`, `TEST_INFRASTRUCTURE_QUICK_REFERENCE.md`, `VFS_TEST_INFRASTRUCTURE.md`; no root `CHANGELOG.md` or `CONTRIBUTING.md` was found.

## Verification Commands

- **Test Command**: `make test-unit`, `make test-integration`, `make test`, `make test-stress`, `make test-stress-heavy`, `make test-integration-extended`, `make test-tcl`, `make test-tcl-quick`, and `cd rust && cargo test` are documented validation commands (`Makefile:197-213`, `Makefile:218-238`, `Makefile:242-254`, `README.md:56-85`, `TEST_DOCS_INDEX.md:72-98`).
- **Lint Command**: Rust docs list `cargo fmt` and `cargo clippy`; no C lint target was found in the Makefile target list (`rust/DEVELOPMENT.md:113-130`, `Makefile:299-314`).
- **Build Command**: `make all` builds the C library/shell/benchmarks/TPC-C targets; `cd rust && cargo build` builds the Rust workspace (`Makefile:103-108`, `README.md:36-48`, `rust/README.md:12-22`).
- **Type Check**: No dedicated type-check target was found. Rust development docs list `cargo build --release`; `cargo check` is not documented as a primary command (`rust/DEVELOPMENT.md:113-119`).
- **Release Gate**: `./scripts/release-gate.sh`, with `--extended`, `--azure`, and `--full`, orchestrates local/extended/Azure gates (`scripts/release-gate.sh:5-19`, `scripts/release-gate.sh:143-148`, `scripts/release-gate.sh:216-232`, `scripts/release-gate.sh:241-249`, `scripts/release-gate.sh:258-288`, `scripts/release-gate.sh:297-324`).

## Detailed Findings

### 1. C VFS Rollback Journal Discovery, Existence Cache, and Hot Journal Recovery

- The VFS has a global per-journal existence cache with states `-1` unknown, `0` absent, and `1` present; the cache is part of the global `sqliteObjsVfsData` stored in `sqlite3_vfs.pAppData` and protected by `journalCacheMutex` (`src/sqlite_objs_vfs.c:247-279`).
- Cache lookup/create functions key entries by blob name and initialize new entries with unknown state (`src/sqlite_objs_vfs.c:287-310`).
- `xAccess` uses a cached journal existence result when present and otherwise calls `ops->blob_exists`; after a real lookup it seeds the cache for names ending in `-journal` (`src/sqlite_objs_vfs.c:3598-3656`).
- `xDelete` calls `ops->blob_delete` for Azure paths; `AZURE_ERR_NOT_FOUND` and successful deletes update a cache entry to absent before returning `SQLITE_OK` (`src/sqlite_objs_vfs.c:3554-3596`).
- `xOpen` for `SQLITE_OPEN_MAIN_JOURNAL` gets/creates a cache entry, reads its cached state, skips the HEAD request when cached absent, otherwise calls `blob_exists`, and downloads an existing journal blob into `aJrnlData` for recovery (`src/sqlite_objs_vfs.c:3416-3466`).
- Journal reads and file-size queries are buffer-backed: `xRead` returns from `aJrnlData` with short-read zero-fill behavior, and `xFileSize` reports `nJrnlData` (`src/sqlite_objs_vfs.c:1420-1457`, `src/sqlite_objs_vfs.c:2163-2175`).
- Journal writes append/write into `aJrnlData`, growing the buffer with `jrnlBufferEnsure` (`src/sqlite_objs_vfs.c:1188-1208`, `src/sqlite_objs_vfs.c:1531-1558`).
- Journal `xSync` uploads the full journal buffer as a block blob when `nJrnlData > 0`; on successful upload it marks the journal cache present (`src/sqlite_objs_vfs.c:1832-1876`).

### 2. Non-WAL Journal Cleanup and Remote Block Blob State

- Rollback-journal `xTruncate` shrinks `nJrnlData`, zeroes truncated buffer space, and returns `SQLITE_OK`; it does not call `blob_delete` or upload an empty/truncated block blob on that path (`src/sqlite_objs_vfs.c:1635-1644`).
- Journal `xSync` only performs a block-blob upload when `nJrnlData > 0`, so a journal truncated to zero has no upload in the current journal sync branch (`src/sqlite_objs_vfs.c:1832-1835`).
- The remote delete path for journals is `xDelete`, which calls `blob_delete` and treats `AZURE_ERR_NOT_FOUND` as success while setting the cache absent (`src/sqlite_objs_vfs.c:3554-3596`).
- Existing unit coverage includes a DELETE-mode PRAGMA check, journal creation as block blob, and deletion-after-commit checks (`test/test_vfs.c:1507-1530`, `test/test_vfs.c:1659-1671`).
- Existing lazy-cache journal coverage also asserts a transaction uploads a journal and leaves no `jrnl.db-journal` blob after commit (`test/test_vfs.c:3466-3481`).
- Code search found WAL checkpoint `TRUNCATE` tests, but no explicit `PRAGMA journal_mode=TRUNCATE` or `PRAGMA journal_mode=PERSIST` tests in `test/`; the nearest PRAGMA coverage is DELETE/WAL/MEMORY in `test/test_vfs.c` (`test/test_vfs.c:1659-1685`, `test/test_vfs.c:1985-2048`).

### 3. WAL Truncate/Delete Failure Propagation

- WAL state is kept in an in-memory `aWalData`/`nWalData` buffer on the file object (`src/sqlite_objs_vfs.c:230-233`).
- WAL `xOpen` requires block-blob upload/download operations, checks `blob_exists`, downloads an existing WAL blob for recovery, and returns an SQLite error if WAL download fails (`src/sqlite_objs_vfs.c:3468-3528`).
- WAL `xSync` uploads the full WAL buffer with either single PUT or parallel upload and returns `SQLITE_IOERR_FSYNC` when the Azure upload result is not `AZURE_OK` (`src/sqlite_objs_vfs.c:1763-1829`).
- WAL `xTruncate(0)` calls `blob_delete` when available, ignores the returned Azure status, clears `nWalData`, zeros the local buffer, and returns `SQLITE_OK` (`src/sqlite_objs_vfs.c:1617-1633`).
- Existing WAL tests assert checkpoint `TRUNCATE` calls `blob_delete`, and upload/download/parallel-upload failure tests assert errors propagate in those paths (`test/test_wal.c:431-510`, `test/test_wal.c:517-596`, `test/test_wal.c:825-887`, `test/test_wal.c:1280-1310`).
- The mock Azure layer can inject `blob_delete` failures and count `blob_delete` calls, which is the existing test mechanism adjacent to WAL truncate/delete behavior (`test/mock_azure_ops.c:48-66`, `test/mock_azure_ops.c:650-681`, `test/mock_azure_ops.c:1022-1061`).

### 4. Azure Batch Page Write Lease Renewal, Mutex Paths, and VFS Locking

- VFS `xSync` renews a held lease before flushing dirty pages, maps a renewal failure to `SQLITE_IOERR_FSYNC`, and later renews periodically in the sequential fallback every 50 ranges (`src/sqlite_objs_vfs.c:1223-1245`, `src/sqlite_objs_vfs.c:1888-1894`, `src/sqlite_objs_vfs.c:2073-2081`).
- VFS `xSync` uses `page_blob_write_batch` when the ops table provides it, passing the held lease ID and returning `SQLITE_IOERR_FSYNC` on non-OK Azure batch results except precondition conflicts (`src/sqlite_objs_vfs.c:1997-2044`).
- The VFS lock path acquires a lease for `RESERVED`, `PENDING`, or `EXCLUSIVE`, stores lease metadata, revalidates after acquisition, and releases the just-acquired lease if revalidation fails (`src/sqlite_objs_vfs.c:2426-2483`).
- `xUnlock` releases the lease when dropping below `RESERVED`, increments the lease-release metric, clears local lease state, and documents release errors as best-effort ignored errors (`src/sqlite_objs_vfs.c:2490-2522`).
- Production `az_page_blob_write_batch` locks `c->mutex` for the whole multi-range write operation to protect the persistent CURLM multi handle (`src/azure_client.c:1919-1923`, `src/azure_client.c:1949-1958`).
- The batch event loop renews the lease every `BATCH_LEASE_RENEWAL_SEC` seconds while still inside the batch mutex, and lease loss returns `AZURE_ERR_LEASE_EXPIRED` after cleanup/unlock (`src/azure_client.c:1668-1675`, `src/azure_client.c:2037-2058`, `src/azure_client.c:2125-2138`).
- `az_lease_renew` calls `execute_with_retry`, which calls `execute_single`; `execute_single` locks `client->mutex` to protect the shared CURL easy handle (`src/azure_client.c:1562-1591`, `src/azure_client.c:629-658`, `src/azure_client.c:311-327`).
- A nearby block-upload path explicitly unlocks before calling `execute_with_retry` because that helper locks internally (`src/azure_client.c:1395-1413`).
- Batch write setup handles initial `done` allocation failure and multi-handle failure with unlocks, but per-attempt `reqs = calloc(...)` failure frees `done`, sets `AZURE_ERR_NOMEM`, and returns without an unlock in the observed branch (`src/azure_client.c:1936-1958`, `src/azure_client.c:1989-1995`).
- Multiple unlock call sites do not inspect the `pthread_mutex_unlock` return value; examples include batch write success/failure exits and container-create error/success/cleanup exits (`src/azure_client.c:2131-2138`, `src/azure_client.c:2158-2175`, `src/azure_client.c:3160-3225`, `src/azure_client.c:3273-3317`).
- Existing lease-adjacent tests cover lease conflict details, lease mismatch, rapid acquire/release cycles, transient lease-acquire recovery, and integration lease break/reacquire behavior (`test/test_azure_client.c:202-229`, `test/test_azure_client.c:611-626`, `test/test_chaos.c:464-488`, `test/test_integration.c:546-567`).

### 5. True Partial-Write and Crash Test Surfaces

- Test sync hooks are compiled under `SQLITE_OBJS_TEST`, are described as hooks "called at the start of critical xSync operations", and return nonzero to abort with `SQLITE_IOERR_FSYNC` (`src/sqlite_objs_vfs.c:48-72`).
- Hook call sites are before WAL upload, before journal upload, before page-blob resize, before batch page write, and before sequential page write (`src/sqlite_objs_vfs.c:1773-1779`, `src/sqlite_objs_vfs.c:1832-1840`, `src/sqlite_objs_vfs.c:1901-1906`, `src/sqlite_objs_vfs.c:1998-2006`, `src/sqlite_objs_vfs.c:2060-2067`).
- `test/test_integration.c` crash hook context counts hook invocations and marks an injection when the target call is reached (`test/test_integration.c:4002-4028`).
- Crash tests fork child processes, set pre-operation hooks, execute transactions, assert the child exits after the injected failure, then reopen and verify integrity/row expectations (`test/test_integration.c:4045-4126`, `test/test_integration.c:4129-4199`, `test/test_integration.c:4202-4260`, `test/test_integration.c:4303-4365`, `test/test_integration.c:4521-4626`).
- Mock block-blob upload replaces entire blob content, mock blob delete removes entries, and helper accessors expose block-blob size/data and blob existence for tests that need to pre-create or inspect remote state (`test/mock_azure_ops.c:496-532`, `test/mock_azure_ops.c:650-681`, `test/mock_azure_ops.c:1125-1143`).
- Representative test insertion surfaces already present in the tree are VFS unit tests (`test/test_vfs.c`), WAL unit tests (`test/test_wal.c`), integration crash/property tests (`test/test_integration.c`), and mock Azure failure-injection helpers (`test/mock_azure_ops.c:1022-1061`).

### 6. Rust Safe Registration, Global C VFS State, Config Validation, MSRV, Feature-Gated Pragmas, and Download Count

- Safe Rust registration methods are `SqliteObjsVfs::register`, `SqliteObjsVfs::register_with_config`, and `SqliteObjsVfs::register_uri`; each calls the corresponding C FFI registration function inside an `unsafe` block and converts non-OK SQLite return codes into Rust errors (`rust/sqlite-objs/src/lib.rs:194-204`, `rust/sqlite-objs/src/lib.rs:217-273`, `rust/sqlite-objs/src/lib.rs:297-307`).
- The raw FFI crate exposes `sqlite_objs_vfs_register`, `sqlite_objs_vfs_register_with_config`, `sqlite_objs_vfs_register_with_ops`, and `sqlite_objs_vfs_register_uri` as unsafe extern functions, with URI-mode docs listing per-database URI credential parameters (`rust/sqlite-objs-sys/src/lib.rs:39-86`).
- The C VFS has one static `g_vfsData` and one static `g_sqliteObjsVfs`; both config and URI registration paths `memset` global state and assign `g_sqliteObjsVfs.pAppData = &g_vfsData` before registering with SQLite (`src/sqlite_objs_vfs.c:3774-3851`, `src/sqlite_objs_vfs.c:3895-3916`).
- Environment-based C registration rejects missing account/container and missing SAS/key before calling config registration (`src/sqlite_objs_vfs.c:3855-3874`).
- URI config parsing uses presence of `azure_account` to decide whether URI config exists, then passes `azure_container`, `azure_sas`, `azure_key`, and `azure_endpoint` through as SQLite URI parameters (`src/sqlite_objs_vfs.c:2889-2904`).
- Production `azure_client_create` validates non-empty account, non-empty container, and at least one non-empty SAS token or shared key (`src/azure_client.c:2977-3014`).
- Rust `register_with_config` validates only embedded NUL bytes in account/container/SAS/key/endpoint before constructing the C config and calling FFI; Rust tests cover URI registration, explicit config FFI invocation, and an embedded-NUL invalid config case (`rust/sqlite-objs/src/lib.rs:217-273`, `rust/sqlite-objs/src/lib.rs:511-543`).
- `UriBuilder::new` stores database/account/container strings and `build` emits `azure_account` and `azure_container`; tests cover encoding, SAS/key precedence, cache parameters, and percent-encoding, including empty percent-encoding (`rust/sqlite-objs/src/lib.rs:333-365`, `rust/sqlite-objs/src/lib.rs:436-477`, `rust/sqlite-objs/src/lib.rs:546-735`).
- Workspace MSRV is declared as Rust `1.70`, and crate manifests inherit `rust-version.workspace`; Rust README also documents Rust 1.70 or later (`rust/Cargo.toml:5-11`, `rust/sqlite-objs/Cargo.toml:1-10`, `rust/README.md:12-22`).
- `pragmas` is gated behind the `rusqlite` feature; Cargo features expose `rusqlite` and `bin-deps`, and crate docs/README describe enabling `features = ["rusqlite"]` for helper APIs (`rust/sqlite-objs/src/lib.rs:88-91`, `rust/sqlite-objs/Cargo.toml:24-30`, `rust/sqlite-objs/README.md:95-115`).
- The Rust pragma helpers use raw-reference syntax for file-control arguments in `get_stats` and `get_download_count` (`rust/sqlite-objs/src/pragmas.rs:57-75`, `rust/sqlite-objs/src/pragmas.rs:118-144`).
- C defines download-count file-control opcode 200, and `xFileControl` returns `p->nDownloads` for that opcode (`src/sqlite_objs.h:120-125`, `src/sqlite_objs_vfs.c:2723-2727`).
- The Rust library helper `pragmas::get_download_count` checks the `sqlite3_file_control` return code and returns `SqliteObjsError::Sqlite` on failure, while the ignored integration-test helper in `vfs_integration.rs` calls `sqlite3_file_control` and returns the default/local count without checking the return code (`rust/sqlite-objs/src/pragmas.rs:118-144`, `rust/sqlite-objs/tests/vfs_integration.rs:95-114`).
- Ignored Rust Azure integration tests use the local download-count helper for cache-reuse assertions (`rust/sqlite-objs/tests/vfs_integration.rs:1033-1055`, `rust/sqlite-objs/tests/vfs_integration.rs:1057-1091`, `rust/sqlite-objs/tests/vfs_integration.rs:1136-1178`).

### 7. Test/Release Gates, Azurite Mode, Property Tests, Docs, and Workflows

- The integration runner starts Azurite with both `--skipApiVersionCheck` and `--loose` in silent and non-silent branches (`test/run-integration.sh:78-92`).
- Research docs describe Azurite `--loose`/`--skipApiVersionCheck` as relaxed validation and recommend strict mode for tests (`research/testing-strategy.md:72-82`).
- Property tests tolerate `SQLITE_ERROR` for generated insert/update/delete operations in basic, multi-seed, and transaction-heavy loops; the suite is registered as "Property-Based: Phase 3 Deterministic Randomized Testing" (`test/test_integration.c:5128-5155`, `test/test_integration.c:5228-5246`, `test/test_integration.c:5352-5370`, `test/test_integration.c:5531-5536`).
- `Makefile` includes test/validation targets for unit tests, integration tests, stress variants, extended property testing, TCL, sanitizer, and coverage (`Makefile:197-238`, `Makefile:242-254`, `Makefile:299-314`).
- The release gate builds C and Rust, performs symbol validation, runs C unit/integration/sanitizer tests, Rust tests, optional TCL/stress/file perf tests, and optional Azure tests (`scripts/release-gate.sh:143-194`, `scripts/release-gate.sh:216-232`, `scripts/release-gate.sh:241-249`, `scripts/release-gate.sh:258-288`, `scripts/release-gate.sh:297-324`).
- Release gate integration and Azure gates can be skipped when Azurite or Azure credentials are unavailable; skips are recorded through `skip_gate` call sites (`scripts/release-gate.sh:219-225`, `scripts/release-gate.sh:317-324`).
- README documents `make all-production` for production builds and benchmark/demo docs/scripts also reference that target (`README.md:44-48`, `README.md:120-135`, `benchmark/README.md:23-35`, `benchmark/README.md:180-219`, `benchmark/tpcc/README.md:37-43`, `demo/azure-demo.sh:71-79`).
- The Makefile default target and help list show `all`, `shell`, `benchmarks`, `tpcc`, tests, sanitizer, coverage, and TCL targets; the shown target lists do not include `all-production` (`Makefile:103-108`, `Makefile:299-314`).
- README limitations state "WAL mode not supported", while VFS/test sources contain WAL support via exclusive locking and WAL unit tests (`README.md:293-299`, `src/sqlite_objs_vfs.c:313-320`, `test/test_wal.c:1-14`).
- `squad-release.yml`, `squad-insider-release.yml`, `squad-preview.yml`, and `squad-docs.yml` contain project-type-not-detected TODO/echo placeholder commands for build/test/release/docs steps (`.github/workflows/squad-release.yml:1-35`, `.github/workflows/squad-insider-release.yml:1-35`, `.github/workflows/squad-preview.yml:1-30`, `.github/workflows/squad-docs.yml:1-28`).
- `squad-ci.yml` installs toolchains/Azurite and runs `./scripts/release-gate.sh`; `squad-promote.yml` uses `package.json` and `CHANGELOG.md` version checks before merging preview to main and states that `squad-release.yml` will tag/publish (`.github/workflows/squad-ci.yml:23-53`, `.github/workflows/squad-promote.yml:86-116`).

### 8. Existing Validation Targets and Tests to Update/Extend

- C unit-test runner includes `test_vfs.c`, `test_azure_client.c`, `test_coalesce.c`, `test_wal.c`, `test_uri.c`, and `test_chaos.c`; `main` calls each suite runner in order (`test/test_main.c:19-30`, `test/test_main.c:43-50`).
- The Makefile builds `test_main` with `ENABLE_VFS_INTEGRATION`, `ENABLE_WAL_TESTS`, and `ENABLE_AZURE_CLIENT_TESTS`, and `make test-unit` runs `build/test_main` (`Makefile:197-204`).
- VFS unit tests already cover rollback-journal creation/deletion, DELETE/WAL PRAGMAs, journal upload failure, WAL mode handling, lazy journal handling, and lease renewal during long writes (`test/test_vfs.c:1507-1530`, `test/test_vfs.c:1659-1685`, `test/test_vfs.c:1745-1768`, `test/test_vfs.c:1985-2048`, `test/test_vfs.c:3466-3481`, `test/test_vfs.c:3578-3595`).
- WAL unit tests already cover checkpoint-to-main-db writes, checkpoint reset/delete calls, WAL upload failures, WAL recovery download failures, and parallel-upload failures (`test/test_wal.c:431-510`, `test/test_wal.c:517-596`, `test/test_wal.c:825-887`, `test/test_wal.c:1280-1310`).
- Integration tests already contain crash/recovery suites, invariant crash stress, and deterministic randomized property suites (`test/test_integration.c:4045-4126`, `test/test_integration.c:4129-4199`, `test/test_integration.c:4202-4260`, `test/test_integration.c:4303-4365`, `test/test_integration.c:4521-4626`, `test/test_integration.c:5531-5536`).
- Rust unit tests in `rust/sqlite-objs/src/lib.rs` cover registration/config and URI-builder behavior; ignored Rust Azure integration tests cover cache reuse/download-count behaviors (`rust/sqlite-objs/src/lib.rs:511-735`, `rust/sqlite-objs/tests/vfs_integration.rs:1033-1178`).
- Documentation and gate surfaces to update/verify are the README/build sections, benchmark/demo docs, test docs index, release gate, integration runner, and GitHub workflows (`README.md:36-99`, `benchmark/README.md:23-35`, `demo/README.md:37-51`, `TEST_DOCS_INDEX.md:72-98`, `scripts/release-gate.sh:5-19`, `test/run-integration.sh:78-92`, `.github/workflows/squad-release.yml:1-35`).

## Code References

- `src/sqlite_objs_vfs.c:247-310` - Journal existence cache structure and helpers.
- `src/sqlite_objs_vfs.c:3416-3466` - Journal `xOpen` cache/HEAD/download recovery path.
- `src/sqlite_objs_vfs.c:3598-3656` - Journal `xAccess` cache and `blob_exists` fallback.
- `src/sqlite_objs_vfs.c:1617-1644` - WAL and journal `xTruncate` behavior.
- `src/sqlite_objs_vfs.c:1763-1876` - WAL and journal `xSync` upload/error/cache updates.
- `src/sqlite_objs_vfs.c:2426-2522` - VFS lease acquire/revalidate/release/unlock behavior.
- `src/azure_client.c:1919-2175` - Production batch page write mutex, renewal, allocation, cleanup, and return paths.
- `src/azure_client.c:1562-1591` and `src/azure_client.c:311-327` - Lease renewal via `execute_with_retry`/`execute_single` mutex lock path.
- `test/test_integration.c:4002-4626` - Crash hook context and crash/recovery integration tests.
- `rust/sqlite-objs/src/lib.rs:194-307` - Rust safe registration methods.
- `src/sqlite_objs_vfs.c:3774-3916` - Global C VFS data and registration reset paths.
- `rust/sqlite-objs/src/pragmas.rs:57-144` - Feature-gated pragma/file-control helpers.
- `scripts/release-gate.sh:143-324` - Release-gate validation stages and optional skips.
- `test/run-integration.sh:78-92` - Azurite startup flags.

## Architecture Documentation

- **C VFS storage split**: MAIN_DB uses Azure page blobs and local cache files; MAIN_JOURNAL and WAL use in-memory buffers plus Azure block blobs on sync (`src/sqlite_objs_vfs.c:1440-1457`, `src/sqlite_objs_vfs.c:1531-1558`, `src/sqlite_objs_vfs.c:1753-1757`).
- **Global VFS design**: A single static VFS object points at a single mutable global state block; registration resets and repopulates that block before calling `sqlite3_vfs_register` (`src/sqlite_objs_vfs.c:3774-3851`, `src/sqlite_objs_vfs.c:3895-3916`).
- **Azure ops abstraction**: Tests use `azure_ops_t` mocks and production uses `azure_client_get_ops`; VFS resolves per-file URI clients or global ops on open (`src/sqlite_objs_vfs.c:2939-2958`, `src/sqlite_objs_vfs.c:3818-3842`).
- **Test architecture**: C tests use a direct-inclusion unit runner and mock Azure operation failure/call-count helpers; integration tests use Azurite and forked child processes for crash simulations (`test/test_main.c:19-30`, `test/mock_azure_ops.c:1022-1061`, `test/test_integration.c:4002-4028`).
- **Rust architecture**: `sqlite-objs-sys` exposes raw FFI and constants; `sqlite-objs` wraps registration, URI building, metrics, and feature-gated `rusqlite` pragma helpers (`rust/sqlite-objs-sys/src/lib.rs:1-6`, `rust/sqlite-objs/src/lib.rs:1-13`, `rust/sqlite-objs/src/lib.rs:88-91`).
- **Validation architecture**: Local validation is Makefile-driven for C and Cargo-driven for Rust; CI delegates to `scripts/release-gate.sh`, and extended/live-Azure gates are opt-in (`Makefile:197-238`, `rust/README.md:24-47`, `.github/workflows/squad-ci.yml:51-53`, `scripts/release-gate.sh:5-19`).

## Open Questions

- No user-blocking open questions from research. Planning should choose exact remediation behavior for unsupported or partially supported modes (for example non-WAL `TRUNCATE`/`PERSIST` journal semantics) based on the specification's data-safety acceptance criteria.
