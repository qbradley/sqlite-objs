# Orchestration Log: Agent Samwise — Testing Strategy Evaluation

**Timestamp:** 2026-03-10T044500Z
**Agent:** Samwise (QA)
**Task:** Testing strategy evaluation for sqlite-objs
**Status:** ✅ Completed

## Artifact

- **research/testing-strategy.md** (682 lines)

## Key Recommendation

**Four-layer test pyramid:**
1. **Layer 1 — Unit Tests (C Mocks):** ~300 tests, <5s, swappable `azure_ops_t` vtable
2. **Layer 2 — Integration (Azurite):** ~75 tests, <60s, real HTTP against emulator
3. **Layer 3 — Fault Injection (Toxiproxy):** ~30 tests, <5min, network latency/drops
4. **Layer 4 — Real Azure (CI):** ~75 tests, weekly, real storage account validation

## Critical Architectural Requirement

**VFS must use a swappable Azure operations vtable** (`azure_ops_t` function pointers) for testability. This is non-negotiable. Precedent: SQLite's own test patterns, Azure SDK for C, mvsqlite.

## Key Tools Evaluated

- **Azurite:** Supports page blobs, leases, block blobs, shared key auth. Known gaps: Range edge cases, lease timing, IP URLs. Cannot inject failures.
- **Toxiproxy:** TCP proxy with HTTP-controlled fault injection (latency, timeouts, resets, blackholes)
- **No existing C Azure mock libraries** — we write our own at the azure_client API boundary

## Integration

Findings drove D5 (testing pyramid decision) and critical architectural constraint on Aragorn/Frodo implementations. Samwise responsible for building mock_azure_ops.c and Layer 1-2 test harness in MVP 1.

## Handoff

Test infrastructure patterns established. MVP 1 scope: Layers 1+2 (Unit + Azurite). MVP 2+: Layers 3+4.
