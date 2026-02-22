# Linux Transport Refactor Strategy

## Goal

Keep `LinuxRawSocketTransport` as a stable `ITransport` facade while splitting internal implementation by responsibility, with Linux-specific naming and clear ownership boundaries.

## Design pattern strategy

### 1) Facade pattern (public surface)

- `LinuxRawSocketTransport` remains the only public Linux transport class.
- `ITransport` contract remains unchanged for applications and `EthercatMaster`.
- Refactors must not leak internal helper types into public API.

### 2) Internal module decomposition (single-responsibility)

Linux-only implementation is split into Linux-scoped translation units:

- `linux_raw_socket_transport.cpp`
  - Core datagram exchange, AL state control, and transport facade plumbing.
- `linux_raw_socket_transport_mailbox.cpp`
  - Mailbox retry policy, SM window/status polling, CoE mailbox read/write helpers.
- `linux_raw_socket_transport_foe_eoe.cpp`
  - FoE/EoE protocol flows built on mailbox helpers.
- `linux_raw_socket_transport_topology.cpp`
  - Topology discovery, identity readout, and redundancy health checks.
- `linux_raw_socket_transport_process_image.cpp`
  - SM/FMMU process-image mapping and simple-IO fallback mapping paths.

Future Linux-specific files should follow the same naming prefix:

- `linux_raw_socket_transport_<domain>.cpp`

### 3) Policy pattern (runtime behavior knobs)

Runtime behavior is configured via policy-like knobs, not hard-coded branches:

- `MailboxStatusMode` (`Strict`, `Hybrid`, `Poll`)
- Retry/backoff tuning (`OEC_MAILBOX_RETRIES`, `OEC_MAILBOX_BACKOFF_*`)
- Emergency queue limit (`OEC_MAILBOX_EMERGENCY_QUEUE_LIMIT`)

Refactor rule:

- New behavior switches should be policy fields or environment-driven configuration with deterministic defaults.

### 4) Template-method style flow for mailbox transactions

Mailbox transactions follow a fixed flow:

1. Resolve SM windows.
2. Write mailbox frame.
3. Read matching response frame.
4. Parse/accept payload.
5. Update diagnostics/error class.

Refactor rule:

- Keep this order centralized in shared helper methods.
- Protocol-specific code (CoE/FoE/EoE) should only provide payload encode/decode and acceptance predicate.

### 5) Diagnostics as first-class contract

`MailboxDiagnostics` and `MailboxErrorClass` are treated as stable observability contracts.

Refactor rule:

- No helper extraction may remove or silently change counter semantics.
- Counter increments remain owned by shared helpers to avoid duplication drift.

## Naming and scope rules

- File names must state Linux scope (`linux_raw_socket_transport_*`).
- Generic names are allowed only for private methods scoped to `LinuxRawSocketTransport`.
- Avoid creating cross-platform-looking filenames unless introducing a true platform-agnostic abstraction.

## Refactor guardrails

For each extraction step:

1. No public API signature changes unless explicitly planned.
2. Keep behavior equivalent (same error text when used by tests/diagnostics).
3. Build and run mailbox-focused tests:
   - `advanced_systems_tests`
   - `coe_mailbox_protocol_tests`
4. Prefer small, reversible commits.

## Next recommended extraction steps

1. Split AL/DC register helpers into `linux_raw_socket_transport_state_dc.cpp`.
2. Optionally isolate socket open/frame I/O primitives into `linux_raw_socket_transport_core_io.cpp`.
3. Add lightweight internal cohesion checks for module-boundary drift.

At each step, keep facade behavior stable and diagnostics unchanged.
