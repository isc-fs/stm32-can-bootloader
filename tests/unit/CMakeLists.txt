cmake_minimum_required(VERSION 3.15)
project(bl_unit_tests LANGUAGES C)

# Host-side unit tests for the bootloader. Run on a developer machine
# or in CI — never against the arm-none-eabi toolchain.
#
#   cmake -B build-tests -S tests/unit
#   cmake --build build-tests
#   ctest --test-dir build-tests --output-on-failure
#
# The mocks/ directory provides:
#   - stm32h7xx_hal.h  — minimal HAL types (HAL_StatusTypeDef, FLASH_*
#                        constants, function declarations) so production
#                        sources compile against host gcc.
#   - bl_memmap.h      — redirects every BL_*_BASE address into a
#                        process-local 1-MB g_fake_flash buffer. The
#                        production sector / metadata layout is
#                        preserved relative to that base.
#   - hal_stubs.c      — implements HAL_FLASH_* against g_fake_flash
#                        with the real STM32 1→0-only programming rule
#                        + sector-erase semantics. Test helpers expose
#                        knobs for fault injection.
#
# Adding a test:
#   1. Drop a `test_<module>.c` into this directory with one or more
#      `void test_<name>(void)` functions.
#   2. Forward-declare and `RUN_TEST` each one in unity_runner.c.
#   3. Add the .c to `add_executable(bl_unit_tests …)` below.

set(CMAKE_C_STANDARD            11)
set(CMAKE_C_STANDARD_REQUIRED   ON)

include(FetchContent)
FetchContent_Declare(Unity
    GIT_REPOSITORY https://github.com/ThrowTheSwitch/Unity.git
    GIT_TAG        v2.6.1
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(Unity)

set(BL_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)

add_executable(bl_unit_tests
    # Production sources under test
    ${BL_ROOT}/Core/Src/bl_isotp.c
    ${BL_ROOT}/Core/Src/bl_fwinfo.c
    ${BL_ROOT}/Core/Src/bl_nvm.c
    ${BL_ROOT}/Core/Src/bl_log.c
    ${BL_ROOT}/Core/Src/bl_proto.c

    # Mocks / stubs
    mocks/hal_stubs.c
    mocks/bl_stubs.c
    mocks/bl_peer_stubs.c

    # Tests
    test_bl_isotp.c
    test_bl_fwinfo.c
    test_bl_nvm.c
    test_bl_proto_id.c
    test_bl_log.c
    test_bl_app_validate.c
    test_bl_proto_dispatch.c
    unity_runner.c
)

# bl_proto.c includes main.h which lives in Core/Inc. The other
# production sources don't need it but it's harmless to expose.
target_include_directories(bl_unit_tests PRIVATE
    ${BL_ROOT}/Core/Inc
)

target_include_directories(bl_unit_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/mocks
    ${BL_ROOT}/Core/Inc
    ${Unity_SOURCE_DIR}/src
)

target_link_libraries(bl_unit_tests PRIVATE unity)

# Match the firmware target's warning posture so host build catches
# the same regressions. -Werror keeps the baseline honest.
#
# `-include` force-injects the mock headers at the top of every
# translation unit so they "win" the include-guard race even when a
# production header in Core/Inc/ does `#include "bl_memmap.h"` (which
# would otherwise pick up the real header from its own directory
# first, per C's `#include "…"` lookup rules).
target_compile_options(bl_unit_tests PRIVATE
    -Wall -Wextra -Wpedantic -Werror
    -Wno-unused-parameter        # Unity helpers leak this
    # SHELL: prefix keeps the two-token "-include PATH" pair from being
    # collapsed into "-include PATH PATH" by CMake's option-deduping.
    "SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/mocks/bl_memmap.h"
    "SHELL:-include ${CMAKE_CURRENT_SOURCE_DIR}/mocks/stm32h7xx_hal.h"
)

enable_testing()
add_test(
    NAME bl_unit_tests
    COMMAND bl_unit_tests
)
