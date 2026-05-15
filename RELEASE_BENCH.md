# v1.2.0 release bench checklist

Mechanical bench session that validates the `v1.2.0` release cut. Built
to be runnable in ~45 minutes by an operator with a HIL bench, a
ST-Link, and a CAN adapter on FDCAN2. The previous `v1.1.0` baseline
shipped without bench validation of several behavioural changes; this
document closes that gap before the next `dev → main` cut.

> **Companion docs**
> - [BENCH_TESTS.md](BENCH_TESTS.md) — the *standing* protection-layer
>   bench tests. Run those first (§A below references them). They're
>   the part of this script that doesn't rotate per release.
> - [PROVISIONING.md](PROVISIONING.md) — operator setup if you've never
>   wired a board.

## Prerequisites

- **Board** provisioned per PROVISIONING.md §1 with the `v1.2.0`
  candidate bootloader (`build/Release/CAN_BL.bin` from the `dev`
  branch at or after commit `[PASTE_DEV_TIP_SHA_HERE]`).
- **Adapter + node ID**. Examples below assume:
  ```sh
  alias cfb='cf --interface slcan --channel /dev/cu.usbmodem1201 \
                --bitrate 500000 --node-id 0x1'
  ```
  Adjust `--channel`, `--node-id` to your bench.
- **Sibling node generator** for the "under load" tests (any second
  board on the same bus emitting heartbeats at ≥ 100 Hz). If you don't
  have one, use `cf send-raw` in a tight loop from a second terminal:
  ```sh
  while true; do
    cf --interface slcan --channel … --bitrate 500000 \
       send-raw 0x015 11 22 33 44 55 66 77 88
    sleep 0.01
  done
  ```
- A built **test application** image — anything that boots and emits
  a recognisable marker over UART or a status pin. Used for the
  flash + jump tests.

## How to use this document

For each test below:
1. Run the **command** column verbatim.
2. Compare against the **expected** column.
3. Tick the **PASS** column in the results table at the bottom.
4. If anything fails, **stop the script** — don't ship until the
   regression is understood. File against the relevant PR.

---

## A. Standing protection-layer tests (from BENCH_TESTS.md)

Re-run these every release cut. They're the floor: if these regress,
nothing else matters.

