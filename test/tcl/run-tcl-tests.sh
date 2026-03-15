#!/bin/bash
#
# run-tcl-tests.sh — Run SQLite TCL test suite against sqlite-objs VFS
#
# Usage:
#   ./test/tcl/run-tcl-tests.sh              # Run all passing tests
#   ./test/tcl/run-tcl-tests.sh select1      # Run specific test
#   ./test/tcl/run-tcl-tests.sh --quick      # Run quick subset (~5 tests)
#   ./test/tcl/run-tcl-tests.sh --verbose    # Show VFS registration msg
#
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
TESTFIXTURE="$REPO_ROOT/build/testfixture-objs"
SQLITE_SRC="$REPO_ROOT/test/sqlite-src"
BLD_DIR="$SQLITE_SRC/bld"
TEST_DIR="$SQLITE_SRC/test"

# Verified passing tests (zero errors with sqlite-objs VFS + mock backend)
# 1,151 test files verified via full sweep on macOS/Darwin
PASSING_TESTS=(
    # SELECT — core query engine
    select1 select2 select3 select4 select5 select6 select7 select8
    select9 selectA selectB selectC selectD selectE selectF selectG
    selectH
    # DML — data manipulation
    delete delete2 delete3 delete4 delete_db insert insert2 insert3
    insert4 insert5 insertfault update update2
    # Transactions
    trans trans2 trans3 transitive1
    # Indexes
    index index2 index3 index4 index5 index6 index7 index8
    index9 indexA indexedby indexexpr1 indexexpr2 indexexpr3 indexfault
    # JOIN operations
    join join2 join3 join4 join5 join6 join7 join8
    join9 joinA joinB joinC joinD joinE joinF joinH
    joinI
    # WHERE clause
    where where2 where3 where4 where5 where6 where7 where8
    where9 whereA whereB whereC whereD whereE whereF whereG
    whereH whereI whereJ whereK whereL whereM whereN wherefault
    wherelfault wherelimit wherelimit2 wherelimit3
    # Triggers
    trigger1 trigger2 trigger3 trigger4 trigger5 trigger6 trigger7 trigger8
    trigger9 triggerA triggerB triggerC triggerD triggerE triggerF triggerG
    triggerupfrom
    # Views
    view view2 view3
    # Subqueries
    subquery subquery2 subselect
    # ALTER TABLE
    alter alter2 alter3 alter4 alterauth alterauth2 altercol altercons
    altercons2 altercorrupt alterdropcol alterdropcol2 alterfault alterlegacy altermalloc altermalloc2
    altermalloc3 alterqf altertab altertab2 altertab3 altertrig
    # Table creation
    createtab table tableapi tableopts
    # Types and affinity
    affinity2 affinity3 types types2
    # Expressions
    between cast check checkfault coalesce conflict conflict2 conflict3
    cse expr expr2 exprfault hexlit istrue literal2 numcast
    # Functions
    basexx1 eval func2 func3 func4 func5 func6 func7
    func8 func9 offset1 percentile printf printf2 quote round1
    substr unhex
    # LIKE and pattern matching
    like like2 like3 regexp1 regexp2
    # IN operator
    in in2 in3 in4 in5 in6 in7
    # EXISTS
    exists existsexpr existsexpr2 existsfault
    # Collation
    collate1 collate2 collate3 collate4 collate5 collate6 collate7 collate8
    collate9 collateA collateB colmeta colname columncount
    # Date/time
    ctime date date2 date3 date4 date5 timediff1
    # Aggregates
    aggerror aggfault aggnested aggorderby count countofview distinct distinct2
    distinctagg minmax minmax2 minmax3 minmax4
    # Encoding
    enc enc2 enc3 enc4 utf16align
    # BLOB and incremental I/O
    blob bloom1 incrblob incrblob2 incrblob3 incrblob4 incrblob_err incrblobfault
    zeroblob zeroblobfault
    # Binding
    bind bind2 bindxfer
    # Locking
    lock lock2 lock3 lock4 lock5 lock6 lock7 nolock
    pendingrace sharedlock
    # WAL mode
    wal wal2 wal3 wal4 wal5 wal6 wal64k wal7
    wal8 wal9 walbak walbig walblock walckptnoop walcksum walcrash
    walcrash2 walcrash4 walfault walfault2 walhook walmode walnoshm waloverwrite
    walpersist walprotocol walprotocol2 walrestart walro walro2 walrofault walseh1
    walsetlk2 walsetlk3 walsetlk_recover walsetlk_snapshot walshared walslow walvfs
    # Memory-mapped I/O
    mmap1 mmap2 mmap3 mmap4 mmapcorrupt mmapfault mmapwarm
    # VACUUM
    vacuum vacuum-into vacuum2 vacuum3 vacuum4 vacuum5 vacuum6 vacuummem
    # ATTACH
    attach attach2 attach3 attach4 attachmalloc
    # Pager and page cache
    pager1 pager2 pager3 pager4 pagerfault2 pagerfault3 pageropt pagesize
    pcache pcache2
    # Journal
    journal1 journal2 journal3 jrnlmode jrnlmode2 jrnlmode3 memjournal memjournal2
    mjournal subjournal
    # Authorization
    auth auth2 auth3
    # Autoincrement/autoindex/autovacuum
    autoanalyze1 autoinc autoindex1 autoindex2 autoindex3 autoindex4 autoindex5 autovacuum
    autovacuum2 autovacuum_ioerr2
    # Foreign keys
    fkey1 fkey2 fkey3 fkey4 fkey5 fkey6 fkey7 fkey8
    fkey_malloc
    # Crash recovery
    crash crash2 crash3 crash4 crash5 crash6 crash7 crash8
    crashM writecrash
    # Corruption detection
    corrupt corrupt2 corrupt3 corrupt4 corrupt5 corrupt6 corrupt7 corrupt8
    corrupt9 corruptA corruptB corruptC corruptD corruptE corruptF corruptG
    corruptH corruptI corruptJ corruptK corruptL corruptM corruptN incrcorrupt
    # IO error simulation
    ioerr ioerr2 ioerr3 ioerr4 ioerr5 ioerr6
    # Malloc/memory testing
    malloc malloc3 malloc4 malloc5 malloc6 malloc7 malloc8 malloc9
    mallocA mallocB mallocC mallocD mallocE mallocF mallocG mallocH
    mallocI mallocJ mallocK mallocL mallocM mem5 memdb memdb1
    memdb2 memsubsys1 memsubsys2 softheap1
    # Shared cache
    shared shared2 shared3 shared4 shared6 shared7 shared8 shared9
    sharedA sharedB shared_err
    # FTS3/FTS4 full-text search
    fts-9fd058691 fts3 fts3aa fts3ab fts3ac fts3ad fts3ae fts3af
    fts3ag fts3ah fts3ai fts3aj fts3ak fts3al fts3am fts3an
    fts3ao fts3atoken fts3atoken2 fts3auto fts3aux1 fts3aux2 fts3b fts3c
    fts3comp1 fts3conf fts3corrupt fts3corrupt2 fts3corrupt3 fts3corrupt4 fts3corrupt5 fts3corrupt6
    fts3corrupt7 fts3cov fts3d fts3defer fts3defer2 fts3defer3 fts3drop fts3dropmod
    fts3e fts3expr fts3expr2 fts3expr3 fts3expr4 fts3expr5 fts3f fts3fault
    fts3fault2 fts3fault3 fts3first fts3fuzz001 fts3integrity fts3join fts3malloc fts3matchinfo
    fts3matchinfo2 fts3misc fts3near fts3offsets fts3prefix fts3prefix2 fts3query fts3rank
    fts3rnd fts3shared fts3snippet fts3snippet2 fts3sort fts3tok1 fts3tok_err fts3varint
    fts4aa fts4check fts4content fts4docid fts4growth fts4growth2 fts4incr fts4intck1
    fts4langid fts4lastrowid fts4merge fts4merge2 fts4merge3 fts4merge4 fts4merge5 fts4min
    fts4noti fts4onepass fts4opt fts4record fts4rename fts4umlaut fts4unicode fts4upfrom
    # JSON
    json102 json103 json104 json105 json106 json107 json108 json109
    json502 jsonb01
    # R-Tree
    rtree
    # Window functions
    window1 window2 window3 window4 window5 window6 window7 window8
    window9 windowA windowB windowC windowD windowE windowerr windowfault
    windowpushd
    # CTEs (WITH)
    with1 with2 with3 with4 with5 with6 withM without_rowid1
    without_rowid2 without_rowid3 without_rowid4 without_rowid5 without_rowid6 without_rowid7
    # Virtual tables
    swarmvtab swarmvtab2 swarmvtab3 swarmvtabfault vtab1 vtab2 vtab3 vtab4
    vtab5 vtab6 vtab7 vtab8 vtab9 vtabA vtabB vtabC
    vtabD vtabE vtabF vtabH vtabI vtabJ vtabK vtabL
    vtab_alter vtab_err vtab_shared vtabdistinct vtabdrop vtabrhs1
    # Snapshot
    snapshot snapshot2 snapshot3 snapshot4 snapshot_fault snapshot_up
    # Savepoint
    savepoint savepoint2 savepoint4 savepoint5 savepoint7 savepointfault
    # Rollback
    rollback rollback2 rollbackfault
    # Schema
    schema schema2 schema3 schema4 schema5 schema6 schemafault
    # Shell tool
    shell1 shell2 shell3 shell4 shell5 shell6 shell7 shell8
    shell9 shellA shellB
    # Pragma
    pragma pragma2 pragma3 pragma4 pragma5 pragma6 pragmafault
    # Backup
    backup backup2 backup4 backup5 backup_malloc
    # Thread safety
    thread001 thread002 thread004 thread005 thread1 thread2 thread3
    # UPSERT
    upsert1 upsert2 upsert3 upsert4 upsert5 upsertfault
    # UPDATE FROM
    upfrom1 upfrom2 upfrom3 upfrom4 upfromfault
    # URI handling
    uri uri2
    # Temp tables
    tempdb tempdb2 tempfault temptable temptable2 temptable3 temptrigger
    # Sort operations
    sort sort2 sort3 sort4 sort5 sorterref
    # Busy handling
    busy busy2
    # Multiplex VFS
    multiplex multiplex2 multiplex3 multiplex4
    # Notify
    notify1 notify2 notify3
    # Spellfix
    spellfix spellfix2 spellfix3 spellfix4
    # Statistics
    scanstatus scanstatus2 stat statfault
    # Strict tables
    strict1 strict2
    # Secure delete
    securedel securedel2
    # Limits
    limit limit2 sqllimits1
    # Atomic operations
    atomic atomic2
    # C API
    capi2 capi3 capi3b capi3c capi3d capi3e
    # Hook functions
    hook hook2
    # Cache control
    cache cacheflush cachespill
    # Incremental vacuum
    incrvacuum incrvacuum2 incrvacuum3 incrvacuum_ioerr
    # Row values
    rowallock rowhash rowid rowvalue rowvalue2 rowvalue3 rowvalue4 rowvalue5
    rowvalue6 rowvalue7 rowvalue8 rowvalue9 rowvalueA rowvaluefault rowvaluevtab
    # Evidence tests (e_*)
    e_blobbytes e_blobclose e_blobopen e_blobwrite e_changes e_createtable e_delete e_droptrigger
    e_dropview e_expr e_fkey e_fts3 e_insert e_reindex e_resolve e_select
    e_select2 e_totalchanges e_update e_uri e_vacuum e_wal e_walauto e_walckpt
    e_walhook
    # Regression tickets (tkt-*)
    tkt-02a8e81d44 tkt-18458b1a tkt-26ff0c2d1e tkt-2a5629202f tkt-2d1a5c67d tkt-2ea2425d34 tkt-31338dca7e tkt-313723c356
    tkt-385a5b56b9 tkt-38cb5df375 tkt-3998683a16 tkt-3a77c9714e tkt-3fe897352e tkt-4a03edc4c8 tkt-4c86b126f2 tkt-4dd95f6943
    tkt-4ef7e3cfca tkt-54844eea3f tkt-5d863f876e tkt-5e10420e8d tkt-5ee23731f tkt-6bfb98dfc0 tkt-752e1646fc tkt-78e04e52ea
    tkt-7a31705a7e6 tkt-7bbfb7d442 tkt-80ba201079 tkt-80e031a00f tkt-8454a207b9 tkt-868145d012 tkt-8c63ff0ec tkt-91e2e8ba6f
    tkt-99378177930f87bd tkt-9a8b09f8e6 tkt-9d68c883 tkt-9f2eb3abac tkt-a7b7803e tkt-a7debbe0 tkt-a8a0d2996a tkt-b1d3a2e531
    tkt-b351d95f9 tkt-b72787b1 tkt-b75a9ca6b0 tkt-ba7cbfaedc tkt-bd484a090c tkt-bdc6bbbb38 tkt-c48d99d690 tkt-c694113d5
    tkt-cbd054fa6b tkt-d11f09d36e tkt-d635236375 tkt-d82e3f3721 tkt-f3e5abed55 tkt-f67b41381a tkt-f777251dc7a tkt-f7b4edec
    tkt-f973c7ac31 tkt-fa7bf5ec tkt-fc62af4523 tkt-fc7bd6358f tkt1435 tkt1443 tkt1444 tkt1449
    tkt1473 tkt1501 tkt1512 tkt1514 tkt1536 tkt1537 tkt1567 tkt1644
    tkt1667 tkt1873 tkt2141 tkt2192 tkt2213 tkt2251 tkt2285 tkt2332
    tkt2339 tkt2391 tkt2409 tkt2450 tkt2565 tkt2640 tkt2643 tkt2686
    tkt2767 tkt2817 tkt2820 tkt2822 tkt2832 tkt2854 tkt2920 tkt2927
    tkt2942 tkt3080 tkt3093 tkt3121 tkt3201 tkt3292 tkt3298 tkt3334
    tkt3346 tkt3357 tkt3419 tkt3424 tkt3442 tkt3457 tkt3461 tkt3493
    tkt3508 tkt3522 tkt3527 tkt3541 tkt3554 tkt3581 tkt35xx tkt3630
    tkt3718 tkt3731 tkt3757 tkt3761 tkt3762 tkt3773 tkt3791 tkt3793
    tkt3810 tkt3824 tkt3832 tkt3838 tkt3841 tkt3871 tkt3879 tkt3911
    tkt3918 tkt3922 tkt3929 tkt3935 tkt3992 tkt3997 tkt4018
    # Analyze
    analyze analyze3 analyze4 analyze5 analyze6 analyze7 analyze8 analyze9
    analyzeC analyzeD analyzeE analyzeF analyzeG analyzer1
    # Order by
    orderby1 orderby2 orderby3 orderby4 orderby5 orderby6 orderby7 orderby8
    orderby9 orderbyA orderbyB
    # Skip scan
    skipscan1 skipscan2 skipscan3 skipscan5 skipscan6
    # Union
    unionall unionall2 unionallfault unionvtab unionvtabfault
    # Other — miscellaneous
    8_3_names amatch1 atof1 atof2 avfs avtrans backcompat badutf
    badutf2 bestindex1 bestindex2 bestindex3 bestindex4 bestindex5 bestindex6 bestindex7
    bestindex8 bestindex9 bestindexA bestindexB bestindexC bestindexD bestindexE bestindexF
    bigrow bigsort bitvec boundary1 boundary2 boundary3 boundary4 btree01
    btree02 btreefault carray01 carray02 carrayfault cffault changes changes2
    chunksize cksumvfs close closure01 contrib01 cost coveridxscan csv01
    cursorhint cursorhint2 dataversion1 dbdata dbfuzz001 dbpage dbpagefault dbstatus
    dbstatus2 decimal default descidx1 descidx2 descidx3 diskfull emptytable
    eqp eqp2 errmsg errofst1 exclusive exclusive2 exec extension01
    external_reader fallocate filectrl filefmt filter1 filter2 filterfault fordelete
    format4 fpconv1 fuzz-oss1 fuzz2 fuzz3 fuzz4 fuzz_malloc fuzzer1
    fuzzer2 fuzzerfault gcfault gencol1 having hidden icu ieee754
    imposter1 init instr instrfault intarray interrupt interrupt2 intpkey
    intreal io keyword1 lastinsert laststmtchanges loadext2 lookaside main
    manydb merge1 misc1 misc2 misc3 misc4 misc5 misc6
    misc7 misc8 misuse mutex1 mutex2 nan nockpt normalize
    notnull notnull2 notnullfault null nulls1 numindex1 openv2 oserror
    ovfl parser1 prefixes progress ptrchng pushdown qrf01 qrf02
    qrf03 qrf05 qrf06 queryonly quickcheck quota quota-glob quota2
    randexpr1 rbu rdonly readonly recover reindex reservebytes resetdb
    resolver01 returning1 returningfault seekscan1 session shmlock shortread1 shrink
    sidedelete speed1 speed1p speed2 speed3 speed4 speed4p sqldiff1
    sqllog starschema1 stmt stmtrand stmtvtab1 subtype1 superlock symlink
    symlink2 sync sync2 syscall sysfault tabfunc01 tclsqlite tokenize
    tpch01 trace trace2 trace3 trustschema1 unique unique2 unixexcl
    unordered values valuesfault varint widetab1 zerodamage zipfilefault
)

