# TPC-C OLTP Benchmark for SQLite

A custom TPC-C style benchmark for measuring OLTP performance of local SQLite versus sqlite-objs (Azure blob-backed SQLite).

## Overview

This benchmark implements the core TPC-C transactions to measure realistic OLTP workload performance:

- **New Order** (~45% of transactions) - Creates new orders with 5-15 line items
- **Payment** (~43% of transactions) - Records customer payments
- **Order Status** (~12% of transactions) - Queries order status (read-only)

## TPC-C Schema

The benchmark uses the standard TPC-C schema with these tables:

- `warehouse` - Warehouses (scaled by W parameter)
- `district` - 10 districts per warehouse
- `customer` - 3,000 customers per district
- `item` - 100,000 items (fixed, shared across warehouses)
- `stock` - Stock levels per warehouse/item
- `orders` / `order_line` / `new_order` - Order tracking
- `history` - Payment history

Scale factor is controlled by the number of warehouses.

## Building

### Local-only version (no Azure support):

```bash
make all
```

This builds `tpcc-local` which runs against standard SQLite.

### Full version (with Azure support):

```bash
make all-production
```

This builds both `tpcc-local` and `tpcc-azure` (which can use the sqlite-objs VFS).

## Usage

### Local SQLite Benchmark

```bash
./tpcc-local --local --warehouses 1 --duration 60
```

Options:
- `--warehouses N` - Number of warehouses (scale factor, default: 1)
- `--duration S` - Benchmark duration in seconds (default: 60)

### Azure SQLite Benchmark

First, set up Azure credentials:

```bash
export AZURE_STORAGE_ACCOUNT=myaccount
export AZURE_STORAGE_CONTAINER=tpcc-bench
export AZURE_STORAGE_KEY=mykey        # OR use SAS
export AZURE_STORAGE_SAS=mysastoken   # OR use KEY
```

Then run the benchmark:

```bash
./tpcc-azure --azure --warehouses 1 --duration 60
```

## Benchmark Workflow

1. **Schema Creation** - Creates TPC-C tables if they don't exist
2. **Data Loading** - Populates tables according to TPC-C spec:
   - 100,000 items
   - W warehouses
   - 10 districts per warehouse
   - 3,000 customers per district
   - Initial stock and orders
3. **Benchmark Run** - Executes transaction mix for specified duration
4. **Results** - Reports throughput and latency statistics

## Example Output

```
TPC-C Benchmark Results
=================================================================
Mode:       local (default VFS)
Warehouses: 1
Duration:   60.2 seconds
Threads:    1

Transaction Mix:
  New Order:    1,234 (45.2%)  avg:   25.3ms  p50:   18.5ms  p95:   62.1ms  p99:   95.3ms
  Payment:      1,180 (43.2%)  avg:   12.1ms  p50:    9.2ms  p95:   28.4ms  p99:   45.7ms
  Order Status:   317 (11.6%)  avg:    5.3ms  p50:    4.1ms  p95:   12.8ms  p99:   18.2ms

Total:      2,731 transactions
Throughput: 45.4 tps
=================================================================
```

## Performance Comparison

Run both benchmarks to compare local vs Azure performance:

```bash
# Local baseline
./tpcc-local --local --warehouses 1 --duration 60

# Azure comparison (with env vars set)
./tpcc-azure --azure --warehouses 1 --duration 60
```

Expected results:
- **Local**: ~40-100 tps depending on hardware
- **Azure**: ~5-20 tps depending on network latency and cache effectiveness

The Azure version will show higher latencies due to network round-trips, but the in-memory cache significantly reduces the performance gap compared to uncached remote storage.

## Database Files

- **Local mode**: Uses `tpcc_local.db` in the current directory
- **Azure mode**: Uses `tpcc.db` as the blob name in the configured container

The database is reused across runs. Delete it to force a fresh data load:

```bash
# Local
rm tpcc_local.db tpcc_local.db-journal

# Azure (delete from blob storage)
# Use Azure CLI or portal to delete tpcc.db blob
```

## Scale Factors

| Warehouses | Items   | Customers | Database Size (approx) |
|-----------|---------|-----------|------------------------|
| 1         | 100,000 | 30,000    | ~100 MB               |
| 2         | 100,000 | 60,000    | ~180 MB               |
| 5         | 100,000 | 150,000   | ~400 MB               |
| 10        | 100,000 | 300,000   | ~750 MB               |

Larger scale factors increase both database size and transaction complexity.

## Workload Characterization

This section describes the I/O patterns of each transaction type and why they matter for comparing local SQLite versus sqlite-objs over the network.

### Transaction Types

| Transaction    | Mix   | Reads | Writes | Description |
|---------------|-------|-------|--------|-------------|
| **New Order** | ~45%  | 3-18  | 5-20   | Heavy write workload. Reads district/item/stock, writes order/order_line/stock updates. 5-15 line items per order means multiple round-trips. |
| **Payment**   | ~43%  | 0     | 4      | Pure write workload. Updates warehouse/district/customer balances and inserts history. Fixed 4 writes per transaction. |
| **Order Status** | ~12% | 3-18 | 0     | Pure read workload. Reads customer info, finds most recent order, fetches all order lines. Variable reads based on order size. |

### I/O Patterns

**Write-Heavy Operations (New Order, Payment):**
- These transactions dominate the mix (~88% combined)
- Each write requires a network round-trip with sqlite-objs
- New Order is particularly expensive due to multiple order line inserts
- Payment is more efficient with fixed, predictable writes

**Read-Heavy Operations (Order Status):**
- Only ~12% of transactions but reveals cache effectiveness
- Customer and order lookups benefit heavily from caching
- Sequential order line scans are cache-friendly

### Network Latency Impact

| Operation Type | Local SQLite | sqlite-objs (uncached) | sqlite-objs (cached) |
|---------------|--------------|--------------------|--------------------|
| Single read   | <1ms         | 20-100ms           | <1ms               |
| Single write  | <1ms         | 20-100ms           | 20-100ms (always remote) |
| New Order (avg) | 10-30ms    | 200-500ms          | 100-300ms          |
| Payment       | 5-15ms       | 80-200ms           | 80-200ms           |
| Order Status  | 2-8ms        | 60-200ms           | 5-20ms             |

**Key Observations:**
- **Writes hurt most**: Every write goes to Azure, no caching helps
- **New Order suffers most**: Multiple writes per transaction compound latency
- **Order Status benefits most from caching**: Read-only with cacheable data
- **Payment is predictable**: Fixed write count makes latency consistent

### Why TPC-C for sqlite-objs Testing

1. **Realistic Mix**: Combines reads and writes like real applications
2. **Variable Transaction Sizes**: New Order's 5-15 items tests both small and large transactions
3. **Cache Testing**: Order Status reveals how well the read cache performs
4. **Write Amplification**: Shows the true cost of network round-trips
5. **Baseline Comparison**: Standard benchmark allows comparison with other systems

### Optimization Opportunities

When analyzing sqlite-objs vs local results, look for:
- **Cache hit rate** on Order Status transactions
- **Write batching** effectiveness in New Order
- **Latency distribution** (p50 vs p99) to identify network variability
- **Throughput degradation** ratio between local and Azure modes

## Limitations

- Single-threaded execution (multi-threading planned for future)
- Simplified transaction mix (no Delivery or Stock-Level transactions)
- No think time between transactions (measures maximum throughput)
- All order lines from home warehouse (no remote warehouse logic)

## See Also

- [TPC-C Specification](http://www.tpc.org/tpcc/) - Official TPC-C benchmark spec
- `../benchmark.c` - Speedtest1-based benchmark harness
- `../../src/sqlite_objs_vfs.c` - Azure VFS implementation
