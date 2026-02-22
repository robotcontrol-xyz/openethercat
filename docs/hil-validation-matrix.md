# HIL Validation Matrix

This document defines a repeatable validation matrix for production hardening on real EtherCAT hardware and in mock CI mode.

Use it together with:

- `docs/phase3-acceptance.md`
- `docs/runtime-determinism.md`
- `roadmap/production-roadmap.md`
- `scripts/hil/run_hil_matrix.sh`

## 1) Campaign modes

### A) Mock campaign (CI-safe)

Purpose:

- guard algorithmic behavior,
- verify deterministic fault sequencing,
- prevent module-boundary refactor regressions.

Core cases:

1. `advanced_systems_tests`
2. `coe_mailbox_protocol_tests`
3. `transport_module_boundary_tests`
4. `mock_hil_soak`
5. `hil_conformance_demo`
6. `redundancy_fault_sequence_demo` (JSON mode)

### B) Physical campaign (bench)

Purpose:

- validate wire behavior and working counters on real slaves,
- measure latency/jitter/drift under real scheduling and NIC conditions,
- validate topology/redundancy transitions on real fault events.

Core cases:

1. `physical_topology_scan_demo`
2. `mailbox_soak_demo` (JSON mode)
3. `dc_soak_demo` (JSON mode)
4. `topology_reconcile_demo` (JSON mode)
5. `redundancy_fault_sequence_demo` (JSON mode)

## 2) Standard execution

Mock:

```bash
scripts/hil/run_hil_matrix.sh --mode mock
```

Physical (example NIC `enp2s0`):

```bash
scripts/hil/run_hil_matrix.sh \
  --mode physical \
  --iface linux:enp2s0 \
  --with-sudo \
  --mailbox-cycles 2000 \
  --dc-duration-sec 300 \
  --dc-period-us 1000
```

Combined:

```bash
scripts/hil/run_hil_matrix.sh --mode all --iface linux:enp2s0 --with-sudo
```

Artifacts are stored in `artifacts/hil/<timestamp>/`:

- `summary.csv`
- `summary.md`
- `environment.txt`
- `logs/*.log`

## 3) KPI matrix

## Mailbox plane

Source:

- `mailbox_soak_demo` JSON output
- `MailboxDiagnostics`

Primary KPIs:

- transaction success ratio
- timeout error rate
- parse-reject/stale-counter rate
- abort-class frequency
- retry counts

Gate intent:

- no sustained timeout bursts,
- no unbounded parse/stale growth,
- failure classes correlate with injected faults only.

## Distributed clocks + cycle timing

Source:

- `dc_soak_demo` JSON output
- `DistributedClockController` quality fields

Primary KPIs:

- `wake_jitter_p95_ns`, `wake_jitter_p99_ns`, `wake_jitter_max_ns`
- `dc_jitter_p95_ns`, `dc_jitter_p99_ns`, `dc_jitter_max_ns`
- lock duty / lock transitions
- control jitter RMS (`dc_ctrl_jitter_rms_ns`)

Gate intent:

- stable lock behavior during steady conditions,
- bounded jitter tails for target period,
- no persistent unlock oscillation without external fault.

## Topology/redundancy plane

Source:

- `topology_reconcile_demo` JSON output
- `redundancy_fault_sequence_demo` timeline/KPI output

Primary KPIs:

- detection latency
- policy trigger latency
- recovery latency
- impacted cycles
- state transition correctness and ordering

Gate intent:

- valid transition sequence under fault/restore,
- no repeated trigger spam for latched sustained faults,
- recover to stable healthy state post-restore.

## 4) Tiered campaign durations

Use the same command matrix with increasing soak windows:

1. Tier-1 smoke: `5-10 min`
2. Tier-2 stability: `1-4 h`
3. Tier-3 endurance: `24 h`
4. Tier-4 release candidate: `72-168 h`

At each tier, archive:

1. artifact folder,
2. git commit SHA,
3. hardware/kernel identity from `environment.txt`,
4. pass/fail decision and notes.

## 5) Fault-injection scenarios (physical)

Recommended sequence per bench run:

1. Baseline no fault.
2. Redundant-segment cable break.
3. Cable restore.
4. Flap sequence (down/up/down/up).
5. Optional slave power-cycle.

Capture each scenario in separate artifact directories to isolate KPI deltas.

## 6) Reporting format

Minimum report block for each campaign run:

1. run identifier (`timestamp + commit + bench id`)
2. command line and env knobs
3. case-level pass/fail table (`summary.md`)
4. KPI excerpts from log JSON lines
5. deviation notes and corrective actions

## 7) Acceptance usage

Phase closure should require:

1. all matrix cases pass in mock mode,
2. physical mode passes for the target hardware profile,
3. KPI trends remain stable or improve across at least three repeated runs.

