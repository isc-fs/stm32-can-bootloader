/*
 * test_bl_flashcount.c — persistent flash-op counter (#187).
 *
 * Two bugs pinned here (both found on the v1.7.0 release bench):
 *
 *   1. The counter restarted at 0 every boot: it was restored from NVM
 *      before bl_nvm_init() had found the append point, so the read
 *      scanned zero slots. The restore now runs after bl_nvm_init (the
 *      main.c ordering itself isn't host-built; these tests pin that a
 *      write -> re-init -> restore cycle round-trips the value).
 *   2. Every flash op appended an NVM record (513 records for one 87 KB
 *      app flash). bump() is now RAM-only and flush() writes at most one
 *      record, and only when the value changed.
 *
 * The dispatcher-level "K chunks -> O(1) records" check lives in
 * test_bl_proto_dispatch.c next to the other session-flow tests.
 */

#include "bl_flashcount.h"
#include "bl_memmap.h"
#include "bl_nvm.h"
#include "stm32h7xx_hal.h"
#include "unity.h"

#include <stdint.h>
#include <string.h>

/* Count every live-magic record for `key` in sector 7 — including the
 * superseded ones, which is exactly the churn bug #187 is about. */
uint32_t flashcount_test_nvm_records_for_key(uint16_t key)
{
    uint32_t n = 0U;
    for (uint32_t off = 0U; off < BL_NVM_SIZE; off += BL_NVM_ENTRY_SIZE) {
        const bl_nvm_entry_t *e = (const bl_nvm_entry_t *)(BL_NVM_BASE + off);
        if (e->magic == BL_NVM_ENTRY_MAGIC && e->key == key) {
            n++;
        }
    }
    return n;
}

static uint32_t persisted_count(void)
{
    uint32_t v = 0U;
    uint8_t len = 0U;
    TEST_ASSERT_EQUAL_INT(BL_NVM_OK,
        bl_nvm_read(BL_NVM_KEY_FLASH_WRITE_COUNT, &v, sizeof(v), &len));
    TEST_ASSERT_EQUAL_UINT8(sizeof(v), len);
    return v;
}

/* setUp() erased the fake flash; bring NVM + the counter up on it the
 * way Bootloader_Init does (nvm_init, then restore). */
static void boot(void)
{
    bl_nvm_init();
    bl_flashcount_restore();
}

void test_flashcount_starts_at_zero_on_blank_nvm(void)
{
    boot();
    TEST_ASSERT_EQUAL_UINT32(0U, bl_flashcount_get());
}

void test_flashcount_survives_simulated_reboot(void)
{
    boot();
    for (int i = 0; i < 5; i++) {
        bl_flashcount_bump();
    }
    bl_flashcount_flush();

    /* Reboot: RAM state is rebuilt from flash — NVM re-scanned first,
     * then the counter restored (the #187 ordering). */
    boot();
    TEST_ASSERT_EQUAL_UINT32(5U, bl_flashcount_get());

    /* ...and keeps counting from there across a second reboot. */
    bl_flashcount_bump();
    bl_flashcount_bump();
    bl_flashcount_flush();
    boot();
    TEST_ASSERT_EQUAL_UINT32(7U, bl_flashcount_get());
}

void test_flashcount_bump_does_not_write_nvm(void)
{
    boot();
    for (int i = 0; i < 500; i++) {
        bl_flashcount_bump();
    }
    TEST_ASSERT_EQUAL_UINT32(500U, bl_flashcount_get());
    TEST_ASSERT_EQUAL_UINT32(0U,
        flashcount_test_nvm_records_for_key(BL_NVM_KEY_FLASH_WRITE_COUNT));
}

void test_flashcount_flush_writes_once_and_is_noop_when_clean(void)
{
    boot();
    for (int i = 0; i < 300; i++) {
        bl_flashcount_bump();
    }
    bl_flashcount_flush();
    bl_flashcount_flush();   /* nothing changed — must not append */
    bl_flashcount_flush();

    TEST_ASSERT_EQUAL_UINT32(1U,
        flashcount_test_nvm_records_for_key(BL_NVM_KEY_FLASH_WRITE_COUNT));
    TEST_ASSERT_EQUAL_UINT32(300U, persisted_count());
}

void test_flashcount_flush_with_nothing_flashed_writes_nothing(void)
{
    boot();
    bl_flashcount_flush();
    TEST_ASSERT_EQUAL_UINT32(0U,
        flashcount_test_nvm_records_for_key(BL_NVM_KEY_FLASH_WRITE_COUNT));
}

void test_flashcount_failed_flush_retries_at_next_boundary(void)
{
    /* G-A2 degraded boot: restore is skipped, NVM rejects writes. The
     * flush must fail quietly and stay dirty, then land once NVM_FORMAT
     * makes the sector writable again. */
    bl_nvm_init_degraded();
    bl_flashcount_restore();   /* reads NOT_FOUND in degraded mode -> 0 */
    bl_flashcount_bump();
    bl_flashcount_bump();
    bl_flashcount_flush();     /* rejected (degraded) */
    TEST_ASSERT_EQUAL_UINT32(2U, bl_flashcount_get());

    TEST_ASSERT_EQUAL_INT(BL_NVM_OK, bl_nvm_format());
    bl_flashcount_flush();     /* retried — now persists */
    TEST_ASSERT_EQUAL_UINT32(2U, persisted_count());
}
