# openEtherCAT

A clean C++17 EtherCAT master stack foundation for Linux, designed for extensibility and low coupling.

## Current scope

This repository provides:
- A C++17 library (`openethercat`) with SOLID-oriented interfaces.
- Linux raw-socket transport with EtherCAT LRW frame build/parse and WKC validation.
- AL state transition primitives (broadcast write/read) for INIT/PRE-OP/SAFE-OP/OP startup sequencing.
- Per-slave diagnostic hooks for AL state + AL status-code reads.
- A mock transport to run examples and tests without EtherCAT hardware.
- ENI/ESI-oriented data models and file-based ENI+ESI configuration loading.
- Configuration validation for process-image bounds and signal integrity.
- A logical I/O mapping layer for decoupling app signals from physical terminals.
- Deterministic cycle controller for fixed-period cyclic exchange with runtime reporting.
- Recovery policy engine with `RetryTransition`, `Reconfigure`, and `Failover` actions.
- Configurable AL-status recovery profile loading from JSON.
- Persistent bounded recovery event history and degraded-mode state tracking.
- CoE mailbox foundation: SDO upload/download service API, PDO mapping API, emergency queue API.
- Linux transport CoE segmented SDO upload/download implementation over configurable mailbox windows.
- FoE/EoE service APIs (read/write file, send/receive encapsulated Ethernet frame).
- Distributed clock sync controller with filtered offset, PI correction, and jitter stats.
- Topology manager with hot-connect/missing detection and redundancy health checks.
- Mock HIL soak harness for repeated fault-injection and recovery validation.
- Example app for Beckhoff EK1100 + EL1008 + EL2008 style topology.

This is an architectural foundation and demonstration.
Implemented now: startup state sequence to OP, per-slave diagnostics/recovery, CoE service APIs, DC correction algorithm, and topology/redundancy management APIs.
Implemented now (Linux transport): raw EtherCAT cyclic exchange, AL state/control/status access, per-slave state/status reads, segmented CoE SDO upload/download with mailbox polling, basic topology scanning by station position, and primary/secondary link failover for cyclic frames.
Still pending for hard real-world production: full CoE mailbox coverage beyond SDO basics (complete mailbox status handling across ESC variants), FoE/EoE wire-format transport implementation, distributed-clock hardware timestamp/DC-register closed-loop wiring, exhaustive topology discovery across diverse ESCs, full redundancy switchover datapath validation under load, and long-duration real-HIL validation campaigns.

## Build

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
cmake --build build --target docs
```

If Doxygen is not installed, the `docs` target prints an installation hint.

## Run example

```bash
./build/beckhoff_io_demo
./build/periodic_controller_demo
./build/recovery_diagnostics_demo
./build/recovery_profile_demo
./build/coe_dc_topology_demo
./build/mock_hil_soak
./build/hil_conformance_demo
./build/ds402_cubic_trajectory_demo
./build/el6692_bridge_demo
./build/el6692_structured_bridge_demo
./build/el6751_can_bridge_demo
```

The demo uses `MockTransport` so it runs without root and without EtherCAT hardware.
The demo loads configuration from:
- `examples/config/beckhoff_demo.eni.xml`
- `examples/config/beckhoff_devices.xml`
- `examples/config/recovery_profile.json` (for recovery profile demo)

Real NIC demo (requires root and EtherCAT interface):

```bash
sudo ./build/linux_raw_socket_cycle_demo eth0
```

## Architecture summary

- `transport`: `ITransport` abstraction with Linux raw socket and mock implementations.
- `config`: ENI/ESI-oriented network/slave/signal data models and file loader.
- `mapping`: `IoMapper` binding logical names (e.g., `StartButton`) to process-image bits.
- `master`: `EthercatMaster` orchestration loop invoking transport exchange and callbacks.
- `master`: `CycleController` fixed-period execution thread with per-cycle reporting.
- `master`: slave diagnostics and recovery policy (`slave_diagnostics`).
- `master`: recovery event history API (`recoveryEvents`, `clearRecoveryEvents`) and degraded flag.
- `master`: `CoeMailboxService` for SDO/PDO/emergency flows.
- `master`: `DistributedClockController` for drift/jitter correction.
- `master`: `TopologyManager` for discovery/hot-connect/missing/redundancy checks.
- `master`: `FoeEoeService` for firmware and Ethernet-over-EtherCAT API integration.
- `master`: `HilCampaignEvaluator` for KPI + conformance-rule evaluation.

## Documentation

- `docs/architecture.md`: layered architecture and runtime/recovery sequence diagrams.
- `docs/use-cases.md`: common deployment/application patterns and data flow diagrams.
- `docs/ds402-cubic-trajectory.md`: DS402 CSP motion example design and sequence.

## Extension points

- Add new drivers by implementing `ITransport`.
- Expand ENI/ESI support with schema-accurate parsing and generated bindings.
- Complete Linux raw-socket mailbox transport implementation and add FoE/EoE.
- Wire distributed-clock correction to hardware timing registers and NIC clock sources.
- Add physical-line topology scan/redundancy switchover handling.
- Run long-duration HIL soak/conformance matrix on real slave chains.
