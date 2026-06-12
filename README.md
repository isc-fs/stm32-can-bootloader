![ISC Logo](http://iscracingteam.com/wp-content/uploads/2022/03/Picture5.jpg)

# IFS08 · STM32 CAN Bootloader

Custom bootloader for the **STM32H733** ECUs of the IFS08 car. It lives in
the first 128 KB of flash (sector 0) and speaks **classic CAN** (not CAN FD)
on all three FDCAN buses at once — **500 kbps, 68.75 % sample point, wire
protocol 0.2** — to erase, program, verify and launch an application image at
`0x08020000`. The bus rate is matched bit-for-bit to the application, so the
host adapter and every node on the bus must run it. Driven by the host-side
**`cf`** CLI ([`isc-fs/can-flasher`](https://github.com/isc-fs/can-flasher))
over any SLCAN / SocketCAN / PCAN adapter.

> **The one rule.** The bootloader is the only component whose failure means
> opening a sealed ECU enclosure in the car. So its single invariant is that it
> **can never become unreachable or unflashable over CAN** — every feature below
> serves that. HIL-validated zero-bricks on the AMS bench (v1.6.2, #178).

## What it does

- **Survives a botched flash.** An interrupted programming cycle — a power-cut
  or a dropped CAN link mid-write — leaves the unit reachable and reflashable,
  not bricked: it self-recovers and you just re-run `cf flash`, no debugger.
  Backed by an always-on independent hardware watchdog (IWDG) and HSE
  clock-security, so a hang or a dead crystal becomes a clean reset, never a
  silent dead bus.
- **Flashes firmware over CAN.** Receives an image, programs it to the
  application region (sectors 1–6, 768 KB), verifies a CRC-32, and stamps a
  metadata word so the next boot auto-jumps.
- **Serves a whole shared bus from one image.** Listens on FDCAN1/2/3 at once
  and replies on the bus a request arrived on, so one binary reaches every ECU;
  each unit is addressed by a node ID burned at flash time.
- **Protects itself.** Range-checks every write, refuses to erase its own
  sector, and supports option-byte WRP latching of sector 0 (one-way over CAN;
  the application stays reflashable) so a misbehaving app can never overwrite
  the bootloader.
- **Is observable.** Logs faults to a DTC ring, streams live telemetry, and
  holds a 30 s session latch — a software inactivity timer, distinct from the
  hardware watchdog — so a host can flash in the pit, recover a unit, or debug
  over the bus.

## Quick start

**Flashing or provisioning a board** (operators) → [**PROVISIONING.md**](PROVISIONING.md).
Once the bootloader is on the board and the adapter is on the bus at 500 kbps:

```sh
cf --bitrate 500000 discover                      # find the node(s) on the bus
cf --bitrate 500000 --node-id 0x1 flash app.bin   # flash one by its node ID
```

(set `--interface` / `--channel` for your adapter — see PROVISIONING.md.)

**Working on the firmware** (developers) → [**CONTRIBUTING.md**](CONTRIBUTING.md)
for the build + host-test loop, and [**ARCHITECTURE.md**](ARCHITECTURE.md) for
the memory map, boot flow, and the brick-safety design.

## Documentation

| Read when | Doc |
|---|---|
| Taking a blank board to shipping-ready, or recovering one in the pit | [**PROVISIONING.md**](PROVISIONING.md) |
| Proving the rejection + recovery layers fire on real hardware | [**BENCH_TESTS.md**](BENCH_TESTS.md) |
| Cutting a release — the per-release bench checklist | [**RELEASE_BENCH.md**](RELEASE_BENCH.md) |
| What changed between releases | [**CHANGELOG.md**](CHANGELOG.md) |
| Memory map, boot flow, and the brick-safety / recovery design | [**ARCHITECTURE.md**](ARCHITECTURE.md) |
| Building, testing, and contributing to the firmware | [**CONTRIBUTING.md**](CONTRIBUTING.md) |
| The host-side unit-test suite | [`tests/unit/README.md`](tests/unit/README.md) |
| Phase-by-phase delivery history (auto-generated) | [ROADMAP.md](ROADMAP.md) |

**Current release: v1.6.2** — see [CHANGELOG.md](CHANGELOG.md). Classic CAN,
500 kbps @ 68.75 % on FDCAN1/2/3, wire protocol 0.2.

---

*ISC Racing Team*
