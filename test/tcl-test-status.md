# SQLite TCL Test Suite Status

Tracking which official SQLite TCL tests pass with the `sqlite-objs` VFS
(using mock in-memory Azure backend via `mock_azure_ops.c`).

**Binary:** `build/testfixture-objs`
**SQLite version:** 3.52.0
**VFS mode:** `sqlite-objs` registered as default VFS, backed by mock Azure ops
**Run command:** `make test-tcl` (full) or `make test-tcl-quick` (smoke test)

## Passing (78 test files — 0 errors each)

| Test File | Tests | Category |
|-----------|-------|----------|
| select1.test | 192 | Core SELECT |
| select2.test | 21 | SELECT subquery |
| select3.test | 91 | SELECT compound |
| select4.test | 124 | SELECT UNION |
| select5.test | 35 | SELECT aggregate |
| select6.test | 88 | SELECT subquery |
| select7.test | 27 | SELECT optimization |
| select8.test | 4 | SELECT edge cases |
| select9.test | 36,717 | SELECT fuzz |
| selectA.test | 231 | SELECT advanced |
| selectB.test | 171 | SELECT advanced |
| selectC.test | 30 | SELECT advanced |
| selectD.test | 32 | SELECT advanced |
| selectE.test | 8 | SELECT advanced |
| selectF.test | 3 | SELECT advanced |
| insert.test | 84 | INSERT |
| insert2.test | 30 | INSERT edge cases |
| update.test | 141 | UPDATE |
| delete.test | 68 | DELETE |
| trans.test | 329 | Transactions |
| trans2.test | 408 | Transaction edge cases |
| index.test | 121 | Index creation/usage |
| index2.test | 8 | Index edge cases |
| index3.test | 12 | Index optimization |
| join.test | 192 | JOIN operations |
| join2.test | 60 | JOIN edge cases |
| join3.test | 130 | JOIN optimization |
| join4.test | 9 | JOIN corner cases |
| where.test | 318 | WHERE clause |
| where2.test | 90 | WHERE optimization |
| where3.test | 106 | WHERE complex |
| where4.test | 42 | WHERE edge cases |
| where5.test | 51 | WHERE optimization |
| where6.test | 21 | WHERE optimization |
| where7.test | 2,027 | WHERE fuzz |
| where8.test | 2,052 | WHERE fuzz |
| where9.test | 90 | WHERE complex |
| trigger1.test | 89 | Triggers |
| trigger2.test | 112 | Trigger edge cases |
| view.test | 119 | Views |
| subquery.test | 82 | Subqueries |
| lock.test | 65 | File locking |
| lock2.test | 12 | Lock edge cases |
| lock3.test | 9 | Lock contention |
| wal.test | 581 | Write-Ahead Logging |
| wal2.test | 266 | WAL edge cases |
| mmap1.test | 85 | Memory-mapped I/O |
| mmap2.test | 153 | mmap edge cases |
| vacuum.test | 47 | VACUUM |
| vacuum2.test | 31 | VACUUM edge cases |
| attach.test | 113 | ATTACH DATABASE |
| attach2.test | 71 | ATTACH edge cases |
| alter.test | 120 | ALTER TABLE |
| alter2.test | 47 | ALTER TABLE |
| alter3.test | 60 | ALTER TABLE |
| table.test | 97 | CREATE/DROP TABLE |
| tableapi.test | 157 | Table C API |
| types.test | 56 | Type affinity |
| types2.test | 399 | Type coercion |
| expr.test | 661 | Expressions |
| coalesce.test | 10 | COALESCE function |
| cast.test | 135 | CAST expressions |
| check.test | 110 | CHECK constraints |
| cse.test | 125 | Common subexpression |
| conflict.test | 148 | ON CONFLICT |
| collate1.test | 73 | Collation sequences |
| collate2.test | 120 | Collation comparison |
| collate3.test | 73 | Collation sort |
| collate4.test | 108 | Collation edge cases |
| collate5.test | 38 | Collation grouping |
| collate6.test | 16 | Collation advanced |
| collate9.test | 23 | Collation unicode |
| date.test | 1,684 | Date/time functions |
| distinctagg.test | 71 | DISTINCT aggregate |
| distinct.test | 87 | DISTINCT queries |
| enc.test | 114 | Text encoding |
| enc2.test | 90 | Encoding conversion |
| enc3.test | 12 | Encoding edge cases |

**Total: ~48,000+ individual test assertions passing**

## Not Supported (skip — not VFS issues)

| Test File | Reason |
|-----------|--------|
| func.test | 9 errors / 15,031 tests — `Inf` vs `inf` capitalization (platform printf difference, not VFS) |
| types3.test | 1 error / 19 tests — TCL 8.5 string representation difference (`"text"` vs `"string text"`) |

## Not Yet Tested

The full SQLite source tree contains ~1,187 test files. Categories to explore next:

- **Crash recovery** (`crash*.test`) — important for our VFS durability guarantees
- **Corruption detection** (`corrupt*.test`) — should be VFS-agnostic
- **Foreign keys** (`fkey*.test`) — SQL logic, should pass
- **Full-text search** (`fts3*.test`, `fts5*.test`) — may need feature flags
- **R-tree** (`rtree*.test`) — spatial index tests
- **Multi-process** (`multiplex*.test`, `lock5.test`) — likely fails (mock is single-process)
- **Backup API** (`backup*.test`) — may need investigation
- **VFS-specific** (`vfs*.test`) — tests VFS interface assumptions directly
- **WAL advanced** (`wal3.test`–`wal9.test`) — needs investigation
- **IO error simulation** (`ioerr*.test`) — uses sqlite3_test_control for fault injection
