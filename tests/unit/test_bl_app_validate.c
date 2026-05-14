/*
 * test_bl_app_validate.c — bl_app_stack_in_legal_range() coverage.
 *
 * Issue #66 surfaced two facts:
 *   1. Bootloader_CheckApplication used a bitmask form
 *        ((sp & 0x2FF00000) == 0x24000000)
 *      that silently accepted the full 1 MB above 0x24000000 — much
 *      larger than the actual 320 KB RAM_D1 region on STM32H733.
 *   2. Bootloader_JumpToApplication used a stricter range form
 *        (sp <= 0x24050000)
 *      that correctly rejected addresses past the real region.
 *
 * The divergence let a malformed app pass Check and silently fail at
 * Jump time with no diagnostic surface. After the refactor both call
 * sites share `bl_app_stack_in_legal_range` from bl_app_validate.h —
 * these tests lock that single predicate down at every boundary.
 *
 * The predicate is a chip-side rule, so the tests are written against
 * the chip's address constants. The host doesn't actually dereference
 * any of these — they're just uint32_t values being compared.
 */

#include "bl_app_validate.h"
#include "unity.h"

#include <stdint.h>

/* ---- DTCM region (128 KB at 0x20000000) ---- */

void test_app_stack_dtcm_base_accepted(void)
{
    TEST_ASSERT_TRUE(bl_app_stack_in_legal_range(0x20000000U));
}

void test_app_stack_dtcm_interior_accepted(void)
{
    TEST_ASSERT_TRUE(bl_app_stack_in_legal_range(0x2001FFFCU));
}

void test_app_stack_dtcm_initial_msp_accepted(void)
{
    /* ARM convention: initial MSP points one byte past the top of
     * stack, so 0x20020000 (= 0x20000000 + 128 KB) is the canonical
     * "MSP at top of DTCM" value and MUST be accepted. */
    TEST_ASSERT_TRUE(bl_app_stack_in_legal_range(0x20020000U));
}

void test_app_stack_just_past_dtcm_rejected(void)
{
    /* One byte past the inclusive upper bound. Conservative: in the
     * 0x20020000..0x23FFFFFF gap between DTCM and RAM_D1 there is no
     * RAM, so reject. */
    TEST_ASSERT_FALSE(bl_app_stack_in_legal_range(0x20020004U));
}

/* ---- RAM_D1 region (320 KB at 0x24000000) — the bug region ---- */

void test_app_stack_d1_base_accepted(void)
{
    TEST_ASSERT_TRUE(bl_app_stack_in_legal_range(0x24000000U));
}

void test_app_stack_d1_interior_accepted(void)
{
    TEST_ASSERT_TRUE(bl_app_stack_in_legal_range(0x2404FFFCU));
}

void test_app_stack_d1_initial_msp_accepted(void)
{
    /* 0x24050000 = 0x24000000 + 320 KB — same "initial MSP at top"
     * pattern as DTCM. */
    TEST_ASSERT_TRUE(bl_app_stack_in_legal_range(0x24050000U));
}

void test_app_stack_just_past_d1_rejected_regression(void)
{
    /* This is the regression test for issue #66. The old bitmask check
     * in Bootloader_CheckApplication used `& 0x2FF00000 == 0x24000000`,
     * which accepted any address 0x24000000..0x240FFFFF (1 MB total).
     * So 0x24080000 was *approved* by Check while Jump rejected it,
     * leading to "valid app refuses to boot, no diagnostic". With the
     * unified predicate both call sites correctly reject this. */
    TEST_ASSERT_FALSE(bl_app_stack_in_legal_range(0x24080000U));
}

void test_app_stack_far_past_d1_rejected(void)
{
    /* Top of the legacy bitmask's 1-MB window — must reject. */
    TEST_ASSERT_FALSE(bl_app_stack_in_legal_range(0x240FFFFCU));
}

/* ---- Gaps and unrelated regions ---- */

void test_app_stack_gap_between_dtcm_and_d1_rejected(void)
{
    /* Anywhere in the AHB/APB peripheral / FMC regions between the
     * two SRAMs is not legal stack. */
    TEST_ASSERT_FALSE(bl_app_stack_in_legal_range(0x21000000U));
    TEST_ASSERT_FALSE(bl_app_stack_in_legal_range(0x23FFFFFCU));
}

void test_app_stack_flash_address_rejected(void)
{
    /* Common gotcha: app linker forgot to point .stack at SRAM and
     * MSP ends up at an address inside FLASH. Must reject. */
    TEST_ASSERT_FALSE(bl_app_stack_in_legal_range(0x08000000U));
    TEST_ASSERT_FALSE(bl_app_stack_in_legal_range(0x08020000U));  /* BL_APP_BASE */
}

void test_app_stack_zero_rejected(void)
{
    /* MSP=0 = uninitialised vector table entry. */
    TEST_ASSERT_FALSE(bl_app_stack_in_legal_range(0x00000000U));
}

void test_app_stack_top_of_address_space_rejected(void)
{
    TEST_ASSERT_FALSE(bl_app_stack_in_legal_range(0xFFFFFFFFU));
}
