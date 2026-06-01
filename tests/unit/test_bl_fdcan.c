/*
 * test_bl_fdcan.c — NVM-backed runtime FDCAN-instance override (#120 Phase B).
 *
 * The BL's FDCAN bus instance has two sources: a compile-time
 * BL_FDCAN_INSTANCE (the host CMake doesn't -D-set it, so it falls through
 * to the bl_config.h default 2) and an NVM override under
 * BL_NVM_KEY_FDCAN_INSTANCE = 0x0004. bl_fdcan_init_from_nvm() resolves +
 * caches the choice at boot; bl_fdcan_get_instance_number() and
 * bl_fdcan_get_handle() return it.
 *
 * Mirrors test_bl_node_id: exhaustively covers the validation so an
 * operator provisioning via CMD_NVM_WRITE can't brick a node, plus the
 * get_handle() identity — the resolved instance maps to the right hfdcanN.
 *
 * Validation under test:
 *   - empty NVM              → fall back to BL_FDCAN_INSTANCE
 *   - valid 1 / 2 / 3        → override (+ get_handle maps correctly)
 *   - 0 / 4 / 0xFF           → fall back (out of range)
 *   - wrong length (≠ 1 byte)→ fall back
 *   - idempotent + reinit-picks-up-changes + tombstone-restores-default
 */

#include "bl_config.h"   /* BL_FDCAN_INSTANCE default */
#include "bl_fdcan.h"
#include "bl_nvm.h"
#include "stm32h7xx_hal.h"  /* hfdcan1 / hfdcan2 / hfdcan3 */

#include "unity.h"

#include <stdint.h>

/* hfdcanN are defined in hal_stubs.c (on-chip: CubeMX main.c). Extern them
 * here so the override tests can assert bl_fdcan_get_handle() resolves to
 * the right one. */
extern FDCAN_HandleTypeDef hfdcan1;
extern FDCAN_HandleTypeDef hfdcan2;
extern FDCAN_HandleTypeDef hfdcan3;

/* Write a single byte under the FDCAN-instance key via the real NVM API —
 * the same path CMD_NVM_WRITE takes, so coverage matches provisioning. */
static void nvm_seed_instance(uint8_t value)
{
    bl_nvm_init();
    bl_nvm_status_t st = bl_nvm_write(BL_NVM_KEY_FDCAN_INSTANCE, &value, 1U);
    TEST_ASSERT_EQUAL_INT(BL_NVM_OK, st);
}

/* ---- fallback path ---- */

void test_fdcan_falls_back_to_compile_time_on_empty_nvm(void)
{
    bl_nvm_init();
    bl_fdcan_init_from_nvm();
    TEST_ASSERT_EQUAL_UINT8(BL_FDCAN_INSTANCE, bl_fdcan_get_instance_number());
}

void test_fdcan_falls_back_on_zero(void)
{
    nvm_seed_instance(0x00U);
    bl_fdcan_init_from_nvm();
    TEST_ASSERT_EQUAL_UINT8(BL_FDCAN_INSTANCE, bl_fdcan_get_instance_number());
}

void test_fdcan_falls_back_on_four(void)
{
    /* The H733 exposes FDCAN1..3; 4 is out of range. */
    nvm_seed_instance(0x04U);
    bl_fdcan_init_from_nvm();
    TEST_ASSERT_EQUAL_UINT8(BL_FDCAN_INSTANCE, bl_fdcan_get_instance_number());
}

void test_fdcan_falls_back_on_high_byte(void)
{
    nvm_seed_instance(0xFFU);
    bl_fdcan_init_from_nvm();
    TEST_ASSERT_EQUAL_UINT8(BL_FDCAN_INSTANCE, bl_fdcan_get_instance_number());
}

