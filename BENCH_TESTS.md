# Bench-validation tests

Tests that prove the bootloader's protection layers fire on real silicon — not
just in unit tests against the in-process stub. They cover the two halves of the
#125 never-unreachable/unflashable invariant:

- **Rejection-layer** — the BL refuses a bad or dangerous write (host-side
  guard, BL range-check, WRP, oversize). Tests 1–5.
- **Recovery-layer** — the BL survives a partial/corrupt flash, a dead bus, or a
  power event and stays reachable + reflashable over CAN. Tests 6–10.

**Standing tests, run once per hardware revision.** Log the outcome somewhere
that outlives the current operator (sticker on the jig, spreadsheet, PR comment
that pins the last known-good date).

**v1.6.2 acceptance.** HIL-accepted on the AMS bench (MLC2 @ 500 kbps), **zero
firmware defects** (#178): power-cut recovery 10/10, G-A2 + G-B5 confirmed,
auto-jump 50/50. Four method-limited items are scheduled in
[IFS08_HIL#81](https://github.com/isc-fs/IFS08_HIL/issues/81): **B-02**
sustained-reflash soak (run on a USB-CAN adapter — the Pi's mcp251x wedges under
sustained flashing), **B-06** node-id-burn cut, **E-01** physical bus-off, and
**E-02** the sector-0 WRP-bypass positive (Test 4).

**For per-release verification** of behavioural changes between dev and main,
see [`RELEASE_BENCH.md`](RELEASE_BENCH.md) — the per-release bench checklist,
whose §A standing floor imports the recovery-layer tests here.

Prerequisites: a board provisioned per
[PROVISIONING.md §1](PROVISIONING.md#1-fresh-board-provisioning-first-time), and
a CAN adapter at **500 kbps, 68.75 % SP** (matched to the app — do **not** raise
to 1 Mbps; that is what amplified the #166 mid-write brick) + ST-Link wired up.

---

## Rejection-layer tests

## Test 1 — "Bad linker": app targets BL sector

**Setup**: build a test app whose linker script uses
`MEMORY { FLASH (rx) : ORIGIN = 0x08000000, LENGTH = ... }` — i.e.
deliberately targets the BL region. Commit the test project
separately; never leave a `0x08000000` linker as a default fallback
in real app source trees.

**Command**:
```sh
cf --interface slcan --channel /dev/cu.usbmodem1201 \
   --bitrate 500000 --node-id 0x1 \
   flash bad-linker-test.elf
```

**Expected**:
- `cf` exits **non-zero before any CAN frame goes out** — the
  firmware loader's `TouchesBootloaderSector` check rejects at
  load time. Error message names segment 0.
- No frames on the bus (verify with `candump` or `cf diagnose
  log-stream` — if the BL sees nothing, the host rejected early).

**Proves**: Layer 1 host-side defence. The BL never has to decide.

---

## Test 2 — Chunk outside writable range

**Setup**: open a session with `cf` first (`cf discover` then any
command that triggers CONNECT, e.g. `cf diagnose health`). Then
send a hand-crafted `CMD_FLASH_WRITE` targeting
`BL_BOOT_BASE + 0x100` via `cf send-raw`.

**Expected**: `NACK(BL_NACK_PROTECTED_ADDR)` (code `0x01`).
Gated by `bl_flash_range_is_writable` in `bl_flash.c` — the
`start >= BL_APP_BASE` guard.

**Proves**: Layer 1.1 BL-side range check, independent of what
the host claims.

---

## Test 3 — Erase sector 0

**Setup**: open a session, then send
`CMD_FLASH_ERASE start=0x08000000 length=0x00020000` via
`cf send-raw`.

**Expected**: `NACK(BL_NACK_PROTECTED_ADDR)`. Rejected by the
same `range_is_writable` check before the HAL ever sees the erase
request.

**Proves**: Layer 1.1 BL-side — erase rejection on the writable
range is orthogonal to program rejection.

---

## Test 4 — WRPERR: flash controller blocks a bypassed write

**Status**: ⚠️ **the WRP-bypass *positive* is not yet validated on the current
hardware revision** — it's the **E-02** trailing item in
[IFS08_HIL#81](https://github.com/isc-fs/IFS08_HIL/issues/81). The sibling
**G-B5 mask-rejection** path (Test 8) *was* validated in #178. This is the one
test that catches a WRP-on-sector-0 regression — record the outcome here after
the first successful run.

**Setup**:
1. Start with a board provisioned per
   [PROVISIONING.md §1](PROVISIONING.md#1-fresh-board-provisioning-first-time)
   — including Step 1.6 (WRP latched).
2. Attach ST-Link and connect via OpenOCD or CubeProgrammer.
3. **Deliberately bypass the BL**: build a ~30-line C test
   routine that calls `HAL_FLASH_Program(FLASH_TYPEPROGRAM_FLASHWORD,
   0x08000000, …)` directly, load it via the debugger, run it.
   No need to flash the test through the BL (we're explicitly
   testing the flash controller, not the bootloader).

**Expected**:
- `HAL_FLASH_Program` returns `HAL_ERROR`.
- `FLASH_SR1.WRPERR` bit is set.
- Debugger-side read of `0x08000000` shows the original BL vector
  table unchanged.

**Proves**: Layer 2 (WRP) engages at the hardware level. If this
test fails, WRP didn't latch — revisit
[PROVISIONING.md Step 1.6](PROVISIONING.md#step-16--latch-wrp-on-sector-0)
and the `HAL_FLASH_OB_Launch` return path.

**Re-run triggers**: whenever the BL firmware's flash-control code
path changes (unlikely; it's stable), or when the hardware revision
changes.

---

## Test 5 — Oversize image rejected

**Setup**: craft a `CMD_FLASH_VERIFY` with
`expected_size = 0xFFFFFFFF` (much larger than `BL_APP_SIZE = 768
KB`). Send via `cf send-raw` against an open session.

**Expected**: `NACK(BL_NACK_OUT_OF_BOUNDS)` from
`handle_flash_verify`'s `expected_size > BL_APP_SIZE` gate. No
metadata written.

**Proves**: Layer 1.1 image-size guard — a verify command can't
walk CRC off the end of the app region into the NVM or metadata
sectors.

---

## Recovery-layer tests

The other half of the invariant: when a flash is left partial, a bus dies, or a
unit loses power, the BL must come back **reachable and reflashable over CAN**
rather than bricking. These are also RELEASE_BENCH §A standing-floor items.

## Test 6 — Power-cut / dropped-CAN mid-write (ECC-brick recovery)

> ⚠️ **Destructive** — can require an SWD trip if recovery regresses. Treat as
> Block B in [#178](https://github.com/isc-fs/stm32-can-bootloader/issues/178);
> recover a wedged carrier with a **full PSU cycle**, not a relay tap.

**Setup**: start flashing a large app, then **interrupt a `WRITE_CHUNK`
mid-word** — cut carrier power (or drop the CAN transport) during programming —
leaving a partially-written FLASHWORD. Power back up.

**Command** (after the cut):
```sh
cf --bitrate 500000 --node-id 0x1 discover
cf --bitrate 500000 --node-id 0x1 flash app.bin   # must succeed
```

**Expected**:
- The BL answers `discover` / `diagnose health` — **not** a reboot loop, **not**
  SWD-only. The guarded app-validation read hit the partial word's double-bit
  ECC, dropped a `.noinit` breadcrumb (`bl_fault_reboot` + `g_appcheck_brick`),
  and the next boot skipped the corrupt read (the NG-1 barrier makes the
  imprecise fault land while the guard is armed).
- A fresh `cf flash` repairs the image and the app boots.
- Per #178: **10/10 power-cuts recovered first-try, zero bricks.**

**Proves**: Layer 3 recovery (#166). See
[ARCHITECTURE.md § Reliability & brick-safety](ARCHITECTURE.md#reliability--brick-safety).

## Test 7 — Corrupt NVM → reachable at default node-id (G-A2)

**Setup**: provision a custom node-id (e.g. `0x2`), then corrupt the sector-7
NVM word the pre-CAN scan reads (a destructive variant of Test 6 aimed at
sector 7, or an SWD-side scribble of the NVM head).

**Expected**: after reboot the BL answers at the **compile-time default**
node-id (not `0x2`), NVM in degraded mode (reads not-found, writes refused).
Recover with `cf … config nvm format` + re-provision.

**Proves**: Layer 3 — G-A2 degraded-NVM recovery keeps a corrupt-NVM unit
discoverable instead of reboot-looping (#178; the **B-06** node-id-burn-cut
variant is trailing in IFS08_HIL#81).

## Test 8 — apply-wrp rejects a non-sector-0 mask (G-B5)

**Command** (against an open session):
```sh
cf --bitrate 500000 --node-id 0x1 config ob apply-wrp --sector-mask 0x02
cf --bitrate 500000 --node-id 0x1 config ob apply-wrp --sector-mask 0x03
```

**Expected**: each is **NACKed** (`BL_NACK_UNSUPPORTED`) with **no option-byte
change** — `bl_obyte_apply_wrp` refuses any bitmap with bits outside sector 0.
The sector-0 happy path (`--sector-mask 0x01`, PROVISIONING §1.6) still works.

**Proves**: Layer 2/3 — G-B5 stops a fat-fingered mask from WRP-locking an
app/NVM sector and making the unit unflashable. Validated in #178.

## Test 9 — Multi-bus reply-on-origin (#120)

**Setup**: move the CAN adapter to each FDCAN tap in turn — FDCAN1 (PD0/PD1),
FDCAN2 (PB12/PB13), FDCAN3 (PG10/PG9).

**Expected**: on each bus, `discover` answers and a range-check NACK (e.g.
Test 2) comes back **on the same bus** the request arrived on. One image serves
all three.

**Proves**: Layer 3 reachability — a board reaches the host no matter which
FDCAN it taps (the point of #120). Run the standing rejection tests on whichever
tap the bench uses.

## Test 10 — Bus-off recovery (NG-9)

**Setup**: force a physical bus-off — briefly short CANH/CANL under load — then
remove the fault.

**Expected**: the BL rejoins and answers `discover` again; DTC `0x0040`
`BL_DTC_FDCAN_BUSOFF` is logged with a per-boot recovery count. The count isn't
on the wire, so confirmation is partly physical — the **E-01** trailing item in
IFS08_HIL#81.

**Proves**: Layer 3 — a transient bus-off doesn't leave a bus permanently deaf
(NG-9 un-wedges the HAL on a Stop/Start timeout).

## Test 11 — One-step SWD seed provisioning (#183)

**Setup**: on a bare board, in the same SWD session, flash the bootloader AND
program a valid provisioning seed FLASHWORD at `0x080FFFC0` (magic `0xB0070D1D`,
node-id, its complement, CRC32) — e.g. `cf swd-flash CAN_BL.bin --seed-node-id
0x2` once host support lands. Do **not** provision over CAN.

**Command** (after a power-cycle, no `cf provision`):
```sh
cf --bitrate 500000 discover
```

**Expected**: the board answers at the **seeded** node-id (`0x2`), not the
compile-time default — the bootloader consumed the seed into NVM on first boot.
A second power-cycle still shows `0x2` (one-shot: NVM holds it now; the seed is
ignored thereafter). A later `cf provision` over CAN overrides it.

**Proves**: Layer 3 — node-id commissioning with no CAN round-trip. Pair with a
**negative** check: a deliberately truncated/half-written seed must come up
reachable at the **default** node-id (the seed read is ECC-guarded), never a
brick.

---

## Test-run log

Record outcomes here after each bench run, one row per test:

| Test | Date | HW rev | Operator | Result | Notes |
|------|------|--------|----------|:------:|-------|
| 1 — bad-linker host reject |  |  |  |  |  |
| 2 — chunk out of range |  |  |  |  |  |
| 3 — erase sector 0 |  |  |  |  |  |
| 4 — WRPERR bypass positive (E-02) |  |  |  |  |  |
| 5 — oversize verify |  |  |  |  |  |
| 6 — power-cut recovery (#166) |  |  |  |  |  |
| 7 — corrupt-NVM default-id (G-A2) |  |  |  |  |  |
| 8 — apply-wrp mask reject (G-B5) |  |  |  |  |  |
| 9 — multi-bus reply-on-origin |  |  |  |  |  |
| 10 — bus-off recovery (NG-9) |  |  |  |  |  |
| 11 — SWD seed provisioning (#183) |  |  |  |  |  |
