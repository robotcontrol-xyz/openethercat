# openEtherCAT

A clean C++17 EtherCAT master stack foundation for Linux, designed for extensibility and low coupling.

## Current scope

This repository provides:
- A C++17 library (`openethercat`) with SOLID-oriented interfaces.
- Linux raw-socket transport with EtherCAT datagram build/parse, cyclic `LWR`/`LRD`, and WKC validation.
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
- Mailbox robustness hardening: strict SDO response matching (index/subindex + segment toggle) with filtering of unrelated CoE mailbox frames.
- Mailbox robustness hardening: SM0/SM1 status-aware mailbox gating and bounded retry/backoff for transient timeout/busy conditions.
- Mailbox robustness hardening: mailbox-counter correlation per SDO transaction and emergency-frame queueing during mailbox waits.
- Mailbox regression coverage: mixed-stream protocol tests for emergency + stale frame + valid correlated response selection.
- Mailbox status modes for ESC variance: `OEC_MAILBOX_STATUS_MODE=strict|hybrid|poll` (default `hybrid`).
- Mailbox emergency queue hardening: bounded queue with overflow/drop accounting (`OEC_MAILBOX_EMERGENCY_QUEUE_LIMIT`).
- Mailbox error taxonomy diagnostics: timeout/busy/parse/stale/abort/transport-IO class counters.
- FoE/EoE service APIs (read/write file, send/receive encapsulated Ethernet frame).
- Distributed clock sync controller with filtered offset, PI correction, and jitter stats.
- Linux transport DC hardware prototype: per-slave DC system-time sampling and DC offset register writes.
- Topology manager with hot-connect/missing detection and redundancy health checks.
- Mock HIL soak harness for repeated fault-injection and recovery validation.
- Example app for Beckhoff EK1100 + EL1004 + EL2004 style topology.

This is an architectural foundation and demonstration.
Implemented now: startup state sequence to OP, per-slave diagnostics/recovery, CoE service APIs, DC correction algorithm, and topology/redundancy management APIs.
Implemented now (Linux transport): raw EtherCAT cyclic exchange, AL state/control/status access, per-slave state/status reads, segmented CoE SDO upload/download with mailbox polling, basic topology scanning by station position, and primary/secondary link failover for cyclic frames.
Still pending for hard real-world production: full CoE mailbox coverage beyond SDO basics (complete mailbox status handling across ESC variants), FoE/EoE protocol-conformance hardening (full interoperability/fragmentation coverage), distributed-clock hardware timestamp/DC-register closed-loop wiring, exhaustive topology discovery across diverse ESCs, full redundancy switchover datapath validation under load, and long-duration real-HIL validation campaigns.

