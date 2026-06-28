#!/usr/bin/env bash
#
# release-gate.sh — Alpha readiness gate for sqlite-objs
#
# Usage:
#   ./scripts/release-gate.sh                    # fast local gate (default)
#   ./scripts/release-gate.sh --extended         # add stress tests + TCL full suite
#   ./scripts/release-gate.sh --azure            # add Azure cloud integration
#   ./scripts/release-gate.sh --full             # all gates (extended + azure)
#
# Environment:
#   RELEASE_GATE_REPORT  — output path (default: build/release-gate-report.txt)
#   SKIP_SYMBOL_CHECK    — if set, skip production symbol validation
#   SKIP_SANITIZE        — if set, skip sanitizer tests (use if ASan/UBSan has issues)
#   GATE_TIMEOUT         — per-gate timeout in seconds (default: 600)
#   AZURE_STORAGE_ACCOUNT, AZURE_STORAGE_CONTAINER, AZURE_STORAGE_SAS
#                        — required when --azure or --full is used
#
# Exit code: 0 if all validations pass, 1 if any fail.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

REPORT="${RELEASE_GATE_REPORT:-build/release-gate-report.txt}"
GATE_TIMEOUT="${GATE_TIMEOUT:-600}"
AZURE=false
EXTENDED=false
FULL=false

for arg in "$@"; do
    case "$arg" in
        --azure)    AZURE=true ;;
        --extended) EXTENDED=true ;;
        --full)     AZURE=true; EXTENDED=true; FULL=true ;;
        --help|-h)
            sed -n '2,/^$/p' "$0" | sed 's/^# \?//'
            exit 0
            ;;
        *) echo "Unknown option: $arg"; exit 2 ;;
    esac
done

# ── Helpers ──────────────────────────────────────────────────────────

PASS_COUNT=0
FAIL_COUNT=0
SKIP_COUNT=0
RESULTS=()
START_WALL=$(date +%s)

red()    { printf '\033[1;31m%s\033[0m' "$*"; }
green()  { printf '\033[1;32m%s\033[0m' "$*"; }
yellow() { printf '\033[1;33m%s\033[0m' "$*"; }
bold()   { printf '\033[1m%s\033[0m' "$*"; }

run_gate() {
    local name="$1"; shift
    local start elapsed status rc

    printf "  %-46s " "$name"
    start=$(date +%s)

    # Ensure build directory exists
    mkdir -p "$REPO_ROOT/build"

    # Simple log file naming (replace spaces/parens with dashes, keep it simple)
    local safe_name="$(echo "$name" | sed 's/[^a-zA-Z0-9]/-/g' | sed 's/--*/-/g' | sed 's/^-//' | sed 's/-$//')"
    local logfile="$REPO_ROOT/build/gate-${safe_name}.log"

    set +e
    if [[ "$name" == *"sanitizer"* || "$name" == *"Sanitizer"* ]]; then
        (
            ASAN_OPTIONS="${ASAN_OPTIONS:-handle_segv=0}" \
            LSAN_OPTIONS="${LSAN_OPTIONS:-detect_leaks=0}" "$@"
        ) > "$logfile" 2>&1 &
        local pid=$!
        local waited=0
        while kill -0 "$pid" 2>/dev/null; do
            if [ "$waited" -ge "$GATE_TIMEOUT" ]; then
                kill "$pid" 2>/dev/null || true
                wait "$pid" 2>/dev/null || true
                echo "TIMEOUT after ${GATE_TIMEOUT}s" >> "$logfile"
                rc=124
                break
            fi
            sleep 1
            waited=$((waited + 1))
        done
        if [ "${rc:-0}" -ne 124 ]; then
            wait "$pid"
            rc=$?
        fi
    else
        timeout "$GATE_TIMEOUT" "$@" > "$logfile" 2>&1
        rc=$?
    fi
    set -e
    if [ "$rc" -eq 0 ]; then
        status="PASS"
        PASS_COUNT=$((PASS_COUNT + 1))
        green "✓ PASS"
    else
        status="FAIL"
        FAIL_COUNT=$((FAIL_COUNT + 1))
        red "✗ FAIL"
        echo ""
        echo "    └─ Error details in: $logfile"
        if [ "$rc" -eq 124 ]; then
            echo "    └─ Timed out after ${GATE_TIMEOUT}s"
            echo "TIMEOUT after ${GATE_TIMEOUT}s" >> "$logfile"
        fi
    fi
    elapsed=$(( $(date +%s) - start ))
    printf "  (%ds)\n" "$elapsed"

    RESULTS+=("$status|$name|${elapsed}s")

    # Append log output to report detail file
    {
        echo ""
        echo "── $name ($status, ${elapsed}s) ──"
        if [ "$status" = "FAIL" ]; then
            cat "$logfile"
        else
            tail -3 "$logfile" 2>/dev/null || echo "(no output)"
        fi
    } >> "$REPORT.detail"
}

