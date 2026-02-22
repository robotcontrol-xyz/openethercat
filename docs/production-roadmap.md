# Production Roadmap

This roadmap defines the path from current openEtherCAT state to a production-ready Linux EtherCAT master.

## Objective

Reach a release state with:

- Stable cyclic data path under long-run load.
- Deterministic mailbox behavior across representative ESC/slave variants.
- Hardware-validated distributed clock synchronization.
- Fault-tolerant topology/redundancy handling.
- Repeatable HIL/conformance evidence with KPI thresholds.

## Exit criteria for "production-ready"

All criteria below must pass:

1. 72h continuous run with target slave chains and no unrecovered OP loss.
2. Bounded cycle jitter and latency percentiles within target profile.
3. Mailbox SDO/PDO operations complete under injected stale frames, emergency traffic, and transient timeouts.
4. Cable/slave fault recovery within defined cycle/time budget.
5. Reproducible CI matrix + packaged artifacts + release notes.

## Phase 1: Core protocol hardening (Mailbox + observability)

Status: Complete.

### Scope

- Complete mailbox robustness:
- SDO segmented upload/download strict matching, counter/toggle checks.
- Status-mode strategy (`strict|hybrid|poll`).
- Retry/backoff policy.
- Emergency queue integration.
- Mailbox diagnostics counters + soak tooling.

### Deliverables

- Transport-level mailbox diagnostics API with stable schema.
- `mailbox_soak_demo` with machine-readable output mode.
- Regression tests for mixed mailbox stream and error class coverage.

### Acceptance gate

- `mailbox_soak_demo` success rate >= 99.99% on reference chain for 1M SDO transactions.
- No deadlock/livelock under injected stale/mismatched mailbox frames.

## Phase 2: Distributed clocks and deterministic runtime

Status: Not started.

### Scope

1. Implement real DC register programming and startup synchronization sequence.
2. Closed-loop correction with explicit drift/jitter KPIs on hardware.
3. Runtime scheduling guidance:
- RT priority/affinity recommendations.
- Optional lock-memory/no-page-fault setup guidance for runtime process.

### Deliverables

- DC hardware integration module.
- DC diagnostics counters and trend reporting.
- `dc_soak_demo` for offset/jitter KPI collection.

### Acceptance gate

- Jitter and offset metrics within target for selected cycle periods on reference hardware (x86 + RPi class).

## Phase 3: Topology, hot-connect, and redundancy

Status: Partial architecture present.

### Scope

1. Automatic topology reconciliation with live hot-connect state updates.
2. Redundancy switchover datapath validation under cable break/restore.
3. Recovery policies tuned by fault type and slave role.

### Deliverables

- Deterministic topology state machine.
- Redundancy fault-injection test scenarios.
- Recovery KPI report integration.

### Acceptance gate

- Fault scenarios recover to OP within target time budget without process crash.

## Phase 4: Protocol breadth and device compatibility

Status: Not started for full breadth.

### Scope

1. FoE/EoE wire implementations in Linux transport.
2. Broader device compatibility matrix:
- Beckhoff IO + bridge terminals (existing baseline).
- Additional vendor slaves (I/O and drive classes).
3. PDO assignment/mapping auto-derivation hardening across mixed slaves.

### Deliverables

- FoE/EoE transport support with tests.
- Compatibility matrix document with pass/fail and notes.

### Acceptance gate

- Protocol and compatibility tests pass across target device matrix.

## Phase 5: Conformance, HIL campaigns, and release engineering

Status: Foundations exist; full campaign not complete.

### Scope

1. Long-run HIL campaigns (24h/72h/168h tiers).
2. KPI dashboard artifacts in CI.
3. Release policy:
- Versioning policy.
- API stability guarantees.
- Changelog and upgrade notes.

### Deliverables

- HIL campaign definitions + automated report generation.
- CI jobs for debug/release + packaging + HIL report archive.
- Production readiness report per release.

### Acceptance gate

- All campaign tiers pass with no blocking regressions and KPI thresholds met.

## Immediate 30-day execution plan

1. Finish Phase 1 remaining items:
- emergency queue bounds + overflow reporting
- timeout/error taxonomy
- retry-exhaustion tests
2. Start Phase 2 spike:
- implement minimal DC register programming prototype on one reference chain
- add KPI logging path
3. Prepare Phase 3 test bench:
- scripted cable-break and slave power-cycle fault scenarios

## Work tracking format

For each work item, track:

- Owner
- Target branch
- Test evidence link
- KPI deltas
- Risk level

Suggested risk labels:

- `R1` low
- `R2` medium
- `R3` high
- `R4` release-blocking
