# sqlite-objs

A drop-in SQLite replacement where all storage is backed by Azure Blob Storage.

sqlite-objs implements a custom SQLite VFS (Virtual File System) layer that stores database files as Azure Page Blobs and journal files as Azure Block Blobs. This enables cloud-native SQLite databases with durability guarantees — if a transaction commits and the machine disappears, a new machine can connect and see all committed data after normal SQLite recovery.

## Features

- **Drop-in replacement**: Use standard SQLite APIs — just register the sqlite-objs VFS
- **Azure Page Blobs for database files**: Random read/write with 512-byte alignment
- **Azure Block Blobs for journals**: Sequential write, whole-object semantics
- **Lease-based locking**: Azure blob leases provide distributed write exclusion
- **Full blob caching**: Entire database downloaded on open for fast queries
- **Dirty page tracking**: Only modified pages uploaded on commit
- **Minimal dependencies**: Only libcurl and OpenSSL (system libraries)

## Requirements

- C11 compiler (gcc, clang)
- libcurl (with SSL support)
- OpenSSL 3.x
- SQLite 3.x (included in `sqlite-autoconf-3520000/`)

### macOS

```bash
brew install openssl@3 curl
```

### Ubuntu/Debian

```bash
sudo apt-get install libcurl4-openssl-dev libssl-dev
```

## Building

### Build

```bash
make all
```

This produces:
- `build/libsqlite_objs.a` — static library
- `sqlite-objs-shell` — SQLite shell with sqlite-objs VFS

## Testing

### Fast Gate Commands (For Contributors)

These are the primary validation commands for pre-commit and release prep:

```bash
# Unit tests (fast, ~30s, no external dependencies)
make test-unit

# Integration tests (requires Azurite, ~2min; strict mode by default)
npm install -g azurite  # One-time setup
make test-integration

# Extended validation (stress + property tests, ~10-30min)
make test-stress                  # 2× multiplier, 3 iterations
make test-stress-heavy            # 4× multiplier, 5 iterations (30+ min)
make test-integration-extended    # 500+ property test operations

# Sanitizers (memory safety, ~1min)
make sanitize                      # AddressSanitizer + UBSan

# Coverage report
make coverage                      # Generates HTML coverage report

# Official SQLite TCL test suite (comprehensive, ~5-10min)
make test-tcl-quick               # Smoke test (5 tests)
make test-tcl                      # Full suite (1187 test files)

# Rust bindings (if Rust changes)
cd rust && cargo test
```

### Validation Tiers

| Tier | Commands | Time | When to Run | External Deps |
|------|----------|------|------------|---------------|
| **Gate** | `test-unit` | ~30s | Every commit (local) | None |
| **Gate** | `sanitize` | ~1m | Before push | None |
| **Gate** | `test-integration` | ~2m | Before PR | Azurite |
| **Extended** | `test-stress` | ~5m | Release candidate | Azurite |
| **Extended** | `test-stress-heavy` | ~30m | Final release validation | Azurite |
| **Extended** | `test-tcl-quick` | ~1m | Before PR | None |
| **Extended** | `test-tcl` | ~10m | Before release | None |
| **Extended** | `test-integration-extended` | ~10m | Property-based validation | Azurite |

### Unit tests (no Azure required)

```bash
make test-unit
```

Runs the mocked C unit suite, including VFS, Azure client, WAL, URI, chaos, and retry tests. Fast (~30s), suitable for pre-commit validation.

### Integration tests (requires Azurite)

```bash
# Install Azurite (Azure Storage emulator) — one-time
npm install -g azurite

# Run integration tests
make test-integration
```

Runs the Azurite-backed integration suite, including multi-client, concurrency, crash recovery, snapshot isolation, and property/invariant tests (~2min).

See [TEST_DOCS_INDEX.md](TEST_DOCS_INDEX.md) for advanced testing modes, stress testing, and sanitizer details.

### Benchmarks

Compare local SQLite vs sqlite-objs performance using SQLite's official speedtest1 benchmark:

```bash
cd benchmark

# Local-only benchmark (no Azure required)
make && ./benchmark --local-only --size 25

# Full comparison (requires Azure credentials)
make all
export AZURE_STORAGE_ACCOUNT=myaccount
export AZURE_STORAGE_KEY=mykey
export AZURE_STORAGE_CONTAINER=benchmarks
./benchmark --size 50

# CSV output for automation
./benchmark --output csv > results.csv
```

See [`benchmark/README.md`](benchmark/README.md) for detailed usage.

**Expected performance:** Azure is 2-50x slower than local SQLite depending on workload characteristics. The in-memory cache significantly reduces the gap for read-heavy workloads.

## Usage

### Environment Variables

Configure Azure connection via environment variables:

```bash
export AZURE_STORAGE_ACCOUNT="yourstorageaccount"
export AZURE_STORAGE_CONTAINER="yourcontainer"

# Authentication (choose one):
export AZURE_STORAGE_SAS="?sv=2021-06-08&ss=b&srt=sco&sp=rwdlac..."  # SAS token (preferred)
# OR
export AZURE_STORAGE_KEY="your-base64-storage-key"  # Shared Key
```

