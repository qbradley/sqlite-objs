# Squad Team

> Azure Blob-backed SQLite (azqlite)

## Coordinator

| Name | Role | Notes |
|------|------|-------|
| Squad | Coordinator | Routes work, enforces handoffs and reviewer gates. |

## Members

| Name | Role | Charter | Emoji |
|------|------|---------|-------|
| Gandalf | Lead | .squad/agents/gandalf/charter.md | 🏗️ |
| Frodo | Azure Expert | .squad/agents/frodo/charter.md | 🔵 |
| Aragorn | SQLite/C Dev | .squad/agents/aragorn/charter.md | ⚔️ |
| Samwise | QA | .squad/agents/samwise/charter.md | 🧪 |
| Gimli | Rust Dev | .squad/agents/gimli/charter.md | 🪓 |
| Merry | AWS/S3 Expert | .squad/agents/merry/charter.md | 🏹 |
| Scribe | Session Logger | .squad/agents/scribe/charter.md | 📋 |
| Ralph | Work Monitor | — | 🔄 |

## Project Context

- **Project:** Azure Blob-backed SQLite (azqlite)
- **User:** Quetzal Bradley
- **Stack:** C, SQLite VFS API, Azure Blob Storage REST API, libcurl, OpenSSL
- **License:** MIT
- **SQLite source:** `sqlite-autoconf-3520000/` (do not modify unless absolutely necessary)
- **Created:** 2026-03-10

### Description

A drop-in replacement for SQLite where all storage is backed by Azure Blob Storage. Implemented as a custom VFS layer, ideally without modifying SQLite source. Single writer / many readers on the same machine with remote storage. Committed transactions survive total machine loss — recovery via normal SQLite mechanisms.

### MVP Roadmap

- **MVP 1:** Drop-in replacement. Single machine, remote blob storage. Committed txns durable.
- **MVP 2:** In-memory read cache for query performance.
- **MVP 3:** Read-only queries from other machines while writes from a different machine.
- **MVP 4:** Multi-machine writes (correct, not necessarily performant).

### Dependencies

- SQLite (source in `sqlite-autoconf-3520000/`)
- OpenSSL (system library) — HMAC-SHA256 signing
- libcurl — HTTP client
- No Azure SDK — direct REST API, informed by existing SDK implementations

### Open Questions

- Page blobs vs block blobs vs append blobs?
- WAL mode vs Journal mode vs both?
- VFS extension approach — can everything be done via sqlite3_vfs?
