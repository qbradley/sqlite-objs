#!/bin/bash
# run-integration.sh — Wrapper script for Layer 2 integration tests
#
# This script:
#   1. Starts Azurite in the background
#   2. Creates the test container
#   3. Runs the integration test binary
#   4. Stops Azurite
#   5. Reports results
#
# Usage:
#   ./test/run-integration.sh
#
# Environment variables (optional):
#   AZURITE_PORT      — Port for blob service (default: 10000)
#   AZURITE_SILENT    — Set to 1 to suppress Azurite output
#
# Part of the sqlite-objs project. License: MIT

set -e

# ──────────────────────────────────────────────────────────────
# Configuration
# ──────────────────────────────────────────────────────────────

AZURITE_PORT="${AZURITE_PORT:-10000}"
AZURITE_SILENT="${AZURITE_SILENT:-0}"
CONTAINER_NAME="sqlite-objs-test"
ACCOUNT_NAME="devstoreaccount1"
AZURITE_ENDPOINT="http://127.0.0.1:${AZURITE_PORT}"

AZURITE_PID=""
TEST_BINARY="build/test_integration"

# Well-known Azurite shared key (same on every install)
AZURITE_KEY="Eby8vdM02xNOcqFlqUwJPLlmEtlCDXJ1OUzFT50uSRZ6IFsuFq2UVErCz4I6tq/K1SZFPTOtr/KBHBeksoGMGw=="

# ──────────────────────────────────────────────────────────────
# Cleanup handler
# ──────────────────────────────────────────────────────────────

cleanup() {
    if [ -n "$AZURITE_PID" ]; then
        echo ""
        echo "🛑  Stopping Azurite (PID: $AZURITE_PID)..."
        kill "$AZURITE_PID" 2>/dev/null || true
        wait "$AZURITE_PID" 2>/dev/null || true
    fi
}

trap cleanup EXIT INT TERM

# ──────────────────────────────────────────────────────────────
# Start Azurite
# ──────────────────────────────────────────────────────────────

echo "╔════════════════════════════════════════════════════════╗"
echo "║  sqlite-objs Integration Test Runner                      ║"
echo "╚════════════════════════════════════════════════════════╝"
echo ""

# Check if the test binary exists
if [ ! -f "$TEST_BINARY" ]; then
    echo "❌  Test binary not found: $TEST_BINARY"
    echo "    Run 'make test-integration' to build it first."
    exit 1
fi

echo "🚀  Starting Azurite on port ${AZURITE_PORT}..."

# Start Azurite in the background
if [ "$AZURITE_SILENT" = "1" ]; then
    npx azurite \
        --blobPort "$AZURITE_PORT" \
        --silent \
        --skipApiVersionCheck \
        --loose \
        >/dev/null 2>&1 &
else
    npx azurite \
        --blobPort "$AZURITE_PORT" \
        --silent \
        --skipApiVersionCheck \
        --loose \
        >/dev/null 2>&1 &
fi

AZURITE_PID=$!
echo "✓   Azurite started (PID: $AZURITE_PID)"

# Wait for Azurite to be ready
echo "⏳  Waiting for Azurite to be ready..."
MAX_WAIT=30
WAITED=0
while ! curl -s -o /dev/null -w "%{http_code}" "${AZURITE_ENDPOINT}/${ACCOUNT_NAME}/?comp=list" | grep -q "200\|400\|403"; do
    sleep 1
    WAITED=$((WAITED + 1))
    if [ $WAITED -ge $MAX_WAIT ]; then
        echo "❌  Azurite did not start within ${MAX_WAIT} seconds"
        exit 1
    fi
done
echo "✓   Azurite is ready"

# ──────────────────────────────────────────────────────────────
# Create the test container via Azurite REST API
# ──────────────────────────────────────────────────────────────

# NOTE: Container creation is now handled by the C test code itself
# (via azure_container_create), so this step is skipped. The test
# binary will create the container using proper authentication.

echo "📦  Container creation delegated to test binary..."
echo ""

# ──────────────────────────────────────────────────────────────
# Run the integration tests
# ──────────────────────────────────────────────────────────────

echo ""
echo "🧪  Running integration tests..."
echo ""

# Run the test binary
if "$TEST_BINARY"; then
    TEST_RESULT=0
else
    TEST_RESULT=$?
fi

# ──────────────────────────────────────────────────────────────
# Report results
# ──────────────────────────────────────────────────────────────

echo ""
if [ $TEST_RESULT -eq 0 ]; then
    echo "╔════════════════════════════════════════════════════════╗"
    echo "║  ✅  All integration tests PASSED                      ║"
    echo "╚════════════════════════════════════════════════════════╝"
else
    echo "╔════════════════════════════════════════════════════════╗"
    echo "║  ❌  Integration tests FAILED                          ║"
    echo "╚════════════════════════════════════════════════════════╝"
fi

exit $TEST_RESULT
