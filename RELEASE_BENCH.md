# Release bench checklist — v1.7.0

Mechanical bench session that validates a release cut on real silicon. Runnable
in ~45 minutes by an operator with a HIL bench, an ST-Link, and a CAN adapter on
any FDCAN tap. **Set the version once** — this cut is **`v1.7.0`** — then swap
that token plus the §B behavioural block each release; everything else
(prerequisites, §A, the gate, the after-session flow) is the stable skeleton.

> **Companion docs**
> - [BENCH_TESTS.md](BENCH_TESTS.md) — the *standing* rejection- and
>   recovery-layer bench tests. Run those first (§A references them); they don't
>   rotate per release.
> - [PROVISIONING.md](PROVISIONING.md) — operator setup if you've never wired a
>   board.

## Prerequisites

- **Board** provisioned per [PROVISIONING.md §1](PROVISIONING.md#1-fresh-board-provisioning-first-time)
  with the **candidate bootloader** — the **CI-built `CAN_BL.bin`** attached to
  the `vX.Y.Z` GitHub Release, or built from the RC tag (`v1.7.0-rc1`) / the
  release-branch HEAD. **Not** a local `dev` build: flash exactly what you will
  ship.
- **Bus**: 500 kbps, 68.75 % sample point. The BL serves **FDCAN1/2/3** at once
  and replies on origin; tap any one (FDCAN2 is the usual bench bus). Every node
  on the bus must be at 500 k — do **not** bench a 1 Mbps image.
- **Adapter + node ID** alias (`cfb` is a documented shorthand *over* `cf`, not a
  separate binary):
  ```sh
  alias cfb='cf --interface slcan --channel /dev/cu.usbmodem1201 \
                --bitrate 500000 --node-id 0x1'
  ```
  Adjust `--channel` / `--node-id` to your bench.
- **Bus-load generator** — referenced as "start the bus-load generator" by the
  under-load tests. Any second board emitting heartbeats at ≥ 100 Hz works, or:
  ```sh
  while true; do
    cf --interface slcan --channel /dev/cu.usbmodem1201 --bitrate 500000 \
       send-raw 0x015 11 22 33 44 55 66 77 88
    sleep 0.01
  done
  ```
- A built **test application** image (boots + emits a recognisable UART/pin
  marker) for the flash + jump tests, and a **large** app image for the
  power-cut test.

## How to use this document

For each test below:
1. Run the **command** verbatim.
2. Compare against the **expected** result.
3. Tick the row in the §C results table.
4. If anything fails, **stop the script** — don't ship until the regression is
   understood. File against the relevant PR.

---

## A. Standing protection-layer tests (from BENCH_TESTS.md)

Re-run every release cut. They're the floor — both halves of the #125
never-unreachable/unflashable invariant. If these regress, nothing else matters.

| # | Test | Reference | Pass criterion |
|---|---|---|---|
| A.1 | Host-side bad-linker rejection | [Test 1](BENCH_TESTS.md#test-1--bad-linker-app-targets-bl-sector) | `cf flash` exits non-zero before any CAN frame |
| A.2 | BL-side range check on FLASH_WRITE | [Test 2](BENCH_TESTS.md#test-2--chunk-outside-writable-range) | `NACK(BL_NACK_PROTECTED_ADDR=0x01)` |
| A.3 | BL-side range check on FLASH_ERASE | [Test 3](BENCH_TESTS.md#test-3--erase-sector-0) | `NACK(BL_NACK_PROTECTED_ADDR=0x01)` |
| A.4 | WRPERR on bypass write (E-02) | [Test 4](BENCH_TESTS.md#test-4--wrperr-flash-controller-blocks-a-bypassed-write) | `HAL_FLASH_Program == HAL_ERROR`, `FLASH_SR1.WRPERR` set |
| A.5 | Oversize image rejected | [Test 5](BENCH_TESTS.md#test-5--oversize-image-rejected) | `NACK(BL_NACK_OUT_OF_BOUNDS=0x02)` |
| A.6 | Power-cut mid-write recovery | [Test 6](BENCH_TESTS.md#test-6--power-cut--dropped-can-mid-write-ecc-brick-recovery) | reachable + reflashable after the cut, no SWD |
| A.7 | Corrupt-NVM → default node-id | [Test 7](BENCH_TESTS.md#test-7--corrupt-nvm--reachable-at-default-node-id-g-a2) | answers at default id, NVM degraded |
| A.8 | apply-wrp rejects non-sector-0 mask | [Test 8](BENCH_TESTS.md#test-8--apply-wrp-rejects-a-non-sector-0-mask-g-b5) | NACK, no option-byte change |
| A.9 | Multi-bus reply-on-origin | [Test 9](BENCH_TESTS.md#test-9--multi-bus-reply-on-origin-120) | discover + NACK reply on each FDCAN tap |
| A.10 | Bus-off recovery | [Test 10](BENCH_TESTS.md#test-10--bus-off-recovery-ng-9) | rejoins + answers discover after a bus-off |

A.4 (WRP-bypass positive), A.9 and A.10 carry method caveats — see the
IFS08_HIL#81 trailing items noted in BENCH_TESTS.md.

---

## B. v1.7.0-specific behavioural checks

> **Per-release block.** Replace this whole section each cut with one test per PR
> that changed observable behaviour. For v1.7.0 that is one feature: the one-step
> SWD provisioning seed (#183 / #184), plus its host side (isc-fs/MingoCAN#336).
> Everything else is unchanged from v1.6.2, so §A is the regression floor.

**Extra prerequisite**: a `can-flasher` build with `swd-flash --provision`
(MingoCAN#336 or a release that contains it, `--features swd`), and the
candidate's **`CAN_BL.elf`**, not the `.bin`.

### B.1 — Candidate identity (the board runs *this* release)

**Why**: every test below assumes the bootloader is the candidate release.

**Command**:
```sh
cfb discover
cfb diagnose health
```

**Expected**: `discover` shows the board at **Proto 0.2**, **500 kbps**, with the
intended FW version + git hash.

**Pass**: version + git hash match the RC; the bus is 500 k.

---

### B.2 — One-step SWD provisioning (#183, headline)

**Why**: the feature this release exists for — a bare board commissioned by the
debug probe alone, with no CAN round-trip.

**Command**: [BENCH_TESTS.md Test 11](BENCH_TESTS.md#test-11--one-step-swd-seed-provisioning-183):
```sh
cf swd-flash CAN_BL.elf --provision ams
# power-cycle, then (no cf provision):
cf --bitrate 500000 discover
```

**Expected**: the tool reports the seed programmed + verified; the board answers
at **`0x2`**, not the compile-time default. A second power-cycle still shows `0x2`.

**Pass**: seeded id adopted, and it survives a power cycle.

---

### B.3 — Bad seed is ignored, never a brick

**Why**: a corrupt seed must leave the board reachable at the default id.

**Command**: on a chip-erased board, burn the candidate over SWD, then program a
seed FLASHWORD with a **wrong CRC** at `0x080FFFC0` (openocd `flash write_image
<32-byte file> 0x080FFFC0 bin`, no erase). Reset, then `cf --node-id 0x1 discover`.

**Expected**: answers at the **default** id (`0x1`); NVM (`0x080E0000`) stays erased.

**Pass**: default id, reachable, nothing written to NVM.

---

### B.4 — CAN app flash after SWD provisioning (metadata page)

**Why**: the seed sits in the same 1 KiB flash page as the app-metadata word,
which the BL programs in place on the first `FLASH_VERIFY`. If provisioning had
programmed that word (probe-rs page fill), this write would double-program it —
the #166 ECC-brick path. MingoCAN#336 writes the seed as a single flashword to
prevent this; this test proves it on silicon.

**Command**: straight after B.2, with no erase in between:
```sh
cf --bitrate 500000 --node-id 0x2 flash app.bin
# true power-cycle, then:
cf --bitrate 500000 --node-id 0x2 discover
```

**Expected**: flash + verify succeed; after the power cycle the app boots (its
CAN traffic appears) and the BL is still reachable at `0x2` via the
reboot-to-BL trigger. No ECC fault, no recovery breadcrumb.

**Pass**: app boots across the power cycle; node-id still `0x2`.

---

### B.5 — CAN `provision` still overrides the seed (NVM wins)

**Why**: the seed is a one-shot; later renumbering over CAN must keep working.

**Command**: on the B.4 board, `cf --node-id 0x2 provision udv`, then power-cycle
and `cf --node-id 0x3 discover`.

**Expected**: the board answers at `0x3` and stays there across a power cycle
(the stale seed is ignored because NVM already holds a node-id).

**Pass**: `0x3` after renumbering, and it survives a power cycle.

---

## C. Results — fill in during the session

Operator: ______________________ · Board serial: ______________________
HW rev: ______________________ · Date: ______________________ · RC tag: ______________________

| Test | Pass / Fail / N/A | Notes |
|------|:-----------------:|-------|
| A.1 — host-side bad-linker reject | | |
| A.2 — BL-side range check (write) | | |
| A.3 — BL-side range check (erase) | | |
| A.4 — WRPERR on bypass (E-02) | | |
| A.5 — oversize image | | |
| A.6 — power-cut recovery | | |
| A.7 — corrupt-NVM default id | | |
| A.8 — apply-wrp mask reject | | |
| A.9 — multi-bus reply-on-origin | | |
| A.10 — bus-off recovery | | |
| B.1 — candidate identity (proto 0.2 @ 500k) | | |
| B.2 — SWD `--provision` seeds the node-id | | |
| B.3 — bad seed → default id, no brick | | |
| B.4 — CAN app flash after provisioning + power cycle | | |
| B.5 — CAN `provision` overrides the seed | | |

**Release gate** — the #125 invariant: *the BL can never become unreachable or
unflashable over CAN.*
- Every row above ticked **PASS** (or **N/A** with a one-line note).
- The completed table is pasted into the release-cut PR body before merge.
- Any **FAIL** blocks the release; file against the relevant PR and re-cut after
  the fix lands.

For reference, v1.6.2 was HIL-accepted with **zero firmware defects** in
[#178](https://github.com/isc-fs/stm32-can-bootloader/issues/178).

---

## After the session — cutting the release

The release-cut flow (full cherry-pick procedure in
[CONTRIBUTING.md § Merging to main](CONTRIBUTING.md#merging-to-main-release-cut)):

1. Cut a **`release/vX.Y.Z` branch off `main`**, populated by cherry-picking
   `dev`'s commits, and tag a release candidate (`vX.Y.Z-rc1`) for the bench.
2. Run this checklist against the **CI-built** candidate binary. Paste the §C
   table into the release-cut PR body.
3. Open the **release-cut PR → `main`** and merge it; the release tag (`vX.Y.Z`)
   sits on that merge.
4. Create the **GitHub Release** for the tag. `attach-release-artifacts.yml`
   (on `release: published`) builds the Release preset from the tag and attaches
   `CAN_BL.{elf,bin,hex}` (~3 min) — the canonical binaries to ship and flash.
5. **Leave `dev` and `main` diverged** — the cut is built from cherry-picks, so
   the two share no SHAs but their trees stay identical. That's by design; don't
   reconcile it, and there's no automated sync (the old `sync-dev-after-release`
   workflow was removed). See CONTRIBUTING.md § *Merging to main*.
6. Update [BENCH_TESTS.md](BENCH_TESTS.md)'s test-run log with the same outcomes.

If you find tests that should be added (a regression for a new bug, a behaviour
this script missed), edit this file and re-PR. Each future cut copies this
template, swaps the version token + the §B block, and runs it again.