## Build

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
cmake --build build --target docs
```

If Doxygen is not installed, the `docs` target prints an installation hint.

## Debian packages

The project supports CPack component-based Debian packaging with:
- Runtime package: `libopenethercat`
- Development package: `openethercat-dev`

Generate packages:

```bash
cmake -S . -B build -DOEC_ENABLE_PACKAGING=ON -DBUILD_SHARED_LIBS=ON
cmake --build build
cpack --config build/CPackConfig.cmake -G DEB
```

Artifacts are emitted in `build/` as `.deb` files, one for each component.

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
./build/physical_topology_scan_demo linux:eth0
./build/topology_reconcile_demo
./build/redundancy_fault_sequence_demo
./build/mailbox_soak_demo linux:eth0 1 0x1018 0x01 1000
./build/dc_hardware_sync_demo linux:eth0 1 500 10
./build/dc_soak_demo linux:eth0 600 1000
./build/topology_to_eni_dump linux:eth0 generated_discovery.eni.xml 1 1
./build/foe_eoe_smoke_demo mock 1
# JSON-lines mode for CI ingestion:
OEC_SOAK_JSON=1 ./build/mailbox_soak_demo linux:eth0 1 0x1018 0x01 1000
# DC demo JSON mode + safe correction limits:
OEC_DC_SOAK_JSON=1 OEC_DC_MAX_CORR_STEP_NS=20000 OEC_DC_MAX_SLEW_NS=5000 \
  ./build/dc_hardware_sync_demo linux:eth0 1 500 10
# DC soak KPI output (runtime + DC quality):
OEC_SOAK_JSON=1 OEC_DC_CLOSED_LOOP=1 OEC_DC_SYNC_MONITOR=1 \
  ./build/dc_soak_demo linux:eth0 600 1000
# Enable closed-loop DC control directly in EthercatMaster::runCycle():
OEC_DC_CLOSED_LOOP=1 OEC_DC_REFERENCE_SLAVE=1 OEC_DC_TARGET_PHASE_NS=0 \
OEC_DC_KP=0.1 OEC_DC_KI=0.01 OEC_DC_CORRECTION_CLAMP_NS=20000 \
OEC_DC_MAX_CORR_STEP_NS=20000 OEC_DC_MAX_SLEW_NS=5000 \
  sudo ./build/beckhoff_io_demo linux:eth0
# Add sync quality supervision and policy action:
OEC_DC_SYNC_MONITOR=1 OEC_DC_SYNC_MAX_PHASE_ERROR_NS=50000 \
OEC_DC_SYNC_LOCK_ACQUIRE_CYCLES=20 OEC_DC_SYNC_MAX_OOW_CYCLES=10 \
OEC_DC_SYNC_ACTION=warn \
  sudo ./build/beckhoff_io_demo linux:eth0
# Runtime DC traces from master cycle path:
OEC_DC_CLOSED_LOOP=1 OEC_TRACE_DC=1 sudo ./build/beckhoff_io_demo linux:eth0
# Demo-level periodic DC quality snapshots (text or JSON):
OEC_TRACE_DC_QUALITY=1 OEC_TRACE_CYCLE_EVERY=100 sudo ./build/beckhoff_io_demo linux:eth0
OEC_TRACE_DC_QUALITY=1 OEC_DC_QUALITY_JSON=1 OEC_TRACE_CYCLE_EVERY=100 \
  sudo ./build/beckhoff_io_demo linux:eth0
```

`mailbox_soak_demo` mailbox diagnostics use schema `2` and include protocol-specific counters for FoE/EoE traffic:

- `foe_read_started`, `foe_read_failed`
- `foe_write_started`, `foe_write_failed`
- `eoe_send_started`, `eoe_send_failed`
- `eoe_receive_started`, `eoe_receive_failed`

Example JSON diagnostics line:

```json
{"type":"mailbox_diag","schema_version":2,"tx_started":1000,"tx_failed":2,"foe_read_started":0,"foe_read_failed":0,"foe_write_started":0,"foe_write_failed":0,"eoe_send_started":0,"eoe_send_failed":0,"eoe_receive_started":0,"eoe_receive_failed":0}
```

The demo uses `MockTransport` so it runs without root and without EtherCAT hardware.
The demo loads configuration from:
- `examples/config/beckhoff_demo.eni.xml`
- `examples/config/beckhoff_devices.xml`
- `examples/config/recovery_profile.json` (for recovery profile demo)

Project layout:
- `examples/`: application-style usage demos (I/O, motion, bridge patterns).
- `diagnostics/`: discovery/soak/fault-injection/observability tools.

### ENI/ESI relationship in this stack

`beckhoff_demo.eni.xml` and `beckhoff_devices.xml` have different roles:

- `beckhoff_demo.eni.xml` is the runtime topology + mapping contract:
  - process image sizes (`<ProcessImage .../>`),
  - expected slave list (`<Slave .../>`),
  - logical signal bindings (`<Signal .../>`).
- `beckhoff_devices.xml` is a lightweight ESI catalog used as identity metadata:
  - maps device `name` -> `vendorId` + `productCode` (`<Device .../>` entries),
  - can include many Beckhoff devices beyond one specific demo topology.

How loading works (`ConfigurationLoader::loadFromEniAndEsiDirectory(...)`):

