# Phase 3 Acceptance Checklist (Topology, Hot-Connect, Redundancy)

This checklist defines the closure criteria for Phase 3 in two layers:

- Software completeness (CI/mocked deterministic behavior)
- Physical validation (real cable/slave fault behavior)

## A) Software completeness (must pass in CI)

1. Topology reconciliation emits deterministic change sets:
- generation monotonic
- `added/removed/updated` entries ordered and reproducible

2. Topology recovery policy executes with grace thresholds:
- missing/hot-connect/redundancy actions triggered only after grace cycles
- sustained faults do not repeatedly re-trigger while latched
- latch resets after recovery

3. Redundancy state machine behavior:
- transitions follow expected sequence under fault and restore:
  `RedundantHealthy -> RedundancyDegraded -> Recovering -> RedundantHealthy`
- transition timeline captured with generation, cycle, and reason

4. Redundancy KPI counters update:
- `degradeEvents`, `recoverEvents`, `impactedCycles`
- latency fields set when corresponding transitions occur

5. Demos compile and run:
- `topology_reconcile_demo`
- `redundancy_fault_sequence_demo`

## B) Physical validation (must pass on target hardware)

## Test bench

- Reference chain including redundancy-capable line setup.
- Record kernel/version, NIC model, CPU, and cycle period.

## Fault scenarios

1. Cable break on redundant segment.
2. Cable restore.
3. Repeated flap (down/up/down/up).
4. Optional slave power-cycle in chain.

## Required evidence per run

- command line and env knobs
- timeline output (`redundancy_fault_sequence_demo`-style fields)
- transition events (generation/cycle/reason)
- KPI summary:
  - detection latency
  - policy trigger latency
  - recovery latency
  - impacted cycles

## Pass/fail gates

- No process crash or unrecoverable deadlock during scenarios.
- Transition order remains valid for each scenario.
- Policy does not spam repeated triggers during sustained fault.
- Recovery returns to stable healthy state after restore.
- KPI values remain within your project-specific budget.

## Recommended command template

```bash
sudo OEC_TOPOLOGY_POLICY_ENABLE=1 \
  OEC_TOPOLOGY_REDUNDANCY_GRACE=2 \
  OEC_TOPOLOGY_REDUNDANCY_ACTION=degrade \
  OEC_TOPOLOGY_REDUNDANCY_HISTORY=1024 \
  ./build/redundancy_fault_sequence_demo
```

For machine-readable logs:

```bash
sudo OEC_SOAK_JSON=1 OEC_TOPOLOGY_POLICY_ENABLE=1 \
  ./build/redundancy_fault_sequence_demo
```