# Quick subset for smoke testing
QUICK_TESTS=(select1 insert expr trans where join fkey1 wal json102 window1)

# ── Parse arguments ─────────────────────────────────────────────────

TESTS_TO_RUN=()
VERBOSE=0
QUICK=0
SPECIFIC_TESTS=()

for arg in "$@"; do
    case "$arg" in
        --verbose) VERBOSE=1 ;;
        --quick)   QUICK=1 ;;
        --list)    printf '%s\n' "${PASSING_TESTS[@]}"; exit 0 ;;
        --help)
            echo "Usage: $0 [--quick|--verbose|--list|TEST_NAME...]"
            echo ""
            echo "Options:"
            echo "  --quick     Run quick subset (~10 tests)"
            echo "  --verbose   Show VFS registration message"
            echo "  --list      List all passing tests"
            echo "  TEST_NAME   Run specific test(s)"
            exit 0
            ;;
        *)         SPECIFIC_TESTS+=("$arg") ;;
    esac
done

if [ ${#SPECIFIC_TESTS[@]} -gt 0 ]; then
    TESTS_TO_RUN=("${SPECIFIC_TESTS[@]}")
elif [ "$QUICK" -eq 1 ]; then
    TESTS_TO_RUN=("${QUICK_TESTS[@]}")
else
    TESTS_TO_RUN=("${PASSING_TESTS[@]}")
fi

# ── Verify prerequisites ───────────────────────────────────────────

if [ ! -x "$TESTFIXTURE" ]; then
    echo "ERROR: testfixture-objs not found at $TESTFIXTURE"
    echo "Run: make test-tcl-build"
    exit 1
fi

if [ ! -d "$TEST_DIR" ]; then
    echo "ERROR: SQLite test directory not found at $TEST_DIR"
    echo "Run: make test-tcl-build"
    exit 1
fi

# ── Set environment ─────────────────────────────────────────────────

if [ "$VERBOSE" -eq 1 ]; then
    export SQLITE_OBJS_VERBOSE=1
fi

# Run from the build directory (some tests expect generated files there)
cd "$BLD_DIR"

# ── Run tests ───────────────────────────────────────────────────────

TOTAL_TESTS=0
TOTAL_ERRORS=0
PASSED_FILES=0
FAILED_FILES=0
FAILED_LIST=()

echo "=== SQLite TCL Test Suite (sqlite-objs VFS) ==="
echo "Running ${#TESTS_TO_RUN[@]} test files..."
echo ""

for test in "${TESTS_TO_RUN[@]}"; do
    test_file="$TEST_DIR/$test.test"
    if [ ! -f "$test_file" ]; then
        echo "  SKIP  $test.test (file not found)"
        continue
    fi

    # Clean up stale test state between runs
    rm -rf testdir 2>/dev/null

    # Run test and capture summary line
    output=$("$TESTFIXTURE" "$test_file" 2>&1)
    summary=$(echo "$output" | grep 'errors out of' | tail -1)

    if [ -z "$summary" ]; then
        echo "  FAIL  $test.test (no summary — possible crash)"
        FAILED_FILES=$((FAILED_FILES + 1))
        FAILED_LIST+=("$test")
        continue
    fi

    errors=$(echo "$summary" | awk '{print $1}')
    tests=$(echo "$summary" | awk '{print $5}')

    TOTAL_TESTS=$((TOTAL_TESTS + tests))
    TOTAL_ERRORS=$((TOTAL_ERRORS + errors))

    if [ "$errors" -eq 0 ]; then
        echo "  PASS  $test.test ($tests tests)"
        PASSED_FILES=$((PASSED_FILES + 1))
    else
        echo "  FAIL  $test.test ($errors/$tests errors)"
        FAILED_FILES=$((FAILED_FILES + 1))
        FAILED_LIST+=("$test")
    fi
done

# Clean up after last test
rm -rf testdir 2>/dev/null

echo ""
echo "═══════════════════════════════════════════════"
echo "  Files:  $PASSED_FILES passed, $FAILED_FILES failed"
echo "  Tests:  $TOTAL_TESTS total, $TOTAL_ERRORS errors"
echo "═══════════════════════════════════════════════"

if [ $FAILED_FILES -gt 0 ]; then
    echo ""
    echo "Failed: ${FAILED_LIST[*]}"
    exit 1
fi

echo ""
echo "All tests passed!"
exit 0
