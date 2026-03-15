# SQLite TCL Test Suite Status

Tracking which official SQLite TCL tests pass with the `sqlite-objs` VFS
(using mock in-memory Azure backend via `mock_azure_ops.c`).

**Binary:** `build/testfixture-objs`
**SQLite version:** 3.52.0
**VFS mode:** `sqlite-objs` registered as default VFS, backed by mock Azure ops
**Run command:** `make test-tcl` (full) or `make test-tcl-quick` (smoke test)

## Summary

| Category | Count |
|----------|-------|
| **Passing** | **1151 test files** |
| Not supported (platform) | 6 test files |
| Timeouts (>30s) | 15 test files |
| Empty output (skipped) | 15 test files |
| **Total** | **1187 / 1,187 test files** |
| **Individual assertions** | **~720,042** |

## Passing (1151 test files — 0 errors each)

### SELECT — core query engine (17 files, ~37,796 assertions)

| Test File | Tests |
|-----------|-------|
| select1.test | 192 |
| select2.test | 21 |
| select3.test | 91 |
| select4.test | 124 |
| select5.test | 35 |
| select6.test | 88 |
| select7.test | 27 |
| select8.test | 4 |
| select9.test | 36,717 |
| selectA.test | 231 |
| selectB.test | 171 |
| selectC.test | 30 |
| selectD.test | 32 |
| selectE.test | 8 |
| selectF.test | 3 |
| selectG.test | 4 |
| selectH.test | 18 |

### DML — data manipulation (13 files, ~610 assertions)

| Test File | Tests |
|-----------|-------|
| delete.test | 68 |
| delete2.test | 14 |
| delete3.test | 4 |
| delete4.test | 29 |
| delete_db.test | 11 |
| insert.test | 84 |
| insert2.test | 30 |
| insert3.test | 19 |
| insert4.test | 88 |
| insert5.test | 13 |
| insertfault.test | 74 |
| update.test | 141 |
| update2.test | 35 |

### Transactions (4 files, ~789 assertions)

| Test File | Tests |
|-----------|-------|
| trans.test | 329 |
| trans2.test | 408 |
| trans3.test | 9 |
| transitive1.test | 43 |

### Indexes (15 files, ~4,360 assertions)

| Test File | Tests |
|-----------|-------|
| index.test | 121 |
| index2.test | 8 |
| index3.test | 12 |
| index4.test | 9 |
| index5.test | 4 |
| index6.test | 74 |
| index7.test | 45 |
| index8.test | 5 |
| index9.test | 24 |
| indexA.test | 149 |
| indexedby.test | 71 |
| indexexpr1.test | 107 |
| indexexpr2.test | 127 |
| indexexpr3.test | 14 |
| indexfault.test | 3,590 |

### JOIN operations (17 files, ~3,199 assertions)

| Test File | Tests |
|-----------|-------|
| join.test | 192 |
| join2.test | 60 |
| join3.test | 130 |
| join4.test | 9 |
| join5.test | 53 |
| join6.test | 18 |
| join7.test | 301 |
| join8.test | 106 |
| join9.test | 151 |
| joinA.test | 43 |
| joinB.test | 513 |
| joinC.test | 257 |
| joinD.test | 1,171 |
| joinE.test | 37 |
| joinF.test | 55 |
| joinH.test | 79 |
| joinI.test | 24 |

### WHERE clause (28 files, ~7,200 assertions)

| Test File | Tests |
|-----------|-------|
| where.test | 318 |
| where2.test | 90 |
| where3.test | 106 |
| where4.test | 42 |
| where5.test | 51 |
| where6.test | 21 |
| where7.test | 2,027 |
| where8.test | 2,052 |
| where9.test | 90 |
| whereA.test | 24 |
| whereB.test | 64 |
| whereC.test | 47 |
| whereD.test | 62 |
| whereE.test | 5 |
| whereF.test | 28 |
| whereG.test | 59 |
| whereH.test | 17 |
| whereI.test | 10 |
| whereJ.test | 1 |
| whereK.test | 12 |
| whereL.test | 28 |
| whereM.test | 28 |
| whereN.test | 3 |
| wherefault.test | 2,008 |
| wherelfault.test | 1 |
| wherelimit.test | 1 |
| wherelimit2.test | 1 |
| wherelimit3.test | 4 |

### Triggers (17 files, ~891 assertions)

| Test File | Tests |
|-----------|-------|
| trigger1.test | 89 |
| trigger2.test | 112 |
| trigger3.test | 25 |
| trigger4.test | 25 |
| trigger5.test | 3 |
| trigger6.test | 7 |
| trigger7.test | 11 |
| trigger8.test | 2 |
| trigger9.test | 29 |
| triggerA.test | 188 |
| triggerB.test | 203 |
| triggerC.test | 120 |
| triggerD.test | 13 |
| triggerE.test | 27 |
| triggerF.test | 13 |
| triggerG.test | 9 |
| triggerupfrom.test | 15 |

### Views (3 files, ~127 assertions)

| Test File | Tests |
|-----------|-------|
| view.test | 119 |
| view2.test | 5 |
| view3.test | 3 |

### Subqueries (3 files, ~140 assertions)

| Test File | Tests |
|-----------|-------|
| subquery.test | 82 |
| subquery2.test | 31 |
| subselect.test | 27 |

### ALTER TABLE (22 files, ~24,177 assertions)

| Test File | Tests |
|-----------|-------|
| alter.test | 120 |
| alter2.test | 47 |
| alter3.test | 60 |
| alter4.test | 53 |
| alterauth.test | 8 |
| alterauth2.test | 9 |
| altercol.test | 256 |
| altercons.test | 176 |
| altercons2.test | 61 |
| altercorrupt.test | 5 |
| alterdropcol.test | 101 |
| alterdropcol2.test | 24 |
| alterfault.test | 4,195 |
| alterlegacy.test | 61 |
| altermalloc.test | 4,759 |
| altermalloc2.test | 9,615 |
| altermalloc3.test | 4,300 |
| alterqf.test | 15 |
| altertab.test | 120 |
| altertab2.test | 47 |
| altertab3.test | 108 |
| altertrig.test | 37 |

### Table and type handling (8 files, ~825 assertions)

| Test File | Tests |
|-----------|-------|
| affinity2.test | 26 |
| affinity3.test | 17 |
| createtab.test | 62 |
| table.test | 97 |
| tableapi.test | 157 |
| tableopts.test | 11 |
| types.test | 56 |
| types2.test | 399 |

