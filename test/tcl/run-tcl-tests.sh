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
PASSING_TESTS=(
    # SELECT — core query engine
    select1 select2 select3 select4 select5 select6 select7
    select8 select9 selectA selectB selectC selectD selectE selectF
    # DML — data manipulation
    insert insert2 update delete
    # Transactions
    trans trans2
    # Indexes
    index index2 index3
    # Joins
    join join2 join3 join4
    # WHERE clause optimization
    where where2 where3 where4 where5 where6 where7 where8 where9
    # Triggers and views
    trigger1 trigger2 view subquery
    # Locking (uses mock, so trivial)
    lock lock2 lock3
    # WAL mode
    wal wal2
    # Memory-mapped I/O
    mmap1 mmap2
    # VACUUM
    vacuum vacuum2
    # ATTACH
    attach attach2
    # ALTER TABLE
    alter alter2 alter3
    # Table and type handling
    table tableapi types types2
    # Expressions and functions
    expr coalesce cast check cse conflict
    # Collation
    collate1 collate2 collate3 collate4 collate5 collate6 collate9
    # Date/time
    date
    # Aggregates and distinct
    distinctagg distinct
    # Encoding
    enc enc2 enc3
)

# Quick subset for smoke testing
QUICK_TESTS=(select1 insert expr trans where)

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
            echo "  --quick     Run quick subset (~5 tests)"
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
