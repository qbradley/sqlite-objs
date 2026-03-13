# Layer 2 Integration Tests

## Overview

Layer 2 integration tests exercise the REAL `azure_client.c` code against **Azurite** — a local Azure Storage emulator. These tests validate HTTP communication, REST API compatibility, and end-to-end VFS functionality without network costs.

## Prerequisites

- **Azurite** (Node.js package):
  ```bash
  npm install -g azurite
  # OR use npx (no install needed):
  npx azurite --version
  ```

- **Azurite running on port 10000** (handled automatically by `run-integration.sh`)

## Running Tests

```bash
make test-integration
```

This will:
1. Build `build/test_integration` (links against production azure_client.c)
2. Run `./test/run-integration.sh` which:
   - Starts Azurite in the background
   - Runs the integration test binary
   - Stops Azurite
   - Reports results

## Test Coverage

The integration tests verify:

### Azure Client Layer
- **Page blob lifecycle**: create → write → read → verify → delete
- **Block blob lifecycle**: upload → download → verify → delete
- **Lease lifecycle**: acquire → renew → release
- **Lease conflict**: concurrent lease attempts return CONFLICT
- **Page blob alignment**: 512-byte aligned writes and reads
- **Error handling**: NOT_FOUND, CONFLICT, and other Azure errors
- **Page blob resize**: growing blobs dynamically

### VFS Layer (Full Round-Trip)
- **VFS registration** with Azurite configuration
- **SQLite CREATE TABLE** on Azure-backed storage
- **INSERT/SELECT** persistence across close/reopen
- **Journal mode** with BEGIN/COMMIT transactions
- **Journal blob** creation and deletion

## Configuration

Azurite uses well-known dev credentials:
- **Account**: `devstoreaccount1`
- **Key**: `Eby8vdM02xNOcqFlqUwJPLlmEtlCDXJ1OUzFT50uSRZ6IFsuFq2UVErCz4I6tq/K1SZFPTOtr/KBHBeksoGMGw==`
- **Endpoint**: `http://127.0.0.1:10000/devstoreaccount1`
- **Container**: `sqlite-objs-test`

These are NOT secrets — every Azurite install uses identical values.

## Known Issues

**CURRENT STATUS (2026-03-10)**: Infrastructure is complete, but tests fail with `AZURE_ERR_NETWORK` due to azure_client auth issues with Azurite.  The endpoint override feature is implemented (`azure_client_config_t.endpoint`), but Shared Key authentication needs adjustment for Azurite's auth quirks.

**Next steps**:
1. Debug azure_client HTTP requests to see what Azurite rejects
2. Add conditional logic or Azurite-specific auth mode
3. All test scenarios are written and ready to pass once auth is fixed

## Files

- `test/test_integration.c` — Integration test binary (10 scenarios)
- `test/run-integration.sh` — Wrapper script (Azurite lifecycle)
- `test/README-INTEGRATION.md` — This file

## Makefile Targets

- `make test-integration` — Build and run integration tests
- `make build/test_integration` — Build the test binary only