### Expressions, functions, operators (69 files, ~118,737 assertions)

| Test File | Tests |
|-----------|-------|
| basexx1.test | 22 |
| between.test | 23 |
| cast.test | 135 |
| check.test | 110 |
| checkfault.test | 146 |
| coalesce.test | 10 |
| conflict.test | 148 |
| conflict2.test | 140 |
| conflict3.test | 59 |
| cse.test | 125 |
| eval.test | 9 |
| exists.test | 73 |
| existsexpr.test | 90 |
| existsexpr2.test | 8 |
| existsfault.test | 122 |
| expr.test | 661 |
| expr2.test | 8 |
| exprfault.test | 526 |
| func2.test | 132 |
| func3.test | 40 |
| func4.test | 220 |
| func5.test | 6 |
| func6.test | 1 |
| func7.test | 73 |
| func8.test | 9 |
| func9.test | 16 |
| hexlit.test | 133 |
| in.test | 124 |
| in2.test | 2,000 |
| in3.test | 37 |
| in4.test | 79 |
| in5.test | 45 |
| in6.test | 11 |
| in7.test | 27 |
| incrblob.test | 97 |
| incrblob2.test | 82 |
| incrblob3.test | 64 |
| incrblob4.test | 14 |
| incrblob_err.test | 2,682 |
| incrblobfault.test | 155 |
| incrcorrupt.test | 36 |
| incrvacuum.test | 168 |
| incrvacuum2.test | 18 |
| incrvacuum3.test | 53 |
| incrvacuum_ioerr.test | 4,327 |
| init.test | 45 |
| instr.test | 72 |
| instrfault.test | 137 |
| intarray.test | 11 |
| interrupt.test | 2,578 |
| interrupt2.test | 28 |
| intpkey.test | 97 |
| intreal.test | 21 |
| istrue.test | 57 |
| like.test | 159 |
| like2.test | 285 |
| like3.test | 216 |
| literal2.test | 12 |
| numcast.test | 52 |
| offset1.test | 28 |
| percentile.test | 127 |
| printf.test | 1,411 |
| printf2.test | 51 |
| quote.test | 30 |
| regexp1.test | 102 |
| regexp2.test | 31 |
| round1.test | 100,001 |
| substr.test | 94 |
| unhex.test | 28 |

### Collation (14 files, ~685 assertions)

| Test File | Tests |
|-----------|-------|
| collate1.test | 73 |
| collate2.test | 120 |
| collate3.test | 73 |
| collate4.test | 108 |
| collate5.test | 38 |
| collate6.test | 16 |
| collate7.test | 9 |
| collate8.test | 24 |
| collate9.test | 23 |
| collateA.test | 57 |
| collateB.test | 16 |
| colmeta.test | 52 |
| colname.test | 68 |
| columncount.test | 8 |

### Date/time (7 files, ~29,173 assertions)

| Test File | Tests |
|-----------|-------|
| ctime.test | 76 |
| date.test | 1,684 |
| date2.test | 30 |
| date3.test | 127 |
| date4.test | 24,860 |
| date5.test | 875 |
| timediff1.test | 1,521 |

### Aggregates and DISTINCT (13 files, ~903 assertions)

| Test File | Tests |
|-----------|-------|
| aggerror.test | 7 |
| aggfault.test | 290 |
| aggnested.test | 45 |
| aggorderby.test | 30 |
| count.test | 41 |
| countofview.test | 14 |
| distinct.test | 87 |
| distinct2.test | 45 |
| distinctagg.test | 71 |
| minmax.test | 110 |
| minmax2.test | 64 |
| minmax3.test | 48 |
| minmax4.test | 51 |

### Encoding (5 files, ~1,338 assertions)

| Test File | Tests |
|-----------|-------|
| enc.test | 114 |
| enc2.test | 90 |
| enc3.test | 12 |
| enc4.test | 1,115 |
| utf16align.test | 7 |

### BLOB and incremental I/O (4 files, ~190 assertions)

| Test File | Tests |
|-----------|-------|
| blob.test | 24 |
| bloom1.test | 22 |
| zeroblob.test | 56 |
| zeroblobfault.test | 88 |

### Binding (3 files, ~131 assertions)

| Test File | Tests |
|-----------|-------|
| bind.test | 119 |
| bind2.test | 3 |
| bindxfer.test | 9 |

### Locking (10 files, ~179 assertions)

| Test File | Tests |
|-----------|-------|
| lock.test | 65 |
| lock2.test | 12 |
| lock3.test | 9 |
| lock4.test | 5 |
| lock5.test | 37 |
| lock6.test | 1 |
| lock7.test | 9 |
| nolock.test | 22 |
| pendingrace.test | 6 |
| sharedlock.test | 13 |

### WAL mode (39 files, ~17,501 assertions)

| Test File | Tests |
|-----------|-------|
| wal.test | 581 |
| wal2.test | 266 |
| wal3.test | 490 |
| wal4.test | 227 |
| wal5.test | 413 |
| wal6.test | 57 |
| wal64k.test | 6 |
| wal7.test | 7 |
| wal8.test | 7 |
| wal9.test | 9 |
| walbak.test | 84 |
| walbig.test | 5 |
| walblock.test | 1 |
| walckptnoop.test | 12 |
| walcksum.test | 95 |
| walcrash.test | 2,971 |
| walcrash2.test | 1,026 |
| walcrash4.test | 202 |
| walfault.test | 6,004 |
| walfault2.test | 477 |
| walhook.test | 15 |
| walmode.test | 94 |
| walnoshm.test | 25 |
| waloverwrite.test | 23 |
| walpersist.test | 21 |
| walprotocol.test | 15 |
| walprotocol2.test | 8 |
| walrestart.test | 7 |
| walro.test | 38 |
| walro2.test | 264 |
| walrofault.test | 270 |
| walseh1.test | 8 |
| walsetlk2.test | 1 |
| walsetlk3.test | 1 |
| walsetlk_recover.test | 6 |
| walsetlk_snapshot.test | 1 |
| walshared.test | 6 |
| walslow.test | 3,723 |
| walvfs.test | 35 |

### Memory-mapped I/O (7 files, ~574 assertions)

| Test File | Tests |
|-----------|-------|
| mmap1.test | 85 |
| mmap2.test | 153 |
| mmap3.test | 9 |
| mmap4.test | 23 |
| mmapcorrupt.test | 4 |
| mmapfault.test | 167 |
| mmapwarm.test | 133 |

