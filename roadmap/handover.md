# Handover

## Current state

Repository is in a refactored Linux transport state with coherent module boundaries and passing mailbox/system regression tests.

Recent significant commit:
- `43b05f0` - `refactor: harden Linux transport boundaries and add HIL matrix runner`

Current in-progress context includes:
- Linux transport split across dedicated modules (`core_io`, `state_dc`, mailbox, FoE/EoE, topology, process image).
- Module-boundary regression guard (`transport_module_boundary_tests`).
- HIL matrix execution harness (`scripts/hil/run_hil_matrix.sh`) plus validation guide (`docs/hil-validation-matrix.md`).

## Where key context now lives

- `roadmap/roadmap.md` - live plan and phase status
- `roadmap/decision-log.md` - architecture decisions and rationale
- `roadmap/handover.md` - session-to-session continuation notes
- `roadmap/production-roadmap.md` - production acceptance path
- `roadmap/linux-transport-refactor-strategy.md` - refactor guardrails/patterns

## Known constraints

- Linux transport remains Linux-specific by design.
- Public transport API should remain stable through refactor phases.
- Mailbox diagnostics schema and semantics are treated as stable contract.

## Recommended immediate next tasks

1. Run Tier-1 physical matrix campaign (`5-10 min`) and archive artifacts.
2. Run Tier-2 physical matrix campaign (`1-4 h`) and compare KPI trend stability.
3. Record pass/fail against `docs/hil-validation-matrix.md` and `docs/phase3-acceptance.md`.

## Verification commands

```bash
cmake --build build-local -j4 --target openethercat advanced_systems_tests coe_mailbox_protocol_tests
./build-local/advanced_systems_tests
./build-local/coe_mailbox_protocol_tests
./build-local/transport_module_boundary_tests
scripts/hil/run_hil_matrix.sh --mode mock --build-dir build-local
```
