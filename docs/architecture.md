# Architecture

## Layered view

```mermaid
flowchart TB
    App[Application / Control Logic]
    Master[EthercatMaster]
    Mapper[IoMapper]
    Mailbox[CoE/FoE/EoE Services]
    DC[DistributedClockController]
    Topology[TopologyManager]
    Transport[ITransport]
    Linux[LinuxRawSocketTransport]
    Mock[MockTransport]
    Wire[(EtherCAT Wire)]

    App --> Master
    Master --> Mapper
    Master --> Mailbox
    Master --> DC
    Master --> Topology
    Master --> Transport
    Transport --> Linux
    Transport --> Mock
    Linux --> Wire
```

## Core classes and responsibilities

### `oec::EthercatMaster`
- Role: central orchestration facade for applications.
- Owns and coordinates: process image, state transitions, diagnostics, recovery, mailbox service, topology, and distributed clock controller.
- Key responsibilities:
- `configure(...)`: validates and binds ENI/ESI-derived configuration.
- `start()/stop()`: manages transport lifecycle and startup state transitions.
- `runCycle()`: performs cyclic process-data exchange and callback dispatch.
- `onInputChange(...)` and `setOutputByName(...)`: app-level logical signal API.
- `collectSlaveDiagnostics()` and recovery APIs: production monitoring and remediation.

### `oec::ITransport` (abstraction)
- Role: hardware/wire access contract.
- Responsibility boundary:
- Cyclic exchange (`exchange(...)`), network/slave state requests, topology discovery, redundancy health.
- Optional mailbox and protocol hooks (SDO, PDO config, emergency, FoE, EoE).

### `oec::LinuxRawSocketTransport`
- Role: real Linux EtherCAT transport over raw sockets.
- Responsibilities:
- Build/parse EtherCAT datagrams and issue cyclic `LWR` (outputs) + `LRD` (inputs) exchanges.
- Read/write AL state and AL status code registers.
- Discover topology (`discoverTopology(...)`) and report redundancy link health.
- Execute CoE mailbox SDO upload/download paths with strict response matching (address/toggle/mailbox-counter), unrelated-frame filtering, and emergency-frame queueing.

### `oec::MockTransport`
- Role: deterministic non-hardware transport for tests and demos.
- Responsibilities:
- Simulated process-image exchange.
- Fault-injection and deterministic diagnostics/recovery test support.
- Simulated topology, mailbox, FoE/EoE behavior for integration-level tests.

### `oec::TransportFactory`
- Role: runtime transport selection and creation.
- Responsibilities:
- Parse transport specs (`mock`, `linux:eth0`, `linux:eth0,eth1`).
- Create configured `ITransport` instance without app-level coupling to concrete transport classes.

### `oec::IoMapper`
- Role: logical/physical signal mapping layer.
- Responsibilities:
- Resolve signal names to process image location (byte/bit).
- Decouple application semantics from physical slave PDO layout.

### `oec::CoeMailboxService`
- Role: CoE service facade exposed by master.
- Responsibilities:
- SDO upload/download service calls.
- PDO assignment/configuration helpers.
- Emergency queue drain API.

### `oec::DistributedClockController`
- Role: cyclic DC correction and timing quality monitoring.
- Responsibilities:
- Compute correction term from reference/local time observations.
- Track jitter/offset statistics for control-loop observability.

### `oec::TopologyManager`
- Role: transport-backed topology state manager.
- Responsibilities:
- Refresh bus snapshot from transport discovery hooks.
- Detect hot-connected and missing slaves relative to configured expectation.

### `oec::SlaveDiagnostics` and recovery policy components
- Role: normalize per-slave health and recovery decisioning.
- Responsibilities:
- Decode AL status/error register context into actionable diagnostic records.
- Execute retry/reconfigure/failover policy actions and record events.

## Class diagram (high level)

```mermaid
classDiagram
    class EthercatMaster {
      +configure(NetworkConfiguration) bool
      +start() bool
      +runCycle() bool
      +stop() void
      +refreshTopology(string&) bool
      +updateDistributedClock(int64,int64) optional<int64>
    }

    class ITransport {
      <<interface>>
      +open() bool
      +close() void
      +exchange(tx, rx) bool
      +discoverTopology(snapshot, err) bool
      +requestNetworkState(state) bool
      +readSlaveState(position, state) bool
    }

    class LinuxRawSocketTransport
    class MockTransport
    class TransportFactory {
      +parseTransportSpec(spec, cfg, err) bool
      +create(cfg, err) unique_ptr~ITransport~
    }
    class IoMapper
    class CoeMailboxService
    class DistributedClockController
    class TopologyManager
    class FoeEoeService

    EthercatMaster --> ITransport : uses
    EthercatMaster --> IoMapper : owns
    EthercatMaster --> CoeMailboxService : owns
    EthercatMaster --> DistributedClockController : owns
    EthercatMaster --> TopologyManager : owns
    EthercatMaster --> FoeEoeService : owns
    LinuxRawSocketTransport ..|> ITransport
    MockTransport ..|> ITransport
    TransportFactory ..> ITransport : creates
```

## Class diagram (configuration and mapping)