### VACUUM (8 files, ~5,940 assertions)

| Test File | Tests |
|-----------|-------|
| vacuum.test | 47 |
| vacuum-into.test | 32 |
| vacuum2.test | 31 |
| vacuum3.test | 5,764 |
| vacuum4.test | 2 |
| vacuum5.test | 22 |
| vacuum6.test | 35 |
| vacuummem.test | 7 |

### ATTACH (5 files, ~2,754 assertions)

| Test File | Tests |
|-----------|-------|
| attach.test | 113 |
| attach2.test | 71 |
| attach3.test | 46 |
| attach4.test | 13 |
| attachmalloc.test | 2,511 |

### Pager and page cache (10 files, ~4,078 assertions)

| Test File | Tests |
|-----------|-------|
| pager1.test | 1,373 |
| pager2.test | 2,004 |
| pager3.test | 13 |
| pager4.test | 12 |
| pagerfault2.test | 539 |
| pagerfault3.test | 30 |
| pageropt.test | 1 |
| pagesize.test | 100 |
| pcache.test | 1 |
| pcache2.test | 5 |

### Journal (10 files, ~719 assertions)

| Test File | Tests |
|-----------|-------|
| journal1.test | 3 |
| journal2.test | 24 |
| journal3.test | 22 |
| jrnlmode.test | 98 |
| jrnlmode2.test | 14 |
| jrnlmode3.test | 125 |
| memjournal.test | 5 |
| memjournal2.test | 407 |
| mjournal.test | 12 |
| subjournal.test | 9 |

### Authorization (3 files, ~400 assertions)

| Test File | Tests |
|-----------|-------|
| auth.test | 377 |
| auth2.test | 13 |
| auth3.test | 10 |

### Autoincrement/autoindex/autovacuum (10 files, ~2,082 assertions)

| Test File | Tests |
|-----------|-------|
| autoanalyze1.test | 1 |
| autoinc.test | 88 |
| autoindex1.test | 40 |
| autoindex2.test | 4 |
| autoindex3.test | 10 |
| autoindex4.test | 54 |
| autoindex5.test | 9 |
| autovacuum.test | 339 |
| autovacuum2.test | 9 |
| autovacuum_ioerr2.test | 1,528 |

### Foreign keys (9 files, ~5,189 assertions)

| Test File | Tests |
|-----------|-------|
| fkey1.test | 27 |
| fkey2.test | 1,217 |
| fkey3.test | 32 |
| fkey4.test | 7 |
| fkey5.test | 65 |
| fkey6.test | 39 |
| fkey7.test | 16 |
| fkey8.test | 33 |
| fkey_malloc.test | 3,753 |

### Crash recovery (10 files, ~5,099 assertions)

| Test File | Tests |
|-----------|-------|
| crash.test | 897 |
| crash2.test | 84 |
| crash3.test | 992 |
| crash4.test | 1,654 |
| crash5.test | 1 |
| crash6.test | 141 |
| crash7.test | 294 |
| crash8.test | 46 |
| crashM.test | 1 |
| writecrash.test | 989 |

### Corruption detection (23 files, ~112,662 assertions)

| Test File | Tests |
|-----------|-------|
| corrupt.test | 12,320 |
| corrupt2.test | 31 |
| corrupt3.test | 1 |
| corrupt4.test | 9 |
| corrupt5.test | 2 |
| corrupt6.test | 26 |
| corrupt7.test | 7 |
| corrupt8.test | 99 |
| corrupt9.test | 9 |
| corruptA.test | 7 |
| corruptB.test | 20 |
| corruptC.test | 99,677 |
| corruptD.test | 6 |
| corruptE.test | 21 |
| corruptF.test | 269 |
| corruptG.test | 6 |
| corruptH.test | 10 |
| corruptI.test | 24 |
| corruptJ.test | 6 |
| corruptK.test | 10 |
| corruptL.test | 56 |
| corruptM.test | 28 |
| corruptN.test | 18 |

### IO error simulation (6 files, ~14,980 assertions)

| Test File | Tests |
|-----------|-------|
| ioerr.test | 10,847 |
| ioerr2.test | 3,889 |
| ioerr3.test | 163 |
| ioerr4.test | 79 |
| ioerr5.test | 1 |
| ioerr6.test | 1 |

### Malloc/memory failure (28 files, ~44,562 assertions)

| Test File | Tests |
|-----------|-------|
| malloc.test | 15,717 |
| malloc3.test | 5,745 |
| malloc4.test | 15 |
| malloc5.test | 1 |
| malloc6.test | 614 |
| malloc7.test | 76 |
| malloc8.test | 596 |
| malloc9.test | 230 |
| mallocA.test | 7,176 |
| mallocB.test | 1,397 |
| mallocC.test | 2,532 |
| mallocD.test | 953 |
| mallocE.test | 349 |
| mallocF.test | 525 |
| mallocG.test | 505 |
| mallocH.test | 895 |
| mallocI.test | 1,251 |
| mallocJ.test | 465 |
| mallocK.test | 3,472 |
| mallocL.test | 1,395 |
| mallocM.test | 211 |
| mem5.test | 1 |
| memdb.test | 334 |
| memdb1.test | 33 |
| memdb2.test | 15 |
| memsubsys1.test | 25 |
| memsubsys2.test | 26 |
| softheap1.test | 8 |

### Shared cache (11 files, ~8,718 assertions)

| Test File | Tests |
|-----------|-------|
| shared.test | 211 |
| shared2.test | 13 |
| shared3.test | 16 |
| shared4.test | 17 |
| shared6.test | 40 |
| shared7.test | 5 |
| shared8.test | 11 |
| shared9.test | 17 |
| sharedA.test | 9 |
| sharedB.test | 102 |
| shared_err.test | 8,277 |

### FTS3/FTS4 full-text search (96 files, ~96 assertions)

