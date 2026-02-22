# OpenEtherCAT Roadmap (Live)

## Scope

This roadmap tracks production-hardening work for the C++17 Linux-focused EtherCAT master stack.

## Current phase snapshot

- Phase 1: Core stack foundation and architecture: largely complete.
- Phase 2: Mailbox robustness and diagnostics: in strong progress.
- Phase 3: Linux transport modularization/refactor for maintainability: in strong progress.
- Remaining production gaps: long-run HIL conformance, broader interoperability matrix, deep DC hardware coupling.

## Completed highlights

- Linux raw-socket transport implemented and operational with cyclic I/O.
- CoE SDO path with retry/backoff/status modes and diagnostics counters.
- FoE/EoE baseline Linux wire path and API integration.
- Topology scan tools and diagnostics demos.
- Extensive docs and examples including Beckhoff I/O and DS402 demo paths.
- Transport refactor into Linux-scoped modules:
  - `linux_raw_socket_transport.cpp`
  - `linux_raw_socket_transport_mailbox.cpp`
  - `linux_raw_socket_transport_foe_eoe.cpp`
  - `linux_raw_socket_transport_topology.cpp`
  - `linux_raw_socket_transport_process_image.cpp`

## Active workstream

### Linux transport architecture hardening

- Keep `LinuxRawSocketTransport` as facade.
- Keep internal responsibilities split by Linux-specific module.
- Preserve diagnostics semantics and public API stability.

## Next steps (ordered)

1. Execute Tier-1 and Tier-2 physical HIL matrix runs with `scripts/hil/run_hil_matrix.sh`.
2. Archive KPI evidence and compare trend stability across repeated runs.

## Validation gate for each refactor step

- Build targets:
  - `openethercat`
  - `advanced_systems_tests`
  - `coe_mailbox_protocol_tests`
- Tests must pass before commit.
- No public API break unless explicitly planned.