```mermaid
classDiagram
    class NetworkConfiguration {
      +processImageInputBytes
      +processImageOutputBytes
      +slaves: vector~SlaveIdentity~
      +signals: vector~SignalBinding~
    }
    class SlaveIdentity {
      +name
      +position
      +vendorId
      +productCode
    }
    class SignalBinding {
      +logicalName
      +direction
      +slaveName
      +byteOffset
      +bitOffset
    }
    class ConfigurationLoader {
      +loadFromEniAndEsiDirectory(...) bool
    }
    class IoMapper {
      +bind(NetworkConfiguration) bool
      +readInputBit(name, out) bool
      +writeOutputBit(name, value) bool
    }

    ConfigurationLoader --> NetworkConfiguration : builds
    NetworkConfiguration --> SlaveIdentity : contains
    NetworkConfiguration --> SignalBinding : contains
    EthercatMaster --> NetworkConfiguration : configures from
    EthercatMaster --> IoMapper : delegates signal mapping
```

## Cyclic runtime sequence

```mermaid
sequenceDiagram
    participant APP as App
    participant M as EthercatMaster
    participant T as ITransport
    participant S as Slaves

    APP->>M: set outputs / writeOutputBytes
    APP->>M: runCycle()
    M->>T: exchange(txProcessImage)
    T->>S: LWR (outputs)
    S-->>T: LWR response + WKC
    T->>S: LRD (inputs)
    S-->>T: LRD response + WKC
    T-->>M: rxProcessImage
    M->>M: update process image + callbacks
    M-->>APP: cycle result + diagnostics
```

## Linux PDO mapping model (current)

Current Linux startup mapping is hybrid:
- Logical signal intent comes from configuration (`NetworkConfiguration`).
- Sync manager base/length (`SM2` for outputs, `SM3` for inputs) is read from each slave ESC at startup.
- FMMU entries are programmed from those SM windows to the master's logical process image.
- Full dynamic PDO descriptor discovery (`0x1C12/0x1C13`, `0x16xx/0x1Axx`) is not yet auto-derived.
- Optional runtime write-verification can read back SM2 process RAM and compare against commanded outputs (`OEC_TRACE_OUTPUT_VERIFY=1`).

```mermaid
flowchart LR
    CFG[ENI/ESI Config\nSignalDirection + SlaveName] --> MASTER[EthercatMaster::start]
    MASTER --> PREOP[Transition INIT->PRE-OP]
    PREOP --> MAP[Transport configureProcessImage]
    MAP --> SMREAD[Read SM2/SM3 from ESC]
    SMREAD --> FMMU[Program FMMU entries]
    FMMU --> SAFEOP[Transition SAFE-OP]
    SAFEOP --> OP[Transition OP]
```

```mermaid
sequenceDiagram
    participant M as EthercatMaster
    participant T as LinuxRawSocketTransport
    participant S as Slave ESC

    M->>T: configureProcessImage(config)
    T->>S: APRD SM2 (outputs)
    S-->>T: SM2 start/len
    T->>S: APWR FMMU (write direction)
    S-->>T: ack
    T->>S: APRD SM3 (inputs)
    S-->>T: SM3 start/len
    T->>S: APWR FMMU (read direction)
    S-->>T: ack
    T-->>M: success/failure
```

## Recovery decision flow

```mermaid
flowchart TD
    CycleFail[Cycle exchange fails] --> Diag[Collect per-slave diagnostics]
    Diag --> Decode[Decode AL status]
    Decode --> Policy{Policy / override}
    Policy -->|RetryTransition| Retry[Request OP transition]
    Policy -->|Reconfigure| Reconf[Reconfigure slave + OP transition]
    Policy -->|Failover| Failover[Move to failover/degraded mode]
    Retry --> Event[Append recovery event]
    Reconf --> Event
    Failover --> Event
```

## Usage snippets

### 1) Runtime-selectable transport and master startup

```cpp
#include "openethercat/config/config_loader.hpp"
#include "openethercat/master/ethercat_master.hpp"
#include "openethercat/transport/transport_factory.hpp"

oec::NetworkConfiguration cfg;
std::string error;
oec::ConfigurationLoader::loadFromEniAndEsiDirectory(
    "examples/config/beckhoff_demo.eni.xml", "examples/config", cfg, error);

oec::TransportFactoryConfig tc;
tc.mockInputBytes = cfg.processImageInputBytes;
tc.mockOutputBytes = cfg.processImageOutputBytes;
oec::TransportFactory::parseTransportSpec("linux:eth0", tc, error);
auto transport = oec::TransportFactory::create(tc, error);

oec::EthercatMaster master(*transport);
master.configure(cfg);
master.start();
```

### 2) Logical callback + logical output write

```cpp
master.onInputChange("StartButton", [&](bool state) {
    master.setOutputByName("LampGreen", state);
});

while (true) {
    if (!master.runCycle()) {
        break; // inspect master.lastError()
    }
}
```

### 3) Physical topology scan (no process-data mapping required)

```cpp
oec::TransportFactoryConfig tc;
std::string error;
oec::TransportFactory::parseTransportSpec("linux:eth0", tc, error);
auto transport = oec::TransportFactory::create(tc, error);
transport->open();

oec::TopologyManager topology(*transport);
topology.refresh(error);
auto snap = topology.snapshot();
for (const auto& slave : snap.slaves) {
    // slave.position, slave.vendorId, slave.productCode, slave.online
}
transport->close();
```

### 4) CoE SDO usage from master

```cpp
const auto wr = master.sdoDownload(2, {.index = 0x2000, .subIndex = 1}, {0x11, 0x22});
const auto rd = master.sdoUpload(2, {.index = 0x2000, .subIndex = 1});
if (!wr.success || !rd.success) {
    // wr.error / rd.error
}
```
