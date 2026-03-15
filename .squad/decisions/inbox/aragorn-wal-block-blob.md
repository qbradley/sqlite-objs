### WAL Files: Append Blobs → Block Blobs
**Date:** 2026-07 | **From:** Aragorn (implementation) + Frodo (Azure API analysis)

WAL sync now uses `block_blob_upload` (single PUT, overwrites) instead of append blob operations (DELETE + CREATE + N×APPEND with 4 MiB chunking). This eliminates the `nWalSynced` / `walNeedFullResync` tracking state and reduces the WAL sync path from ~100 lines to ~20.

**Impact:** xOpen for WAL files now requires `block_blob_upload` and `block_blob_download` in the `azure_ops_t` vtable. The `append_blob_*` entries are preserved but unused by WAL.

**Note:** `block_blob_download` (GET Blob) already worked on append blobs, so crash recovery path was unchanged.
