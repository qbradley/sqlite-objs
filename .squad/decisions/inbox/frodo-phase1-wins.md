# Phase 1 Azure Immediate Wins — Implementation

**Date:** 2026-07  
**Agent:** Frodo (Azure Expert)  
**Status:** Implemented

---

## 1. Conditional Headers (If-Match on Put Page) ✅

**What:** Added `If-Match: <etag>` header support to page blob write operations for optimistic concurrency control.

**Changes:**
- `azure_client.h`: Added `const char *if_match` parameter to `page_blob_write` and `page_blob_write_batch` vtable signatures
- `azure_auth.c`: Updated `azure_auth_sign_request()` to include If-Match value in StringToSign (position 9, between If-Modified-Since and If-None-Match)
- `azure_client_impl.h`: Updated `azure_auth_sign_request()` declaration
- `azure_client.c`: 
  - `execute_single()` and `execute_with_retry()` accept and thread `if_match` parameter
  - `az_page_blob_write()` passes `if_match` to execute_with_retry
  - `batch_init_easy()` includes If-Match in both SharedKey signing and HTTP headers
  - `az_page_blob_write_batch()` passes `if_match` through to batch_init_easy
- `sqlite_objs_vfs.c`:
  - xSync passes `p->etag` (when available) to both batch and sequential write paths
  - `azureErrToSqlite()` maps `AZURE_ERR_PRECONDITION` → `SQLITE_BUSY`
  - Batch and sequential write paths return `SQLITE_BUSY` on 412 (not `SQLITE_IOERR_FSYNC`)
- `azure_client_stub.c`: Updated stub signature and converted to designated initializers
- `mock_azure_ops.c`: Updated mock signature (ignores if_match)
- All test files: 44+ call sites updated with NULL for if_match

**Behavior:**
- When VFS has a cached ETag, writes include `If-Match` header
- Azure returns 412 Precondition Failed if blob was modified by another client
- 412 maps to `AZURE_ERR_PRECONDITION` (non-retryable) → `SQLITE_BUSY`
- Caller can retry the transaction (standard SQLite busy handling)
- This is a SECOND layer of defense alongside lease-based protection

## 2. Soft Delete — Undelete Blob API ✅

**What:** Added `az_blob_undelete()` function to recover soft-deleted blobs.

**Changes:**
- `azure_client.h`: Added `blob_undelete` to `azure_ops_t` vtable
- `azure_client.c`: Implemented `az_blob_undelete()` — `PUT ?comp=undelete`
- Production vtable wired up; mock and stub set to NULL

**Notes:**
- Soft delete must be enabled at the Azure Storage account level (portal/CLI)
- Typical retention: 7 days
- This API enables programmatic recovery of accidentally deleted databases

## 3. Last Access Time Tracking — Documentation Only ✅

**What:** No code change needed. This is an account-level Azure Storage setting.

**Recommendation:** Enable "Last access time tracking" in the Azure Storage account settings. This allows:
- Automated lifecycle policies (e.g., move to Cool tier after 30 days of inactivity)
- Cost optimization for large-scale deployments with cold databases
- The `Get Blob Properties` response already includes the last access time field

---

## Testing

- 294/295 unit tests pass (1 pre-existing failure unrelated to this work)
- Zero new warnings in the changed files
- Synced to `rust/sqlite-objs-sys/csrc/`
