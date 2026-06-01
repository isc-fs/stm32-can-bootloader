/*
 * test_bl_fdcan.c — multi-bus FDCAN front-end (#120).
 *
 * The BL serves its protocol on ALL THREE FDCAN peripherals at once, so a
 * host on whichever bus the board wires CAN to is heard. These tests pin
 * that contract:
 *
 *   - bl_fdcan_bus(i) maps 0/1/2 -> hfdcan1/2/3 (and clamps out-of-range
 *     so a caller can't dereference past the array);
 *   - bl_fdcan_configure_filters installs the RX filters on EVERY bus, and
 *     propagates a HAL failure instead of booting half-deaf;
 *   - bl_fdcan_start_all starts EVERY bus, and propagates failure;
 *   - bl_fdcan_set_active routes bl_fdcan_get_handle() to the bus a reply
 *     should go back out on (reply-on-origin), and refuses a NULL.
 *
 * The hfdcanN handles are defined in hal_stubs.c (on-chip: CubeMX main.c).
 * The stub HAL_FDCAN_{ConfigFilter,ConfigGlobalFilter,Start} count their
 * calls per bus, so "all three were covered" is asserted, not assumed.
 */

#include "bl_fdcan.h"
#include "stm32h7xx_hal.h"   /* hfdcanN + the mock per-bus call counters */

#include "unity.h"

#include <stdint.h>

extern FDCAN_HandleTypeDef hfdcan1;
extern FDCAN_HandleTypeDef hfdcan2;
extern FDCAN_HandleTypeDef hfdcan3;

/* ---- bus index -> handle ---- */

void test_fdcan_bus_count_is_three(void)
{
    TEST_ASSERT_EQUAL_UINT(3U, BL_FDCAN_BUS_COUNT);
}

void test_fdcan_bus_maps_index_to_handle(void)
{
    TEST_ASSERT_EQUAL_PTR(&hfdcan1, bl_fdcan_bus(0U));
    TEST_ASSERT_EQUAL_PTR(&hfdcan2, bl_fdcan_bus(1U));
    TEST_ASSERT_EQUAL_PTR(&hfdcan3, bl_fdcan_bus(2U));
}

void test_fdcan_bus_out_of_range_clamps_to_bus0(void)
{
    /* An out-of-range index must not read past the array — it returns bus 0
     * so a caller can never dereference garbage. */
    TEST_ASSERT_EQUAL_PTR(&hfdcan1, bl_fdcan_bus(3U));
    TEST_ASSERT_EQUAL_PTR(&hfdcan1, bl_fdcan_bus(255U));
}

/* ---- reply-on-origin: set_active / get_handle ---- */

void test_fdcan_set_active_routes_get_handle(void)
{
    bl_fdcan_set_active(&hfdcan1);
    TEST_ASSERT_EQUAL_PTR(&hfdcan1, bl_fdcan_get_handle());
    bl_fdcan_set_active(&hfdcan2);
    TEST_ASSERT_EQUAL_PTR(&hfdcan2, bl_fdcan_get_handle());
    bl_fdcan_set_active(&hfdcan3);
    TEST_ASSERT_EQUAL_PTR(&hfdcan3, bl_fdcan_get_handle());
}

void test_fdcan_set_active_null_is_ignored(void)
{
    /* A NULL must never become the reply target (it would crash the TX
     * path). set_active(NULL) keeps the previous bus. */
    bl_fdcan_set_active(&hfdcan2);
    bl_fdcan_set_active((FDCAN_HandleTypeDef *)0);
    TEST_ASSERT_EQUAL_PTR(&hfdcan2, bl_fdcan_get_handle());
}

/* ---- configure_filters: every bus ---- */

void test_fdcan_configure_filters_returns_ok(void)
{
    TEST_ASSERT_EQUAL_INT(HAL_OK, bl_fdcan_configure_filters(0x05U));
}

void test_fdcan_configure_filters_covers_all_buses(void)
{
    /* Every bus must get both FIFO0 filters (unicast + broadcast) and the
     * reject-everything-else global filter — otherwise a host on an
     * unconfigured bus would never be heard. */
    TEST_ASSERT_EQUAL_INT(HAL_OK, bl_fdcan_configure_filters(0x05U));
    for (int b = 0; b < 3; b++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(2, mock_fdcan_cfgfilter_count(b),
                                      "each bus needs 2 RX filters");
        TEST_ASSERT_EQUAL_INT_MESSAGE(1, mock_fdcan_globalfilter_count(b),
                                      "each bus needs its global filter");
    }
}

void test_fdcan_configure_filters_propagates_failure(void)
{
    /* If the HAL rejects a filter on any bus, configure_filters must return
     * the error (BL init treats it as fatal + reboots) rather than coming
     * up with a partly-deaf bus. Fail the first ConfigFilter call. */
    mock_fdcan_set_configfilter_fail(1);
    TEST_ASSERT_TRUE(bl_fdcan_configure_filters(0x05U) != HAL_OK);
}

/* ---- start_all: every bus ---- */

void test_fdcan_start_all_returns_ok(void)
{
    TEST_ASSERT_EQUAL_INT(HAL_OK, bl_fdcan_start_all());
}

void test_fdcan_start_all_starts_every_bus(void)
{
    TEST_ASSERT_EQUAL_INT(HAL_OK, bl_fdcan_start_all());
    for (int b = 0; b < 3; b++) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(1, mock_fdcan_start_count(b),
                                      "every bus must be started");
    }
}

void test_fdcan_start_all_propagates_failure(void)
{
    mock_fdcan_set_start_fail(1);
    TEST_ASSERT_TRUE(bl_fdcan_start_all() != HAL_OK);
}
