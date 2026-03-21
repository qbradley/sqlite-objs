#!/usr/bin/env bash
#
# release-gate.sh — Run all validations and produce a pass/fail report.
#
# Usage:
#   ./scripts/release-gate.sh              # local-only validations
#   ./scripts/release-gate.sh --azure      # include Azure integration tests
#   ./scripts/release-gate.sh --full       # all validations (Azure + TCL full suite)
#
# Environment:
#   RELEASE_GATE_REPORT  — output path (default: build/release-gate-report.txt)
#   AZURE_STORAGE_ACCOUNT, AZURE_STORAGE_CONTAINER, AZURE_STORAGE_SAS
#                        — required when --azure or --full is used
#
# Exit code: 0 if all validations pass, 1 if any fail.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

REPORT="${RELEASE_GATE_REPORT:-build/release-gate-report.txt}"
AZURE=false
FULL=false

for arg in "$@"; do
    case "$arg" in
        --azure) AZURE=true ;;
        --full)  AZURE=true; FULL=true ;;
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
    local start elapsed status

    printf "  %-40s " "$name"
    start=$(date +%s)

    local logfile
    logfile=$(mktemp)
    if "$@" > "$logfile" 2>&1; then
        status="PASS"
        PASS_COUNT=$((PASS_COUNT + 1))
        green "PASS"
    else
        status="FAIL"
        FAIL_COUNT=$((FAIL_COUNT + 1))
        red "FAIL"
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
            tail -5 "$logfile"
        fi
    } >> "$REPORT.detail"

    rm -f "$logfile"
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
bold "  sqlite-objs Release Gate"
echo ""
bold "═══════════════════════════════════════════════════════════"
echo ""
echo "  Mode: $(if $FULL; then echo 'full'; elif $AZURE; then echo 'azure'; else echo 'local'; fi)"
echo "  Time: $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
echo "  Repo: $REPO_ROOT"
echo ""

# ── Stage 1: Build ───────────────────────────────────────────────────

bold "── Stage 1: Build ──"
echo ""

run_gate "C build (make all)" \
    make -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)" all

run_gate "Rust build (cargo build)" \
    bash -c "cd rust && cargo build --workspace 2>&1"

echo ""

# ── Stage 2: Lint & Format ───────────────────────────────────────────

bold "── Stage 2: Lint & Format ──"
echo ""

run_gate "Rust format check (cargo fmt)" \
    bash -c "cd rust && cargo fmt --all -- --check 2>&1"

run_gate "Rust clippy (cargo clippy)" \
    bash -c "cd rust && cargo clippy --workspace --all-targets -- -D warnings 2>&1"

echo ""

# ── Stage 3: C Tests ─────────────────────────────────────────────────

bold "── Stage 3: C Tests ──"
echo ""

run_gate "C unit tests (Layer 1 — mocked)" \
    make test-unit

# Integration tests require Azurite
if command -v azurite &>/dev/null || [ -f "$REPO_ROOT/__azurite_db_blob__.json" ]; then
    run_gate "C integration tests (Layer 2 — Azurite)" \
        make test-integration
else
    skip_gate "C integration tests (Layer 2 — Azurite)" "azurite not available"
fi

run_gate "C sanitizer tests (ASan + UBSan)" \
    make sanitize

echo ""

# ── Stage 4: Rust Tests ──────────────────────────────────────────────

bold "── Stage 4: Rust Tests ──"
echo ""

run_gate "Rust unit & threading tests" \
    bash -c "cd rust && cargo test --workspace --lib 2>&1"

run_gate "Rust threading integration" \
    bash -c "cd rust && cargo test --test threading 2>&1"

run_gate "Rust perf matrix (memory mode)" \
    bash -c "cd rust && PERF_MODE=memory PERF_ITERATIONS=10 cargo test --test perf_matrix 2>&1"

run_gate "Rust perf matrix (file mode)" \
    bash -c "cd rust && PERF_MODE=file PERF_ITERATIONS=10 cargo test --test perf_matrix 2>&1"

echo ""

# ── Stage 5: TCL Test Suite ──────────────────────────────────────────

bold "── Stage 5: TCL Test Suite ──"
echo ""

if $FULL; then
    run_gate "TCL test suite (1,151 tests)" \
        make test-tcl
else
    run_gate "TCL test suite (quick subset)" \
        make test-tcl-quick
fi

echo ""

# ── Stage 6: Azure Integration (optional) ────────────────────────────

bold "── Stage 6: Azure Integration ──"
echo ""

if $AZURE; then
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
        run_gate "Rust VFS integration (50 Azure tests)" \
            bash -c "cd rust && cargo test --test vfs_integration -- --ignored 2>&1"

        run_gate "Rust perf matrix (Azure mode)" \
            bash -c "cd rust && PERF_MODE=azure PERF_ITERATIONS=1 cargo test --test perf_matrix -- schema_ 2>&1"
    else
        skip_gate "Rust VFS integration (50 Azure tests)" "Azure credentials not set"
        skip_gate "Rust perf matrix (Azure mode)" "Azure credentials not set"
    fi
else
    skip_gate "Rust VFS integration (50 Azure tests)" "--azure not specified"
    skip_gate "Rust perf matrix (Azure mode)" "--azure not specified"
fi

echo ""

# ── Report ────────────────────────────────────────────────────────────

TOTAL_WALL=$(( $(date +%s) - START_WALL ))
TOTAL_GATES=$(( PASS_COUNT + FAIL_COUNT + SKIP_COUNT ))

{
    echo "sqlite-objs Release Gate Report"
    echo "==============================="
    echo ""
    echo "Date:   $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
    echo "Mode:   $(if $FULL; then echo 'full'; elif $AZURE; then echo 'azure'; else echo 'local'; fi)"
    echo "Repo:   $REPO_ROOT"
    echo "Branch: $(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo 'unknown')"
    echo "Commit: $(git rev-parse --short HEAD 2>/dev/null || echo 'unknown')"
    echo ""
    echo "Results"
    echo "-------"
    printf "  %-8s %-42s %s\n" "Status" "Gate" "Time"
    printf "  %-8s %-42s %s\n" "------" "----" "----"
    for result in "${RESULTS[@]}"; do
        IFS='|' read -r status name timing <<< "$result"
        printf "  %-8s %-42s %s\n" "$status" "$name" "$timing"
    done
    echo ""
    echo "Summary"
    echo "-------"
    echo "  Total:   $TOTAL_GATES gates"
    echo "  Passed:  $PASS_COUNT"
    echo "  Failed:  $FAIL_COUNT"
    echo "  Skipped: $SKIP_COUNT"
    echo "  Wall:    ${TOTAL_WALL}s"
    echo ""
    if [ "$FAIL_COUNT" -gt 0 ]; then
        echo "RESULT: FAILED — $FAIL_COUNT gate(s) did not pass"
    else
        echo "RESULT: PASSED — all gates green"
    fi
} | tee "$REPORT"

# Append detailed logs
if [ -f "$REPORT.detail" ]; then
    {
        echo ""
        echo ""
        echo "Detailed Logs"
        echo "============="
        cat "$REPORT.detail"
    } >> "$REPORT"
    rm -f "$REPORT.detail"
fi

echo ""
echo "  Report saved to: $REPORT"
echo ""

if [ "$FAIL_COUNT" -gt 0 ]; then
    red "  ❌ RELEASE GATE FAILED"
    echo ""
    exit 1
else
    green "  ✅ RELEASE GATE PASSED"
    echo ""
    exit 0
fi
