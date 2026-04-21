# Bench-validation tests

Tests that prove the bootloader's protection layers fire on real
silicon — not just in unit tests against the in-process stub.

**Run once per hardware revision.** Log the outcome somewhere that
outlives the current operator (sticker on the jig, spreadsheet,
PR comment that pins the last known-good date).

Prerequisites: a board provisioned per
[PROVISIONING.md §1](PROVISIONING.md#1-fresh-board-provisioning-first-time)
and a CAN adapter + ST-Link wired up.

---

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

**Status**: ⚠️ **not yet validated on current hardware revision.**
This is the one test that catches a WRP-on-sector-0 regression —
record the outcome in this file after the first successful run.

**Setup**:
1. Start with a board provisioned per
   [PROVISIONING.md §1](PROVISIONING.md#1-fresh-board-provisioning-first-time)
   — including Step 1.3 (WRP latched).
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
[PROVISIONING.md Step 1.3](PROVISIONING.md#step-13--latch-wrp-on-sector-0)
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

## Test-run log

Record outcomes here after each bench run:

| Date | HW rev | Operator | Tests 1–3, 5 | Test 4 | Notes |
|------|--------|----------|:------------:|:------:|-------|
|      |        |          |              |        |       |
