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

## 📢 In a hurry? Jump to §5 (Recovery)

Most people open this file during an incident. §5 is the single
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

> ⚠️ **STM32H72x/H73x WRP is per-sector `WRPSn` bits, NOT the
> `WRP1A_STRT/END` range syntax used on F4/F7/L4.** A WRP'd sector 0
> silently rejects both erase and program; CubeProgrammer then fails
> verify with a data mismatch at `0x08000004` (the reset vector) while
> the *old* bootloader stays in flash. **Mass / "Full chip erase" does
> NOT clear WRP** on the H7 (WRP lives in the option bytes, and the
> protected sector can't be erased) — you must reprogram the WRP
> option byte to unprotect, as below.

**CubeProgrammer GUI (recommended):**

1. Connect via ST-LINK, open the **OB** (Option Bytes) panel.
2. Expand **Write Protection**. On the H73x each sector has a `WRPSn`
   bit (`1` = unprotected, `0` = protected). Sector 0 will read
   protected.
3. Set every sector you are not deliberately protecting to
   **unprotected** — simplest is to set the whole write-protection
   field to all-ones (all 8 sectors unprotected).
4. **Apply.** The option-byte reprogram triggers a device reset.

**CLI:** the exact field name varies by CubeProgrammer version, so
read it first rather than guessing (the old `WRP1A_STRT/END` form does
**not** apply to this part):

```sh
# 1. See the current option bytes + the exact WRP field name:
STM32_Programmer_CLI -c port=SWD -ob displ

# 2. Set that field to all-unprotected (field name from step 1,
#    e.g. WRPS / nWRP — value 0xFF = all 8 sectors unprotected):
STM32_Programmer_CLI -c port=SWD -ob <WRP_FIELD>=0xFF

# 3. Verify nothing reads protected before proceeding:
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

## 3. Changing the node ID via NVM (v1.3.0+)

Each board's 4-bit node ID lives in two places after v1.3.0:

1. **Compile-time `BL_NODE_ID`** — the fallback, set by
   `-DBL_NODE_ID=0x…` at build time (see Step 1.1). Always valid
   (the build asserts `0x1..0xE`).
2. **NVM-backed override** — a 1-byte entry under
   `BL_NVM_KEY_NODE_ID = 0x0001` in sector 7. When present and
   valid, the bootloader prefers this byte over the compile-time
   default at every boot.

This lets a fleet share one firmware image: SWD-flash the same
`.bin` to every board, then write a different node ID per board over
CAN. No per-board build, no per-board reflash. The mechanism is
documented in detail in
[`ARCHITECTURE.md § Node ID provisioning and FDCAN filtering`](ARCHITECTURE.md#node-id-provisioning-and-fdcan-filtering).

> **Fleet on one shared bus.** Several ECUs can share a single CAN bus
> even if each board taps it from a *different* FDCAN peripheral — the
> BL listens on FDCAN1/2/3 simultaneously, so one identical image
> reaches every board regardless of which one it uses
> ([rationale](ARCHITECTURE.md#multi-bus-fdcan--the-bl-serves-all-three-at-once)).
> Give each ECU a **distinct** node ID so the host can address them
> individually. On a shared bus, assign those IDs **at burn time**
> (compile-time `-DBL_NODE_ID`, or an SWD-side NVM write) — or provision
> over CAN **one ECU at a time, before they share the bus**: blank /
> identically-defaulted boards all answer to the same ID and would
> collide.

> ⚠️ Pre-v1.3.0 bootloaders defined the same key but **never read it
> back at boot**. Writing key `0x0001` on a v1.2.0 or older board has
> no observable effect; the board keeps answering at its compile-time
> ID until you reflash with v1.3.0+.

### Step 3.1 — Write the override

Talk to the board at its **current** ID — the one it's answering at
right now, which may still be the compile-time default if the board
was just flashed:

```sh
# Replace 0x1 (current ID) and 0x2 (new ID) for your case.
cf --node-id 0x1 config nvm write 0x0001 0x02
```

Value format is a `0x`-prefixed hex blob of exactly **1 byte**
(`0x00..0xFF`); anything else gets parsed as UTF-8 and won't survive
the BL's validation. The BL accepts only `0x01..0x0E` — `0x00` is
the host's reserved ID and `0x0F` is the broadcast pseudo-node, both
rejected with a silent fall-back to `BL_NODE_ID` on next boot.

### Step 3.2 — Round-trip verify

Don't trust a write you haven't read back. The host's NVM-read
opcode returns the latest live entry for the key:

```sh
cf --node-id 0x1 config nvm read 0x0001
```

Expected output:

```
key 0x0001 → 1 byte: 02
```

If the read says `key not found in NVM` the write didn't land — most
likely cause is the value was malformed (decimal instead of hex, or
more than one byte) and `cf` rejected it before sending the frame.

### Step 3.3 — Reboot the board

The override only takes effect on the next `bl_nvm_init` →
`bl_node_id_init_from_nvm` cycle, which happens once at boot. Two
options:

```sh
# Option A — over CAN, if your app cooperates (depends on what's
# in sector 1..6 right now).
cf --node-id 0x1 diagnose reset

# Option B — physical power-cycle. Always works; required if no app
# is installed yet.
```

> ℹ️ A `cf config nvm write --reset` flag is tracked at
> [isc-fs/can-flasher#231](https://github.com/isc-fs/can-flasher/issues/231)
> to wrap steps 3.1 and 3.3 into one call. Until that lands, the
> two-step dance above is the official workflow.

### Step 3.4 — Confirm the new ID is live

```sh
cf discover
```

Should now show the board at the new ID (`0x02` in the example).
`cf --node-id 0x2 diagnose health` is a stronger check — it walks
through `CONNECT` at the new ID and reads back the heartbeat, which
contains the resolved node ID in byte 1.

### Step 3.5 — Clear the override (rollback)

To return a board to its compile-time `BL_NODE_ID`, write a
zero-length tombstone under the same key:

```sh
cf --node-id 0x2 config nvm erase 0x0001
```

After reboot the BL re-reads NVM, finds no live entry, and falls
back to `BL_NODE_ID`. Useful if a bad override took effect (e.g.
collision with another board) and you want the deterministic
compile-time identity back without a SWD reflash.

---

## 4. RDP policy

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

## 5. Recovery — when things go wrong

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

### "CubeProgrammer fails verify at `0x08000004` after an SWD BL flash"

Symptom: erase + download report success, then **`Error: Data
mismatch found at address 0x08000004`** (often `0x9D` instead of the
expected reset-vector byte) and `Download verification failed`.

Cause: **sector 0 is write-protected (WRP).** The erase and program
were silently rejected, so the *old* bootloader is still in flash —
the mismatch is at the reset vector, the first word that differs
between the old and new image (the stack-pointer word at `0x08000000`
usually matches, which is why the first reported mismatch is at
`+4`). This is the expected state on any board that has had
`apply-wrp` run on it (e.g. a provisioned production unit, or a bench
board after testing WRP).

Fix: clear WRP per [§2.1](#step-21--clear-wrp) (reprogram the `WRPSn`
option byte — **not** "Full chip erase", which can't clear WRP on the
H7), then reflash. On a bench / iteration board, leave WRP off
afterward.

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

## 6. Quick reference — commands by goal

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
| Set runtime node-id override (v1.3.0+) | `cf --node-id <current> config nvm write 0x0001 <new>` then reboot |
| Clear node-id override (back to compile-time) | `cf --node-id <current> config nvm erase 0x0001` then reboot |
