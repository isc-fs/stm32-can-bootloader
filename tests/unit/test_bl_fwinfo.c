/*
 * test_bl_fwinfo.c — bl_fwinfo_is_present() acceptance tests.
 *
 * The bootloader reads a 64-byte bl_fwinfo_t struct at BL_FWINFO_ADDR
 * (= BL_APP_BASE + 0x400) and uses two checks to gate JUMP:
 *   1. magic == BL_FWINFO_MAGIC
 *   2. record_version major >= 1
 *
 * Both must hold; either failure makes the BL surface NO_VALID_APP.
 * The mocks/bl_memmap.h override puts BL_APP_BASE inside the fake
 * flash buffer, so we can plant valid / invalid records at the right
 * offset by writing into g_fake_flash directly.
 */

#include "bl_fwinfo.h"
#include "bl_memmap.h"
#include "stm32h7xx_hal.h"   /* mock_flash_reset */
#include "unity.h"

#include <stdint.h>
#include <string.h>

static void plant_fwinfo(const bl_fwinfo_t *record)
{
    /* g_fake_flash is filled with 0xFF by setUp(). Write the record at
     * the BL_FWINFO_ADDR offset (which lives inside the buffer thanks
     * to the mock bl_memmap.h). */
    memcpy((void *)(uintptr_t)BL_FWINFO_ADDR, record, sizeof(*record));
}

void test_fwinfo_absent_returns_false_on_erased_flash(void)
{
    /* setUp() left flash as 0xFF — magic reads 0xFFFFFFFF, fails. */
    TEST_ASSERT_FALSE(bl_fwinfo_is_present());
    TEST_ASSERT_NULL(bl_fwinfo_get());
}

void test_fwinfo_valid_magic_and_major1_is_present(void)
{
    bl_fwinfo_t rec = {0};
    rec.magic = BL_FWINFO_MAGIC;
    rec.record_version = BL_FWINFO_RECORD_VERSION;  /* 0x00010000 = 1.0 */
    rec.fw_version_major = 1;
    rec.mcu_id = 0x00000450U;
    memcpy(rec.product_name, "BL-TEST", 7);
    plant_fwinfo(&rec);

    TEST_ASSERT_TRUE(bl_fwinfo_is_present());

    const bl_fwinfo_t *got = bl_fwinfo_get();
    TEST_ASSERT_NOT_NULL(got);
    TEST_ASSERT_EQUAL_UINT32(BL_FWINFO_MAGIC, got->magic);
}

void test_fwinfo_wrong_magic_rejected(void)
{
    bl_fwinfo_t rec = {0};
    rec.magic = BL_FWINFO_MAGIC ^ 0x00000001U;     /* off by one bit */
    rec.record_version = BL_FWINFO_RECORD_VERSION;
    plant_fwinfo(&rec);

    TEST_ASSERT_FALSE(bl_fwinfo_is_present());
}

void test_fwinfo_major_version_zero_rejected(void)
{
    bl_fwinfo_t rec = {0};
    rec.magic = BL_FWINFO_MAGIC;
    /* major = 0 (high 16 bits), minor = 5. Should be rejected even
     * though the magic is right. */
    rec.record_version = 0x00000005U;
    plant_fwinfo(&rec);

    TEST_ASSERT_FALSE(bl_fwinfo_is_present());
}

void test_fwinfo_future_minor_accepted(void)
{
    bl_fwinfo_t rec = {0};
    rec.magic = BL_FWINFO_MAGIC;
    /* major = 1, minor = 9999. Forward-compat policy says accept. */
    rec.record_version = 0x0001270FU;
    plant_fwinfo(&rec);

    TEST_ASSERT_TRUE(bl_fwinfo_is_present());
}