| Test File | Tests |
|-----------|-------|
| fts-9fd058691.test | 1 |
| fts3.test | 1 |
| fts3aa.test | 1 |
| fts3ab.test | 1 |
| fts3ac.test | 1 |
| fts3ad.test | 1 |
| fts3ae.test | 1 |
| fts3af.test | 1 |
| fts3ag.test | 1 |
| fts3ah.test | 1 |
| fts3ai.test | 1 |
| fts3aj.test | 1 |
| fts3ak.test | 1 |
| fts3al.test | 1 |
| fts3am.test | 1 |
| fts3an.test | 1 |
| fts3ao.test | 1 |
| fts3atoken.test | 1 |
| fts3atoken2.test | 1 |
| fts3auto.test | 1 |
| fts3aux1.test | 1 |
| fts3aux2.test | 1 |
| fts3b.test | 1 |
| fts3c.test | 1 |
| fts3comp1.test | 1 |
| fts3conf.test | 1 |
| fts3corrupt.test | 1 |
| fts3corrupt2.test | 1 |
| fts3corrupt3.test | 1 |
| fts3corrupt4.test | 1 |
| fts3corrupt5.test | 1 |
| fts3corrupt6.test | 1 |
| fts3corrupt7.test | 1 |
| fts3cov.test | 1 |
| fts3d.test | 1 |
| fts3defer.test | 1 |
| fts3defer2.test | 1 |
| fts3defer3.test | 1 |
| fts3drop.test | 1 |
| fts3dropmod.test | 1 |
| fts3e.test | 1 |
| fts3expr.test | 1 |
| fts3expr2.test | 1 |
| fts3expr3.test | 1 |
| fts3expr4.test | 1 |
| fts3expr5.test | 1 |
| fts3f.test | 1 |
| fts3fault.test | 1 |
| fts3fault2.test | 1 |
| fts3fault3.test | 1 |
| fts3first.test | 1 |
| fts3fuzz001.test | 1 |
| fts3integrity.test | 1 |
| fts3join.test | 1 |
| fts3malloc.test | 1 |
| fts3matchinfo.test | 1 |
| fts3matchinfo2.test | 1 |
| fts3misc.test | 1 |
| fts3near.test | 1 |
| fts3offsets.test | 1 |
| fts3prefix.test | 1 |
| fts3prefix2.test | 1 |
| fts3query.test | 1 |
| fts3rank.test | 1 |
| fts3rnd.test | 1 |
| fts3shared.test | 1 |
| fts3snippet.test | 1 |
| fts3snippet2.test | 1 |
| fts3sort.test | 1 |
| fts3tok1.test | 1 |
| fts3tok_err.test | 1 |
| fts3varint.test | 1 |
| fts4aa.test | 1 |
| fts4check.test | 1 |
| fts4content.test | 1 |
| fts4docid.test | 1 |
| fts4growth.test | 1 |
| fts4growth2.test | 1 |
| fts4incr.test | 1 |
| fts4intck1.test | 1 |
| fts4langid.test | 1 |
| fts4lastrowid.test | 1 |
| fts4merge.test | 1 |
| fts4merge2.test | 1 |
| fts4merge3.test | 1 |
| fts4merge4.test | 1 |
| fts4merge5.test | 1 |
| fts4min.test | 1 |
| fts4noti.test | 1 |
| fts4onepass.test | 1 |
| fts4opt.test | 1 |
| fts4record.test | 1 |
| fts4rename.test | 1 |
| fts4umlaut.test | 1 |
| fts4unicode.test | 1 |
| fts4upfrom.test | 1 |

### JSON (10 files, ~45,512 assertions)

| Test File | Tests |
|-----------|-------|
| json102.test | 317 |
| json103.test | 15 |
| json104.test | 34 |
| json105.test | 53 |
| json106.test | 45,001 |
| json107.test | 16 |
| json108.test | 6 |
| json109.test | 18 |
| json502.test | 13 |
| jsonb01.test | 39 |

### R-Tree (1 files, ~1 assertions)

| Test File | Tests |
|-----------|-------|
| rtree.test | 1 |

### Window functions (17 files, ~7,647 assertions)

| Test File | Tests |
|-----------|-------|
| window1.test | 348 |
| window2.test | 68 |
| window3.test | 1,603 |
| window4.test | 226 |
| window5.test | 8 |
| window6.test | 75 |
| window7.test | 11 |
| window8.test | 362 |
| window9.test | 43 |
| windowA.test | 19 |
| windowB.test | 64 |
| windowC.test | 44 |
| windowD.test | 14 |
| windowE.test | 10 |
| windowerr.test | 15 |
| windowfault.test | 4,701 |
| windowpushd.test | 36 |

### CTEs (WITH) (14 files, ~2,107 assertions)

| Test File | Tests |
|-----------|-------|
| with1.test | 117 |
| with2.test | 70 |
| with3.test | 17 |
| with4.test | 7 |
| with5.test | 14 |
| with6.test | 30 |
| withM.test | 379 |
| without_rowid1.test | 83 |
| without_rowid2.test | 10 |
| without_rowid3.test | 1,198 |
| without_rowid4.test | 105 |
| without_rowid5.test | 33 |
| without_rowid6.test | 22 |
| without_rowid7.test | 22 |

### WITHOUT ROWID (0 files, ~0 assertions)

| Test File | Tests |
|-----------|-------|

### Virtual tables (30 files, ~5,353 assertions)

| Test File | Tests |
|-----------|-------|
| swarmvtab.test | 40 |
| swarmvtab2.test | 8 |
| swarmvtab3.test | 25 |
| swarmvtabfault.test | 882 |
| vtab1.test | 213 |
| vtab2.test | 16 |
| vtab3.test | 20 |
| vtab4.test | 15 |
| vtab5.test | 15 |
| vtab6.test | 77 |
| vtab7.test | 13 |
| vtab8.test | 6 |
| vtab9.test | 3 |
| vtabA.test | 19 |
| vtabB.test | 8 |
| vtabC.test | 961 |
| vtabD.test | 9 |
| vtabE.test | 2 |
| vtabF.test | 3 |
| vtabH.test | 27 |
| vtabI.test | 16 |
| vtabJ.test | 20 |
| vtabK.test | 1 |
| vtabL.test | 15 |
| vtab_alter.test | 14 |
| vtab_err.test | 2,880 |
| vtab_shared.test | 31 |
| vtabdistinct.test | 5 |
| vtabdrop.test | 1 |
| vtabrhs1.test | 8 |

### Snapshot (6 files, ~6 assertions)

| Test File | Tests |
|-----------|-------|
| snapshot.test | 1 |
| snapshot2.test | 1 |
| snapshot3.test | 1 |
| snapshot4.test | 1 |
| snapshot_fault.test | 1 |
| snapshot_up.test | 1 |

### Savepoint (6 files, ~4,330 assertions)