skip_gate() {
    local name="$1"
    local reason="$2"
    printf "  %-40s " "$name"
    yellow "SKIP"
    printf "  (%s)\n" "$reason"
    RESULTS+=("SKIP|$name|$reason")
    SKIP_COUNT=$((SKIP_COUNT + 1))
}

# ── Setup ────────────────────────────────────────────────────────────

mkdir -p "$(dirname "$REPORT")"
: > "$REPORT"
: > "$REPORT.detail"

echo ""
bold "═══════════════════════════════════════════════════════════"
echo ""
bold "  sqlite-objs Alpha Release Gate"
echo ""
bold "═══════════════════════════════════════════════════════════"
echo ""
echo "  Mode:   $(if $FULL; then echo 'full (extended + azure)'; elif $EXTENDED; then echo 'extended (stress + TCL)'; elif $AZURE; then echo 'azure integration'; else echo 'fast local'; fi)"
echo "  Time:   $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
echo "  Repo:   $REPO_ROOT"
echo "  Branch: $(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo 'unknown')"
echo "  Commit: $(git rev-parse --short HEAD 2>/dev/null || echo 'unknown')"
echo ""

# ── Stage 1: Build ───────────────────────────────────────────────────

bold "── Stage 1: Build ──"
echo ""

# Determine CPU count for parallel builds
NPROC=$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)

run_gate "C build (make all)" \
    make -j"$NPROC" all

run_gate "Rust build (cargo build --workspace)" \
    bash -c "cd rust && cargo build --workspace --quiet 2>&1"

echo ""

# ── Stage 2: Symbol Validation ───────────────────────────────────────

bold "── Stage 2: Production Symbols ──"
echo ""

if [ -z "${SKIP_SYMBOL_CHECK:-}" ]; then
    run_gate "Production symbol check (no test/mock symbols)" \
        bash -c '
            # Check that production library does not export test/mock symbols
            if [ ! -f build/libsqlite_objs.a ]; then
                echo "ERROR: build/libsqlite_objs.a not found"
                exit 1
            fi

            # Extract all exported symbols (T = text/code, R = read-only data)
            symbols=$(nm build/libsqlite_objs.a 2>/dev/null | grep " [TR] " | awk "{print \$3}" || true)

            # Check for prohibited test/mock symbols (exclude sqlite3_test_control which is standard SQLite API)
            bad_symbols=$(echo "$symbols" | grep -E "^(mock_|Mock|test_[^c]|test_c[^o]|_test$)" || true)

            if [ -n "$bad_symbols" ]; then
                echo "ERROR: Production library contains test/mock symbols:"
                echo "$bad_symbols"
                exit 1
            fi

            # Verify expected VFS symbols are present
            required_symbols="sqlite_objs_vfs_register"
            for sym in $required_symbols; do
                if ! echo "$symbols" | grep -q "^$sym$"; then
                    echo "ERROR: Required symbol not found: $sym"
                    echo "Available sqlite_objs symbols:"
                    echo "$symbols" | grep -i objs || echo "(none found)"
                    exit 1
                fi
            done

            echo "✓ Symbol validation passed"
            echo "  - No test/mock symbols found"
            echo "  - Required VFS symbols present: $required_symbols"
        '
else
    skip_gate "Production symbol check (no test/mock symbols)" "SKIP_SYMBOL_CHECK set"
fi

echo ""

# ── Stage 2: Lint & Format ───────────────────────────────────────────

bold "── Stage 3: Lint & Format ──"
echo ""

run_gate "Rust format check (cargo fmt --check)" \
    bash -c "cd rust && cargo fmt --all -- --check 2>&1"

run_gate "Rust clippy (warnings as errors)" \
    bash -c "cd rust && cargo clippy --workspace --all-targets --quiet -- -D warnings 2>&1"

echo ""

# ── Stage 3: C Tests ─────────────────────────────────────────────────

