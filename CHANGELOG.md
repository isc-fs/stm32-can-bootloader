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
