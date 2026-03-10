# azqlite

A drop-in SQLite replacement where all storage is backed by Azure Blob Storage.

azqlite implements a custom SQLite VFS (Virtual File System) layer that stores database files as Azure Page Blobs and journal files as Azure Block Blobs. This enables cloud-native SQLite databases with durability guarantees — if a transaction commits and the machine disappears, a new machine can connect and see all committed data after normal SQLite recovery.

## Features

- **Drop-in replacement**: Use standard SQLite APIs — just register the azqlite VFS
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

### Development build (stub Azure client, for unit tests)

```bash
make all
```

### Production build (real Azure client)

```bash
make all-production
```

This produces:
- `build/libazqlite.a` — static library
- `azqlite-shell` — SQLite shell with azqlite VFS

## Testing

### Unit tests (no Azure required)

```bash
make test-unit
```

Runs 148 tests against mock Azure operations.

### Integration tests (requires Azurite)

```bash
# Install Azurite (Azure Storage emulator)
npm install -g azurite

# Run integration tests
make test-integration
```

Runs 10 tests against a local Azurite instance.

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
./azqlite-shell mydb.db
```

The shell automatically registers the azqlite VFS. Your database file `mydb.db` will be stored as a page blob in the configured container.

```sql
sqlite> CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT);
sqlite> INSERT INTO users VALUES (1, 'Alice');
sqlite> SELECT * FROM users;
1|Alice
sqlite> .quit
```

### Programmatic Usage (C)

```c
#include "azqlite.h"
#include <sqlite3.h>

int main() {
    // Register azqlite VFS (reads config from environment)
    int rc = azqlite_register_vfs("azqlite", 1);  // 1 = make default
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

azqlite uses Azure blob leases for write exclusion:

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

## Limitations

- **Journal mode only**: WAL mode not supported (no shared memory over Azure)
- **Single writer**: One machine can write at a time (lease-based)
- **Full download**: Entire database loaded into memory on open
- **No streaming**: Not suitable for databases larger than available RAM

## Roadmap

- **MVP 1** ✅: Drop-in replacement, single machine, remote storage
- **MVP 2**: In-memory read cache for large databases
- **MVP 3**: Read-only queries from multiple machines
- **MVP 4**: Multi-machine writes (not performant, but correct)

## License

MIT License. See [LICENSE](LICENSE) for details.

## Contributing

This project was built by the azqlite Squad:
- 🏗️ Gandalf — Lead/Architect
- 🔧 Aragorn — SQLite/C Expert  
- 🔵 Frodo — Azure Expert
- 🧪 Samwise — QA Expert

See `.squad/` for team documentation and decision history.
