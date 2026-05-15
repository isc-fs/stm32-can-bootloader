![ISC Logo](http://iscracingteam.com/wp-content/uploads/2022/03/Picture5.jpg)

# IFS08 · STM32 CAN Bootloader

Custom bootloader for the **STM32H733** nodes of the IFS08. Lives in
the first 128 KB of flash (sector 0) and speaks classic CAN (not
CAN FD) over FDCAN2 to erase, program, verify and jump to an
application image at `0x08020000`. Companion to the host-side
[`isc-fs/can-flasher`](https://github.com/isc-fs/can-flasher) CLI,
which talks this protocol through any SLCAN / SocketCAN / PCAN
adapter.

## What it does

- Receives firmware over CAN, programs it to the application region
  (sectors 1–6, 768 KB), verifies a CRC-32 of the received image,
  and stamps a metadata FLASHWORD so the next boot auto-jumps.
- Protects itself: range-checks every write against
  `[BL_APP_BASE, BL_APP_END]`, rejects erase commands targeting
  sector 0, supports option-byte WRP latching so a misbehaving app
  can never overwrite the bootloader even with a bad linker script.
- Logs faults to a DTC ring, streams live telemetry, holds a
  session latch with a 30 s watchdog — everything a host tool needs
  to flash in the pit, recover a brick, or debug over the bus.

## Documentation

| Read when | Doc |
|---|---|
| Taking a blank board to shipping-ready, or recovering one in the pit | [**PROVISIONING.md**](PROVISIONING.md) |
| Proving the protection layers fire on real hardware | [**BENCH_TESTS.md**](BENCH_TESTS.md) |
| Cutting a release — mechanical bench checklist per-PR | [**RELEASE_BENCH.md**](RELEASE_BENCH.md) |
| What changed between releases | [**CHANGELOG.md**](CHANGELOG.md) |
| Understanding the memory map, boot flow, and design rationale | [**ARCHITECTURE.md**](ARCHITECTURE.md) |
| Working on the bootloader code itself | [**CONTRIBUTING.md**](CONTRIBUTING.md) |
| Phase-by-phase delivery history (auto-generated) | [ROADMAP.md](ROADMAP.md) |

## Quick status

**v1.0.x** — feature-complete against the v1.0.0 host-tool contract,
plus a hardening pass covering: ISO-TP deadline encapsulation,
CF-length-1 stall fix, dispatcher NACK on malformed PCI, BL→APP
jump barriers + IRQ window, SP-range predicate unified across
`CheckApplication` and `JumpToApplication`, BKPSRAM log-ring
defence-in-depth, persistent `flash_write_count` in the health
record, TX-FIFO drain before terminal opcodes, OB protocol-
invariant cleanup. Phases 1–4 (frame layout, ISO-TP, core opcodes,
flash opcodes, session watchdog, health + DTC + log + live data,
option bytes + WRP) all shipped. Phase 5 (signed images, replay
counter, encrypted transport) is deferred pending a deployment-
model change.

**Test surface**: 65 host-side unit tests cover ISO-TP, NVM, log,
fwinfo, dispatcher gates, ID parsing, app-stack validation. See
[`tests/unit/README.md`](tests/unit/README.md).

---

*ISC Racing Team — IFS08 Driverless*
