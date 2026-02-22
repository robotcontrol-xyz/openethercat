# Decision Log

## 2026-02-22 - Keep Linux-specific naming for raw-socket transport

Decision:
- Keep class/file naming with `linux_*` prefix (`LinuxRawSocketTransport`, `linux_raw_socket_transport_*.cpp`).

Why:
- Implementation depends on Linux-specific `AF_PACKET`/`sockaddr_ll` stack and ioctl APIs.
- Generic naming would imply cross-platform portability that does not exist today.

Consequence:
- Naming stays explicit and honest.
- Future cross-platform work can introduce platform-agnostic facade + Linux backend.

## 2026-02-22 - Refactor strategy: facade + internal Linux modules

Decision:
- Keep `LinuxRawSocketTransport` as stable facade while splitting implementation by responsibility.

Why:
- Reduces monolith complexity and risk.
- Improves maintainability/testing without API churn.

Consequence:
- New modules: mailbox, FoE/EoE, topology, process-image.
- CMake/build updated accordingly.

## 2026-02-22 - Mailbox diagnostics schema version 2

Decision:
- Use `MailboxDiagnostics.schemaVersion = 2`.
- Add FoE/EoE counters (`foe_*`, `eoe_*`).

Why:
- Preserve machine-parseable telemetry compatibility while expanding visibility.

Consequence:
- Docs and tests aligned to schema v2.