bold "── Stage 4: C Tests ──"
echo ""

run_gate "C unit tests (mocked layer)" \
    make test-unit

# Integration tests require Azurite
if command -v azurite &>/dev/null || [ -f "$REPO_ROOT/__azurite_db_blob__.json" ]; then
    run_gate "C integration tests (Azurite)" \
        make test-integration
else
    skip_gate "C integration tests (Azurite)" "azurite not available"
fi

if [ -z "${SKIP_SANITIZE:-}" ]; then
    run_gate "C sanitizer tests (ASan + UBSan)" \
        make sanitize
else
    skip_gate "C sanitizer tests (ASan + UBSan)" "SKIP_SANITIZE set"
fi

echo ""

# ── Stage 4: Rust Tests ──────────────────────────────────────────────

bold "── Stage 5: Rust Tests ──"
echo ""

run_gate "Rust unit tests (cargo test --lib)" \
    bash -c "cd rust && cargo test --workspace --lib --quiet 2>&1"

run_gate "Rust threading integration" \
    bash -c "cd rust && cargo test --test threading --quiet 2>&1"

# Fast perf matrix for smoke testing (not full benchmarks)
run_gate "Rust perf smoke test (memory mode, 10 iters)" \
    bash -c "cd rust && PERF_MODE=memory PERF_ITERATIONS=10 cargo test --test perf_matrix --quiet 2>&1"

echo ""

# ── Stage 5: TCL Test Suite (Quick) ──────────────────────────────────

bold "── Stage 6: TCL Test Suite ──"
echo ""

if $EXTENDED || $FULL; then
    run_gate "TCL full suite (1,151 tests)" \
        make test-tcl
elif [ -d "$REPO_ROOT/test/sqlite-src" ]; then
    # Only run TCL quick if the sqlite source is already set up
    run_gate "TCL quick subset (~5 core tests)" \
        make test-tcl-quick
else
    skip_gate "TCL quick subset (~5 core tests)" "test/sqlite-src not found (use --extended for full TCL)"
fi

echo ""

# ── Stage 6: Extended Tests (Optional) ───────────────────────────────

bold "── Stage 7: Extended Tests ──"
echo ""

if $EXTENDED || $FULL; then
    run_gate "Stress tests (2× multiplier, 3 iterations)" \
        bash -c 'for i in 1 2 3; do
            echo "Stress iteration $i/3"
            SQLITE_OBJS_STRESS_MULTIPLIER=2 ./test/run-integration.sh || exit 1
        done'

    run_gate "Rust perf matrix (file mode)" \
        bash -c "cd rust && PERF_MODE=file PERF_ITERATIONS=10 cargo test --test perf_matrix --quiet 2>&1"
else
    skip_gate "Stress tests (2× multiplier, 3 iterations)" "--extended not specified"
    skip_gate "Rust perf matrix (file mode)" "--extended not specified"
fi

echo ""

# ── Stage 6: Azure Integration (optional) ────────────────────────────

bold "── Stage 8: Azure Cloud Integration ──"
echo ""

if $AZURE || $FULL; then
    if [ -z "${AZURE_STORAGE_ACCOUNT:-}" ] || [ -z "${AZURE_STORAGE_CONTAINER:-}" ] || [ -z "${AZURE_STORAGE_SAS:-}" ]; then
        # Load from .env (can't use `source` — SAS tokens contain unquoted &)
        if [ -f "$REPO_ROOT/.env" ]; then
            while IFS='=' read -r key value; do
                case "$key" in
                    AZURE_STORAGE_ACCOUNT|AZURE_STORAGE_CONTAINER|AZURE_STORAGE_SAS)
                        export "$key=$value"
                        ;;
                esac
            done < "$REPO_ROOT/.env"
        fi
    fi

    if [ -n "${AZURE_STORAGE_ACCOUNT:-}" ] && [ -n "${AZURE_STORAGE_CONTAINER:-}" ] && [ -n "${AZURE_STORAGE_SAS:-}" ]; then
        run_gate "Azure VFS integration (50 tests)" \
            bash -c "cd rust && cargo test --test vfs_integration --quiet -- --ignored 2>&1"

        run_gate "Azure perf smoke test (1 iter)" \
            bash -c "cd rust && PERF_MODE=azure PERF_ITERATIONS=1 cargo test --test perf_matrix --quiet -- schema_ 2>&1"
    else
        skip_gate "Azure VFS integration (50 tests)" "Azure credentials not set"
        skip_gate "Azure perf smoke test (1 iter)" "Azure credentials not set"
    fi