| # | Test | Reference | Pass criterion |
|---|---|---|---|
| A.1 | Host-side bad-linker rejection | [BENCH_TESTS.md Test 1](BENCH_TESTS.md#test-1--bad-linker-app-targets-bl-sector) | `cf flash` exits non-zero before any CAN frame |
| A.2 | BL-side range check on FLASH_WRITE | [BENCH_TESTS.md Test 2](BENCH_TESTS.md#test-2--chunk-outside-writable-range) | `NACK(BL_NACK_PROTECTED_ADDR=0x01)` |
| A.3 | BL-side range check on FLASH_ERASE | [BENCH_TESTS.md Test 3](BENCH_TESTS.md#test-3--erase-sector-0) | `NACK(BL_NACK_PROTECTED_ADDR=0x01)` |
| A.4 | WRPERR on bypass write | [BENCH_TESTS.md Test 4](BENCH_TESTS.md#test-4--wrperr-flash-controller-blocks-a-bypassed-write) | `HAL_FLASH_Program == HAL_ERROR`, `FLASH_SR1.WRPERR` set |
| A.5 | Oversize image rejected | [BENCH_TESTS.md Test 5](BENCH_TESTS.md#test-5--oversize-image-rejected) | `NACK(BL_NACK_OUT_OF_BOUNDS=0x02)` |

Test A.4 has been outstanding since hardware revision; if you can
borrow a programmer for 15 minutes during this session, do it now and
finally tick the row.

---

## B. v1.2.0-specific behavioural checks

One test per PR that changed observable behaviour. Skip a test only
if its setup is genuinely impossible on the bench (Test B.7 calls this
out explicitly).

### B.1 — `dev` matches the firmware on the board (#89)

**Why**: every test below assumes the bootloader is *the candidate
release*. Confirm before you start.

**Command**:
```sh
cfb diagnose health
```

**Expected**:
- The health reply includes a protocol version field of **0.2**
  (was 0.1 in v1.1.0). If `cf` doesn't surface the minor directly,
  decode the raw CONNECT reply with `cf send-raw … 01` and read byte
  index [2] (minor) → should be `0x02`.

**Pass**: protocol minor reads as `2`.

---

### B.2 — Bad-PCI NACK (PR #63)

**Why**: pre-v1.2.0 the BL silently dropped malformed PCI bytes. Now
it NACKs with `BL_NACK_TRANSPORT_ERROR` (0x0A).

**Command** (PCI nibble = 0x4, not SF/FF/CF/FC):
```sh
cfb send-raw 0x01 40 00 00 00 00 00 00 00
```

**Expected** (within ≤ 50 ms):
- A frame from `id=0x10` (node→host, node 0x1) carrying:
  `[03 02 00 0A]` → SF-len-3, msg_type=NACK, rejected_opcode=0,
  code=`BL_NACK_TRANSPORT_ERROR=0x0A`.

**Pass**: NACK observed; no 1 s wait.

**Pre-v1.2.0 reproduction note**: if you wired this same command
against a v1.1.0 board, you'd see **no reply at all**. The new
diagnostic visibility is the whole point of the fix.

---

### B.3 — Zero-payload CF rejected immediately (PR #78)

**Why**: pre-v1.2.0 a CF with `length == 1` (PCI only, no data) would
stall reassembly until the 1 s timeout, then NACK with
`TRANSPORT_TIMEOUT`. Now it NACKs with `TRANSPORT_ERROR` immediately.

**Command sequence** (first send a valid FF, then a zero-data CF):
```sh
# FF with total_len = 14, 6 payload bytes
cfb send-raw 0x01 10 0E AA BB CC DD EE FF
# wait for FC(CTS) reply (~10 ms)

# CF with seq=1, length=1 (PCI only, no data)
cfb send-raw 0x01 21
```

**Expected**:
- FF → FC(CTS) reply (`30 00 00 …` from `id=0x10`).
- CF → **immediately** (within ≤ 50 ms): NACK frame `[03 02 00 0A]`
  (TRANSPORT_ERROR). **Not** a 1-second wait followed by NACK 0x09.

**Pass**: time-from-CF-to-NACK is well under 100 ms.

---

### B.4 — BL→APP jump under bus load (PRs #62 + #74)

**Why**: PR #62 stopped re-enabling IRQs on the BL side of the jump
(could dispatch a queued IRQ through the new VTOR before the app's
Reset_Handler ran). PR #74 added DSB/ISB barriers around the VTOR +
MSP writes for Cortex-M7. Both manifest as "rare boot-into-fault under
high RX activity at jump time" — hard to reproduce, but the test
builds confidence.

**Command sequence**:
```sh
# (Terminal 1) Start sibling-node spam (heartbeat-like, ≥ 100 Hz)
while true; do
  cf --interface slcan --channel … --bitrate 500000 \
     send-raw 0x015 11 22 33 44 55 66 77 88
  sleep 0.01
done

# (Terminal 2) Flash app, jump
cfb flash test-app.elf --verify-after --jump
```

**Expected**:
- `cf flash` reports success and the JUMP ACK is observed.
- The board boots into the app cleanly (app's UART marker / pin /
  LED pattern as you configured it).
- No fault LED, no fault-handler trap, no instant reboot.

**Repeat the cycle 10×**. If even one jump faults, the fix didn't
hold in your operating environment — file against #62 / #74 with the
fault log (read via SWD if you can, or `cf diagnose log-stream` if
the BL came back up).

**Pass**: 10/10 clean jumps.

---

### B.5 — SP-range divergence fix (PR #72)

**Why**: pre-v1.2.0 an app with MSP in `0x24050000..0x240FFFFF`
silently failed at jump time (CheckApplication approved via the loose
bitmask, JumpToApplication rejected via the strict range, ERR LED
solid). v1.2.0 unifies both checks against the strict range so
CheckApplication rejects with code `0x15`.

**Setup**: build a tiny test app that places its stack at
**`0x24080000`** (firmly in the legacy 1 MB window, firmly outside
the real 320 KB RAM_D1). The simplest way: take any working app's
linker script, change `_estack = 0x24050000` to `_estack = 0x24080000`,
rebuild, generate the metadata with `cf` (or by hand with magic +
size + CRC32).

**Command**:
```sh
cfb flash bad-sp-app.elf --verify-after --jump
```

**Expected**:
- `flash` and `verify` succeed (the image is structurally valid; only
  the SP is bogus).
- `--jump` triggers, and the BL responds with the `JUMP` ACK followed
  by `NACK(BL_NACK_NO_VALID_APP=0x0C)` because `CheckApplication`
  now correctly rejects. (Pre-v1.2.0: ACK then silent fail, board sits
  on ERR LED, no NACK.)

**Pass**: NACK code `0x0C` observed *and* the board returns to the
listen state (responds to a follow-up `cfb diagnose health`).

---

### B.6 — Persistent `flash_write_count` (PR #83)

**Why**: pre-v1.2.0 the field in the health record was hard-coded
zero. Now it counts every `bl_flash_{write,erase}` success and
persists across reboots via NVM.

**Command sequence**:
```sh
# Reset counter baseline by reading current value
cfb diagnose health | tee /tmp/health-before.json
N0=$(jq -r .flash_write_count /tmp/health-before.json)
echo "starting count: $N0"

# Force a few flash ops
cfb flash test-app.elf --verify-after

# Re-read
cfb diagnose health | tee /tmp/health-mid.json
N1=$(jq -r .flash_write_count /tmp/health-mid.json)
echo "after flash: $N1"

# Reset the board (cycle power or NRST)
# Re-read after reboot
cfb diagnose health | tee /tmp/health-after-reboot.json
N2=$(jq -r .flash_write_count /tmp/health-after-reboot.json)
echo "after reboot: $N2"
```

**Expected**:
- `N1 > N0` (counter rose after flashing — at least one erase + ≥1
  program op for a non-trivial app).
- `N2 == N1` (counter persisted; reboot didn't reset it to 0).

**Pass**: both inequalities hold.

---

### B.7 — TX-FIFO drain before terminal opcodes (PR #85)

**Why**: pre-v1.2.0 `handle_reset` / `handle_jump` / `handle_ob_apply_wrp`
used `HAL_Delay(10)` to "let the ACK drain". Under bus load the ACK
could still be in the FIFO when NVIC_SystemReset fired. PR #85
replaced the delay with a TX-FIFO-empty poll bounded at 50 ms.

**Command sequence** (sibling spam running in another terminal):
```sh
# (Terminal 1) Sibling-node spam as in B.4, but at higher rate to
# stress the TX path:
while true; do
  cf --interface slcan --channel … --bitrate 500000 \
     send-raw 0x015 11 22 33 44 55 66 77 88
done

# (Terminal 2) Trigger a hard reset and confirm the ACK arrived
cfb send-raw 0x01 01 00 60 00  # CMD_RESET mode=0 (hard)
# Hash the next ~10 frames from the bus:
candump -L slcan0 -n 10
```

**Expected**:
- The ACK frame for `CMD_RESET` (msg_type=ACK, opcode=0x60) appears
  on the bus **before** the board reboots.
- After reboot the board comes back and responds to `cfb diagnose
  health` cleanly.

**Repeat 10×** like B.4. Failure mode pre-v1.2.0: under sufficient
load the host occasionally sees the reboot before the ACK and
reports a hung command.

**Pass**: 10/10 ACKs observed before reboot.

**Not covered by this test**: the OB_APPLY_WRP equivalent path. That
test would require either (a) clearing WRP first via SWD, applying
again, and verifying ACK→reset (high friction, brick risk if anything
slips), or (b) trusting the unit test path. The bench-side
verification here covers the same code path (the `wait_tx_drain`
helper); accept the OB path as **covered-by-mechanism**.

---

### B.8 — Backward compatibility with v0.1 host tooling

**Why**: PR #89 bumps `BL_PROTO_VERSION_MINOR` from 1 to 2. Per the
project's wire-format-changes section, MINOR bumps must be backward-
compatible: a v0.1-compliant host must still work against a 0.2 BL.

**Command** (using an explicit v0.1-compatible cf release if you can
keep one on hand, otherwise spot-check via raw frames):
```sh
# CONNECT with host advertising MAJOR=0, MINOR=1
cfb send-raw 0x01 00 01 00 01     # msg=CMD, opcode=CONNECT, host major=0 minor=1
```

**Expected**:
- Positive ACK (msg_type=0x01, opcode=0x01, BL's advertised version
  `00 02`). No version-mismatch NACK.
- A subsequent `cfb diagnose health` returns a sensible record.

**Pass**: session opens; no `NACK(BL_NACK_PROTOCOL_VERSION=0x0B)`.

---

## C. Results — fill in during the session

Operator: ______________________ · Board serial: ______________________
HW rev: ______________________ · Date: ______________________

| Test | Pass / Fail / N/A | Notes |
|------|:-----------------:|-------|
| A.1 — host-side bad-linker reject | | |
| A.2 — BL-side range check (write) | | |
| A.3 — BL-side range check (erase) | | |
| A.4 — WRPERR on bypass | | |
| A.5 — oversize image | | |
| B.1 — protocol minor == 2 | | |
| B.2 — bad-PCI NACK | | |
| B.3 — zero-payload CF immediate NACK | | |
| B.4 — jump under load × 10 | | |
| B.5 — SP-range edge case | | |
| B.6 — flash_write_count persistent | | |
| B.7 — TX drain × 10 | | |
| B.8 — v0.1 host compat | | |

**Release gate**:
- Every row above ticked **PASS** (or **N/A** with a one-line note).
- The completed table is pasted into the `dev → main` release-cut PR
  body before merge.
- Any **FAIL** blocks the release; file against the relevant PR and
  re-cut after the regression fix lands.

---

## After the session

1. Commit the filled-in `RELEASE_BENCH.md` to the release branch (or
   paste the results table into the release-cut PR body).
2. Update `BENCH_TESTS.md`'s test-run log row with the same outcomes.
3. Tag the merge commit `v1.2.0` only after every row above is green.

If you find tests that should be added (regression for a new bug, a
behaviour this script missed), edit this file and re-PR. Each future
release cut copies this template, swaps the version + relevant PR
list, and runs it again.
