# Host-side unit tests

[Unity](https://github.com/ThrowTheSwitch/Unity)-based unit suite for the
bootloader's pure-logic modules and the dispatcher. Runs on a developer
machine or in CI in <1 s — no toolchain, no hardware, no flash. Catches
regressions in parsing, framing, NVM management, dispatch gates, and other
invariants without waiting for a bench session.

## Run

```sh
cmake -B build-tests -S tests/unit
cmake --build build-tests
ctest --test-dir build-tests --output-on-failure
```

Or in one shot:

```sh
cmake --build build-tests --target test
```

## Layout

```
tests/unit/
├── README.md                ← you are here
├── CMakeLists.txt           host build: Unity via FetchContent + ctest
├── unity_runner.c           UNITY_BEGIN / RUN_TEST(...) entry point
├── mocks/
│   ├── stm32h7xx_hal.h      minimal HAL types so production .c files compile
│   ├── bl_memmap.h          redirects BL_*_BASE / BL_*_ADDR into g_fake_flash[]
│   ├── bl_stubs.h           settable knobs paired with bl_stubs.c
│   ├── hal_stubs.c          simulated flash + tick + FDCAN TX capture
│   ├── bl_stubs.c           BKPSRAM ring storage + bl_health_uptime_seconds mock
│   └── bl_peer_stubs.c      stubs for bl_dtc / bl_flash / bl_health / bl_iwdg / bl_live / bl_obyte (several observable — counters/knobs, not all no-op)
├── test_bl_isotp.c          SF/FF/CF framing, reassembly, timeout, wrap-around
├── test_bl_fwinfo.c         magic + record-version acceptance
├── test_bl_node_id.c        NVM node-id override validation + safe fallback (v1.3.0)
├── test_bl_fdcan.c          multi-bus map + filters/start-all + reply-on-origin (#120)
├── test_bl_nvm.c            dedup, compaction, format, degraded-mode recovery (G-A2)
├── test_bl_log.c            BKPSRAM ring corruption-guard + drain semantics
├── test_bl_proto_id.c       wire-format build/parse over every legal ID
├── test_bl_proto_dispatch.c dispatcher entry-gate coverage + bad-PCI NACK regression
└── test_bl_app_validate.c   BL→APP stack-pointer range predicate boundaries
```

## What's covered today

**Suite size: 119 PASS / 0 IGNORE** in well under 1 s. (The authoritative count
lives in `unity_runner.c`'s `RUN_TEST` list — quote that, not this number, if
they ever drift.)

| Module | Test file | Tests | Notable coverage |
|---|---|---:|---|
| `bl_isotp` | `test_bl_isotp.c` | 12 | SF/FF/CF chains, bad-seq, no-FF, overflow, timeout, **tick wrap-around** (#54), **zero-payload CF reject** (#68) |
| `bl_fwinfo` | `test_bl_fwinfo.c` | 5 | magic match, `record_version` ≥ 1, future-minor accepted |
| `bl_node_id` | `test_bl_node_id.c` | 11 | override accepted for `0x1..0xE`; safe fall-back to the compile-time default on empty / `0x00` / `0x0F` / wrong-length, so a bad `cf config nvm write` can't brick a node |
| `bl_fdcan` | `test_bl_fdcan.c` | 12 | `bus(i)` → hfdcan1/2/3 (+clamp), `configure_filters` / `start_all` cover all three + propagate HAL failure, `set_active` = reply-on-origin + NULL refusal (#120) |
| `bl_nvm` | `test_bl_nvm.c` | 13 | dedup, tombstones, compaction, format, **HAL_ERROR retry via compaction** (#53), `compact_replace_meta` (#13), **degraded-mode reads-not-found / writes-rejected until format** (G-A2, #166) |
| `bl_log` | `test_bl_log.c` | 8 | drain happy path, severity filter, **corrupt-ent_len clamp** + **undersized-unread guard** (#65) |
| `bl_proto_id` | `test_bl_proto_id.c` | 14 | every legal direction × node pair + round-trip + reserved-bit rejection |
| `bl_proto_dispatch` | `test_bl_proto_dispatch.c` | 31 | direction / dst / length / **bad-PCI NACK** (#60) gates, valid-SF passthrough, plus apply-wrp mask validation (G-B5), watchdog-kick, and jump / cache-invalidate plumbing |
| `bl_app_validate` | `test_bl_app_validate.c` | 13 | DTCM + RAM_D1 boundary cases, including the **0x24080000 regression** (#66) |

The suite has nearly doubled since the v1.2.0 baseline (65 → 119) and gained two
modules (`bl_node_id`, `bl_fdcan`). `bl_health` and `bl_dtc` still have no
dedicated test file — they ride along via `mocks/bl_peer_stubs.c`, which is
mostly linker-satisfaction but exposes a few observable knobs (the watchdog-kick
counter, the apply-wrp mask recorder, the jump counter) that dispatcher tests
assert against. A dedicated file for each is a follow-up when there's a
behavioural invariant worth pinning down.

## How the mocks work

Production sources include `bl_memmap.h` and `stm32h7xx_hal.h` directly.
CMake force-includes the `mocks/` versions before any other header
(`-include` flag, gated with `SHELL:` to keep CMake from collapsing the
flag-and-path pair). This sets the include guards first, so production
headers with the same name are silently skipped — without us having to
maintain divergent copies of bl_memmap or HAL types in production code.

Real flash semantics are preserved in `mocks/hal_stubs.c`:

- **1 → 0 only.** `HAL_FLASH_Program` simulates STM32H7's NOR flash by
  refusing any FLASHWORD write that requires flipping a `0` bit back to
  `1`. This is exactly the failure mode behind #52 and the test
  `test_nvm_compact_replace_meta_erases_old_metadata` locks it down.
- **Sector erase** wipes the requested sectors to `0xFF`.
- **Fault injection.** `mock_flash_set_program_fail(N)` /
  `_erase_fail(N)` make the next N HAL calls return `HAL_ERROR` even on
  legal writes — used by the `bl_nvm_write` compaction-retry regression
  test (#53).
- **Tick control.** `mock_set_tick(t)` / `mock_advance_tick(dt)` make
  `HAL_GetTick()` return deterministic values for timeout tests.
- **Call counters.** `mock_flash_program_call_count()` /
  `_erase_call_count()` let tests assert on "exactly N writes happened".
- **FDCAN TX capture.** Every `HAL_FDCAN_AddMessageToTxFifoQ` call from
  the dispatcher gets recorded into a ring inspectable via
  `mock_fdcan_get(i)` / `mock_fdcan_tx_count()`. The dispatcher
  regression tests use this to assert which frames the BL emitted in
  response to a given input.
- **BKPSRAM redirection.** `mocks/bl_memmap.h` overrides
  `BL_LOG_RING_ADDR` to point at a process-local `g_fake_log_ring[]`
  buffer in `mocks/bl_stubs.c`, so `bl_log` tests can plant arbitrary
  ring state and verify the consumer's bounds checks.

`setUp()` (in `unity_runner.c`) calls `mock_flash_reset()` before every
test — that also resets the FDCAN TX capture ring, so no test can pollute
the next one through stale flash content or leftover emitted frames.

## Adding a test

1. Drop a `test_<module>.c` file in this directory with one or more
   `void test_<name>(void)` functions, each using Unity's
   `TEST_ASSERT_*` macros.
2. Add the .c to the `add_executable(bl_unit_tests …)` list in
   `CMakeLists.txt`.
3. Forward-declare + `RUN_TEST(...)` each function in `unity_runner.c`.
4. Rebuild and run.

If your test needs to call into a `bl_*` module not yet compiled into the host
build, either link the production source in (add it to `CMakeLists.txt`) or add
a stub in `mocks/bl_peer_stubs.c`. Most peer stubs are linker-satisfaction with
neutral return values; a few already expose observable knobs (call counters,
last-mask recorders) — follow that pattern when your test needs to assert on
behaviour.

## CI

Every push and PR runs `.github/workflows/build-and-test.yml` — six jobs:

- **firmware-build** (matrix Debug + Release): cross-compile the BL with
  arm-none-eabi-gcc + Ninja. Warnings-as-errors, link failures, and uploads
  `.elf` / `.bin` / `.hex` artifacts.
- **host-tests**: builds + runs this suite. **Required check** on `dev` and
  `main` — no PR merges without it green.
- **host-tests-sanitized**: the suite again under AddressSanitizer +
  UndefinedBehaviorSanitizer (`-fno-sanitize-recover=all`) — UAF / OOB /
  signed-overflow / shift / null-deref caught on every PR.
- **host-coverage**: runs under `gcovr` and **fails if total line coverage of
  `Core/Src/bl_*.c` drops below 50 %** — a don't-backslide floor, ratcheted up
  from the original 40 %.
- **clang-tidy** (PR-only, changed lines): `bugprone-* / readability-* / cert-* /
  clang-analyzer-* / misc-*` applied to the lines a PR touches.
- **firmware-size** (PR-only): Release size-delta vs the base branch, posted to
  the PR step summary, fails on a text / data / bss regression over threshold.

## Why this matters

The bootloader is the most safety-critical piece of firmware in the
fleet: a flaw here makes every chip carrying it un-recoverable from
the field without SWD. The cost of writing a host test against a pure
function is in seconds; the cost of fielding a fleet-wide BL bug runs
into hours per carrier. The suite above takes ~0.4 s to run. Add a test
for any non-trivial change to a `bl_*` module — every audit finding
this suite has caught (and there have been several) paid for itself
inside the first hour.
