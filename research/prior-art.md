# Prior Art: SQLite on Remote/Cloud Storage

> Compiled by Gandalf (Lead/Architect) — March 2026
>
> A comprehensive survey of existing projects that back SQLite with remote or cloud
> storage, with particular attention to Azure Blob Storage, S3, and other object stores.
> This research informs the design of **sqlite-objs** — our Azure Blob-backed SQLite VFS.

---

## Table of Contents

1. [SQLite's Official Position on Remote Storage](#1-sqlites-official-position-on-remote-storage)
2. [Cloud Backed SQLite (CBS) — The Official Module](#2-cloud-backed-sqlite-cbs--the-official-module)
3. [Litestream](#3-litestream)
4. [LiteFS](#4-litefs)
5. [mvsqlite](#5-mvsqlite)
6. [rqlite](#6-rqlite)
7. [dqlite](#7-dqlite)
8. [libSQL / Turso](#8-libsql--turso)
9. [Cloudflare D1](#9-cloudflare-d1)
10. [sqlite-s3vfs](#10-sqlite-s3vfs)
11. [sqlite-s3-query](#11-sqlite-s3-query)
12. [sql.js-httpvfs](#12-sqljshttpvfs)
13. [sqlite3vfshttp (Go)](#13-sqlite3vfshttp-go)
14. [cr-sqlite / Vulcan](#14-cr-sqlite--vulcan)
15. [Corrosion](#15-corrosion)
16. [ElectricSQL](#16-electricsql)
17. [gRPSQLite](#17-grpsqlite)
18. [go-cloud-sqlite-vfs (PDOK)](#18-go-cloud-sqlite-vfs-pdok)
19. [ncruces/go-sqlite3 VFS Framework](#19-ncrucesgo-sqlite3-vfs-framework)
20. [Taxonomy & Comparison Matrix](#20-taxonomy--comparison-matrix)
21. [Key Lessons for sqlite-objs](#21-key-lessons-for-sqlite-objs)
22. [Anti-Patterns & Known Failure Modes](#22-anti-patterns--known-failure-modes)
23. [Architectural Recommendations](#23-architectural-recommendations)

---

## 1. SQLite's Official Position on Remote Storage

**Source:** [sqlite.org/useovernet.html](https://sqlite.org/useovernet.html), [sqlite.org/whentouse.html](https://sqlite.org/whentouse.html), [sqlite.org/wal.html](https://sqlite.org/wal.html)

SQLite's maintainers are unambiguous: **SQLite is not designed for remote storage.**

### Key Warnings

- **File locking is unreliable over network filesystems** (NFS, SMB/CIFS, AFP). SQLite's POSIX advisory locks do not propagate correctly over network mounts. Multiple machines can believe they hold exclusive locks simultaneously → corruption.
- **WAL mode absolutely does not work across machines.** WAL requires a shared memory file (`-shm`) backed by `mmap()`. This is physically impossible to share between machines. There is no workaround except `PRAGMA locking_mode = EXCLUSIVE`, which eliminates all concurrency benefits.
- **Performance degrades catastrophically.** Every I/O syscall becomes a network round-trip. SQLite's design assumes microsecond-latency I/O; network storage introduces millisecond-latency or worse.
- **Corruption is the expected outcome** of concurrent multi-machine access via network mounts.

### Official Recommendation

> "If you need concurrent, multi-machine access, use a client/server database like PostgreSQL."

### What This Means For Us

The SQLite team is warning against *naively* mounting a remote filesystem and pointing SQLite at it. They are **not** saying a custom VFS can't work — they're saying the *default* VFS won't work over the network. A purpose-built VFS that properly implements locking, caching, and I/O semantics for a specific remote backend is precisely the approach that *can* work, and CBS (below) proves this.

---

## 2. Cloud Backed SQLite (CBS) — The Official Module

| Field | Value |
|-------|-------|
| **URL** | [sqlite.org/cloudsqlite](https://sqlite.org/cloudsqlite/doc/trunk/www/index.wiki) |
| **Language** | C |
| **License** | Public domain (same as SQLite) |
| **Approach** | Custom VFS (`blockcachevfs`) |
| **Cloud Backends** | Azure Blob Storage, Google Cloud Storage |
| **Status** | Actively maintained by the SQLite team |

### Architecture

CBS is **the most directly relevant prior art for sqlite-objs**, developed by the SQLite team themselves.

- **Block-based storage:** The database is split into fixed-size blocks (default 4MB, configurable). Each block is stored as a separate blob (`.bcv` files).
- **Manifest file:** A central `manifest.bcv` maps block IDs to database offsets, enabling reassembly.
- **Local cache:** A local directory caches recently-accessed blocks, avoiding repeated cloud fetches.
- **Two operating modes:**
  - **Daemonless:** Direct client read/write to cloud blocks.
  - **Daemon mode:** A local daemon manages the block cache, allowing multiple processes to share cached data (read-only in this mode).

### Blob Type (Azure)

CBS uses **block blobs** (not page blobs). Each 4MB chunk is a complete block blob object. This means writes replace entire blocks — there is no sub-block random write.

### Locking Strategy

- **No built-in distributed locking.** CBS leaves write coordination to the application layer.
- **Single-writer assumption.** Multiple readers are supported, but only one writer should operate at a time.
- **Conditional writes** (ETags / If-Match headers) are mentioned as a mechanism to detect conflicting writes, but are not enforced by default.

### Caching Strategy

- **Read-through block cache** in a local directory.
- Blocks are fetched on demand and cached locally.
- **No write-back cache** — writes go directly to cloud storage.
- Cache invalidation requires polling the manifest for changes.

### What Worked Well

- Proven that the VFS approach works for cloud storage at the SQLite team's own hands.
- Block-based chunking elegantly handles the "no partial object writes" constraint of cloud storage.
- The manifest concept cleanly separates metadata from data.
- Local caching makes repeated reads fast.

### What Went Wrong / Limitations

- No push-based cache invalidation — readers must poll.
- No distributed locking — multi-writer is explicitly not supported.
- Block size of 4MB means even small writes require uploading a full 4MB block.
- Flat storage structure — no subdirectories in the blob container.
- Limited community adoption despite being official.

### Lessons for sqlite-objs

- **The VFS approach is blessed by SQLite's own team.** This validates our architectural direction.
- **Block-based chunking is the proven pattern** for mapping SQLite's page I/O to object storage.
- **We can do better on locking** — CBS punts on this; we should use Azure Blob Leases.
- **We can do better on write amplification** — our use of Azure Page Blobs (512-byte aligned random writes) could avoid the 4MB write amplification problem entirely.
- **We should not use block blobs for the database file** if we want efficient random writes. Page blobs are the right primitive for sqlite-objs.

---

## 3. Litestream

| Field | Value |
|-------|-------|
| **URL** | [github.com/benbjohnson/litestream](https://github.com/benbjohnson/litestream) |
| **Language** | Go |
| **License** | Apache 2.0 |
| **Approach** | WAL-based replication + Writable VFS |
| **Cloud Backends** | S3, Azure Blob Storage, GCS, SFTP |
| **Status** | Active, widely used |

### Architecture

Litestream takes two approaches:

1. **Backup/Restore mode:** Continuously ships WAL frames from a local SQLite database to S3/Azure, enabling point-in-time recovery. The database remains local; cloud is for durability.

2. **Writable VFS mode** (newer): A custom VFS that allows SQLite to write to a local buffer, which periodically syncs to S3. Background "hydration" populates a local copy from S3 for fast reads.

### Key Design Decisions

- **Local write buffer:** All writes go to a local buffer file first. Dirty pages are tracked and periodically flushed to cloud storage as LTX (Lite Transaction) files.
- **Background hydration:** On cold start, the VFS lazily downloads needed pages from S3 while serving reads directly from the remote store. As hydration completes, reads shift to the fast local copy.
- **Single writer only.** No distributed write coordination.
- **Eventual durability:** Writes aren't durable in the cloud until the next sync interval (configurable, e.g., 1 second). A crash before sync means data loss.

### Caching Strategy

- Write-back cache with periodic flush (configurable interval).
- Read-through via background hydration.

### What Worked Well

- Elegant design for edge/serverless where ephemeral local storage backs persistent cloud storage.
- Background hydration is clever — fast cold starts without downloading the whole DB.
- Very popular in the Fly.io ecosystem.
- Supports multiple cloud backends including Azure.

### What Went Wrong / Limitations

- **Eventual durability window** — unsynced writes are lost on crash.
- **Single-writer only** — no multi-machine writes.
- **Not a transparent VFS** — the backup mode requires running Litestream as a sidecar process.
- The writable VFS is relatively new and less battle-tested.

### Lessons for sqlite-objs

- **The hybrid local-buffer + cloud-sync pattern is powerful** but introduces a durability gap. We need to decide: are we willing to accept eventual durability, or do we require synchronous cloud writes?
- **Background hydration is a pattern worth stealing** for our MVP 2 (in-memory read cache).
- **Litestream proves Azure Blob Storage can work** as a backend (they already have an Azure adapter).

---

## 4. LiteFS

| Field | Value |
|-------|-------|
| **URL** | [github.com/superfly/litefs](https://github.com/superfly/litefs) |
| **Language** | Go |
| **License** | Apache 2.0 |
| **Approach** | FUSE filesystem + transaction replication |
| **Status** | Active (Fly.io) |

### Architecture

- **FUSE-based:** LiteFS presents itself as a local filesystem to SQLite. It intercepts all file I/O at the OS level via FUSE.
- **LTX files:** Each committed transaction produces a set of changed pages, packaged as an LTX file and streamed to replicas.
- **Leader election:** Uses Consul-based leases (not Raft) for primary election. Only the primary accepts writes.
- **Full local copies:** Every replica has a complete copy of the database, so reads are always local and fast.

### Locking Strategy

- Single primary writer, elected via distributed lease (Consul).
- Replicas are read-only.
- No multi-writer support.

### What Worked Well

- **Transparent to applications** — SQLite sees a normal filesystem. No VFS changes needed.
- **Extremely low-latency reads** on replicas (full local copies).
- **Good for global distribution** — Fly.io's use case of edge nodes worldwide.

### What Went Wrong / Limitations

- **Requires FUSE** — not available on all platforms (notably not on macOS in production, not on Windows).
- **Asynchronous replication** — replicas may lag.
- **No split-brain protection built-in** — depends on external lease/Consul.
- **Coupled to Fly.io's infrastructure** in practice.

### Lessons for sqlite-objs

- **The FUSE approach is too platform-dependent for us.** Our C VFS approach is more portable.
- **The LTX transaction-shipping pattern is interesting** for our MVP 3/4 (multi-machine reads/writes).
- **Leader election via distributed lease** is a pattern we should consider for MVP 4.

---

## 5. mvsqlite

| Field | Value |
|-------|-------|
| **URL** | [github.com/losfair/mvsqlite](https://github.com/losfair/mvsqlite) |
| **Language** | Rust |
| **License** | Apache 2.0 |
| **Approach** | Custom VFS backed by FoundationDB |
| **Status** | Experimental / research project |

### Architecture

The most ambitious project in this space. mvsqlite replaces SQLite's entire storage layer with FoundationDB, gaining distributed, MVCC, time-travel capabilities.

- **Page-level versioning:** Every SQLite page is stored as a versioned key in FoundationDB.
- **Optimistic concurrency:** Transactions check page versions at commit. Conflicts cause abort and retry.
- **Time travel:** Any historical version of the database can be queried.
- **Two-level storage:** A page index (lightweight) and a content-addressed store (actual pages) work around FoundationDB's 10MB/5s transaction limits.
- **Drop-in replacement:** Uses `LD_PRELOAD` to transparently intercept SQLite's I/O.

### Locking Strategy

- **No centralized locks.** FoundationDB provides optimistic, serializable transactions.
- **Single-writer per database** still applies — concurrent writers to the same DB will see frequent transaction conflicts.
- **Multi-database scaling** via partitioning (one DB per user/tenant).

### What Worked Well

- Proves that a VFS-only approach can enable sophisticated distributed storage.
- MVCC and time-travel are genuinely novel for SQLite.
- The content-addressed store for pages is clever — deduplication and efficient storage.

### What Went Wrong / Limitations

- **Requires FoundationDB** — a significant operational dependency.
- **Not production-hardened** — still experimental.
- **Single-writer per DB** — the fundamental SQLite constraint persists.
- **Heavy conflict rate** under concurrent write load to a single DB.

### Lessons for sqlite-objs

- **Page-level versioning is powerful** but probably overkill for MVP. Consider for future.
- **Content-addressed page storage** is an interesting optimization for deduplication.
- **The single-writer-per-DB constraint is universal** — every project hits this wall. We should not try to solve multi-writer at the VFS level.

---

## 6. rqlite

| Field | Value |
|-------|-------|
| **URL** | [github.com/rqlite/rqlite](https://github.com/rqlite/rqlite) |
| **Language** | Go |
| **License** | MIT ✅ |
| **Approach** | Raft consensus + HTTP API wrapper around SQLite |
| **Status** | Mature, production-used |

### Architecture

- SQLite is the storage engine, but all access is through an HTTP API.
- Writes are serialized through the Raft leader and replicated to all nodes.
- **Not a VFS approach** — it's a distributed database *built on* SQLite, not a storage backend *for* SQLite.

### Locking Strategy

- Raft consensus guarantees single-writer linearizability.
- Small clusters only (3-5 nodes typically).

### What Worked Well

- Simple deployment (single binary).
- Strong consistency via Raft.
- MIT licensed — compatible with us.

### What Went Wrong / Limitations

- **Not a drop-in replacement for SQLite** — requires API changes.
- **Write throughput limited by Raft** — every write requires majority acknowledgment.
- **No sharding** — single Raft group bottleneck.

### Lessons for sqlite-objs

- **Different problem space.** rqlite solves distributed consensus; we solve durable remote storage.
- **The HTTP API approach would break our "drop-in replacement" requirement.**
- However, rqlite's MIT license and maturity are worth noting as a reference for error handling patterns.

---

## 7. dqlite

| Field | Value |
|-------|-------|
| **URL** | [github.com/canonical/dqlite](https://github.com/canonical/dqlite) |
| **Language** | C |
| **License** | LGPL-3.0 ⚠️ (not MIT-compatible for linking) |
| **Approach** | Embedded library with C Raft implementation |
| **Status** | Production (used in LXD) |

### Architecture

- Like rqlite but embeddable — a C library that wraps SQLite with Raft consensus.
- Direct C API (not HTTP), closer to SQLite's native interface.
- Leader-serialized writes, Raft-replicated.

### Lessons for sqlite-objs

- **LGPL license is problematic** for our MIT project — can't statically link.
- **Interesting C Raft implementation** but again solves a different problem (consensus vs. storage).
- Confirms that C-level SQLite integration is viable and performant.

---

## 8. libSQL / Turso

| Field | Value |
|-------|-------|
| **URL** | [github.com/tursodatabase/libsql](https://github.com/tursodatabase/libsql) |
| **Language** | C (fork of SQLite), Rust (server) |
| **License** | MIT ✅ |
| **Approach** | SQLite fork with pluggable WAL backends + cloud sync |
| **Status** | Active, commercial (Turso) |

### Architecture

- **Fork of SQLite** with extensions for server mode, async I/O, and pluggable storage.
- **Block-based cloud storage:** Database is chunked into configurable blocks (e.g., 4MB), content-addressed, tracked by a manifest.
- **Server mode (sqld):** Adds HTTP/WebSocket protocols and JWT auth.
- **Working on MVCC** to eliminate SQLite's single-writer bottleneck.

### What Worked Well

- Active commercial development with real users.
- MIT licensed.
- Proves that block-based cloud storage + manifest pattern works at scale.

### What Went Wrong / Limitations

- **Requires forking SQLite** — they maintain their own SQLite fork, which is a significant ongoing burden.
- **Not a pure VFS approach** — modifications to SQLite internals.

### Lessons for sqlite-objs

- **We explicitly chose NOT to fork SQLite** (project constraint: `sqlite-autoconf-3520000/` — do not modify unless absolutely necessary). This is the right call — maintaining a fork is expensive.
- **The block + manifest pattern appears independently in CBS and Turso** — it's a strong convergent design.
- **MVCC at the storage level** is where the industry is heading, but it's a massive undertaking. Not for our MVP.

---

## 9. Cloudflare D1

| Field | Value |
|-------|-------|
| **URL** | [developers.cloudflare.com/d1](https://developers.cloudflare.com/d1/) |
| **Language** | Proprietary (Workers runtime) |
| **License** | Proprietary ⛔ |
| **Approach** | Managed service with SQLite as storage engine |
| **Status** | Production |

### Architecture

- SQLite at the core, with Cloudflare's edge replication and management layer.
- Accessed via Workers API, not as a library.
- Point-in-time recovery ("Time Travel") via WAL-based snapshotting.

### Lessons for sqlite-objs

- **Closed-source, so limited direct learning.** But confirms that SQLite + cloud storage is a viable and growing market.
- **10GB per-database limit** suggests they hit scalability challenges.
- **Sharding across many small DBs** is their scaling strategy — relevant for our multi-tenant thinking.

---

## 10. sqlite-s3vfs

| Field | Value |
|-------|-------|
| **URL** | [github.com/uktrade/sqlite-s3vfs](https://github.com/uktrade/sqlite-s3vfs) (archived; forks by simonw, dpedu, LorenzoBoccaccia) |
| **Language** | Python |
| **License** | MIT ✅ |
| **Approach** | Custom VFS via APSW (Another Python SQLite Wrapper) |
| **Cloud Backend** | S3 |
| **Status** | Archived/forked, community-maintained |

### Architecture

- Each SQLite page is stored as a separate S3 object.
- Reads and writes map directly to S3 GET/PUT per page.
- Uses APSW's VFS extension points (Python-level VFS).

### What Worked Well

- Simple, working implementation. Proves the concept.
- MIT licensed.
- Good reference for the page-per-object storage model.

### What Went Wrong / Limitations

- **No locking whatsoever.** Concurrent writers corrupt the database.
- **Every page write = full S3 PUT.** Extremely slow for write-heavy workloads.
- **Original repo archived** — community fragmented across forks.
- **Python-only** — not usable from C applications.
- **No caching** — every read is an S3 round-trip.

### Lessons for sqlite-objs

- **Page-per-object is too granular for object storage.** The overhead of one HTTP request per 4KB page is crippling. CBS's 4MB block approach is better.
- **However, Azure Page Blobs give us sub-object random writes** — we don't need one-object-per-page. We can store the entire DB as a single page blob and do 512-byte aligned reads/writes. This is a significant advantage over S3-based approaches.
- **The "no locking" approach is unacceptable** for any production use. We must have locking from day one.

---

## 11. sqlite-s3-query

| Field | Value |
|-------|-------|
| **URL** | [github.com/michalc/sqlite-s3-query](https://github.com/michalc/sqlite-s3-query) |
| **Language** | Python |
| **License** | MIT ✅ |
| **Approach** | HTTP Range Request VFS (read-only) |
| **Status** | Maintained |

### Architecture

- Read-only querying of SQLite databases stored on S3.
- Uses HTTP range requests to fetch only needed pages.
- Repeatable-read semantics via S3 object versioning.

### Lessons for sqlite-objs

- **HTTP range requests are the right primitive for read-only access** — we should use Azure Blob range reads similarly.
- **Repeatable-read via object versioning** is a useful pattern for snapshot isolation.
- **Read-only limitation** means this solves only half our problem.

---

## 12. sql.js-httpvfs

| Field | Value |
|-------|-------|
| **URL** | [github.com/phiresky/sql.js-httpvfs](https://github.com/phiresky/sql.js-httpvfs) |
| **Language** | TypeScript/JavaScript (WASM) |
| **License** | MIT ✅ (but check sql.js license) |
| **Approach** | HTTP Range Request VFS in the browser |
| **Status** | Active |

### Architecture

- Runs SQLite compiled to WASM in the browser.
- Implements a VFS that fetches pages via HTTP range requests from any static file host.
- Read-only.

### Lessons for sqlite-objs

- Proves HTTP range requests work well for random-access reads of SQLite pages.
- Interesting "virtual read head" optimization for sequential access patterns.
- Browser-only, so no direct code reuse for us.

---

## 13. sqlite3vfshttp (Go)

| Field | Value |
|-------|-------|
| **URL** | [github.com/psanford/sqlite3vfshttp](https://github.com/psanford/sqlite3vfshttp) |
| **Language** | Go |
| **License** | MIT ✅ |
| **Approach** | HTTP Range Request VFS |
| **Status** | Active |

### Architecture

- Go implementation of a read-only HTTP VFS for SQLite.
- Can query remote databases hosted on S3, CDNs, or any HTTP server supporting range requests.
- Can be compiled as a loadable SQLite extension.

### Lessons for sqlite-objs

- Clean, minimal implementation. Good reference for how a VFS translates reads to HTTP range requests.
- Read-only only.

---

## 14. cr-sqlite / Vulcan

| Field | Value |
|-------|-------|
| **URL** | [github.com/vlcn-io/cr-sqlite](https://github.com/vlcn-io/cr-sqlite) |
| **Language** | C (SQLite extension) + Rust |
| **License** | Apache 2.0 |
| **Approach** | CRDT extension for SQLite |
| **Status** | Active |

### Architecture

- **Not a VFS** — a SQLite extension that adds CRDT semantics to tables.
- Tables upgraded to "CRR" (Conflict-free Replicated Relations) gain automatic merge capabilities.
- Changes tracked via `crsql_changes` virtual table.
- Uses Lamport timestamps for causal ordering.

### What Worked Well

- Truly solves multi-writer without consensus — CRDTs guarantee convergence.
- Read performance identical to vanilla SQLite.
- Enables offline-first, peer-to-peer sync.

### What Went Wrong / Limitations

- **~2.5x write overhead** for CRDT metadata.
- **Limited data types** for CRDT support.
- **Not a storage backend** — still needs a local SQLite file.

### Lessons for sqlite-objs

- **CRDTs are the only approach that truly solves multi-writer.** Everything else (Raft, leases, locks) serializes writes.
- **But CRDTs are a layer above our VFS** — they could potentially be combined with sqlite-objs for MVP 4.
- **The 2.5x write overhead is non-trivial** and might not be acceptable for all workloads.

---

## 15. Corrosion

| Field | Value |
|-------|-------|
| **URL** | [github.com/superfly/corrosion](https://github.com/superfly/corrosion) |
| **Language** | Rust |
| **License** | Apache 2.0 |
| **Approach** | Gossip protocol (SWIM) + cr-sqlite + QUIC transport |
| **Status** | Production (Fly.io internal) |

### Architecture

- Each node has a local SQLite database with cr-sqlite.
- Changes propagate via gossip protocol (SWIM) over QUIC.
- No central leader — fully decentralized.
- Eventual consistency via CRDTs.

### Lessons for sqlite-objs

- **Shows the cr-sqlite + gossip approach at production scale** (800+ nodes at Fly.io).
- **Different problem** — service discovery / state sync, not durable storage.
- **The gossip pattern** could be relevant for our MVP 3/4 cache invalidation.

---

## 16. ElectricSQL

| Field | Value |
|-------|-------|
| **URL** | [github.com/electric-sql/electric](https://github.com/electric-sql/electric) |
| **Language** | Elixir (sync service), TypeScript (client) |
| **License** | Apache 2.0 |
| **Approach** | Postgres ↔ SQLite bidirectional sync with CRDTs |
| **Status** | Active |

### Architecture

- Postgres is the canonical source of truth in the cloud.
- SQLite instances on edge/client devices.
- Sync service mediates via Postgres logical replication + WebSocket protocol.
- CRDTs for conflict resolution.

### Lessons for sqlite-objs

- **Different architecture** — sync between two database engines, not a VFS.
- **The "local-first" pattern** (fast local reads, async sync) is philosophically aligned with our MVP 2 cache.
- **Postgres dependency** makes it irrelevant as a direct comparison.

---

## 17. gRPSQLite

| Field | Value |
|-------|-------|
| **URL** | [github.com/danthegoodman1/gRPSQLite](https://github.com/danthegoodman1/gRPSQLite) |
| **Language** | Go |
| **License** | MIT ✅ |
| **Approach** | gRPC-based remote VFS |
| **Status** | Experimental |

### Architecture

- Exposes SQLite file operations over gRPC.
- A server implements the actual storage backend; clients use a VFS that forwards I/O over gRPC.
- Backend-agnostic — could be backed by any storage.

### Lessons for sqlite-objs

- **Interesting abstraction** but adds a network hop (gRPC) on top of the storage hop.
- **We don't need the intermediary** — our VFS talks directly to Azure REST API.

---

## 18. go-cloud-sqlite-vfs (PDOK)

| Field | Value |
|-------|-------|
| **URL** | [github.com/PDOK/go-cloud-sqlite-vfs](https://github.com/PDOK/go-cloud-sqlite-vfs) |
| **Language** | Go |
| **License** | MIT ✅ |
| **Approach** | Go wrapper around CBS (Cloud Backed SQLite) |
| **Cloud Backends** | Azure Blob Storage, GCS |
| **Status** | Active |

### Architecture

- Wraps the official CBS C code for use with Go SQLite packages.
- Supports both Azure and GCS.
- Same block-based architecture as CBS.

### Lessons for sqlite-objs

- Confirms CBS's Azure support is real and usable.
- Go-only, but validates the CBS approach in a different language ecosystem.

---

## 19. ncruces/go-sqlite3 VFS Framework

| Field | Value |
|-------|-------|
| **URL** | [github.com/ncruces/go-sqlite3](https://github.com/ncruces/go-sqlite3) |
| **Language** | Go (pure, no cgo — uses WASM) |
| **License** | MIT ✅ |
| **Approach** | VFS framework for custom backends |
| **Status** | Active, well-maintained |

### Architecture

- Pure Go SQLite bindings via WASM (wazero runtime).
- Exposes Go interfaces for implementing custom VFS backends.
- Includes `memdb` and `readervfs` sample implementations.
- Integrates with Go's `io/fs.FS` interface.

### Lessons for sqlite-objs

- **Good reference for VFS interface design** — clean Go abstractions.
- Shows how VFS methods map to custom backends.
- We're in C, but the interface patterns are universal.

---

## 20. Taxonomy & Comparison Matrix

### Approach Categories

| Category | Projects | Description |
|----------|----------|-------------|
| **VFS (read/write)** | CBS, Litestream VFS, mvsqlite, sqlite-s3vfs, sqlite-objs (us) | Custom VFS replaces file I/O with cloud storage calls |
| **VFS (read-only)** | sql.js-httpvfs, sqlite3vfshttp, sqlite-s3-query | VFS for read-only access via HTTP range requests |
| **FUSE filesystem** | LiteFS | OS-level filesystem intercept |
| **Replication/sync** | Litestream (backup), ElectricSQL, cr-sqlite, Corrosion | Database remains local; changes replicated to cloud/peers |
| **Distributed DB on SQLite** | rqlite, dqlite | Consensus protocol wraps SQLite as storage engine |
| **SQLite fork** | libSQL/Turso | Modified SQLite internals for cloud/distributed use |
| **Managed service** | Cloudflare D1 | Proprietary cloud service |

### Comparison Matrix

| Project | VFS? | Writable? | Multi-machine? | Locking | Caching | License | Azure? |
|---------|------|-----------|----------------|---------|---------|---------|--------|
| **CBS** | ✅ | ✅ | Readers only | None (app layer) | Block cache | Public domain | ✅ |
| **Litestream VFS** | ✅ | ✅ | No | None | Write-back + hydration | Apache 2.0 | ✅ |
| **LiteFS** | FUSE | ✅ | Readers only | Consul lease | Full local copy | Apache 2.0 | No |
| **mvsqlite** | ✅ | ✅ | Readers only | FDB optimistic | FDB-backed | Apache 2.0 | No |
| **rqlite** | No | ✅ | ✅ (Raft) | Raft consensus | In-memory | MIT | No |
| **dqlite** | No | ✅ | ✅ (Raft) | Raft consensus | In-memory | LGPL | No |
| **libSQL/Turso** | Modified | ✅ | ✅ (server) | Server-level | Block cache | MIT | No |
| **sqlite-s3vfs** | ✅ | ✅ | No | None | None | MIT | No |
| **sqlite-s3-query** | ✅ | No | Readers only | N/A | None | MIT | No |
| **sql.js-httpvfs** | ✅ | No | Readers only | N/A | Browser cache | MIT | No |
| **cr-sqlite** | No | ✅ | ✅ (CRDT) | None needed | Local SQLite | Apache 2.0 | No |
| **sqlite-objs (planned)** | ✅ | ✅ | MVP 3/4 | Azure Blob Lease | MVP 2: page cache | MIT | ✅ |

---

## 21. Key Lessons for sqlite-objs

### Lesson 1: The VFS Approach Is Validated

CBS, Litestream, mvsqlite, and sqlite-s3vfs all prove that a custom VFS can successfully back SQLite with remote storage. This is not speculative — it's proven at multiple levels of ambition.

### Lesson 2: Azure Page Blobs Are Our Secret Weapon

Every S3-based project struggles with the same problem: **S3 objects are immutable**. You can't update 4KB in the middle of a 100MB file. This forces either:
- One object per page (sqlite-s3vfs) — too many HTTP requests
- Block-based chunking (CBS, Turso) — write amplification (4MB blocks for small changes)

**Azure Page Blobs support 512-byte aligned random writes.** This means we can:
- Store the entire database as a single page blob
- Write individual SQLite pages (4KB) directly to the correct offset
- Read individual pages with range requests
- Avoid all write amplification

This is a fundamental architectural advantage over every S3-based solution. No prior project has exploited this.

### Lesson 3: Locking Must Be Built-In, Not "Left to the Application"

CBS, sqlite-s3vfs, and others punt on locking. This leads to data corruption in practice. We must use **Azure Blob Leases** to enforce single-writer semantics from MVP 1.

### Lesson 4: WAL Mode Requires Special Handling

Standard WAL mode requires shared memory (`-shm` file) via `mmap()`, which is impossible across machines. Options:
- **Rollback journal mode** (simplest, works everywhere, but single-writer blocks readers)
- **WAL with EXCLUSIVE locking mode** (single-process only — viable for MVP 1)
- **Custom WAL implementation** that stores the WAL in Azure (complex, future consideration)

For MVP 1, rollback journal mode is the safe choice. WAL can be considered for MVP 2+.

### Lesson 5: Local Caching Is Essential for Performance

Every successful project (CBS, Litestream, LiteFS, mvsqlite) implements local caching. Cloud storage latency (10-100ms per request) makes uncached SQLite unusable. Our MVP 2 (in-memory read cache) is correctly sequenced.

### Lesson 6: Single-Writer Is the Universal Constraint

Every project — without exception — is limited to single-writer for a given database. The approaches differ only in how they enforce it:
- Locks/leases (CBS implied, our approach)
- Leader election (LiteFS, rqlite, dqlite)
- Optimistic concurrency (mvsqlite)
- CRDTs (cr-sqlite — technically multi-writer, but with overhead)

For MVP 3/4, Azure Blob Leases + a reader/writer protocol is the right approach.

### Lesson 7: Don't Fork SQLite

libSQL/Turso demonstrates both the power and the burden of forking SQLite. Maintaining a fork requires tracking upstream changes indefinitely. Our decision to use the unmodified `sqlite-autoconf-3520000` source and work purely at the VFS layer is correct.

---

## 22. Anti-Patterns & Known Failure Modes

### ❌ Mounting Azure File Shares as a Network Filesystem

Multiple Stack Overflow answers and Azure documentation suggest mounting Azure File Shares (SMB) and pointing SQLite at them. **This will corrupt your database under concurrent access.** SQLite's advisory locks do not work correctly over SMB.

### ❌ One Object Per SQLite Page on S3

sqlite-s3vfs stores each page as a separate S3 object. This creates O(N) HTTP requests for any query that touches N pages, making performance abysmal. The overhead of S3 request signing, connection setup, and latency per request dominates.

### ❌ No Locking / "The Application Will Handle It"

CBS and sqlite-s3vfs leave locking to the application. In practice, applications don't handle it, and databases get corrupted. Locking must be built into the VFS layer.

### ❌ Assuming WAL Mode Works Over the Network

WAL requires shared memory. Shared memory requires a local filesystem. There is no workaround except exclusive locking mode (which defeats the purpose of WAL).

### ❌ Synchronous Cloud Writes for Every SQLite Page

If every `xWrite` call in the VFS synchronously writes to cloud storage, a simple INSERT might generate dozens of cloud API calls (page writes, journal updates, etc.), each with 10-100ms latency. This makes SQLite unusably slow. Some form of write batching or buffering is essential.

---

## 23. Architectural Recommendations

Based on this survey, here are my recommendations for sqlite-objs's design:

### MVP 1: Single Machine, Remote Storage

1. **Use Azure Page Blobs** for the database file. This gives us random-access read/write at 512-byte granularity — far superior to block blobs or S3 objects.
2. **Implement rollback journal mode** (not WAL) for simplicity and correctness.
3. **Use Azure Blob Leases** for write locking. Acquire a 60-second lease on the page blob before any write operation; renew during long transactions; release on commit.
4. **Buffer writes locally** and flush to Azure in batches at `xSync` time, not on every `xWrite`. This is critical for performance.
5. **No read caching in MVP 1** — every read goes to Azure. This will be slow but correct. Performance comes in MVP 2.

### MVP 2: In-Memory Read Cache

6. **Implement a page cache** in memory (LRU or similar). Cache pages read from Azure; invalidate on write.
7. **Consider background hydration** (à la Litestream) for warming the cache.

### MVP 3: Multi-Machine Reads

8. **Use Azure Blob Lease as the writer token.** Only the lease holder can write. All other machines are readers.
9. **Readers can use conditional reads** (If-None-Match with ETags) to detect when their cached pages are stale.
10. **No shared memory / no WAL** across machines. Rollback journal only.

### MVP 4: Multi-Machine Writes

11. **Lease-based writer election.** Only one machine writes at a time. Other machines queue writes or forward them to the lease holder.
12. **Consider cr-sqlite integration** for true multi-writer semantics as a future extension, but this is not a VFS-level concern.

---

## References

- [SQLite Over a Network](https://sqlite.org/useovernet.html)
- [SQLite VFS Documentation](https://sqlite.org/vfs.html)
- [SQLite WAL Documentation](https://sqlite.org/wal.html)
- [Cloud Backed SQLite](https://sqlite.org/cloudsqlite/doc/trunk/www/index.wiki)
- [CBS blockcachevfs.h](https://sqlite.org/cloudsqlite/doc/trunk/src/blockcachevfs.h)
- [Litestream](https://litestream.io/)
- [Litestream Writable VFS](https://litestream.io/guides/vfs-write-mode/)
- [LiteFS Architecture](https://github.com/superfly/litefs/blob/main/docs/ARCHITECTURE.md)
- [mvsqlite](https://github.com/losfair/mvsqlite)
- [mvsqlite Blog Post](https://su3.io/posts/mvsqlite)
- [rqlite](https://rqlite.io/)
- [dqlite](https://dqlite.io/)
- [libSQL](https://github.com/tursodatabase/libsql)
- [Cloudflare D1](https://developers.cloudflare.com/d1/)
- [sqlite-s3vfs](https://github.com/simonw/sqlite-s3vfs)
- [sqlite-s3-query](https://github.com/michalc/sqlite-s3-query)
- [sql.js-httpvfs](https://github.com/phiresky/sql.js-httpvfs)
- [sqlite3vfshttp](https://github.com/psanford/sqlite3vfshttp)
- [cr-sqlite](https://github.com/vlcn-io/cr-sqlite)
- [Corrosion](https://github.com/superfly/corrosion)
- [ElectricSQL](https://electric-sql.com/)
- [gRPSQLite](https://github.com/danthegoodman1/gRPSQLite)
- [go-cloud-sqlite-vfs](https://github.com/PDOK/go-cloud-sqlite-vfs)
- [ncruces/go-sqlite3](https://github.com/ncruces/go-sqlite3)
- [Azure Page Blobs Overview](https://learn.microsoft.com/en-us/azure/storage/blobs/storage-blob-pageblob-overview)
- [Azure Blob Concurrency Management](https://learn.microsoft.com/en-us/azure/storage/blobs/concurrency-manage)
- [Aalto University: Adapting SQLite to the Distributed Edge](https://aaltodoc.aalto.fi/bitstreams/e3df40d2-a6e3-4a6a-8341-d10837b2f834/download)
