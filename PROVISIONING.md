# Provisioning & bench-validation checklist

Operator-facing procedures for taking a blank STM32H733 board to a
shipping-ready state, updating the bootloader itself safely, and
verifying that the protection layers actually fire on real hardware.

Companion to `ARCHITECTURE.md` (which explains *why* the protection
layers look the way they do). This file answers the three questions
an operator has in front of the bench:

1. **How do I get a fresh board to production-ready?**
2. **How do I update the bootloader without bricking the part?**
3. **How do I prove the protections are actually on?**

If you find yourself wanting to paste one of these commands into a
chat to check "is this right?", that means this doc needs a fix —
PR it rather than keeping the correction in your head.

---

## 0. Prerequisites

- **Hardware**: STM32H733 target + ST-Link V3 (or CMSIS-DAP compatible
  probe) on SWD + a CAN adapter (CANable or equivalent) on FDCAN2.
- **Host tools**:
  - `STM32CubeProgrammer` or `openocd` + `arm-none-eabi-gdb` for the
    SWD flash step. CubeProgrammer has the friendlier option-byte UI;
    openocd scripts better.
  - `cf` (`can-flasher`) — built from `isc-fs/can-flasher` at a
    revision that matches the BL's protocol version. Check with
    `cf --version` against `BL_PROTO_VERSION_*` in `bl_proto.h`.
- **Firmware artifacts**:
  - `CAN_BL.bin` (the bootloader, targets `0x08000000`). Built from
    this repo via `cmake --preset Release && cmake --build
    build/Release`.

---

## 1. Fresh-board provisioning (first time)

A brand-new H733 ships with all option bytes at their factory
defaults: RDP Level 0, no WRP, BOR off. Goal of this flow: get the
BL onto sector 0 and then **latch WRP so a misbehaving app can never
overwrite it over CAN**.

### Step 1.1 — Flash the bootloader via SWD

```sh
# Option A — STM32CubeProgrammer CLI
STM32_Programmer_CLI -c port=SWD -w CAN_BL.bin 0x08000000 -v

# Option B — openocd
openocd -f interface/stlink.cfg -f target/stm32h7x.cfg \
        -c "program CAN_BL.bin 0x08000000 verify reset exit"
```

Expected: success, MCU resets, LED behaviour matches "idle BL waiting
for CAN traffic" from `ARCHITECTURE.md § LED semantics`.

### Step 1.2 — Sanity-check CAN comms

With the CAN adapter wired to FDCAN2:

```sh
cf --interface slcan --channel /dev/cu.usbmodem1201 \
   --bitrate 500000 discover
```

