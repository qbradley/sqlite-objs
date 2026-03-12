# Merry — History

## Project Context
- **Project:** azqlite — Azure Blob-backed SQLite VFS
- **User:** Quetzal Bradley
- **Stack:** C, SQLite VFS API, Azure Blob Storage REST API, libcurl, OpenSSL
- **Role:** AWS/S3 Expert — adding Amazon S3 as an alternative cloud storage backend
- **Team root:** /workspace/home/qbradley/src/azqlite

## Key Context
- azqlite already has a working Azure Blob Storage backend (Frodo's domain)
- The VFS uses an ops vtable (`azure_ops_t`) for storage operations
- URI-based per-file configuration is implemented (Phase 1-6 complete)
- URI params: `azure_account`, `azure_container`, `azure_sas`, `azure_key`, `azure_endpoint`
- S3 support will need analogous URI params and a compatible ops vtable
- libcurl is already a dependency — reuse for S3 HTTP calls
- OpenSSL is already a dependency — reuse for SigV4 HMAC-SHA256 signing
- Testing uses Azurite for Azure emulation; S3 will need LocalStack or MinIO

## Learnings
