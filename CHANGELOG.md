# Changelog

Notable changes to the STM32 CAN bootloader, grouped by release. Format
loosely follows [Keep a Changelog](https://keepachangelog.com); semantic
version meaning is firmware-level (matching the git tag) rather than
public-API as for a library — the wire-protocol version moves on its
own `BL_PROTO_VERSION_MAJOR.MINOR` cadence and is called out per release.

Releases before `v1.2.0` (`v1.0.0`, `v1.0.x`, `v1.1.0`) predate this
file; their content lives in the matching git tag annotations and in
the PR titles between consecutive tags.

---

## v1.6.1 — 500 kbps revert + ECC-brick recovery

**Wire protocol**: unchanged at `0.2`. **CAN bitrate reverts 1 Mbps → 500 kbps
on all FDCAN buses**, coordinated with the app revert (IFS08-CE-AMS#351) — BL
and app share the bus and must match. This **withdraws the v1.6.0 1 Mbps
cutover** (IFS08-CE-AMS#341). If you deployed v1.6.0, move to v1.6.1; do not run
1 Mbps.

Two fixes for the brick crisis the HIL bench found in v1.6.0 — one removes the
trigger, one survives it — restoring the #125 invariant that the bootloader can
never become unflashable over CAN.

### Changed

- **Reverted all FDCAN buses to 500 kbps** (#171), at a **68.75% sample point**
  (`NominalPrescaler 3, Seg1 10, Seg2 5` → 16 Tq) — matched bit-for-bit to the
  AMS app's revert (IFS08-CE-AMS#351) so every node on the bus samples at the
  same point — up from the previous ~50%.
  1 Mbps ran too close to the bus's signal-integrity margin: a flash is
  thousands of frames, and a bit error landing mid-write (~60% of a sector on
  one bench reflash, a `TRANSPORT_ERROR`) left a half-written app that then
  tripped #166 → a permanent brick (two STM32H733 lost this way). The doubled
  bit time + proper sample point restore the margin, and the mid-write failures
  disappear. The multi-bus design (#120) is unchanged — only the rate. (Also
  resolves the sample-point concern from #163; its PLL-source / HSE-fallback
  items remain open.)

### Fixed

- **An interrupted flash write no longer bricks the unit past CAN recovery**
  (#166). A write cut short — by power loss **or** a CAN-transport failure —
  leaves a partially-programmed flash word whose double-bit ECC raises a bus
  fault the instant it is read. The app-validation CRC read hit that word on
  **every boot, before CAN came up**, looping the bootloader unreachable —
  recoverable only by an SWD chip-erase. The validation read is now guarded: a
  fault while validating leaves a reset-surviving breadcrumb, and the next boot
  **skips the corrupt read** and comes up reachable + reflashable (one brief
  fault+reboot per cold boot of a corrupt unit, then steady). Latched flash
  ECC/error state is also cleared at boot so the recovery reflash can't be
  rejected by a stale lock. The recovery lives in `bl_fault_reboot` — no CubeMX
  fault handler is touched. Validated on the bench by an F-077 re-run at
  500 kbps.

---

## v1.6.0 — Multi-bus FDCAN, 1 Mbps fleet cutover

**Wire protocol**: message format unchanged at `0.2`. **CAN bitrate moves
500 kbps → 1 Mbps.** This is a *coordinated fleet cutover*, not a drop-in
upgrade: a 1 Mbps bootloader is physically deaf to a 500 kbps host or peer.
Flash this BL, the AMS application (IFS08-CE-AMS#337), and the host/flasher
tools to 1 Mbps **together, per car**. Do **not** flash this BL onto a
vehicle still running a 500 kbps fleet — the ECU goes unreachable over CAN
until every node on its bus matches, which is exactly the
open-the-enclosure failure this project exists to avoid.

The bootloader is the one component whose failure means opening a sealed
ECU enclosure in the car. This release makes one BL image serve an entire
shared bus and folds in every HIL-found boot-path defect, validated
end-to-end on the AMS bench.

### Highlights

- **One bootloader image on all three FDCAN buses** (#120). The three ECUs
  share a single CAN bus but each taps it through a *different* FDCAN
  peripheral (FDCAN1/2/3) — a board-level wiring mismatch we can't reflow.
  Rather than maintain three per-peripheral builds, the BL now listens on
  **all three FDCAN instances at once** and replies on whichever bus a
  request arrived on, so one identical binary reaches every ECU. Each unit
  gets its distinct identity from the **node-id burned at flash time** (NVM
  key `0x0001`); leave it unset and the compile-time default applies. All
  three run at **1 Mbps**. Rationale captured in `ARCHITECTURE.md`.

- **NOTIFY_LOG can't overflow the TX path** (PR #147). Log emission is
  capped to the TX-FIFO depth and gated when the FIFO is busy, so a burst
  of log traffic can't stall or desync the ISO-TP path mid-flash.
  (HIL-found.)

- **Reset-to-app after a write, so a warm jump can't strand the app**
  (PR #142). Once a flash write has happened this boot, a JUMP / RESET-to-app
  routes through a full reset (via a boot-request magic) instead of a warm
  branch — a warm jump after writing could leave the app running on stale
  bootloader state. (HIL-found.)

- **Filter-aliased frames can't cancel the auto-jump** (PR #154). The 5-bit
  acceptance filter aliased the charger's `0x101` onto the node-1 unicast
  id, and any received frame used to cancel the boot-timer auto-jump
  *before* the parser rejected it — so a BL would park in listen mode
  forever whenever the charger was energised. Auto-jump is now cancelled
  only after a frame parses as genuinely addressed to us, and the hardware
  filter does an exact match. (HIL-found — blocked this tag.)

- **Crystal BOM guard** (PR #144). A compile-time
  `_Static_assert(HSE_VALUE == 24 MHz)` — the FDCAN bit-timing is derived
  from the 24 MHz crystal, so a mis-stuffed board fails the build instead of
  coming up silently unreachable at the wrong baud.

- **CheckApplication off the hot path** (PR #146). The full-image CRC is
  cached and invalidated on every flash mutation, so the per-tick boot
  decision no longer re-CRCs the whole application — removing an RX-stall
  risk on large images.

- **Stay-in-bootloader survives a power cycle** (PR #145). An operator
  "hold in the BL" request is now persisted in NVM; it used to live only in
  an RTC backup register, which a power-off wipes. The hold is cleared on an
  explicit boot or a successful image verify. A held unit stays held across
  a true power cycle instead of silently auto-jumping a possibly-bad app on
  the next power-up.

### Bench validation

HIL-validated on the AMS bench @ 1 Mbps across the development cycle:
reachability + reply-on-origin on **all three FDCAN buses** under concurrent
traffic (#143); the #147 / #142 / #154 fixes each reproduced-then-confirmed
on silicon; and the #145 power-cycle gate — `RESET` mode 2 held the BL
across **3 / 3 power cycles** (the NVM flag survives a POR), a baseline
cold boot still auto-jumps a valid app (no fleet power-up regression), and
both clear paths (explicit boot *and* `FLASH_VERIFY`) survive a subsequent
power cycle.

### Cutover checklist (per car)

1. Stage the 1 Mbps host / flasher tools.
2. Flash this BL to every ECU (`0x08000000`, STM32CubeProgrammer over SWD —
   **not** MingoCAN, which corrupts the bootloader), burning a distinct
   node-id per unit.
3. Flash the matching 1 Mbps AMS application (IFS08-CE-AMS#337).
4. Confirm every node on the bus is at 1 Mbps before closing up — a mixed
   500 k / 1 M bus is non-functional.

### Known follow-ups (not blocking)

- FDCAN clock-tree hardening: crystal-independent PLL source, HSE-fail
  fallback, and a 75 % sample point (currently ~50 %) — #163.
- `FLASH_VERIFY` length cross-check (H7) — needs an app-side `fwinfo`
  length field — #146.

---

## v1.5.0 — Fault-operational hardening

**Wire protocol**: unchanged at `0.2`. No host upgrade required.

The bootloader is the one component whose failure means opening a sealed
ECU enclosure in the car. This release is the output of a full reliability
audit (#125) around a single invariant: **the BL must never become
unreachable or unflashable over CAN.** Every change is a recovery or
self-protection mechanism, validated end-to-end on the HIL bench (#138).

### Highlights

- **Independent watchdog** (PR #137). The IWDG is now enabled (~8 s period)
  as the hardware backstop against any hang software can't catch — an
  interrupt-masked spin, a wedged loop. Sized to clear the worst
  uninterruptible CPU stall, a single 128 KB sector erase (measured
  1768 ms; bench-confirmed 1822 ms). `bl_flash_erase` was refactored to
  erase **one sector per HAL call, kicking the watchdog between sectors**,
  so a full multi-sector app erase (~10.6 s in one call) can't trip the
  period mid-erase and brick the unit. The AMS application cooperates by
  refreshing the inherited watchdog (IFS08-CE-AMS#280); after an IWDG or
  fault reset the BL deliberately stays in the bootloader rather than
  auto-jumping into a possibly-faulty app. Build knob `BL_IWDG_ENABLE`
  (default on).

- **Reboot on terminal CPU faults** (PR #135). The fault handlers
  (HardFault / MemManage / BusFault / UsageFault / NMI), `Error_Handler`,
  and a failed FDCAN init no longer spin forever — they leave a breadcrumb
  in a `.noinit` RAM word and reboot (`bl_fault`), so a transient fault
  recovers to a flashable BL instead of a dead board. The recovered reason
  is logged as DTC `0x0050` (CPU_FAULT). Bench-confirmed: forced HardFault
  and Error_Handler both reboot and log the DTC.

- **FDCAN RX/TX robustness** (PR #133). RX_FIFO0 is drained in a bounded
  batch (up to the 16-deep FIFO) per main-loop pass, so a post-stall
  backlog can't overflow and desync ISO-TP reassembly; a full TX FIFO is
  surfaced (edge-triggered log) instead of silently dropped; a boot-time
  guard shouts if AutoRetransmission ever comes up disabled (the #94
  regression signature).

- **FDCAN bus-off auto-recovery + erase-duration probe** (PR #130). On
  Bus_Off the BL performs a Stop/Start recovery (DTC `0x0040`, per-boot
  counter) instead of going permanently deaf; the health record's former
  reserved fields now carry `fdcan_recovery_count` and `max_flash_op_ms`
  (same 32-byte layout — no host-side size change).

- **Docs**: H72x/H73x WRP-clear procedure corrected (PR #131).

### Bench validation (#138)

On the AMS HIL bench: full flash cycle, no spurious IWDG, single-sector
erase timing, deliberate spin → IWDG reset (`reset_cause = 0x04`), both
fault paths → DTC `0x0050`, a 55 s six-sector flash session surviving the
watchdog, and the app↔BL `002` handoff with no boot-loop.

---

## v1.4.0 — Field-brick prevention

**Wire protocol**: unchanged at `0.2`.

Two field-brick paths surfaced on the HIL bench, both closed (#125):

- **Sustained-flash ISO-TP timeout** (PR #124). A 1 Hz multi-frame
  `NOTIFY_HEARTBEAT` emitted mid-`WRITE_CHUNK` stranded reassembly on long
  transfers; heartbeats (and all NOTIFYs) are now suppressed while a host
  exchange is in flight.
- **Two field bricks** (PR #127). A session timeout mid-flash no longer
  auto-jumps into a half-written app (C2 flash-dirty latch), and
  `OB_APPLY_WRP` can no longer self-lock a non-bootloader sector (C4 WRP
  validation).

---

## v1.3.1 — CI / docs / tests catch-up

**Wire protocol**: unchanged at `0.2`. No firmware behaviour change —
required-CI promotion, a documentation sweep, a tighter firmware-size gate,
and `bl_proto` dispatch tests with a ratcheted coverage floor.

---

## v1.3.0 — NVM-backed node-id override + CI hardening

**Wire protocol**: unchanged at `0.2`. No host upgrade required.

Single user-facing feature: the bootloader's node ID can now be
provisioned over the wire via the host's NVM-write opcode, eliminating
the per-board rebuild-and-SWD-reflash step. Everything else this
release ships is internal CI hardening; no firmware behaviour change
beyond the override path.

### Highlights

- **Runtime node-id override** (PR #112). The bootloader consumes
  `BL_NVM_KEY_NODE_ID = 0x0001` at boot and prefers a valid 1-byte
  override over the compile-time `BL_NODE_ID`. Surfaced on the
  IFS08 HIL bench (isc-fs/IFS08-CE-AMS#123) — operator couldn't
  reprovision MLC1 without a full BL rebuild. New module
  `Core/{Inc,Src}/bl_node_id.{h,c}` owns the resolution + caching;
  the FDCAN RX filter, every TX call site (`bl_proto`, `bl_health`),
  and the dispatcher addressed-to-us check all go through
  `bl_node_id_get()` now. Validation rules (length must be 1 byte;
  value must be in `0x1..0xE`) make a corrupt NVM entry fall back
  silently to the compile-time default — the override path can
  never brick a node.
- **CI suite grew to 7 required gates.** Four new jobs landed
  between v1.2.0 and this release (sanitizers, coverage, clang-tidy
  diff, firmware-size delta — see below). All seven jobs are now
  required checks on `dev` and `main` per branch protection.
- **Releases auto-attach firmware artifacts.** This is the first
  release where the `attach-release-artifacts` workflow fires on
  `release: published` and uploads `CAN_BL.{elf,bin,hex}` to the
  release page within ≈3 minutes (PR #100).

### Wire-surface changes (protocol v0.2)

**None.** `BL_PROTO_VERSION_MAJOR.MINOR` stays at `0.2`.
`CMD_NVM_WRITE` / `CMD_NVM_READ` for key `0x0001` were already
defined in v1.2.0; only the bootloader's internal boot-time consumer
of the stored value is new in v1.3.0. A v0.2 host doesn't see any
new opcodes, NACK codes, or frame shapes.

### Reliability + correctness

- **Silent fall-back on invalid NVM byte.** The override accepts
  only well-formed values; anything else (length ≠ 1, byte `0x0`,
  byte `0xF`, byte `0x10..0xFF`) is ignored and the BL boots at the
  compile-time default. 11 unit tests in `test_bl_node_id.c` cover
  every rejection path plus idempotent re-init and the
  tombstone-restores-default flow.

### Infrastructure / CI

- **`Host unit tests (ASan + UBSan)`** (PR #104) — second host-test
  build wired with `-fsanitize=address,undefined` +
  `-fno-sanitize-recover=all` so UAF / OOB / signed-overflow /
  shift-out-of-range / null-deref are caught on every PR.
- **`Host coverage`** (PR #105, retuned for landing) — runs the
  test suite under `--coverage`, reports per-file `bl_*.c` numbers
  via `gcovr 7.2`, fails if total line coverage drops below 40 %.
  Floor will ratchet up as bl_proto / bl_log gain tests.
- **`clang-tidy (changed lines)`** (PR #108) — `bugprone-* +
  readability-* + cert-* + clang-analyzer-* + misc-*` (with
  carve-outs documented inline in `.clang-tidy`) applied only to
  lines a PR touches via `clang-tidy-diff.py`. Diff-scope filter
  refined in v1.3.0 to drive off `compile_commands.json` so the
  gate doesn't false-fail on firmware-only TUs (#112).
- **`Firmware size delta`** (PR #110) — builds the Release preset
  on both the PR head and the base branch, posts a delta table to
  the PR's step summary, fails if `text +2048 B` / `data +256 B`
  / `bss +256 B` is exceeded. Loose thresholds for first landing.
- **`attach-release-artifacts.yml`** (PR #100) — release workflow
  described in *Highlights* above.
- **`close-on-dev-merge.yml`** (PR #102) — Pass 2 added that
  closes the branch's `[<head-ref>]` tracking issue when a dev PR
  merges, alongside the existing `Closes #N` keyword scan.

### Backward compatibility

A v0.2 host tool ([can-flasher](https://github.com/isc-fs/can-flasher))
keeps working unchanged:

- **No opcodes added, renumbered, or removed.**
- `CONNECT` semantics unchanged.
- `CMD_NVM_WRITE` to key `0x0001` (which the host could already
  send) now persists across reboot **and** is consumed by the BL
  on the next boot — a v0.2 host that wasn't aware of this path
  experiences no change in behaviour.
- A board provisioned at v1.2.0 with a stale or bad NVM entry
  under key `0x0001` will simply fall back to the compile-time
  `BL_NODE_ID` on v1.3.0 first-boot — no manual cleanup needed.

### Companion-tool sync

The host CLI [`isc-fs/can-flasher`](https://github.com/isc-fs/can-flasher)
already supports `config nvm write 0x0001 <hex_byte>` end-to-end;
the workflow works today with one operator-friction caveat: there's
no post-write auto-reset, so the operator power-cycles the board
manually after the NVM write. Tracked at
[`isc-fs/can-flasher#231`](https://github.com/isc-fs/can-flasher/issues/231)
along with a proposed `node-id` named alias and a fix for stale
`--key`/`--value` examples in `docs/USAGE.md`.

### Memory budget

- Bootloader image: 24 348 B `.text` / 128 KB sector 0 (≈ **19 %
  used**) — net `+140 B` of `.text` vs v1.2.0 for the new
  `bl_node_id` TU and its callers.
- `.data`: 108 B (+4 B for the cached node-id byte + alignment).
- `.bss`: 4 228 B (+4 B alignment shadow).
- Well inside the new firmware-size delta gate's thresholds.

### Operator notes (bench)

If you're updating an existing board to v1.3.0:

1. Flash the v1.3.0 `CAN_BL.bin` from the release page **with the
   matching `-DBL_NODE_ID=<addr>`** for that carrier (the binary
   in the release is built at `BL_NODE_ID = 0x1`; rebuild from the
   `v1.3.0` tag if your board needs a different boot-default).
2. After first boot, `can-flasher --node-id <current> config nvm
   write 0x0001 <new_addr>` persists the override.
3. Round-trip verify with `can-flasher --node-id <current> config
   nvm read 0x0001` before rebooting.
4. Power-cycle the board (no over-CAN reset yet — see
   `can-flasher#231`).
5. `can-flasher discover` should now see the new ID.

See `PROVISIONING.md §3` for the same workflow with explanatory
context.

---

## v1.2.0 — Reliability + observability hardening

**Wire protocol**: `0.1` → `0.2` (backward-compatible; v0.1 hosts keep
working — see *Backward compatibility* below).

20 PRs landed between `v1.1.0` and this release. The headline is a
hardening pass against latent bugs the host-side unit-test scaffold
surfaced once it was wired up — every audit finding now has either a
regression test, a fix, or both.

### Highlights

- **Audit-driven hardening pass.** Eight standalone bugs filed and
  closed: ISO-TP deadline encapsulation, IRQ window before app jump,
  Cortex-M7 jump barriers, SP-range divergence between
  `CheckApplication` and `JumpToApplication`, BKPSRAM log-ring
  scratch overflow, malformed-PCI silent drop, zero-payload CF stall,
  TX-FIFO drain before terminal opcodes. See PR list per section
  below.
- **Host test suite grew 2.6×** — 25 → **65 tests** across 7
  modules, including a dispatcher mock scaffold (FDCAN TX capture,
  BKPSRAM redirection) that lets bench-side regressions get caught
  before they reach the bench.
- **Documentation refreshed end-to-end** — three new Mermaid
  diagrams in `ARCHITECTURE.md` (module map, boot flow, session
  state machine), Mermaid `gitGraph` replacing ASCII branch art in
  `CONTRIBUTING.md`, and a new `RELEASE_BENCH.md` that turns
  release-cut bench validation into a mechanical 13-row checklist.

### Wire-surface changes (protocol v0.2)

Behavioural deltas a host observes against a v0.1 baseline. None
break a v0.1-compliant host — every change is additive observable
behaviour.

| PR | Change |
|---|---|
| [#63](https://github.com/isc-fs/stm32-can-bootloader/pull/63) | Malformed-PCI frames now reply with `NACK(BL_NACK_TRANSPORT_ERROR=0x0A)` (was silent drop). |
| [#78](https://github.com/isc-fs/stm32-can-bootloader/pull/78) | Zero-payload CF (PCI byte only) replies with `NACK(BL_NACK_TRANSPORT_ERROR)` immediately (was 1 s reassembly-timeout stall). |
| [#83](https://github.com/isc-fs/stm32-can-bootloader/pull/83) | `bl_health.flash_write_count` is a real persistent counter (was hard-coded zero). Persists across reboots via NVM key `0x0003`. |
| [#85](https://github.com/isc-fs/stm32-can-bootloader/pull/85) | `OB_APPLY_WRP` no longer emits a contradictory `NACK(FLASH_HW)` after its positive `ACK` on the rare "OB_Launch unexpectedly returned" path. |
| [#89](https://github.com/isc-fs/stm32-can-bootloader/pull/89) | `BL_PROTO_VERSION_MINOR` bumped 1 → 2. |

### Reliability + correctness

| PR | Closes | What changed |
|---|---|---|
| [#56](https://github.com/isc-fs/stm32-can-bootloader/pull/56) | #54 | ISO-TP reassembly deadline armed inside `bl_isotp_rx_feed` (was: invariant depended on caller stamping it externally). |
| [#62](https://github.com/isc-fs/stm32-can-bootloader/pull/62) | #59 | IRQs stay masked across `Bootloader_JumpToApplication` (was: re-enabled before the jump, letting a pending IRQ dispatch through the new VTOR before the app's `Reset_Handler` ran). |
| [#70](https://github.com/isc-fs/stm32-can-bootloader/pull/70) | #65 | `bl_log_drain` and `evict_oldest` bound-check the `ent_len` byte from BKPSRAM before any `memcpy` / pointer advance (was: a corrupt `ent_len=0xFF` could overflow the 126-byte stack scratch). Same guard catches the `unread_bytes` underflow that #68 also tracked. |
| [#72](https://github.com/isc-fs/stm32-can-bootloader/pull/72) | #66 | `Bootloader_CheckApplication` and `Bootloader_JumpToApplication` now share the same `bl_app_stack_in_legal_range()` predicate (was: bitmask vs range-check divergence let `CheckApplication` approve apps the jump would silently reject). |
| [#74](https://github.com/isc-fs/stm32-can-bootloader/pull/74) | #67 | `__DSB()` after `SCB->VTOR` write and `__ISB()` after `__set_MSP` in the jump path — Cortex-M7 memory and pipeline barriers per ARM AN-298. |
| [#78](https://github.com/isc-fs/stm32-can-bootloader/pull/78) | #68 | `handle_cf` rejects zero-payload CFs as malformed instead of stalling reassembly (see *Wire-surface* above). |
| [#79](https://github.com/isc-fs/stm32-can-bootloader/pull/79) | #68 | OB WRP state cached at boot in `bl_live` (was re-read via `HAL_FLASHEx_OBGetConfig` on every 50 Hz snapshot); FDCAN RX-FIFO0 notification activation removed (was enabled but the callback was the default no-op, so every RX frame entered an empty ISR for nothing). |
| [#81](https://github.com/isc-fs/stm32-can-bootloader/pull/81) | #68 | `bl_nvm` `compact_and_append` self-validates `new_len ≤ BL_NVM_MAX_VALUE_LEN` (was: trusted the caller). |
| [#85](https://github.com/isc-fs/stm32-can-bootloader/pull/85) | #68 | New `wait_tx_drain()` polls FDCAN TX FIFO empty with a 50 ms timeout before `NVIC_SystemReset` / `Bootloader_JumpToApplication` / `HAL_FLASH_OB_Launch` (was `HAL_Delay(10)` — hope, not guarantee). Plus the OB protocol-invariant fix (see *Wire-surface*). |
| [#53](https://github.com/isc-fs/stm32-can-bootloader/pull/53) | #52 | `bl_nvm_write` recovers from `HAL_ERROR` on append by falling through to compaction; new `BL_CMD_NVM_FORMAT` (token-gated) wipes sector 7. |

### Test infrastructure

| PR | Effect |
|---|---|
| [#55](https://github.com/isc-fs/stm32-can-bootloader/pull/55) | Host-side unit test scaffold: Unity via FetchContent, `tests/unit/` build with mocked HAL (`g_fake_flash`, 1→0 programming rule enforced, tick control), `.github/workflows/build-and-test.yml` runs the suite + cross-compiles firmware Debug + Release on every push. |
| [#57](https://github.com/isc-fs/stm32-can-bootloader/pull/57) | 14 tests over `bl_proto_parse_id` / `_build_id` (every legal direction × node, round-trip, reserved-bit rejection). |
| [#76](https://github.com/isc-fs/stm32-can-bootloader/pull/76) | Dispatcher scaffold: real `bl_proto.c` linked into host build, FDCAN TX capture mock (`mock_fdcan_get` / `_tx_count`), `bl_peer_stubs.c` for the modules dispatch transitively touches. 5 regression tests over the dispatcher gates including the #60 bad-PCI NACK. |

### Documentation

| PR | Effect |
|---|---|
| [#51](https://github.com/isc-fs/stm32-can-bootloader/pull/51) | `ARCHITECTURE.md` trimmed to BL-internal rationale; wire-spec content moved to a single source of truth in the host-tool repo (`can-flasher/REQUIREMENTS.md`). |
| [#87](https://github.com/isc-fs/stm32-can-bootloader/pull/87) | Top-level docs refresh + Mermaid: module-map flowchart, boot-flow flowchart, session-lifecycle state diagram in `ARCHITECTURE.md`; gitGraph in `CONTRIBUTING.md`. Stale ASCII art retired. Per-PR correctness fixes for the jump path, OB launch, NVM keys. |
| [#91](https://github.com/isc-fs/stm32-can-bootloader/pull/91) | New `RELEASE_BENCH.md` — mechanical 13-row bench checklist for the release cut. |

### Infrastructure / CI

| PR | Effect |
|---|---|
| [#50](https://github.com/isc-fs/stm32-can-bootloader/pull/50) | `.github/workflows/sync-dev-after-release.yml` fast-forwards `dev` to match `main` automatically on `release: published`. |

Also during this cycle (no PR, separate operational work):

- Branch protection rules formalised on `dev` and `main` — required
  status checks (host tests + both firmware builds), no force pushes,
  no deletions, conversation resolution required; `main` adds
  linear-history + strict-status.
- Tag deletion-and-force-push protection added via the GitHub
  Rulesets API (admin bypass retained).
- Repository made public (unblocked GitHub Actions on the free tier
  for the org).

### Backward compatibility

A v0.1 host tool still works against a v0.2 BL:

- `CONNECT` from a host advertising MAJOR=0 / MINOR=1 still succeeds.
  Major version mismatch (host MAJOR ≥ 1) is still rejected with
  `NACK(BL_NACK_PROTOCOL_VERSION)`.
- New `NACK(TRANSPORT_ERROR)` responses fire only on inputs that
  pre-v1.2.0 produced silent drops or timeout-NACKs — i.e. malformed
  frames a well-behaved host doesn't send.
- `health.flash_write_count` moving from "always 0" to a real counter
  is a strict information gain.
- No opcodes added, renumbered, or removed.

Host tools that want to assert on the new behaviours (e.g.
regression-test the bad-PCI NACK) should pin their minimum BL
protocol version to `0.2`.

### Companion-tool sync

The host CLI [`isc-fs/can-flasher`](https://github.com/isc-fs/can-flasher)
should:

1. Update its expected BL minor to `2`.
2. Add regression tests against the new NACK paths
   (see `RELEASE_BENCH.md` §B.2 and §B.3 for the exact frame
   sequences).
3. Stop hard-coding `flash_write_count == 0` in any assertions.

A tracking issue is filed against `isc-fs/can-flasher` referencing
this changelog section.

### Memory budget

- Bootloader image: 42 572 B / 128 KB sector 0 (≈ **33 % used**)
- `.bss`: 4 224 B / 320 KB RAM_D1 (≈ 1.3 % used)
- Net text drift vs `v1.1.0`: roughly **+200 bytes** for the audit
  fixes (mostly explanatory comments + four small helpers:
  `wait_tx_drain`, `entry_consistent` / `reset_ring_corrupt`,
  `bl_app_stack_in_legal_range`, `bl_health_record_flash_write`).
