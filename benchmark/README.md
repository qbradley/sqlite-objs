# sqlite-objs Benchmark Harness

This benchmark harness compares the performance of local SQLite against sqlite-objs (Azure blob-backed SQLite) using SQLite's official `speedtest1` test suite.

## Overview

The harness runs identical workloads against both:
- **Local SQLite**: Using the default VFS with local file storage
- **Azure SQLite**: Using the sqlite-objs VFS with Azure Blob Storage backend

It captures timing for each run and produces a comparison report showing speedup/slowdown ratios.

## Building

### Stub Build (No Azure Dependencies)

Build with stub Azure client (local-only benchmarks):

```bash
make
```

### Production Build (Full Azure Support)

Build with real Azure client (requires libcurl and OpenSSL):

```bash
make all-production
```

Dependencies:
- libcurl (with development headers)
- OpenSSL (with development headers)

On macOS (Homebrew):
```bash
brew install curl openssl
```

On Ubuntu/Debian:
```bash
sudo apt-get install libcurl4-openssl-dev libssl-dev
```

## Running Benchmarks

### Environment Variables

For Azure benchmarks, set these environment variables:

```bash
export AZURE_STORAGE_ACCOUNT=myaccount
export AZURE_STORAGE_CONTAINER=mybenchmarks

# Authentication: Use either Shared Key OR SAS token
export AZURE_STORAGE_KEY=mykey           # Shared Key auth
# OR
export AZURE_STORAGE_SAS=mysastoken      # SAS token auth
```

### Basic Usage

Run both local and Azure benchmarks:

```bash
./benchmark
```

Run only local benchmark:

```bash
./benchmark --local-only
```

Run only Azure benchmark:

```bash
./benchmark --azure-only
```

### Command-Line Options

- `--local-only` — Run only local SQLite benchmark
- `--azure-only` — Run only sqlite-objs benchmark
- `--size N` — Size parameter passed to speedtest1 (default: 25)
  - Larger values = more operations, longer runtime
  - Recommended range: 10-100
- `--output FORMAT` — Output format: `text` (default) or `csv`
- `--help` — Show help message

### Examples

Quick benchmark with small dataset:

```bash
./benchmark --size 10
```

Comprehensive benchmark with large dataset:

```bash
./benchmark --size 100
```

CSV output for analysis:

```bash
./benchmark --size 50 --output csv > results.csv
```

Compare only Azure performance across different sizes:

```bash
./benchmark --azure-only --size 25
./benchmark --azure-only --size 50
./benchmark --azure-only --size 100
```

## Output Format

### Text Output (Default)

The text output provides a human-readable comparison:

```
=================================================================
                   BENCHMARK RESULTS
=================================================================

Local SQLite (default VFS):
  Elapsed time:  12.345 seconds

Azure SQLite (sqlite-objs VFS):
  Elapsed time:  45.678 seconds

Performance Comparison:
  Azure vs Local:  3.70x (270.0% slower)

=================================================================
```

### CSV Output

The CSV output is suitable for scripting and analysis:

```csv
test,elapsed_seconds,status
local,12.345,success
azure,45.678,success
```

## What speedtest1 Tests

The speedtest1 benchmark measures:

1. **Table creation** — CREATE TABLE statements
2. **Insertions** — INSERT operations (both prepared and dynamic SQL)
3. **Queries** — SELECT statements with various WHERE clauses
4. **Updates** — UPDATE operations
5. **Deletions** — DELETE operations
6. **Transactions** — BEGIN/COMMIT overhead
7. **Index operations** — CREATE INDEX and indexed queries
8. **Sorting** — ORDER BY performance

The `--size` parameter scales the number of operations proportionally.

## Performance Expectations

Expected performance characteristics:

- **Local SQLite**: Fast baseline (local disk I/O)
- **Azure SQLite**: Higher latency due to network round-trips

Typical slowdown factors for sqlite-objs:
- **Best case** (large transactions, good batching): 2-5x slower
- **Worst case** (many small operations): 10-50x slower

The in-memory cache (D4) significantly reduces the gap by eliminating repeated blob downloads.

## Troubleshooting

### Azure benchmark fails with "Failed to register sqlite-objs VFS"

Check that the production build was used:
```bash
make clean
make all-production
```

### Azure benchmark fails with environment variable errors

Verify all required variables are set:
```bash
echo $AZURE_STORAGE_ACCOUNT
echo $AZURE_STORAGE_CONTAINER
echo $AZURE_STORAGE_KEY  # or AZURE_STORAGE_SAS
```

### Benchmark runs but shows zero time

The dataset may be too small. Increase the size parameter:
```bash
./benchmark --size 50
```

## Integration with CI/CD

Example GitHub Actions workflow:

```yaml
- name: Run benchmarks
  env:
    AZURE_STORAGE_ACCOUNT: ${{ secrets.AZURE_STORAGE_ACCOUNT }}
    AZURE_STORAGE_KEY: ${{ secrets.AZURE_STORAGE_KEY }}
    AZURE_STORAGE_CONTAINER: ci-benchmarks
  run: |
    cd benchmark
    make all-production
    ./benchmark --size 50 --output csv > results.csv
    
- name: Upload results
  uses: actions/upload-artifact@v3
  with:
    name: benchmark-results
    path: benchmark/results.csv
```

## Files

- `benchmark.c` — Main harness implementation
- `speedtest1.c` — SQLite's official speedtest1 benchmark
- `Makefile` — Build system
- `README.md` — This file

## License

This benchmark harness is part of sqlite-objs and is licensed under the MIT License.