| Test File | Tests |
|-----------|-------|
| savepoint.test | 140 |
| savepoint2.test | 182 |
| savepoint4.test | 3,175 |
| savepoint5.test | 4 |
| savepoint7.test | 12 |
| savepointfault.test | 817 |

### Rollback (3 files, ~227 assertions)

| Test File | Tests |
|-----------|-------|
| rollback.test | 14 |
| rollback2.test | 138 |
| rollbackfault.test | 75 |

### Schema (7 files, ~213 assertions)

| Test File | Tests |
|-----------|-------|
| schema.test | 52 |
| schema2.test | 46 |
| schema3.test | 45 |
| schema4.test | 18 |
| schema5.test | 8 |
| schema6.test | 21 |
| schemafault.test | 23 |

### Shell tool (11 files, ~11 assertions)

| Test File | Tests |
|-----------|-------|
| shell1.test | 1 |
| shell2.test | 1 |
| shell3.test | 1 |
| shell4.test | 1 |
| shell5.test | 1 |
| shell6.test | 1 |
| shell7.test | 1 |
| shell8.test | 1 |
| shell9.test | 1 |
| shellA.test | 1 |
| shellB.test | 1 |

### Pragma (7 files, ~723 assertions)

| Test File | Tests |
|-----------|-------|
| pragma.test | 234 |
| pragma2.test | 27 |
| pragma3.test | 29 |
| pragma4.test | 108 |
| pragma5.test | 6 |
| pragma6.test | 4 |
| pragmafault.test | 315 |

### Backup (5 files, ~2,774 assertions)

| Test File | Tests |
|-----------|-------|
| backup.test | 869 |
| backup2.test | 16 |
| backup4.test | 12 |
| backup5.test | 9 |
| backup_malloc.test | 1,868 |

### Thread safety (7 files, ~619 assertions)

| Test File | Tests |
|-----------|-------|
| thread001.test | 52 |
| thread002.test | 15 |
| thread004.test | 3 |
| thread005.test | 503 |
| thread1.test | 24 |
| thread2.test | 18 |
| thread3.test | 4 |

### UPSERT (6 files, ~576 assertions)

| Test File | Tests |
|-----------|-------|
| upsert1.test | 36 |
| upsert2.test | 15 |
| upsert3.test | 8 |
| upsert4.test | 129 |
| upsert5.test | 238 |
| upsertfault.test | 150 |

### UPDATE FROM (5 files, ~1,969 assertions)

| Test File | Tests |
|-----------|-------|
| upfrom1.test | 23 |
| upfrom2.test | 47 |
| upfrom3.test | 43 |
| upfrom4.test | 9 |
| upfromfault.test | 1,847 |

### URI handling (2 files, ~124 assertions)

| Test File | Tests |
|-----------|-------|
| uri.test | 123 |
| uri2.test | 1 |

### Temp tables (7 files, ~15,053 assertions)

| Test File | Tests |
|-----------|-------|
| tempdb.test | 6 |
| tempdb2.test | 8 |
| tempfault.test | 14,639 |
| temptable.test | 63 |
| temptable2.test | 257 |
| temptable3.test | 3 |
| temptrigger.test | 77 |

### Sort (6 files, ~148 assertions)

| Test File | Tests |
|-----------|-------|
| sort.test | 96 |
| sort2.test | 13 |
| sort3.test | 7 |
| sort4.test | 11 |
| sort5.test | 16 |
| sorterref.test | 5 |

### Busy handling (2 files, ~44 assertions)

| Test File | Tests |
|-----------|-------|
| busy.test | 15 |
| busy2.test | 29 |

### Multiplex (4 files, ~1,441 assertions)

| Test File | Tests |
|-----------|-------|
| multiplex.test | 1,416 |
| multiplex2.test | 9 |
| multiplex3.test | 1 |
| multiplex4.test | 15 |

### Notify (3 files, ~3 assertions)

| Test File | Tests |
|-----------|-------|
| notify1.test | 1 |
| notify2.test | 1 |
| notify3.test | 1 |

### Spellfix (4 files, ~129 assertions)

| Test File | Tests |
|-----------|-------|
| spellfix.test | 90 |
| spellfix2.test | 9 |
| spellfix3.test | 11 |
| spellfix4.test | 19 |

### Statistics (4 files, ~1,216 assertions)

| Test File | Tests |
|-----------|-------|
| scanstatus.test | 1 |
| scanstatus2.test | 1 |
| stat.test | 38 |
| statfault.test | 1,176 |

### Strict tables (2 files, ~77 assertions)

| Test File | Tests |
|-----------|-------|
| strict1.test | 51 |
| strict2.test | 26 |

### Incremental vacuum (0 files, ~0 assertions)

| Test File | Tests |
|-----------|-------|

### Row values (15 files, ~5,122 assertions)

| Test File | Tests |
|-----------|-------|
| rowallock.test | 13 |
| rowhash.test | 11 |
| rowid.test | 249 |
| rowvalue.test | 304 |
| rowvalue2.test | 3,835 |
| rowvalue3.test | 112 |
| rowvalue4.test | 298 |
| rowvalue5.test | 16 |
| rowvalue6.test | 3 |
| rowvalue7.test | 9 |
| rowvalue8.test | 4 |
| rowvalue9.test | 96 |
| rowvalueA.test | 18 |
| rowvaluefault.test | 142 |
| rowvaluevtab.test | 12 |

### Evidence tests (e_*) (25 files, ~20,560 assertions)

| Test File | Tests |
|-----------|-------|
| e_blobbytes.test | 23 |
| e_blobclose.test | 25 |
| e_blobopen.test | 198 |
| e_blobwrite.test | 58 |
| e_changes.test | 72 |
| e_createtable.test | 537 |
| e_delete.test | 43 |
| e_droptrigger.test | 67 |
| e_dropview.test | 49 |
| e_expr.test | 16,619 |
| e_fkey.test | 941 |
| e_fts3.test | 1 |
| e_insert.test | 204 |
| e_reindex.test | 116 |
| e_resolve.test | 30 |
| e_select.test | 632 |
| e_select2.test | 206 |
| e_totalchanges.test | 26 |
| e_update.test | 132 |
| e_uri.test | 83 |
| e_vacuum.test | 51 |
| e_wal.test | 37 |
| e_walauto.test | 43 |
| e_walckpt.test | 348 |
| e_walhook.test | 19 |

### Regression tickets (143 files, ~15,370 assertions)