Expected output (one row, your node ID, protocol version, WRP column
reads `✗` because we haven't applied it yet):

```
Node  Proto  FW Version        Git Hash  Product  WRP  Reset Cause
────  ─────  ────────────────  ────────  ───────  ───  ───────────
0x01  0.1    no app installed  —         —        ✗    PIN
```

If this step fails, stop — every later step depends on working CAN.
See `ARCHITECTURE.md § Troubleshooting` or `can-flasher/README.md`.

### Step 1.3 — Latch WRP on sector 0 (irrevocable over CAN)

```sh
cf --interface slcan --channel /dev/cu.usbmodem1201 \
   --bitrate 500000 --node-id 0x1 \
   config ob apply-wrp --sector-mask 0x01
```

Default `sector-mask 0x01` protects **sector 0 only** — the
bootloader. The command token-gates the apply (requires
`BL_OB_APPLY_TOKEN = 0x00505257` / ASCII `"WRP\0"` LE, which `cf`
supplies automatically), ACKs before the OB launch, then the MCU
resets as the launch pumps the new WRP mask into the active area.

### Step 1.4 — Verify WRP stuck

```sh
cf --interface slcan --channel /dev/cu.usbmodem1201 \
   --bitrate 500000 discover
```

Expected row: identical to Step 1.2 **except `WRP ✓`**. If still `✗`,
the apply didn't launch — dig into logs (`cf diagnose log-stream`)
before trusting the board to production use.

The board is now **shipping-ready from the bootloader's perspective**.
Application firmware can be loaded via the usual `cf flash` flow; it
cannot overwrite the bootloader even with a bad linker script, because
both the BL's range-check (Layer 1.1) and the flash controller's WRP
(Layer 2.1) will refuse.

---

## 2. Updating the bootloader itself

The bootloader **is not updated over CAN in production** — there's no
protocol command that can rewrite sector 0, and WRP would refuse even
if there were. Updates go through SWD, which is the only path that
can clear WRP.

### Step 2.1 — Clear WRP via CubeProgrammer

WRP cleared by writing the "no sectors protected" mask (`0x000000FF`
in the raw option-byte register maps to "all sectors unprotected" on
H7 — the register uses inverted polarity). Easiest via
CubeProgrammer's OB tab, or CLI:

```sh
STM32_Programmer_CLI -c port=SWD \
                     -ob WRP1A_STRT=0x7F WRP1A_END=0x00 \
                     -ob WRP1B_STRT=0x7F WRP1B_END=0x00
```

**Verify WRP cleared** before proceeding:
```sh
STM32_Programmer_CLI -c port=SWD -ob displ
```
Look for `WRP` fields showing no protected sectors. This step is
intentionally pedantic because once the next flash runs, a bad BL
image + active WRP = bricked board requiring the next step of
recovery (chip erase via `-c halt` + `-e all`, then re-fetch the
board from the shelf of spares).

### Step 2.2 — Flash the new BL

Same as Step 1.1.

### Step 2.3 — Re-apply WRP

Same as Step 1.3 — `cf config ob apply-wrp --sector-mask 0x01`.

### Step 2.4 — Verify

Same as Step 1.4.

---

## 3. RDP policy

STM32 read-protection has three levels. Current policy for this
project:

| Level | Meaning                                 | When to use |
|------:|-----------------------------------------|-------------|
| 0     | No protection (debug full, readout full)| Dev boards only |
| 1     | Debug blocked when unlocked; readout blocked; regression to Level 0 triggers a full chip erase | Production units |
| 2     | Level 1 + permanent lockout; **irreversible** | **Never** |

**Level 2 is forbidden** by this project. It's a one-way trip: the
part loses SWD access permanently, no factory workflow can undo it, and
you cannot re-provision a unit that's drifted onto the wrong firmware.
Tooling in this repo has no code path that writes RDP — the reporting
surface is read-only (`cf config ob read` shows the current level).

**Setting Level 1** for production units is done out of band via
CubeProgrammer during the provisioning flow. Insert as Step 1.5 once a
dev board has been fully validated:

```sh
STM32_Programmer_CLI -c port=SWD -ob RDP=0xBB
```

(`0xBB` is Level 1 on H7; `0xAA` is Level 0 — the values that are
*not* Level 2.) Confirmation prompts are intentional — read them.

---

## 4. Bench-validation tests

These tests prove the protection layers actually fire on real
silicon, not just in unit tests against the in-process stub. Run them
once per hardware revision; log the result somewhere that outlives
the current operator.

### Test 4.1 — "Bad linker": app targets BL sector

**Setup**: Build a test app whose linker script uses
`MEMORY { FLASH (rx) : ORIGIN = 0x08000000, LENGTH = ... }` — i.e.
deliberately points at the BL region. Sign-off in the commit message
of the test project; do not leave it lying around as a default
fallback.

**Command**:
```sh
cf --interface slcan --channel /dev/cu.usbmodem1201 \
   --bitrate 500000 --node-id 0x1 \
   flash bad-linker-test.elf
```

**Expected**:
- `cf` exits **non-zero before any CAN frame goes out** — the
  firmware loader's `TouchesBootloaderSector` check rejects at load
  time. Error message points at segment 0.
- No frames on the bus (verify with `candump` or `cf diagnose
  log-stream` — if the BL sees nothing, the host rejected early).

**What this proves**: Layer 1 host-side defence (fix/12's
`validate_fits_app_region` and the `base_addr == BL_APP_BASE` guard
in `cli/flash.rs`). The BL never has to decide.

### Test 4.2 — Chunk outside writable range

**Setup**: Use `cf send-raw` (generic primitive for building arbitrary
frames) to send a hand-crafted `CMD_FLASH_WRITE` targeting
`BL_BOOT_BASE + 0x100` — inside the bootloader's own sector. Requires
an open session (`CONNECT` first).

**Expected**: Device answers `NACK(BL_NACK_PROTECTED_ADDR)` (code
`0x01`). The frame is gated by `bl_flash_range_is_writable` in
`bl_flash.c`, which requires `start >= BL_APP_BASE`.

**What this proves**: Layer 1.1 BL-side range check, independent of
what the host claims.

### Test 4.3 — Erase sector 0

**Setup**: Send `CMD_FLASH_ERASE start=0x08000000 length=0x00020000`
(one full BL sector) via `cf send-raw` or a hand-crafted test.

**Expected**: `NACK(BL_NACK_PROTECTED_ADDR)`. Rejected by the same
`range_is_writable` check before the HAL ever sees the erase
request.

### Test 4.4 — **WRPERR: flash controller blocks a bypassed write**

**The one test in this doc that isn't covered by automated suites
yet.** Validates that Layer 2 (WRP) would have caught the write even
if Layer 1 failed.

**Setup**:
1. Provision a board with WRP on sector 0 (Steps 1.1–1.4 above).
2. Attach ST-Link + connect via OpenOCD or CubeProgrammer.
3. **Deliberately bypass the BL**: use the debugger to load and run a
   test routine that calls `HAL_FLASH_Program(FLASH_TYPEPROGRAM_FLASHWORD,
   0x08000000, …)` directly. A 30-line C program + openocd load/run
   script is sufficient; no need to flash the test through the BL.

**Expected**: `HAL_FLASH_Program` returns `HAL_ERROR`, the flash
controller's `FLASH_SR1.WRPERR` bit is set, and a debugger-side read of
`0x08000000` shows the original BL vector table unchanged.

**What this proves**: WRP actually engages at the hardware level. If
this test fails, WRP didn't latch — revisit Step 1.3 and the
`HAL_FLASH_OB_Launch` return path.

**Status today**: ⚠️ **not validated on current hardware revision**.
Target: do this once and record the outcome here. After the first
successful run, re-validate whenever the BL firmware's flash-control
code path changes (unlikely — it's stable).

### Test 4.5 — Oversize image rejected

**Setup**: Craft a binary whose `expected_size` in `CMD_FLASH_VERIFY`
exceeds `BL_APP_SIZE` (768 KB). Easiest: flash a 700 KB app
normally, then manually issue `CMD_FLASH_VERIFY` with
`expected_size = 0xFFFFFFFF` via `cf send-raw` against an open
session.

**Expected**: `NACK(BL_NACK_OUT_OF_BOUNDS)` from `handle_flash_verify`'s
`expected_size > BL_APP_SIZE` gate. No metadata written.

---

## 5. Quick reference — commands by goal

| Goal | Command |
|------|---------|
| Find the board | `cf adapters` then `cf discover` |
| Check WRP status | `cf config ob read --json \| jq .wrp_sector_mask` |
| Apply WRP (sector 0) | `cf config ob apply-wrp --sector-mask 0x01` |
| Flash an app | `cf flash my-app.elf --verify-after --jump` |
| Force app → BL (from running app) | `cf send-raw 0x001 03 06 01` |
| Dump BL logs live | `cf diagnose log-stream` |
| Show health record | `cf diagnose health` |

---

## 6. When things go wrong

### "WRP stuck on but I want to update the BL"
Follow §2 — SWD-clear WRP first. If CubeProgrammer refuses the OB
write, the chip may be at RDP Level 1 with a locked state — a
Level-1 → Level-0 transition triggers a full mass-erase, which is
acceptable (you're about to reflash BL + app anyway) but means any
NVM data in sector 7 is lost.

### "Accidentally wrote RDP Level 2"
Unrecoverable. Swap the MCU from spares; file an incident report
noting which unit serial + who; audit the provisioning workflow that
let it happen.

### "Board responds to `cf discover` but refuses CONNECT"
Likely cause: a prior `cf` run left the BL with `g_session_active =
true` and the 30 s watchdog hasn't fired yet. Either wait 30 s or NRST
the board.

### "Board ignores every CAN frame after a flash"
Likely cause: `cf flash --jump` succeeded and the app is running, but
the app doesn't implement the ISO-TP + `APP_CTRL` convention and has
no FDCAN filter installed. NRST the board; within the 2 s auto-jump
window, blast a `cf discover` to cancel auto-jump and stay in BL.
Future: a boot-time force-BL GPIO check would make this less
time-sensitive (see ROADMAP).
