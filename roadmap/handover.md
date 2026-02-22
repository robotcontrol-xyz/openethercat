# Handover

## Current state

Repository is in a refactored Linux transport state with coherent module boundaries and passing mailbox/system regression tests.

Recent significant commit:
- `7f33d8b` - `refactor: modularize Linux raw socket transport internals`

Current in-progress context includes:
- Roadmap/doc relocation to `roadmap/` folder.
- Additional context docs added for continuity (`roadmap.md`, `decision-log.md`, `handover.md`).

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

1. Update any remaining references from `docs/...roadmap...` to `roadmap/...`.
2. Commit relocation/context-doc updates as one coherent docs+structure commit.
3. Run extended physical HIL validation matrix now that module boundaries are guarded by tests.

## Verification commands

```bash
cmake --build build-local -j4 --target openethercat advanced_systems_tests coe_mailbox_protocol_tests
./build-local/advanced_systems_tests
./build-local/coe_mailbox_protocol_tests
./build-local/transport_module_boundary_tests
```
