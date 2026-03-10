#!/bin/bash
# Quick test of Azurite connection

set -e

# Start Azurite
echo "Starting Azurite..."
npx azurite --blobPort 10000 --silent >/dev/null 2>&1 &
AZURITE_PID=$!
echo "Azurite PID: $AZURITE_PID"

# Wait for it to be ready
sleep 3

# Test connection
echo "Testing connection..."
curl -v "http://127.0.0.1:10000/devstoreaccount1/azqlite-test?restype=container&comp=list" 2>&1 | head -20

# Kill Azurite
kill $AZURITE_PID 2>/dev/null || true
