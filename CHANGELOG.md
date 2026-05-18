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