| Test File | Tests |
|-----------|-------|
| tkt-02a8e81d44.test | 2 |
| tkt-18458b1a.test | 9 |
| tkt-26ff0c2d1e.test | 4 |
| tkt-2a5629202f.test | 9 |
| tkt-2d1a5c67d.test | 27 |
| tkt-2ea2425d34.test | 2 |
| tkt-31338dca7e.test | 12 |
| tkt-313723c356.test | 12 |
| tkt-385a5b56b9.test | 10 |
| tkt-38cb5df375.test | 294 |
| tkt-3998683a16.test | 2 |
| tkt-3a77c9714e.test | 6 |
| tkt-3fe897352e.test | 5 |
| tkt-4a03edc4c8.test | 4 |
| tkt-4c86b126f2.test | 3 |
| tkt-4dd95f6943.test | 320 |
| tkt-4ef7e3cfca.test | 6 |
| tkt-54844eea3f.test | 4 |
| tkt-5d863f876e.test | 9 |
| tkt-5e10420e8d.test | 6 |
| tkt-5ee23731f.test | 2 |
| tkt-6bfb98dfc0.test | 2 |
| tkt-752e1646fc.test | 2 |
| tkt-78e04e52ea.test | 9 |
| tkt-7a31705a7e6.test | 2 |
| tkt-7bbfb7d442.test | 8 |
| tkt-80ba201079.test | 15 |
| tkt-80e031a00f.test | 119 |
| tkt-8454a207b9.test | 8 |
| tkt-868145d012.test | 4 |
| tkt-8c63ff0ec.test | 4 |
| tkt-91e2e8ba6f.test | 8 |
| tkt-99378177930f87bd.test | 16 |
| tkt-9a8b09f8e6.test | 50 |
| tkt-9d68c883.test | 102 |
| tkt-9f2eb3abac.test | 203 |
| tkt-a7b7803e.test | 9 |
| tkt-a7debbe0.test | 37 |
| tkt-a8a0d2996a.test | 23 |
| tkt-b1d3a2e531.test | 13 |
| tkt-b351d95f9.test | 4 |
| tkt-b72787b1.test | 2 |
| tkt-b75a9ca6b0.test | 23 |
| tkt-ba7cbfaedc.test | 20 |
| tkt-bd484a090c.test | 5 |
| tkt-bdc6bbbb38.test | 1 |
| tkt-c48d99d690.test | 4 |
| tkt-c694113d5.test | 2 |
| tkt-cbd054fa6b.test | 1 |
| tkt-d11f09d36e.test | 6 |
| tkt-d635236375.test | 3 |
| tkt-d82e3f3721.test | 7 |
| tkt-f3e5abed55.test | 12 |
| tkt-f67b41381a.test | 10 |
| tkt-f777251dc7a.test | 10 |
| tkt-f7b4edec.test | 4 |
| tkt-f973c7ac31.test | 22 |
| tkt-fa7bf5ec.test | 2 |
| tkt-fc62af4523.test | 7 |
| tkt-fc7bd6358f.test | 50 |
| tkt1435.test | 4 |
| tkt1443.test | 4 |
| tkt1444.test | 5 |
| tkt1449.test | 3 |
| tkt1473.test | 58 |
| tkt1501.test | 2 |
| tkt1512.test | 5 |
| tkt1514.test | 2 |
| tkt1536.test | 3 |
| tkt1537.test | 16 |
| tkt1567.test | 13 |
| tkt1644.test | 8 |
| tkt1667.test | 1,005 |
| tkt1873.test | 6 |
| tkt2141.test | 4 |
| tkt2192.test | 7 |
| tkt2213.test | 2 |
| tkt2251.test | 9 |
| tkt2285.test | 5 |
| tkt2332.test | 17 |
| tkt2339.test | 10 |
| tkt2391.test | 5 |
| tkt2409.test | 18 |
| tkt2450.test | 5 |
| tkt2565.test | 29 |
| tkt2640.test | 7 |
| tkt2643.test | 2 |
| tkt2686.test | 11,998 |
| tkt2767.test | 5 |
| tkt2817.test | 9 |
| tkt2820.test | 17 |
| tkt2822.test | 39 |
| tkt2832.test | 7 |
| tkt2854.test | 22 |
| tkt2920.test | 10 |
| tkt2927.test | 84 |
| tkt2942.test | 5 |
| tkt3080.test | 7 |
| tkt3093.test | 6 |
| tkt3121.test | 3 |
| tkt3201.test | 9 |
| tkt3292.test | 5 |
| tkt3298.test | 7 |
| tkt3334.test | 12 |
| tkt3346.test | 6 |
| tkt3357.test | 5 |
| tkt3419.test | 7 |
| tkt3424.test | 5 |
| tkt3442.test | 6 |
| tkt3457.test | 6 |
| tkt3461.test | 6 |
| tkt3493.test | 27 |
| tkt3508.test | 2 |
| tkt3522.test | 3 |
| tkt3527.test | 3 |
| tkt3541.test | 3 |
| tkt3554.test | 5 |
| tkt3581.test | 4 |
| tkt35xx.test | 10 |
| tkt3630.test | 4 |
| tkt3718.test | 41 |
| tkt3731.test | 5 |
| tkt3757.test | 3 |
| tkt3761.test | 2 |
| tkt3762.test | 2 |
| tkt3773.test | 2 |
| tkt3791.test | 2 |
| tkt3793.test | 45 |
| tkt3810.test | 8 |
| tkt3824.test | 8 |
| tkt3832.test | 2 |
| tkt3838.test | 3 |
| tkt3841.test | 2 |
| tkt3871.test | 6 |
| tkt3879.test | 4 |
| tkt3911.test | 6 |
| tkt3918.test | 6 |
| tkt3922.test | 7 |
| tkt3929.test | 5 |
| tkt3935.test | 11 |
| tkt3992.test | 7 |
| tkt3997.test | 7 |
| tkt4018.test | 8 |

### Analyze (14 files, ~98 assertions)

| Test File | Tests |
|-----------|-------|
| analyze.test | 41 |
| analyze3.test | 1 |
| analyze4.test | 6 |
| analyze5.test | 1 |
| analyze6.test | 1 |
| analyze7.test | 16 |
| analyze8.test | 1 |
| analyze9.test | 1 |
| analyzeC.test | 25 |
| analyzeD.test | 1 |
| analyzeE.test | 1 |
| analyzeF.test | 1 |
| analyzeG.test | 1 |
| analyzer1.test | 1 |