### Azure Setup

```bash
# Create storage account
az storage account create \
  --name yourstorageaccount \
  --resource-group yourgroup \
  --location eastus \
  --sku Standard_LRS

# Create container
az storage container create \
  --name yourcontainer \
  --account-name yourstorageaccount

# Generate SAS token (valid for 1 year)
az storage container generate-sas \
  --name yourcontainer \
  --account-name yourstorageaccount \
  --permissions rwdlac \
  --expiry $(date -u -v+1y '+%Y-%m-%dT%H:%MZ') \
  --output tsv
```

### Command Line Shell

```bash
# Environment variable mode:
./sqlite-objs-shell mydb.db

# URI mode (no environment variables needed):
./sqlite-objs-shell --uri "file:mydb.db?azure_account=myacct&azure_container=mycontainer&azure_sas=sv%3D2024..."
```

The shell automatically registers the sqlite-objs VFS. Your database file `mydb.db` will be stored as a page blob in the configured container.

```sql
sqlite> CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT);
sqlite> INSERT INTO users VALUES (1, 'Alice');
sqlite> SELECT * FROM users;
1|Alice
sqlite> .quit
```

### Programmatic Usage (C)

```c
#include "sqlite_objs.h"
#include <sqlite3.h>

int main() {
    // Register sqlite-objs VFS (reads config from environment)
    int rc = sqlite_objs_register_vfs("sqlite-objs", 1);  // 1 = make default
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Failed to register VFS\n");
        return 1;
    }

    // Use SQLite normally — storage goes to Azure
    sqlite3 *db;
    rc = sqlite3_open("mydb.db", &db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Cannot open database: %s\n", sqlite3_errmsg(db));
        return 1;
    }

    // Execute queries...
    sqlite3_exec(db, "CREATE TABLE test (x INTEGER)", NULL, NULL, NULL);

    sqlite3_close(db);
    return 0;
}
```

## Architecture

### Blob Type Mapping

| SQLite File Type | Azure Blob Type | Rationale |
|------------------|-----------------|-----------|
| Main database    | Page Blob       | Random R/W, 512-byte alignment |
| Journal          | Block Blob      | Sequential write, whole-object |
| Temp files       | Local filesystem | Delegated to default VFS |

### Locking Model

sqlite-objs uses Azure blob leases for write exclusion:

- **SHARED lock**: No lease required (read-only)
- **RESERVED/PENDING/EXCLUSIVE**: 30-second lease acquired
- Lease auto-renewed during long transactions
- Lease released on unlock or connection close

### Caching Strategy

- Full blob download on `xOpen` into memory buffer
- `xRead`/`xWrite` operate on in-memory buffer (fast)
- Dirty page bitmap tracks modified 4KB pages
- `xSync` uploads only dirty pages to Azure
- Future: LRU page cache for large databases (MVP 2)

## URI Configuration

Instead of environment variables, you can pass Azure credentials directly in the database URI:

```c
// Register VFS without any global credentials
sqlite_objs_vfs_register_uri(1);  // 1 = make default

// Open with credentials in the URI
sqlite3_open_v2(
    "file:mydb.db?azure_account=myacct&azure_container=mycontainer&azure_sas=sv%3D2024...",
    &db,
    SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI,
    "sqlite-objs"
);
```

Supported URI parameters:

| Parameter          | Description                              |
|--------------------|------------------------------------------|
| `azure_account`    | Storage account name (required)          |
| `azure_container`  | Container name                           |
| `azure_sas`        | SAS token                                |
| `azure_key`        | Shared Key (alternative to SAS)          |
| `azure_endpoint`   | Custom endpoint (e.g. for Azurite)       |

This enables opening databases across different Azure accounts and containers within the same process. If `azure_account` is not present in the URI, `xOpen` returns `SQLITE_CANTOPEN`.

## Limitations

- **WAL requires exclusive locking**: Shared-memory WAL is not supported over Azure; use `PRAGMA locking_mode=EXCLUSIVE` before enabling WAL.
- **Single writer**: One machine can write at a time (lease-based)
- **Default full download**: Databases are downloaded on open unless URI mode uses `prefetch=none`
- **Local cache required**: Database pages are cached locally and revalidated with ETags when cache reuse is enabled

## Roadmap

- **MVP 1** ✅: Drop-in replacement, single machine, remote storage
- **MVP 2**: In-memory read cache for large databases
- **MVP 3**: Read-only queries from multiple machines
- **MVP 4**: Multi-machine writes (not performant, but correct)

## License

MIT License. See [LICENSE](LICENSE) for details.

## Contributing

This project was built by the sqlite-objs Squad:
- 🏗️ Gandalf — Lead/Architect
- 🔧 Aragorn — SQLite/C Expert  
- 🔵 Frodo — Azure Expert
- 🧪 Samwise — QA Expert

See `.squad/` for team documentation and decision history.
