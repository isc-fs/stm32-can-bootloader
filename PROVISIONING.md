# Provisioning & recovery

Operator-facing procedures for the STM32H733 CAN bootloader:
taking a blank board to production-ready, updating the bootloader
itself without bricking, and recovering a board that's gone dark
in the pit.

Companion docs:
- [ARCHITECTURE.md](ARCHITECTURE.md) — why the protection layers
  look the way they do
- [BENCH_TESTS.md](BENCH_TESTS.md) — how to prove they still fire
  on real silicon (run once per hardware revision)

If you find yourself wanting to paste one of these commands into a
chat to check "is this right?", that means this doc needs a fix —
PR it rather than keeping the correction in your head.

---

## 📢 In a hurry? Jump to §4 (Recovery)

Most people open this file during an incident. §4 is the single
most-read section; glance there first.

---

## 0. Prerequisites

- **Hardware**: STM32H733 target + ST-Link V3 (or CMSIS-DAP compatible
  probe) on SWD + a CAN adapter (CANable or equivalent) on FDCAN2.
- **Host tools**:
  - `STM32CubeProgrammer` (friendlier OB UI) or `openocd` +
    `arm-none-eabi-gdb` (scripts better) for SWD.
  - `cf` (`can-flasher`) from
    [`isc-fs/can-flasher`](https://github.com/isc-fs/can-flasher).
    Flash tool and bootloader need matching protocol versions — if
    in doubt, rebuild both from a matching git tag.
- **Firmware artifacts**:
  - `CAN_BL.bin` — the bootloader. Build from this repo with:
    ```sh
    cmake --preset Release && cmake --build build/Release
    ```
    Output lands at `build/Release/CAN_BL.bin`.

---

## 1. Fresh-board provisioning (first time)

A brand-new H733 ships with option bytes at factory defaults: RDP
Level 0, no WRP, BOR off. Goal: get the BL onto sector 0 and latch
WRP so a misbehaving app can never overwrite it over CAN.

### Step 1.1 — Flash the bootloader via SWD

```sh
# Option A — STM32CubeProgrammer CLI
STM32_Programmer_CLI -c port=SWD -w CAN_BL.bin 0x08000000 -v

# Option B — openocd
openocd -f interface/stlink.cfg -f target/stm32h7x.cfg \
        -c "program CAN_BL.bin 0x08000000 verify reset exit"
```

Expected: success, MCU resets, LEDs go to "idle BL" pattern (see
[ARCHITECTURE.md § LED semantics](ARCHITECTURE.md)).

### Step 1.2 — Sanity-check CAN comms

Wire the CAN adapter to FDCAN2, then:

```sh
cf --interface slcan --channel /dev/cu.usbmodem1201 \
   --bitrate 500000 discover
```

Expected output — **one row, WRP column reads `✗`** (we haven't
applied it yet):

```
Node  Proto  FW Version        Git Hash  Product  WRP  Reset Cause
────  ─────  ────────────────  ────────  ───────  ───  ───────────
0x01  0.1    no app installed  —         —        ✗    PIN
```

If this fails, stop — every later step depends on working CAN.
Check adapter wiring, 120 Ω termination, and that `cf adapters`
sees the dongle.

### Step 1.3 — Latch WRP on sector 0

```sh
cf --interface slcan --channel /dev/cu.usbmodem1201 \
   --bitrate 500000 --node-id 0x1 \
   config ob apply-wrp --sector-mask 0x01
```

Default `--sector-mask 0x01` protects sector 0 only (the bootloader).
The command auto-supplies the `BL_OB_APPLY_TOKEN` safety token, ACKs
before the OB launch, then the MCU resets.

### Step 1.4 — Verify + checkpoint

```sh
cf --interface slcan --channel /dev/cu.usbmodem1201 \
   --bitrate 500000 discover
```

**Provisioning done when**:
- `WRP` column reads `✓` — sector 0 is now write-protected at the
  flash-controller level.
- You've recorded the board's serial number in whatever provisioning
  log the team uses (spreadsheet, sticker, nothing — but pick one
  and stick to it).

The board is now **shipping-ready from the bootloader's perspective**.
Application firmware can be loaded via `cf flash` afterwards; it
cannot overwrite the bootloader even with a bad linker script,
because both the BL's range-check and the flash controller's WRP
will refuse.

### Optional Step 1.5 — Enable RDP Level 1 (production units only)

For units that leave the workshop, set RDP Level 1 via
STM32CubeProgrammer to block firmware readout via SWD:

```sh
STM32_Programmer_CLI -c port=SWD -ob RDP=0xBB
```

(`0xBB` is Level 1 on H7; `0xAA` is Level 0. **Never write `0xCC`
— that's Level 2, which is irreversible.** See §3.)

---

## 2. Updating the bootloader itself

The bootloader **is not updated over CAN**. There's no protocol
command that can rewrite sector 0, and WRP would refuse even if
there were. Updates go through SWD.

### Step 2.1 — Clear WRP

WRP cleared by writing the "no sectors protected" mask. Via
CubeProgrammer (easier UI) or CLI:

```sh
STM32_Programmer_CLI -c port=SWD \
                     -ob WRP1A_STRT=0x7F WRP1A_END=0x00 \
                     -ob WRP1B_STRT=0x7F WRP1B_END=0x00

# Always verify:
STM32_Programmer_CLI -c port=SWD -ob displ
```

Confirm no WRP fields show protected sectors before proceeding. This
step is pedantic on purpose: bad BL + active WRP = bricked part
until a full chip erase on the programmer.

### Step 2.2 — Flash the new BL

Same as Step 1.1.

### Step 2.3 — Re-apply WRP

Same as Step 1.3.

### Step 2.4 — Verify

Same as Step 1.4.

---

## 3. RDP policy

STM32 read-protection has three levels. Current project policy:

| Level | Meaning | When to use |
|------:|---------|-------------|
| 0 (`0xAA`) | No protection (debug full, readout full) | Dev boards |
| 1 (`0xBB`) | Debug blocked; readout blocked; downgrade to 0 triggers full chip erase | Production units |
| 2 (`0xCC`) | Level 1 + permanent lockout; **irreversible** | **Never** |

**Level 2 is forbidden.** One-way trip: the part loses SWD access
permanently, no workflow can undo it. This project's tooling has no
code path that writes RDP at all — `cf config ob read` reports the
current level, nothing writes it. RDP changes happen out of band
via CubeProgrammer, at provisioning time only, with a second person
watching.

---

## 4. Recovery — when things go wrong

### "Board responds to `cf discover` but refuses CONNECT"

**Likely cause**: a prior `cf` run left the BL's session latch set
and the 30 s watchdog hasn't fired yet.

**Fix**: wait 30 s, or hit NRST on the board.

### "Board ignores every CAN frame after a flash"

**Likely cause**: `cf flash --jump` succeeded and the application
is running, but the app doesn't have an FDCAN filter installed, so
nothing reaches the dispatcher.

**Fix**:
1. Hit NRST on the board.
2. Within the 2 s auto-jump window, spam `cf discover` to cancel
   auto-jump and keep the BL on.
3. Once BL responds, re-flash or debug the application.

### "WRP is stuck on and I need to update the BL"

Follow [§2 — Updating the bootloader](#2-updating-the-bootloader-itself).
Clear WRP via SWD first.

If CubeProgrammer refuses the OB write, the chip is probably at
RDP Level 1 with a locked state. Downgrading to Level 0 triggers a
full mass-erase — acceptable here (you're about to reflash
anyway), but any data in sector 7 (NVM) is lost.

### "Accidentally wrote RDP Level 2"

Unrecoverable. Swap the MCU from spares, file an incident report
noting serial + who + when, audit the provisioning workflow that
allowed it.

### "Board boots but LEDs show fault"

Check `cf diagnose log-stream` — the BL logs fault reasons (HAL
init failure, bad boot magic, flash read error) to its log ring,
and the log stream replays whatever it's buffered. Most boot-time
faults leave a WARN or ERROR line explaining the cause.

---

## 5. Quick reference — commands by goal

| Goal | Command |
|------|---------|
| Find the board | `cf adapters` then `cf discover` |
| Check WRP status | `cf config ob read --json \| jq .wrp_sector_mask` |
| Apply WRP (sector 0) | `cf config ob apply-wrp --sector-mask 0x01` |
| Flash an app | `cf flash my-app.elf --verify-after --jump` |
| Force app → BL (from a running app) | `cf send-raw 0x001 03 06 01` |
| Dump BL logs live | `cf diagnose log-stream` |
| Show health record | `cf diagnose health` |
| Read option bytes | `cf config ob read` |