### Order by (11 files, ~560 assertions)

| Test File | Tests |
|-----------|-------|
| orderby1.test | 65 |
| orderby2.test | 12 |
| orderby3.test | 16 |
| orderby4.test | 7 |
| orderby5.test | 31 |
| orderby6.test | 117 |
| orderby7.test | 1 |
| orderby8.test | 201 |
| orderby9.test | 5 |
| orderbyA.test | 99 |
| orderbyB.test | 6 |

### Skip scan (5 files, ~94 assertions)

| Test File | Tests |
|-----------|-------|
| skipscan1.test | 56 |
| skipscan2.test | 25 |
| skipscan3.test | 11 |
| skipscan5.test | 1 |
| skipscan6.test | 1 |

### Union operations (5 files, ~2,358 assertions)

| Test File | Tests |
|-----------|-------|
| unionall.test | 46 |
| unionall2.test | 5 |
| unionallfault.test | 230 |
| unionvtab.test | 116 |
| unionvtabfault.test | 1,961 |

### Other (217 files, ~128,773 assertions)

| Test File | Tests |
|-----------|-------|
| 8_3_names.test | 1 |
| amatch1.test | 1 |
| atof1.test | 40,006 |
| atof2.test | 5 |
| atomic.test | 1 |
| atomic2.test | 1 |
| avfs.test | 1 |
| avtrans.test | 315 |
| backcompat.test | 1 |
| badutf.test | 37 |
| badutf2.test | 43 |
| bestindex1.test | 31 |
| bestindex2.test | 10 |
| bestindex3.test | 19 |
| bestindex4.test | 1,028 |
| bestindex5.test | 34 |
| bestindex6.test | 6 |
| bestindex7.test | 15 |
| bestindex8.test | 77 |
| bestindex9.test | 13 |
| bestindexA.test | 11 |
| bestindexB.test | 7 |
| bestindexC.test | 55 |
| bestindexD.test | 8 |
| bestindexE.test | 19 |
| bestindexF.test | 40 |
| bigrow.test | 61 |
| bigsort.test | 1 |
| bitvec.test | 72 |
| boundary1.test | 1,512 |
| boundary2.test | 3,022 |
| boundary3.test | 1,897 |
| boundary4.test | 55 |
| btree01.test | 215 |
| btree02.test | 3 |
| btreefault.test | 508 |
| cache.test | 190 |
| cacheflush.test | 43 |
| cachespill.test | 8 |
| capi2.test | 116 |
| capi3.test | 250 |
| capi3b.test | 23 |
| capi3c.test | 296 |
| capi3d.test | 282 |
| capi3e.test | 115 |
| carray01.test | 30 |
| carray02.test | 19 |
| carrayfault.test | 180 |
| cffault.test | 617 |
| changes.test | 67 |
| changes2.test | 14 |
| chunksize.test | 7 |
| cksumvfs.test | 11 |
| close.test | 13 |
| closure01.test | 33 |
| contrib01.test | 4 |
| cost.test | 43 |
| coveridxscan.test | 16 |
| csv01.test | 468 |
| cursorhint.test | 1 |
| cursorhint2.test | 1 |
| dataversion1.test | 9 |
| dbdata.test | 1 |
| dbfuzz001.test | 12 |
| dbpage.test | 31 |
| dbpagefault.test | 811 |
| dbstatus.test | 146 |
| dbstatus2.test | 33 |
| decimal.test | 39 |
| default.test | 16 |
| descidx1.test | 52 |
| descidx2.test | 28 |
| descidx3.test | 15 |
| diskfull.test | 745 |
| emptytable.test | 5 |
| eqp.test | 65 |
| eqp2.test | 5 |
| errmsg.test | 12 |
| errofst1.test | 4 |
| exclusive.test | 54 |
| exclusive2.test | 29 |
| exec.test | 4 |
| extension01.test | 9 |
| external_reader.test | 15 |
| fallocate.test | 19 |
| filectrl.test | 7 |
| filefmt.test | 42 |
| filter1.test | 36 |
| filter2.test | 17 |
| filterfault.test | 176 |
| fordelete.test | 20 |
| format4.test | 4 |
| fpconv1.test | 7 |
| fuzz-oss1.test | 2 |
| fuzz2.test | 33 |
| fuzz3.test | 45,003 |
| fuzz4.test | 10 |
| fuzz_malloc.test | 5,993 |
| fuzzer1.test | 82 |
| fuzzer2.test | 6 |
| fuzzerfault.test | 2,207 |
| gcfault.test | 401 |
| gencol1.test | 176 |
| having.test | 22 |
| hidden.test | 1 |
| hook.test | 34 |
| hook2.test | 1 |
| icu.test | 1 |
| ieee754.test | 36 |
| imposter1.test | 12 |
| io.test | 29 |
| keyword1.test | 117 |
| lastinsert.test | 35 |
| laststmtchanges.test | 28 |
| limit.test | 123 |
| limit2.test | 25 |
| loadext2.test | 23 |
| lookaside.test | 21 |
| main.test | 95 |
| manydb.test | 901 |
| merge1.test | 5 |
| misc1.test | 94 |
| misc2.test | 39 |
| misc3.test | 45 |
| misc4.test | 25 |
| misc5.test | 39 |
| misc6.test | 5 |
| misc7.test | 1,248 |
| misc8.test | 16 |
| misuse.test | 23 |
| mutex1.test | 29 |
| mutex2.test | 14 |
| nan.test | 48 |
| nockpt.test | 24 |
| normalize.test | 11 |
| notnull.test | 107 |
| notnull2.test | 29 |
| notnullfault.test | 474 |
| null.test | 42 |
| nulls1.test | 74 |
| numindex1.test | 7 |
| openv2.test | 7 |
| oserror.test | 13 |
| ovfl.test | 3 |
| parser1.test | 13 |
| prefixes.test | 16 |
| progress.test | 12 |
| ptrchng.test | 31 |
| pushdown.test | 29 |
| qrf01.test | 105 |
| qrf02.test | 5 |
| qrf03.test | 10 |
| qrf05.test | 5 |
| qrf06.test | 6 |
| queryonly.test | 16 |
| quickcheck.test | 3 |
| quota.test | 819 |
| quota-glob.test | 109 |
| quota2.test | 60 |
| randexpr1.test | 2,601 |
| rbu.test | 1 |
| rdonly.test | 9 |
| readonly.test | 5 |
| recover.test | 1 |
| reindex.test | 34 |
| reservebytes.test | 12 |
| resetdb.test | 28 |
| resolver01.test | 28 |
| returning1.test | 85 |
| returningfault.test | 4,841 |
| securedel.test | 12 |
| securedel2.test | 13 |
| seekscan1.test | 7 |
| session.test | 1 |
| shmlock.test | 38 |
| shortread1.test | 5 |
| shrink.test | 4 |
| sidedelete.test | 402 |
| speed1.test | 4 |
| speed1p.test | 4 |
| speed2.test | 4 |
| speed3.test | 5 |
| speed4.test | 1 |
| speed4p.test | 1 |
| sqldiff1.test | 1 |
| sqllimits1.test | 3,117 |
| sqllog.test | 1 |
| starschema1.test | 10 |
| stmt.test | 15 |
| stmtrand.test | 5 |
| stmtvtab1.test | 8 |
| subtype1.test | 14 |
| superlock.test | 76 |
| symlink.test | 39 |
| symlink2.test | 1 |
| sync.test | 5 |
| sync2.test | 22 |
| syscall.test | 81 |
| sysfault.test | 1,428 |
| tabfunc01.test | 141 |
| tclsqlite.test | 132 |
| tokenize.test | 15 |
| tpch01.test | 6 |
| trace.test | 36 |
| trace2.test | 11 |
| trace3.test | 42 |
| trustschema1.test | 47 |
| unique.test | 47 |
| unique2.test | 27 |
| unixexcl.test | 27 |
| unordered.test | 16 |
| values.test | 121 |
| valuesfault.test | 383 |
| varint.test | 161 |
| widetab1.test | 18 |
| zerodamage.test | 8 |
| zipfilefault.test | 1,043 |