void test_fdcan_falls_back_on_multi_byte(void)
{
    /* 1-byte key by contract; a 2-byte write must NOT use byte 0. */
    const uint8_t two[2] = { 0x01U, 0x03U };
    bl_nvm_init();
    bl_nvm_status_t st = bl_nvm_write(BL_NVM_KEY_FDCAN_INSTANCE, two, 2U);
    TEST_ASSERT_EQUAL_INT(BL_NVM_OK, st);
    bl_fdcan_init_from_nvm();
    TEST_ASSERT_EQUAL_UINT8(BL_FDCAN_INSTANCE, bl_fdcan_get_instance_number());
}

/* ---- override path + handle identity ---- */

void test_fdcan_override_1_selects_hfdcan1(void)
{
    nvm_seed_instance(1U);
    bl_fdcan_init_from_nvm();
    TEST_ASSERT_EQUAL_UINT8(1U, bl_fdcan_get_instance_number());
    TEST_ASSERT_EQUAL_PTR(&hfdcan1, bl_fdcan_get_handle());
}

void test_fdcan_override_2_selects_hfdcan2(void)
{
    nvm_seed_instance(2U);
    bl_fdcan_init_from_nvm();
    TEST_ASSERT_EQUAL_UINT8(2U, bl_fdcan_get_instance_number());
    TEST_ASSERT_EQUAL_PTR(&hfdcan2, bl_fdcan_get_handle());
}

void test_fdcan_override_3_selects_hfdcan3(void)
{
    nvm_seed_instance(3U);
    bl_fdcan_init_from_nvm();
    TEST_ASSERT_EQUAL_UINT8(3U, bl_fdcan_get_instance_number());
    TEST_ASSERT_EQUAL_PTR(&hfdcan3, bl_fdcan_get_handle());
}

/* ---- flow / idempotence ---- */

void test_fdcan_init_is_idempotent(void)
{
    nvm_seed_instance(3U);
    bl_fdcan_init_from_nvm();
    bl_fdcan_init_from_nvm();
    bl_fdcan_init_from_nvm();
    TEST_ASSERT_EQUAL_UINT8(3U, bl_fdcan_get_instance_number());
}

void test_fdcan_reinit_picks_up_nvm_changes(void)
{
    /* provision → reboot → BL re-reads NVM and the new instance applies. */
    nvm_seed_instance(1U);
    bl_fdcan_init_from_nvm();
    TEST_ASSERT_EQUAL_UINT8(1U, bl_fdcan_get_instance_number());

    uint8_t v = 3U;
    TEST_ASSERT_EQUAL_INT(BL_NVM_OK,
                          bl_nvm_write(BL_NVM_KEY_FDCAN_INSTANCE, &v, 1U));

    bl_fdcan_init_from_nvm();  /* simulated reboot */
    TEST_ASSERT_EQUAL_UINT8(3U, bl_fdcan_get_instance_number());
}

void test_fdcan_reinit_after_tombstone_restores_default(void)
{
    nvm_seed_instance(1U);
    bl_fdcan_init_from_nvm();
    TEST_ASSERT_EQUAL_UINT8(1U, bl_fdcan_get_instance_number());

    /* Clear the override (len=0 tombstone) → next boot returns to default. */
    TEST_ASSERT_EQUAL_INT(BL_NVM_OK,
                          bl_nvm_write(BL_NVM_KEY_FDCAN_INSTANCE, NULL, 0U));

    bl_fdcan_init_from_nvm();
    TEST_ASSERT_EQUAL_UINT8(BL_FDCAN_INSTANCE, bl_fdcan_get_instance_number());
}

/* ---- filter config ---- */

void test_fdcan_configure_filters_returns_ok(void)
{
    /* Smoke: the filter geometry is instance-agnostic; on the resolved
     * handle, with the host HAL stubs succeeding, configure_filters should
     * run its two unicast/broadcast ConfigFilter calls + the global filter
     * and return HAL_OK. */
    bl_nvm_init();
    bl_fdcan_init_from_nvm();
    TEST_ASSERT_EQUAL_INT(HAL_OK, bl_fdcan_configure_filters(0x05U));
}