else
    skip_gate "Azure VFS integration (50 tests)" "--azure not specified"
    skip_gate "Azure perf smoke test (1 iter)" "--azure not specified"
fi

echo ""

# ── Report ────────────────────────────────────────────────────────────

TOTAL_WALL=$(( $(date +%s) - START_WALL ))
TOTAL_GATES=$(( PASS_COUNT + FAIL_COUNT + SKIP_COUNT ))

{
    echo "sqlite-objs Alpha Release Gate Report"
    echo "======================================"
    echo ""
    echo "Timestamp: $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
    echo "Mode:      $(if $FULL; then echo 'full (extended + azure)'; elif $EXTENDED; then echo 'extended (stress + TCL)'; elif $AZURE; then echo 'azure integration'; else echo 'fast local'; fi)"
    echo "Repo:      $REPO_ROOT"
    echo "Branch:    $(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo 'unknown')"
    echo "Commit:    $(git rev-parse --short HEAD 2>/dev/null || echo 'unknown') ($(git log -1 --format=%s 2>/dev/null || echo 'unknown'))"
    echo ""
    echo "Gate Results"
    echo "------------"
    printf "  %-8s %-48s %s\n" "Status" "Gate" "Time"
    printf "  %-8s %-48s %s\n" "------" "----" "----"
    for result in "${RESULTS[@]}"; do
        IFS='|' read -r status name timing <<< "$result"
        printf "  %-8s %-48s %s\n" "$status" "$name" "$timing"
    done
    echo ""
    echo "Summary"
    echo "-------"
    echo "  Total:   $TOTAL_GATES gates"
    echo "  Passed:  $PASS_COUNT"
    echo "  Failed:  $FAIL_COUNT"
    echo "  Skipped: $SKIP_COUNT"
    echo "  Wall:    ${TOTAL_WALL}s ($(($TOTAL_WALL / 60))m $(($TOTAL_WALL % 60))s)"
    echo ""
    if [ "$FAIL_COUNT" -gt 0 ]; then
        echo "RESULT: ❌ FAILED — $FAIL_COUNT gate(s) did not pass"
        echo ""
        echo "Failed gates:"
        for result in "${RESULTS[@]}"; do
            IFS='|' read -r status name timing <<< "$result"
            if [ "$status" = "FAIL" ]; then
                echo "  - $name"
            fi
        done
    elif [ "$SKIP_COUNT" -gt 0 ]; then
        echo "RESULT: ✅ PASSED WITH SKIPS — fast/local gate is green, but this is not full release readiness"
        echo ""
        echo "Skipped gates:"
        for result in "${RESULTS[@]}"; do
            IFS='|' read -r status name timing <<< "$result"
            if [ "$status" = "SKIP" ]; then
                echo "  - $name ($timing)"
            fi
        done
    else
        echo "RESULT: ✅ PASSED — all gates green, alpha ready"
    fi
} | tee "$REPORT"

# Append detailed logs
if [ -f "$REPORT.detail" ]; then
    {
        echo ""
        echo ""
        echo "Detailed Gate Logs"
        echo "=================="
        cat "$REPORT.detail"
    } >> "$REPORT"
    rm -f "$REPORT.detail"
fi

echo ""
echo "  📄 Report saved to: $REPORT"
echo ""

if [ "$FAIL_COUNT" -gt 0 ]; then
    red "  ❌ ALPHA RELEASE GATE FAILED"
    echo ""
    echo "  Review gate logs above or in: $REPORT"
    echo ""
    exit 1
elif $FULL && [ "$SKIP_COUNT" -gt 0 ]; then
    red "  ❌ FULL RELEASE GATE INCOMPLETE"
    echo ""
    echo "  Full mode requires zero skipped gates. Review: $REPORT"
    echo ""
    exit 1
else
    if [ "$SKIP_COUNT" -gt 0 ]; then
        green "  ✅ FAST/LOCAL RELEASE GATE PASSED"
    else
        green "  ✅ ALPHA RELEASE GATE PASSED"
    fi
    echo ""
    if [ "$SKIP_COUNT" -gt 0 ]; then
        echo "  Note: $SKIP_COUNT gate(s) skipped; use --full with credentials/tooling for full release readiness"
        echo ""
    fi
    exit 0
fi