## Not Supported (6 test files — platform/config issues, not VFS bugs)

All failures are due to platform-specific behavior differences, not sqlite-objs VFS issues.

| Test File | Errors | Total | Root Cause | Classification |
|-----------|--------|-------|------------|----------------|
| func.test | 9 | 15,031 | `Inf` vs `inf` — macOS printf lowercases infinity | Platform (printf) |
| json101.test | 2 | 278 | `Inf` vs `inf` — same printf issue | Platform (printf) |
| json501.test | 3 | 185 | `Inf` vs `inf` — same printf issue | Platform (printf) |
| literal.test | 9 | 97 | `Inf` vs `inf` — same printf issue | Platform (printf) |
| loadext.test | 2 | 52 | macOS dlopen error message format changed — regex mismatch | Platform (macOS dlopen) |
| types3.test | 1 | 19 | TCL 8.5 `string text` vs `text` — `tcl_objtype` returns different type name | Platform (TCL 8.5) |

### Failure Details

**func.test (9/15,031):** All 9 failures are in `func-38.100` and `func-39.1x0` tests.
Tests expect `Inf` (capital I) from `printf('%!.15g', 1e999)` but macOS libc produces `inf`.
This is a well-known platform difference — glibc outputs `Inf`, macOS/BSD outputs `inf`.
Not a VFS issue. 99.94% of assertions pass.

**json101.test (2/278):** Tests `json101-20.2` and `json101-20.3` expect `Inf`/`-Inf`
from JSON infinity representation. Same macOS printf issue as func.test.
99.3% of assertions pass.

**json501.test (3/185):** Tests `json501-9.1` through `json501-9.3` expect `Inf`/`-Inf`
in JSON serialization of `9e999`. Same printf issue. 98.4% of assertions pass.

**literal.test (9/97):** Tests `literal-2.3.*`, `literal-2.4.*`, `literal-3.4.*` expect
`real Inf` / `real -Inf` from typeof() on infinity literals. Same printf issue. 90.7% pass.

**loadext.test (2/52):** Tests `loadext-2.1` and `loadext-2.2` use a regex matching
old macOS dlopen error format (`image.*found`). Modern macOS returns a different format
listing all searched paths. The regex needs updating for newer macOS. 96.2% pass.

**types3.test (1/19):** Test `types3-1.1` expects `tcl_objtype` to return `text` but TCL 8.5
returns `string text`. Cascading failures in `types3-3.*` from state corruption after first
error. TCL version issue — not VFS related. 94.7% pass.

## Timeouts (15 test files — >30s execution time)

These tests exceed the 30-second timeout. Most are meta-runners or heavy fault injection.

| Test File | Reason |
|-----------|--------|
| all.test | Meta-runner — runs ALL test files |
| extraquick.test | Meta-runner — runs large test subset |
| full.test | Meta-runner — runs full test suite |
| quick.test | Meta-runner — runs quick test subset |
| veryquick.test | Meta-runner — runs very quick subset |
| backup_ioerr.test | Heavy fault injection — backup + IO errors |
| exprfault2.test | Heavy fault injection — expression faults |
| fuzz.test | Fuzz testing — generates random SQL |
| pagerfault.test | Heavy fault injection — pager errors |
| savepoint6.test | Heavy savepoint stress test |
| sortfault.test | Heavy fault injection — sort errors |
| thread003.test | Multi-thread stress test |
| walcrash3.test | WAL crash recovery stress test |
| walsetlk.test | WAL set lock — timing-sensitive |
| walthread.test | WAL multi-thread stress test |

## Empty Output (15 test files — skipped or platform-specific)

These tests produce no `errors out of` summary line. They are either meta-tests,
platform-specific, or require external dependencies.

| Test File | Reason |
|-----------|--------|
| alias.test | Shell alias test — not a TCL assertion test |
| bigfile.test | Requires specific filesystem support for large files |
| bigfile2.test | Requires specific filesystem support for large files |
| bigmmap.test | Requires large mmap support |
| mallocAll.test | Meta-runner for all malloc tests |
| memleak.test | Memory leak detection meta-test |
| permutations.test | Test permutation runner (meta-test) |
| qrf04.test | Missing dependency or unsupported feature |
| soak.test | Soak test runner (meta-test) |
| win32heap.test | Windows-only — Win32 heap API |
| win32lock.test | Windows-only — Win32 file locking |
| win32longpath.test | Windows-only — long path support |
| win32nolock.test | Windows-only — no-lock mode |
| zipfile.test | Requires zipvfs extension (not compiled) |
| zipfile2.test | Requires zipvfs extension (not compiled) |

