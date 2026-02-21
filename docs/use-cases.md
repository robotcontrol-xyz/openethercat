# Use Cases

## 1) Simple digital I/O station (EK1100 + EL1008 + EL2008)

```mermaid
flowchart LR
    Input[EL1008 Inputs] --> Map[Logical Signal Map]
    Map --> AppLogic[App Callback Logic]
    AppLogic --> Map2[Logical Output Mapping]
    Map2 --> Output[EL2008 Outputs]
```

Reference example: `examples/beckhoff_io_demo.cpp`.

## 2) Recovery diagnostics and profile-driven policy

```mermaid
flowchart TD
    Fault[Injected/Detected fault] --> Diagnostics[collectSlaveDiagnostics()]
    Diagnostics --> Profile[Recovery override profile]
    Profile --> Action[Retry / Reconfigure / Failover]
    Action --> History[recoveryEvents()]
```

Reference examples:
- `examples/recovery_diagnostics_demo.cpp`
- `examples/recovery_profile_demo.cpp`

## 3) Topology + DC + mailbox service integration

```mermaid
flowchart TB
    Topo[Topology refresh] --> Master[EthercatMaster]
    DC[DC sample update] --> Master
    SDO[SDO/PDO config] --> Master
    Master --> Cycle[Cyclic exchange]
```

Reference example: `examples/coe_dc_topology_demo.cpp`.

## 4) Long-run HIL-style campaign

```mermaid
flowchart TD
    Soak[Run N cycles + fault injections] --> KPI[Collect KPIs]
    KPI --> Conformance[Evaluate conformance rules]
    Conformance --> Report[PASS/FAIL report]
```

Reference examples:
- `examples/mock_hil_soak.cpp`
- `examples/hil_conformance_demo.cpp`

## 5) EL6692 master bridge pattern (dual strand data exchange)

```mermaid
flowchart LR
    A[Master A + EL6692_A] -->|Bridge TX payload| B[Master B + EL6692_B]
    B -->|Bridge TX payload| A
    A --> AApp[Strand A App Logic]
    B --> BApp[Strand B App Logic]
```

Reference example: `examples/el6692_bridge_demo.cpp`.

The example demonstrates cyclic payload exchange over a simulated EL6692 bridge data area
using two independent EtherCAT master instances and two process images.

## 6) EL6692 structured bridge protocol (CRC + seq/ack + retry/timeout)

```mermaid
sequenceDiagram
    participant A as Master A Endpoint
    participant W as EL6692 Bridge
    participant B as Master B Endpoint

    A->>W: Frame(seq, cmd, payload, crc)
    W->>B: Forward frame
    B->>W: Ack(ackSeq, crc)
    W->>A: Forward ack
    Note over A: If ack timeout, retry up to N then declare timeout
```

Reference example: `examples/el6692_structured_bridge_demo.cpp`.

## 7) EL6751 CAN bridge pattern

```mermaid
flowchart LR
    App[Application] --> TxPDO[EL6751 TX PDO Buffer]
    TxPDO --> Bridge[EL6751 CAN Bridge]
    Bridge --> CAN[(CAN Bus)]
    CAN --> Bridge
    Bridge --> RxPDO[EL6751 RX PDO Buffer]
    RxPDO --> App
```

Reference example: `examples/el6751_can_bridge_demo.cpp`.

The example demonstrates packing/unpacking CAN frames into process-image regions, status bit handling,
and cyclic transfer behavior through a simulated EL6751 bridge path.
