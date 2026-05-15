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
│   └── bl_peer_stubs.c      no-op stubs for bl_dtc / bl_flash / bl_live / bl_obyte
├── test_bl_isotp.c          SF/FF/CF framing, reassembly, timeout, wrap-around
├── test_bl_fwinfo.c         magic + record-version acceptance
├── test_bl_nvm.c            dedup, compaction, format, compact_replace_meta
├── test_bl_log.c            BKPSRAM ring corruption-guard + drain semantics
├── test_bl_proto_id.c       wire-format build/parse over every legal ID
├── test_bl_proto_dispatch.c dispatcher entry-gate coverage + bad-PCI NACK regression
└── test_bl_app_validate.c   BL→APP stack-pointer range predicate boundaries
```

## What's covered today

**Suite size: 65 PASS / 0 IGNORE** in ~0.4 s wall time.

| Module | Test file | Tests | Notable coverage |
|---|---|---:|---|
| `bl_isotp` | `test_bl_isotp.c` | 11 | SF/FF/CF chains, bad-seq, no-FF, overflow, timeout, **tick wrap-around** (#54), **zero-payload CF reject** (#68) |
| `bl_fwinfo` | `test_bl_fwinfo.c` | 5 | magic match, `record_version` ≥ 1, future-minor accepted |
| `bl_nvm` | `test_bl_nvm.c` | 11 | dedup, tombstones, compaction, format, **HAL_ERROR retry via compaction** (#53), `compact_replace_meta` (fix #13) |
| `bl_log` | `test_bl_log.c` | 5 | drain happy path, severity filter, **corrupt-ent_len clamp** + **undersized-unread guard** (#65) |
| `bl_proto_id` | `test_bl_proto_id.c` | 14 | every legal direction × node pair + round-trip + reserved-bit rejection |
| `bl_proto_dispatch` | `test_bl_proto_dispatch.c` | 5 | direction / dst / length / **bad-PCI NACK** (#60) gates + valid-SF passthrough |
| `bl_app_validate` | `test_bl_app_validate.c` | 13 | DTCM + RAM_D1 boundary cases, including the **0x24080000 regression** (#66) |

The pre-#76 plan also called out `bl_health` and `bl_dtc` for host-side
coverage. Today they ride along via `bl_peer_stubs.c` (no-op linker
satisfaction) so dispatcher tests can run; a proper test file for each
is a follow-up when there's a behavioural invariant worth pinning down.

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

If your test needs to call into a `bl_*` module not yet compiled into
the host build, either link the production source in (add it to
`CMakeLists.txt`) or write a stub in `mocks/bl_peer_stubs.c`. The
existing peer stubs are linker-satisfaction only — replace them with
proper fakes if your test needs observable behaviour.

## CI

Every push and PR runs `.github/workflows/build-and-test.yml`:

- **firmware-build** (×2): cross-compile the BL for Debug and Release
  with arm-none-eabi-gcc 14.3 + Ninja. Catches warnings as errors,
  link failures, and uploads `.elf` / `.bin` / `.hex` artifacts.
- **host-tests**: builds + runs this suite. Required check on `dev`
  and `main` branch protection — no PR merges without it green.

The two jobs run in parallel. Total wall time should be under 2 minutes.

## Why this matters

The bootloader is the most safety-critical piece of firmware in the
fleet: a flaw here makes every chip carrying it un-recoverable from
the field without SWD. The cost of writing a host test against a pure
function is in seconds; the cost of fielding a fleet-wide BL bug runs
into hours per carrier. The suite above takes ~0.4 s to run. Add a test
for any non-trivial change to a `bl_*` module — every audit finding
this suite has caught (and there have been several) paid for itself
inside the first hour.
