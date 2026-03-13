# Test Matrix for sqlite-objs

> Comprehensive test coverage matrix for the sqlite-objs SQLite VFS backed by Azure Blob Storage.
>
> **Legend:** ✅ Covered | ⚠️ Partial | ❌ Missing
>
> **Current totals:** 148 unit tests + 10 integration tests = 158 tests

---

## Table of Contents

1. [VFS Layer Tests](#1-vfs-layer-tests)
2. [Azure Client Tests](#2-azure-client-tests)
3. [Integration Tests](#3-integration-tests)
4. [State Transitions](#4-state-transitions)
5. [Security-Critical Paths](#5-security-critical-paths)
6. [Coverage Gaps & Priorities](#6-coverage-gaps--priorities)

---

## 1. VFS Layer Tests

Source: `src/sqlite_objs_vfs.c` (1,167 lines)
Tests: `test/test_vfs.c` (118 tests)

### 1.1 File Operations

| Operation | Test Case | Status | Notes |
|-----------|-----------|--------|-------|
| **xOpen** | Open main database (page blob) | ✅ | `vfs_open_close_basic` |
| | Open journal file (block blob) | ✅ | `journal_roundtrip` (integration) |
| | Open with `SQLITE_OPEN_CREATE` flag | ✅ | Creates page blob if not exists |
| | Open existing database (re-download) | ✅ | Reads existing page blob data |
| | Open with invalid VFS name | ❌ | **P2** — error path |
| | Open with NULL filename | ❌ | **P2** — edge case |
| | Open temp file (delegates to default VFS) | ❌ | **P3** — delegation path |
| | Open WAL file (should fail/reject) | ❌ | **P2** — `xFileControl` rejects WAL mode |
| | Open when `page_blob_create` fails | ✅ | Failure injection via mock |
| | Open when `page_blob_read` fails on existing blob | ⚠️ | **P1** — partial coverage |
| | Memory allocation failure during open | ❌ | **P1** — `malloc` returns NULL |
| **xClose** | Close with clean pages | ✅ | `vfs_open_close_basic` |
| | Close with dirty pages (flush) | ✅ | Write-then-close tests |
| | Close when flush fails (Azure error) | ❌ | **P1** — error during close |
| | Close when lease release fails | ❌ | **P2** — error path |
| | Double close | ❌ | **P2** — defensive programming |
| **xRead** | Read within bounds | ✅ | `vfs_read_write_basic` |
| | Read at offset | ✅ | Multiple offset tests |
| | Read past EOF (zero-fill) | ✅ | `page_blob_read_past_eof_zero_fills` |
| | Read with offset past EOF | ✅ | `page_blob_read_offset_past_eof` |
| | Read zero bytes | ❌ | **P3** — edge case |
| | Read from closed file | ❌ | **P3** — error path |
| **xWrite** | Write within existing bounds | ✅ | `vfs_read_write_basic` |
| | Write past EOF (grows buffer) | ✅ | `page_blob_write_grows_blob` |
| | Write at offset | ✅ | `page_blob_write_at_offset` |
| | Write multiple pages | ✅ | `page_blob_write_multiple_pages` |
| | Overwrite existing data | ✅ | `page_blob_write_overwrite` |
| | Write zero bytes | ❌ | **P3** — edge case |
| | Write when realloc fails | ❌ | **P1** — allocation failure |
| | Write marks pages dirty | ✅ | Dirty bitmap tests |
| **xSync** | Sync with dirty pages (flush to Azure) | ✅ | Implicit in write-read roundtrips |
| | Sync with no dirty pages (no-op) | ⚠️ | **P2** — not explicitly tested |
| | Sync when `page_blob_write` fails | ❌ | **P1** — Azure error during flush |
| | Sync journal file (block blob upload) | ✅ | `journal_roundtrip` (integration) |
| | Sync when `block_blob_upload` fails | ❌ | **P1** — journal upload failure |
| | Sync with lease renewal needed | ❌ | **P1** — time-dependent |
| **xTruncate** | Truncate to smaller size | ✅ | `page_blob_resize_shrink` |
| | Truncate to zero | ✅ | `page_blob_resize_to_zero` |
| | Truncate to larger size | ✅ | `page_blob_resize_grow` |
| | Truncate preserves data | ✅ | `page_blob_resize_preserves_data` |
| | Truncate when `page_blob_resize` fails | ❌ | **P2** — Azure error |
| **xFileSize** | Size of empty file | ✅ | After create with zero size |
| | Size after writes | ✅ | Various write tests |
| | Size after truncate | ✅ | Resize tests |
| **xDelete** | Delete existing blob | ✅ | `blob_delete_page_blob` |
| | Delete non-existent blob | ✅ | `blob_delete_nonexistent_fails` |
| | Delete journal file | ⚠️ | **P2** — block blob delete |
| **xAccess** | Check existing blob | ✅ | `blob_exists_page_blob` |
| | Check non-existent blob | ✅ | `blob_exists_nonexistent` |
| **xFullPathname** | Basic path copy | ⚠️ | **P2** — not directly tested |
| | Long path handling | ❌ | **P2** — buffer boundary |
| **xFileControl** | WAL mode rejection | ❌ | **P1** — security-relevant |
| | `SQLITE_FCNTL_VFSNAME` | ❌ | **P3** — informational |

### 1.2 Locking Operations

| Operation | Test Case | Status | Notes |
|-----------|-----------|--------|-------|
| **xLock** | NONE → SHARED (no-op) | ✅ | Implicit in all read tests |
| | SHARED → RESERVED (acquire lease) | ✅ | `lease_acquire_basic` |
| | RESERVED → EXCLUSIVE (reuse lease) | ⚠️ | **P1** — implicit, not directly tested |
| | Lock when lease already held by other | ✅ | `lease_acquire_already_leased_fails` |
| | Lock when blob doesn't exist | ✅ | `lease_acquire_nonexistent_fails` |
| | Lock when network fails | ❌ | **P1** — transient error handling |
| **xUnlock** | EXCLUSIVE → SHARED (release lease) | ✅ | `lease_release_basic` |
| | SHARED → NONE (no-op) | ✅ | Implicit in close |
| | Unlock with wrong lease ID | ✅ | `lease_release_wrong_id_fails` |
| | Unlock when release fails | ❌ | **P2** — error path |
| **xCheckReservedLock** | Check when locally locked | ⚠️ | **P2** — not directly tested |
| | Check when remotely locked | ❌ | **P2** — requires lease state query |
| | Check when not locked | ⚠️ | **P2** — not directly tested |

### 1.3 Device Characteristics

| Test Case | Status | Notes |
|-----------|--------|-------|
| Sector size returns 512 | ❌ | **P3** — trivial to add |
| Device characteristics flags correct | ❌ | **P3** — trivial to add |

### 1.4 Journal File Handling

| Test Case | Status | Notes |
|-----------|--------|-------|
| Journal creation (block blob) | ✅ | `journal_roundtrip` (integration) |
| Journal write + sync (upload) | ✅ | Integration test |
| Journal read (download) | ✅ | Integration test |
| Journal delete after commit | ⚠️ | **P2** — implicit |
| Journal hot restart (crash recovery) | ❌ | **P1** — critical for data safety |
| Journal with large transaction | ❌ | **P2** — size boundaries |

### 1.5 Error Path Coverage

| Error Scenario | Status | Notes |
|----------------|--------|-------|
| `malloc` failure in xOpen | ❌ | **P1** — OOM handling |
| `malloc` failure in xWrite (buffer growth) | ❌ | **P1** — OOM handling |
| `realloc` failure in dirty bitmap | ❌ | **P1** — OOM handling |
| Azure 500 during xSync flush | ❌ | **P1** — transient error |
| Azure 409 (lease conflict) during lock | ✅ | Mock failure injection |
| Azure 404 during initial read | ✅ | Creates new blob |
| Azure network timeout during sync | ❌ | **P1** — network failure |
| Lease expiration during long transaction | ❌ | **P1** — critical edge case |
| Dirty page flush partial failure | ❌ | **P1** — some pages written, some fail |

---

## 2. Azure Client Tests

Source: `src/azure_client.c` (1,272 lines), `src/azure_auth.c` (285 lines), `src/azure_error.c` (203 lines)
Tests: `test/test_azure_client.c` (39 tests, but most VFS tests also exercise client via mock)

### 2.1 Page Blob Operations

| Operation | Test Case | Status | Notes |
|-----------|-----------|--------|-------|
| **Create** | Basic creation | ✅ | `page_blob_create_basic` |
| | Zero-size blob | ✅ | `page_blob_create_zero_size` |
| | Unaligned size fails | ✅ | `page_blob_create_unaligned_fails` |
| | Already-exists resizes | ✅ | `page_blob_create_already_exists_resizes` |
| | Large blob (>1MB) | ✅ | `page_blob_create_large` |
| | Max size (TB range) | ❌ | **P3** — boundary test |
| **Write** | Basic write | ✅ | `page_blob_write_basic` |
| | Write at offset | ✅ | `page_blob_write_at_offset` |
| | Unaligned offset fails | ✅ | `page_blob_write_unaligned_offset_fails` |
| | Unaligned length fails | ✅ | `page_blob_write_unaligned_length_fails` |
| | Write to non-existent blob | ✅ | `page_blob_write_nonexistent_fails` |
| | Write grows blob | ✅ | `page_blob_write_grows_blob` |
| | Write with lease ID | ⚠️ | **P2** — used but not directly validated |
| | Write with expired lease | ❌ | **P1** — should return error |
| | Write with wrong lease ID | ❌ | **P1** — should return 412 |
| **Read** | Basic read | ✅ | `page_blob_read_basic` |
| | Read at offset | ✅ | `page_blob_read_at_offset` |
| | Read non-existent blob | ✅ | `page_blob_read_nonexistent_fails` |
| | Read past EOF (zero-fill) | ✅ | `page_blob_read_past_eof_zero_fills` |
| | Read entire blob | ✅ | Various roundtrip tests |
| **Resize** | Grow | ✅ | `page_blob_resize_grow` |
| | Shrink | ✅ | `page_blob_resize_shrink` |
| | Unaligned size fails | ✅ | `page_blob_resize_unaligned_fails` |
| | Non-existent blob fails | ✅ | `page_blob_resize_nonexistent_fails` |
| | Preserves data | ✅ | `page_blob_resize_preserves_data` |
| | Resize to zero | ✅ | `page_blob_resize_to_zero` |

### 2.2 Block Blob Operations

| Operation | Test Case | Status | Notes |
|-----------|-----------|--------|-------|
| **Upload** | Basic upload | ✅ | `block_blob_upload_basic` |
| | Upload replaces existing | ✅ | `block_blob_upload_replaces` |
| | Upload empty blob | ✅ | `block_blob_upload_empty` |
| | Upload large blob (>4MB) | ❌ | **P2** — size boundary |
| **Download** | Basic download | ✅ | `block_blob_download_basic` |
| | Download non-existent | ✅ | `block_blob_download_nonexistent_fails` |
| | Roundtrip | ✅ | `block_blob_upload_download_roundtrip` |

### 2.3 Lease Operations

| Operation | Test Case | Status | Notes |
|-----------|-----------|--------|-------|
| **Acquire** | Basic acquire | ✅ | `lease_acquire_basic` |
| | Already leased fails | ✅ | `lease_acquire_already_leased_fails` |
| | Non-existent blob fails | ✅ | `lease_acquire_nonexistent_fails` |
| | Duration validation | ❌ | **P2** — 15-60s range check |
| **Renew** | Basic renew | ✅ | `lease_renew_basic` |
| | Wrong ID fails | ✅ | `lease_renew_wrong_id_fails` |
| | Not leased fails | ✅ | `lease_renew_not_leased_fails` |
| **Release** | Basic release | ✅ | `lease_release_basic` |
| | Wrong ID fails | ✅ | `lease_release_wrong_id_fails` |
| | Not leased fails | ✅ | `lease_release_not_leased_fails` |
| **Break** | Immediate break | ✅ | `lease_break_immediate` |
| | Break with period | ⚠️ | **P2** — mock may not simulate time |
| | Break non-leased blob | ⚠️ | **P3** |

### 2.4 Error Handling

| Test Case | Status | Notes |
|-----------|--------|-------|
| HTTP 500 → `AZURE_ERR_SERVER` | ✅ | `http_500_is_server_error` |
| HTTP 429 → `AZURE_ERR_THROTTLED` | ✅ | `http_429_is_throttle_error` |
| HTTP 401 → `AZURE_ERR_AUTH` | ✅ | `http_401_is_auth_error` |
| HTTP 404 → `AZURE_ERR_NOT_FOUND` | ✅ | `http_404_is_not_found` |
| HTTP 409 → `AZURE_ERR_CONFLICT` | ✅ | `http_409_is_conflict` |
| HTTP 412 → `AZURE_ERR_PRECONDITION` | ❌ | **P2** — not explicitly tested |
| Network error | ✅ | `network_error` |
| Timeout error | ✅ | `timeout_error` |
| Error detail: HTTP status | ✅ | `error_detail_http_status` |
| Error detail: error code string | ✅ | `error_detail_error_code_string` |
| Error detail: error message | ✅ | `error_detail_error_message` |
| Error detail: request ID | ✅ | `error_detail_request_id` |
| Error detail: lease conflict | ✅ | `error_detail_lease_conflict` |
| Error detail: lease mismatch | ✅ | `error_detail_lease_mismatch` |
| Transient errors are retryable | ✅ | `transient_errors_are_retryable` |
| Fatal errors not retried | ✅ | `fatal_errors_distinct_from_transient` |
| Retry backoff timing | ❌ | **P2** — not tested in mock layer |
| Retry-After header respected | ❌ | **P2** — production client only |
| Max retries exhausted | ❌ | **P2** — production client only |

### 2.5 Authentication

| Test Case | Status | Notes |
|-----------|--------|-------|
| HMAC-SHA256 known vector | ✅ | `auth_hmac_sha256_known_vector` |
| SAS token append | ✅ | `auth_sas_token_append` |
| SAS token with existing params | ✅ | `auth_sas_token_with_existing_params` |
| Missing credentials error | ✅ | `auth_missing_credentials` |
| Shared Key header format | ✅ | `auth_shared_key_header_format` |
| Expired SAS token | ❌ | **P1** — security-relevant |
| Malformed SAS token | ❌ | **P2** — input validation |
| Malformed account key (bad Base64) | ❌ | **P2** — input validation |
| Empty account key | ❌ | **P2** — edge case |

### 2.6 Buffer Management

| Test Case | Status | Notes |
|-----------|--------|-------|
| Alloc and free | ✅ | `buffer_alloc_free` |
| Double free safety | ✅ | `buffer_double_free_safe` |
| Growth on read | ✅ | `buffer_growth_on_read` |
| Preserves capacity | ✅ | `buffer_preserves_capacity` |
| Growth overflow (huge size) | ❌ | **P1** — integer overflow risk |
| Zero-size buffer | ❌ | **P3** — edge case |

### 2.7 XML Error Parsing

| Test Case | Status | Notes |
|-----------|--------|-------|
| Basic XML parse | ✅ | `xml_error_parse_basic` |
| Lease conflict XML | ✅ | `xml_error_parse_lease_conflict` |
| Empty XML | ✅ | `xml_error_parse_empty` |
| Malformed XML | ✅ | `xml_error_parse_malformed` |
| Very long XML values | ❌ | **P1** — buffer overflow risk |
| Nested/escaped XML | ❌ | **P2** — parsing edge case |
| Binary data in XML | ❌ | **P2** — fuzzing target |
| NULL input | ❌ | **P2** — defensive programming |

### 2.8 URL Construction

| Test Case | Status | Notes |
|-----------|--------|-------|
| Basic URL format | ⚠️ | Tested implicitly via integration |
| Special characters in blob name | ❌ | **P1** — injection risk |
| Very long blob name | ❌ | **P1** — buffer overflow risk |
| Empty blob name | ❌ | **P2** — input validation |
| Custom endpoint (Azurite) | ✅ | Integration tests use Azurite |

---

## 3. Integration Tests

Source: `test/test_integration.c` (10 tests against Azurite)

### 3.1 SQLite Operations

| Operation | Test Case | Status | Notes |
|-----------|-----------|--------|-------|
| CREATE TABLE | ✅ | `vfs_roundtrip` |
| INSERT | ✅ | `vfs_roundtrip` |
| SELECT | ✅ | `vfs_roundtrip` |
| UPDATE | ❌ | **P1** — basic CRUD |
| DELETE (SQL) | ❌ | **P1** — basic CRUD |
| Multi-table operations | ❌ | **P2** — complexity |
| Prepared statements | ❌ | **P3** — SQLite feature |
| FTS5 operations | ❌ | **P3** — enabled feature, untested |
| JSON1 operations | ❌ | **P3** — enabled feature, untested |

### 3.2 Transaction Handling

| Test Case | Status | Notes |
|-----------|--------|-------|
| Implicit transaction (auto-commit) | ✅ | `vfs_roundtrip` |
| Explicit BEGIN/COMMIT | ❌ | **P1** — transaction correctness |
| BEGIN/ROLLBACK | ❌ | **P1** — transaction correctness |
| Nested savepoints | ❌ | **P2** — SQLite feature |
| Large transaction (many rows) | ❌ | **P1** — buffer/page management |
| Transaction with multiple tables | ❌ | **P2** — complexity |

### 3.3 Crash Recovery

| Test Case | Status | Notes |
|-----------|--------|-------|
| Journal hot restart | ❌ | **P1** — data safety critical |
| Journal file left after crash | ❌ | **P1** — SQLite recovery mechanism |
| Incomplete sync (partial flush) | ❌ | **P1** — data integrity |
| Lease expiry during transaction | ❌ | **P1** — concurrent access |
| Database corruption detection | ❌ | **P2** — `PRAGMA integrity_check` |

### 3.4 Large Database Handling

| Test Case | Status | Notes |
|-----------|--------|-------|
| Database > 1 page (>4096 bytes) | ✅ | Implicit in most tests |
| Database > 1 MB | ❌ | **P2** — many dirty pages |
| Database > 10 MB | ❌ | **P3** — performance test |
| Many small writes | ❌ | **P2** — dirty bitmap stress |
| Sequential page writes | ✅ | `write_read_many_pages_sequentially` |

### 3.5 Concurrent Access

| Test Case | Status | Notes |
|-----------|--------|-------|
| Two connections, one writes | ❌ | **P1** — lease-based locking |
| Writer blocks reader upgrade | ❌ | **P1** — SQLITE_BUSY behavior |
| Lease conflict returns SQLITE_BUSY | ✅ | `lease_conflict` (integration) |
| Lease break allows new writer | ✅ | `lease_break` (integration) |
| Multiple readers simultaneous | ❌ | **P2** — no lease needed |

---

## 4. State Transitions

### 4.1 SQLite Lock State Machine

```
NONE ──→ SHARED ──→ RESERVED ──→ PENDING ──→ EXCLUSIVE
  ↑         │          │            │            │
  └─────────┴──────────┴────────────┴────────────┘
                    (unlock)
```

| Transition | Test Case | Status | Notes |
|------------|-----------|--------|-------|
| NONE → SHARED | Implicit in all reads | ✅ | No Azure action |
| SHARED → RESERVED | Lease acquire | ✅ | `lease_acquire_basic` |
| RESERVED → PENDING | Reuse lease | ⚠️ | Not directly tested |
| PENDING → EXCLUSIVE | Reuse lease | ⚠️ | Not directly tested |
| EXCLUSIVE → SHARED | Release lease | ✅ | `lease_release_basic` |
| SHARED → NONE | No Azure action | ✅ | Implicit in close |
| RESERVED → SHARED | Release lease | ⚠️ | Not directly tested |
| SHARED → RESERVED (conflict) | Returns BUSY | ✅ | `lease_conflict` |
| Invalid transition (NONE → EXCLUSIVE) | ❌ | **P2** — defensive check |

### 4.2 Azure Lease State Machine

```
AVAILABLE ──→ LEASED ──→ BREAKING ──→ AVAILABLE
    │            │            │
    │            └──(renew)───┘
    │            └──(release)─→ AVAILABLE
    └──(acquire)─→ LEASED
```

| Transition | Test Case | Status | Notes |
|------------|-----------|--------|-------|
| AVAILABLE → LEASED | `lease_acquire` | ✅ | `lease_acquire_basic` |
| LEASED → AVAILABLE (release) | `lease_release` | ✅ | `lease_release_basic` |
| LEASED → LEASED (renew) | `lease_renew` | ✅ | `lease_renew_basic` |
| LEASED → BREAKING (break) | `lease_break` | ✅ | `lease_break_immediate` |
| BREAKING → AVAILABLE | After break period | ⚠️ | Mock may not simulate time |
| LEASED → AVAILABLE (expiry) | Auto-expire after 30s | ❌ | **P1** — time-dependent |
| AVAILABLE → AVAILABLE (release) | Error case | ✅ | `lease_release_not_leased_fails` |
| AVAILABLE → AVAILABLE (renew) | Error case | ✅ | `lease_renew_not_leased_fails` |

### 4.3 Dirty Page Tracking

| State | Test Case | Status | Notes |
|-------|-----------|--------|-------|
| Page clean → dirty (on write) | ✅ | Write marks bitmap |
| Page dirty → clean (on flush) | ✅ | Sync clears bitmap |
| Multiple dirty pages flush | ✅ | `page_blob_write_multiple_pages` |
| Bitmap growth on new pages | ⚠️ | Implicit in write-grows tests |
| Bitmap overflow (very large file) | ❌ | **P2** — boundary test |
| No dirty pages on sync (no-op) | ❌ | **P2** — optimization path |
| Partial flush failure | ❌ | **P1** — some pages written, some fail |

---

## 5. Security-Critical Paths

### 5.1 Input Validation

| Check | Test Case | Status | Priority |
|-------|-----------|--------|----------|
| Blob name: special characters | ❌ | **P1** |
| Blob name: path traversal (`../`) | ❌ | **P1** |
| Blob name: URL-unsafe characters | ❌ | **P1** |
| Blob name: empty string | ❌ | **P2** |
| Blob name: maximum length | ❌ | **P2** |
| Container name: validation | ❌ | **P2** |
| Account name: validation | ❌ | **P2** |
| SAS token: injection characters | ❌ | **P1** |
| Endpoint URL: format validation | ❌ | **P2** |

### 5.2 Buffer Boundaries

| Check | Test Case | Status | Priority |
|-------|-----------|--------|----------|
| `error_code[128]` — truncation on long input | ❌ | **P1** |
| `error_message[256]` — truncation | ❌ | **P1** |
| `request_id[64]` — truncation | ❌ | **P2** |
| Lease ID buffer (37 bytes) — exact fit | ⚠️ | **P2** |
| `azure_buffer_t` — growth overflow | ❌ | **P1** |
| URL buffer — long account/container/blob | ❌ | **P1** |
| `sqlite-objsFullPathname` — path buffer | ❌ | **P2** |

### 5.3 Integer Overflow Risks

| Check | Test Case | Status | Priority |
|-------|-----------|--------|----------|
| `offset + len` overflow in xRead | ❌ | **P1** |
| `offset + len` overflow in xWrite | ❌ | **P1** |
| `dirtyBitmapSize()` — page count overflow | ❌ | **P1** |
| `(size + 511) & ~511` — alignment rounding overflow | ❌ | **P2** |
| `nData` realloc size calculation | ❌ | **P1** |
| Retry delay calculation overflow | ❌ | **P3** |

### 5.4 Credential Handling

| Check | Test Case | Status | Priority |
|-------|-----------|--------|----------|
| Account key not in log output | ❌ | **P1** |
| SAS token not in log output | ❌ | **P1** |
| Credentials zeroed after use | ❌ | **P1** |
| Error messages don't contain credentials | ❌ | **P1** |
| Account key validated (Base64 format) | ❌ | **P2** |
| SAS token validated (basic format) | ❌ | **P2** |

---

## 6. Coverage Gaps & Priorities

### 6.1 Summary by Priority

| Priority | Category | Count | Description |
|----------|----------|-------|-------------|
| **P0** | — | 0 | All critical functionality is tested |
| **P1** | Error paths & Security | ~30 | OOM handling, Azure errors during sync, credential safety, buffer overflows, integer overflows |
| **P2** | Edge cases & Validation | ~25 | Input validation, uncommon transitions, delegation paths |
| **P3** | Completeness | ~15 | Trivial getters, rarely-used features, performance tests |

### 6.2 Top 10 Missing Tests (Priority Order)

| # | Test | Why Critical | Effort |
|---|------|-------------|--------|
| 1 | **Azure error during xSync/flush** | Data loss if dirty pages can't be written | Medium |
| 2 | **malloc/realloc failure paths** | OOM → crash if not handled | Medium |
| 3 | **Lease expiry during long transaction** | Silent data corruption risk | High |
| 4 | **Integer overflow in size calculations** | Security vulnerability (buffer overflow) | Low |
| 5 | **XML parsing with long/malicious input** | Buffer overflow in error handling | Low (fuzzing) |
| 6 | **URL injection via blob names** | HTTP request manipulation | Low |
| 7 | **Credential leak in error messages** | Information disclosure | Low |
| 8 | **Journal hot restart / crash recovery** | Data integrity fundamental | High |
| 9 | **Explicit BEGIN/COMMIT/ROLLBACK** | Transaction correctness | Medium |
| 10 | **Concurrent write/read (lease conflict)** | Multi-user scenario | Medium |

### 6.3 Coverage by Source File (Estimated)

| File | Line Coverage (Est.) | Branch Coverage (Est.) | Notes |
|------|---------------------|----------------------|-------|
| `sqlite_objs_vfs.c` | ~70% | ~50% | Error paths and delegation poorly covered |
| `azure_client.c` | ~40% | ~30% | Production client only tested via integration |
| `azure_auth.c` | ~60% | ~50% | Happy paths well tested, error paths missing |
| `azure_error.c` | ~75% | ~60% | Good coverage, needs fuzz testing |
| `azure_client_stub.c` | ~10% | ~5% | Stub — intentionally minimal |
| `mock_azure_ops.c` | ~85% | ~70% | Well exercised by unit tests |

> **Recommendation:** Run `make coverage` to get actual numbers and validate these estimates.

### 6.4 Recommended Test Implementation Order

**Phase 1 — Error Paths (Week 1)**
- Add failure injection tests for xSync, xClose, xWrite realloc
- Add integer overflow boundary tests
- Add buffer boundary tests for error structs

**Phase 2 — Security (Week 2)**
- Add input validation tests (blob names, URLs)
- Add credential handling tests
- Set up libFuzzer for XML parsing and URL construction

**Phase 3 — Integration (Week 3)**
- Add explicit transaction tests (BEGIN/COMMIT/ROLLBACK)
- Add crash recovery tests (journal hot restart)
- Add concurrent access tests

**Phase 4 — Completeness (Week 4)**
- Add remaining VFS method tests (xFileControl, xFullPathname)
- Add state transition tests (lock upgrades/downgrades)
- Add large database tests
