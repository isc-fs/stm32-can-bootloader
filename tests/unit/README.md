# Host-side unit tests

[Unity](https://github.com/ThrowTheSwitch/Unity)-based unit suite for the
bootloader's pure-logic modules. Runs on a developer machine or in CI
in <1 s — no toolchain, no hardware, no flash. Catches regressions in
parsing, framing, NVM management, and other invariants without waiting
for a bench session.

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
├── README.md            ← you are here
├── CMakeLists.txt       host build: Unity via FetchContent + ctest
├── unity_runner.c       UNITY_BEGIN / RUN_TEST(...) entry point
├── mocks/
│   ├── stm32h7xx_hal.h  minimal HAL types so production .c files compile
│   ├── bl_memmap.h      redirects BL_*_BASE / BL_*_ADDR into g_fake_flash[]
│   └── hal_stubs.c      simulated flash + tick (1→0 rule enforced)
├── test_bl_isotp.c      SF/FF/CF framing + reassembly + timeout
├── test_bl_fwinfo.c     magic + version acceptance
└── test_bl_nvm.c        dedup, compaction, format, compact_replace_meta
```

## What's covered today

| Module | Test file | Tests |
|---|---|---:|
| `bl_isotp` | `test_bl_isotp.c` | 10 (9 pass, 1 IGNORE pending #54) |
| `bl_fwinfo` | `test_bl_fwinfo.c` | 5 |
| `bl_nvm` | `test_bl_nvm.c` | 11 (incl. fix #13's `compact_replace_meta` + `format` + 1→0 retry) |

Modules not yet covered host-side (planned in follow-up PRs):
`bl_proto` frame parsing + handler dispatch · `bl_flash` range / alignment
checks · `bl_dtc` ring buffer · `bl_log` ring + severity filter ·
`bl_health` record layout · `bl_live` snapshot · `bl_obyte` WRP token.

## How the mocks work

Production sources include `bl_memmap.h` and `stm32h7xx_hal.h` directly.
CMake force-includes the `mocks/` versions before any other header
(`-include` flag, gated with `SHELL:` to keep CMake from collapsing the
flag-and-path pair). This sets the include guards first, so production
headers with the same name are silently skipped — without us having to
maintain divergent copies of bl_memmap or HAL types in production code.

Real flash semantics are preserved in `mocks/hal_stubs.c`:

- **1 → 0 only**. `HAL_FLASH_Program` simulates STM32H7's NOR flash by
  refusing any FLASHWORD write that requires flipping a `0` bit back to
  `1`. This is exactly the failure mode behind issue #52 and the test
  `test_nvm_compact_replace_meta_erases_old_metadata` locks it down.
- **Sector erase** wipes the requested sectors to `0xFF`.
- **Fault injection**. `mock_flash_set_program_fail(N)` / `_erase_fail(N)`
  make the next N HAL calls return `HAL_ERROR` even on legal writes —
  used by the `bl_nvm_write` compaction-retry regression test.
- **Tick control**. `mock_set_tick(t)` / `mock_advance_tick(dt)` make
  `HAL_GetTick()` return deterministic values for timeout-related tests.
- **Call counters**. `mock_flash_program_call_count()` /
  `_erase_call_count()` let tests assert on "exactly N writes happened".

## Adding a test

1. Drop a `test_<module>.c` file in this directory with one or more
   `void test_<name>(void)` functions, each using Unity's
   `TEST_ASSERT_*` macros.
2. Add the .c to the `add_executable(bl_unit_tests …)` list in
   `CMakeLists.txt`.
3. Forward-declare + `RUN_TEST(...)` each function in `unity_runner.c`.
4. Rebuild and run.

`setUp()` (in `unity_runner.c`) calls `mock_flash_reset()` before every
test so flash starts erased + counters zeroed. No test can pollute the
next one through stale flash content.

## CI

Every push and PR runs `.github/workflows/build-and-test.yml`:

- **firmware-build** (×2): cross-compile the BL for Debug and Release
  with arm-none-eabi-gcc 14.3 + Ninja. Catches warnings as errors,
  link failures, and uploads `.elf` / `.bin` / `.hex` artifacts.
- **host-tests**: builds + runs this suite. Must be green for any PR
  to merge once branch protection is configured.

The two jobs run in parallel. Total wall time should be under 2 minutes.

## Why this matters

The bootloader is the most safety-critical piece of firmware in the
fleet: a flaw here makes every chip carrying it un-recoverable from
the field without SWD. The cost of writing a host test against a pure
function is in seconds; the cost of fielding a fleet-wide BL bug runs
into hours per carrier. The suite above takes 0.4 s to run. Add a test
for any non-trivial change to a `bl_*` module.