1. Parse ENI first and build `NetworkConfiguration`.
2. Scan all `*.xml` in the ESI directory and collect `<Device .../>`/`<Slave .../>` identity entries.
3. Merge by slave `name`:
   - only fill missing ENI identity fields (`vendorId`, `productCode`),
   - ENI values win if they are already present/non-zero.
4. Run configuration validation.

Practical implication:

- Edit ENI when you change actual mapped topology/signals.
- Expand ESI catalog when you want better identity coverage for more device names.
- ESI does not override signal byte/bit mapping from ENI.

### Topology scan to ENI generator

Use `topology_to_eni_dump` to discover the current bus and generate a starter ENI file:

```bash
sudo ./build/topology_to_eni_dump linux:eth0 generated_discovery.eni.xml 1 1
```

Arguments:
- `transport-spec`
- `output-eni` (default: `generated_discovery.eni.xml`)
- `fallback-input-bytes` (default: `1`)
- `fallback-output-bytes` (default: `1`)

The generated ENI includes:
- discovered `<Slave .../>` entries,
- auto-generated `<Signal .../>` entries for known Beckhoff digital I/O product codes,
- a loader-compatible placeholder signal if no known mapping rule matches.

Real NIC demo (requires root and EtherCAT interface):

```bash
sudo ./build/linux_raw_socket_cycle_demo eth0
```

Examples that support runtime transport selection now accept:
- `mock`
- `linux:<ifname>`
- `linux:<ifname_primary>,<ifname_secondary>`

Examples:

```bash
./build/beckhoff_io_demo mock
sudo ./build/beckhoff_io_demo linux:eth0
sudo ./build/coe_dc_topology_demo linux:eth0
sudo ./build/physical_topology_scan_demo linux:eth0
./build/topology_reconcile_demo
# Topology recovery policy (phase-3 kickoff):
OEC_TOPOLOGY_POLICY_ENABLE=1 OEC_TOPOLOGY_MISSING_GRACE=1 \
OEC_TOPOLOGY_MISSING_ACTION=degrade \
  sudo ./build/physical_topology_scan_demo linux:eth0
# Redundancy policy controls:
# OEC_TOPOLOGY_REDUNDANCY_GRACE=<cycles>
# OEC_TOPOLOGY_REDUNDANCY_ACTION=monitor|retry|reconfigure|degrade|failstop
# OEC_TOPOLOGY_REDUNDANCY_HISTORY=<N>   # transition timeline history depth (default 512)
# Scripted redundancy fault sequence (timeline + KPIs):
./build/redundancy_fault_sequence_demo
```

Physical I/O troubleshooting (mapping + WKC + output RAM readback):

```bash
sudo OEC_TRACE_MAP=1 OEC_TRACE_WKC=1 OEC_TRACE_OUTPUT_VERIFY=1 \
  ./build/beckhoff_io_demo linux:eth0
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
- `docs/ethercat-primer.md`: EtherCAT fundamentals, acronym glossary, and trace interpretation guide.
  Includes process-image structure diagrams and EL1004/EL2004 byte/bit mapping examples.
- `docs/production-roadmap.md`: phased plan and acceptance gates for production readiness.
- `docs/runtime-determinism.md`: RT scheduling/affinity/memory-lock guidance and Phase 2 KPI campaign recipe.
- `docs/phase3-acceptance.md`: explicit Phase 3 software and physical validation checklist.
- `docs/dependencies.md`: required/optional dependencies and install commands (Ubuntu/Raspberry Pi OS).
- `docs/ethercat-for-dummies.md`: beginner-friendly mini-book connecting EtherCAT fundamentals to this stack.

## Extension points

- Add new drivers by implementing `ITransport`.
- Expand ENI/ESI support with schema-accurate parsing and generated bindings.
- Harden Linux raw-socket mailbox FoE/EoE interoperability and conformance coverage.
- Wire distributed-clock correction to hardware timing registers and NIC clock sources.
- Add physical-line topology scan/redundancy switchover handling.
- Run long-duration HIL soak/conformance matrix on real slave chains.
