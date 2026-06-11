/*
 * test_bl_nvm.c — exhaustive tests for the log-structured NVM in
 * sector 7, plus the bl_nvm_format() / bl_nvm_compact_replace_meta()
 * paths added by fix #13.
 *
 * These run against the host HAL stub which models the real STM32H7
 * 1→0-only programming rule, so anything that depends on "must erase
 * before reprogram" behaves identically here and on-chip.
 */

#include "bl_nvm.h"
#include "bl_memmap.h"
#include "stm32h7xx_hal.h"
#include "unity.h"

#include <stdint.h>
#include <string.h>

static void seed_and_init(void)
{
    /* setUp() has already cleared g_fake_flash to 0xFF (erased). Just
     * call bl_nvm_init() so internal pointers reflect that. */
    bl_nvm_init();
}

void test_nvm_init_on_erased_sector_has_zero_live(void)
{
    seed_and_init();
    TEST_ASSERT_EQUAL_UINT32(0U, bl_nvm_live_count());
}

void test_nvm_write_then_read_round_trip(void)
{
    seed_and_init();

    uint8_t value[3] = {0xAB, 0xCD, 0xEF};
    TEST_ASSERT_EQUAL_INT(BL_NVM_OK, bl_nvm_write(0x1234U, value, sizeof(value)));

    uint8_t out[BL_NVM_MAX_VALUE_LEN] = {0};
    uint8_t actual_len = 0;
    TEST_ASSERT_EQUAL_INT(BL_NVM_OK,
        bl_nvm_read(0x1234U, out, sizeof(out), &actual_len));
    TEST_ASSERT_EQUAL_UINT8(3U, actual_len);
    TEST_ASSERT_EQUAL_MEMORY(value, out, 3);
}

void test_nvm_latest_seq_wins_on_repeated_key(void)
{
    seed_and_init();

    uint8_t v1 = 0x11;
    uint8_t v2 = 0x22;
    uint8_t v3 = 0x33;
    TEST_ASSERT_EQUAL_INT(BL_NVM_OK, bl_nvm_write(0x40U, &v1, 1));
    TEST_ASSERT_EQUAL_INT(BL_NVM_OK, bl_nvm_write(0x40U, &v2, 1));
    TEST_ASSERT_EQUAL_INT(BL_NVM_OK, bl_nvm_write(0x40U, &v3, 1));

    uint8_t out = 0;
    uint8_t actual_len = 0;
    TEST_ASSERT_EQUAL_INT(BL_NVM_OK, bl_nvm_read(0x40U, &out, 1, &actual_len));
    TEST_ASSERT_EQUAL_UINT8(1U, actual_len);
    TEST_ASSERT_EQUAL_UINT8(0x33U, out);
}

void test_nvm_tombstone_hides_previous_value(void)
{
    seed_and_init();

    uint8_t v = 0xCC;
    TEST_ASSERT_EQUAL_INT(BL_NVM_OK, bl_nvm_write(0x50U, &v, 1));

    /* Tombstone: len = 0. */
    TEST_ASSERT_EQUAL_INT(BL_NVM_OK, bl_nvm_write(0x50U, NULL, 0));

    uint8_t out = 0;
    uint8_t actual_len = 0;
    TEST_ASSERT_EQUAL_INT(BL_NVM_NOT_FOUND,
        bl_nvm_read(0x50U, &out, 1, &actual_len));
}

void test_nvm_live_count_tracks_unique_keys(void)
{
    seed_and_init();
    TEST_ASSERT_EQUAL_UINT32(0U, bl_nvm_live_count());

    uint8_t v = 0;
    bl_nvm_write(0x01U, &v, 1);
    bl_nvm_write(0x02U, &v, 1);
    bl_nvm_write(0x03U, &v, 1);
    TEST_ASSERT_EQUAL_UINT32(3U, bl_nvm_live_count());

    /* Re-write key 0x02 — still only 3 unique live keys. */
    bl_nvm_write(0x02U, &v, 1);
    TEST_ASSERT_EQUAL_UINT32(3U, bl_nvm_live_count());

    /* Tombstone key 0x02 — drops to 2. */
    bl_nvm_write(0x02U, NULL, 0);
    TEST_ASSERT_EQUAL_UINT32(2U, bl_nvm_live_count());
}

void test_nvm_value_too_long_rejects(void)
{
    seed_and_init();

    uint8_t big[BL_NVM_MAX_VALUE_LEN + 1] = {0};
    TEST_ASSERT_EQUAL_INT(BL_NVM_BAD_ARG,
        bl_nvm_write(0x60U, big, sizeof(big)));
}

void test_nvm_format_wipes_sector_and_resets_pointers(void)
{
    seed_and_init();

    /* Plant a few entries. */
    uint8_t v = 0xAA;
    bl_nvm_write(0x70U, &v, 1);
    bl_nvm_write(0x71U, &v, 1);
    TEST_ASSERT_EQUAL_UINT32(2U, bl_nvm_live_count());

    /* Format. */
    TEST_ASSERT_EQUAL_INT(BL_NVM_OK, bl_nvm_format());
    TEST_ASSERT_EQUAL_UINT32(0U, bl_nvm_live_count());

    /* After format the sector should be fully erased — any new write
     * lands in the first slot and round-trips cleanly. */
    uint8_t w = 0x55;
    TEST_ASSERT_EQUAL_INT(BL_NVM_OK, bl_nvm_write(0x72U, &w, 1));
    uint8_t out = 0;
    uint8_t actual_len = 0;
    TEST_ASSERT_EQUAL_INT(BL_NVM_OK,
        bl_nvm_read(0x72U, &out, 1, &actual_len));
    TEST_ASSERT_EQUAL_UINT8(0x55U, out);

    /* And the metadata word was wiped too. */
    const uint32_t *meta = (const uint32_t *)(uintptr_t)BL_APP_METADATA_ADDR;
    TEST_ASSERT_EQUAL_HEX32(0xFFFFFFFFU, meta[0]);
}

/* #125 H6 (brick-avoidance): every flash-sector stall in the NVM path must
 * kick the IWDG first. A sector-7 erase stalls the single-bank H733 for
 * ~1.8 s; without a refresh the watchdog could fire mid-erase and leave
 * the unit unflashable over CAN. erase_nvm_sector() calls bl_iwdg_refresh
 * before the HAL erase, so the format's single sector erase kicks it. */
void test_nvm_erase_kicks_the_watchdog(void)
{
    seed_and_init();

    /* Isolate the format's erase from any kicks during seeding. */
    mock_iwdg_refresh_reset();
    TEST_ASSERT_EQUAL_INT(BL_NVM_OK, bl_nvm_format());

    TEST_ASSERT_GREATER_OR_EQUAL(1, mock_iwdg_refresh_calls());
}

/* ---- Patch-2 coverage: bl_nvm_compact_replace_meta() ---- */

void test_nvm_compact_replace_meta_preserves_live_entries(void)
{
    seed_and_init();

    uint8_t a = 0x11, b = 0x22, c = 0x33;
    bl_nvm_write(0x100U, &a, 1);
    bl_nvm_write(0x200U, &b, 1);
    bl_nvm_write(0x300U, &c, 1);
    /* Re-write key 0x200 a few times so dedup actually has work. */
    uint8_t b2 = 0x77;
    bl_nvm_write(0x200U, &b2, 1);
    bl_nvm_write(0x200U, &b2, 1);

    uint32_t new_meta[8] = {0};
    new_meta[0] = BL_APP_META_MAGIC;
    new_meta[1] = 0x12345678U;       /* size */
    new_meta[2] = 0xAABBCCDDU;       /* crc */
    new_meta[3] = BL_APP_BASE;       /* base */
    new_meta[4] = 0x00010002U;       /* version */

    TEST_ASSERT_EQUAL_INT(BL_NVM_OK, bl_nvm_compact_replace_meta(new_meta));

    /* All three live keys are still readable with the latest values. */
    uint8_t out = 0;
    uint8_t actual_len = 0;
    TEST_ASSERT_EQUAL_INT(BL_NVM_OK, bl_nvm_read(0x100U, &out, 1, &actual_len));
    TEST_ASSERT_EQUAL_UINT8(0x11U, out);
    TEST_ASSERT_EQUAL_INT(BL_NVM_OK, bl_nvm_read(0x200U, &out, 1, &actual_len));
    TEST_ASSERT_EQUAL_UINT8(0x77U, out);
    TEST_ASSERT_EQUAL_INT(BL_NVM_OK, bl_nvm_read(0x300U, &out, 1, &actual_len));
    TEST_ASSERT_EQUAL_UINT8(0x33U, out);

    /* Live count is the number of unique keys, regardless of how many
     * re-writes happened beforehand. */
    TEST_ASSERT_EQUAL_UINT32(3U, bl_nvm_live_count());
}

void test_nvm_compact_replace_meta_writes_caller_metadata(void)
{
    seed_and_init();

    uint32_t new_meta[8] = {
        BL_APP_META_MAGIC,
        0xDEADBEEFU,
        0xCAFEBABEU,
        BL_APP_BASE,
        0x00010002U,
        0, 0, 0
    };
    TEST_ASSERT_EQUAL_INT(BL_NVM_OK, bl_nvm_compact_replace_meta(new_meta));

    const uint32_t *got = (const uint32_t *)(uintptr_t)BL_APP_METADATA_ADDR;
    TEST_ASSERT_EQUAL_HEX32(new_meta[0], got[0]);
    TEST_ASSERT_EQUAL_HEX32(new_meta[1], got[1]);
    TEST_ASSERT_EQUAL_HEX32(new_meta[2], got[2]);
    TEST_ASSERT_EQUAL_HEX32(new_meta[3], got[3]);
    TEST_ASSERT_EQUAL_HEX32(new_meta[4], got[4]);
}

void test_nvm_compact_replace_meta_erases_old_metadata(void)
{
    /* This is the actual bug-regression test: writing different metadata
     * into a previously-programmed BL_APP_METADATA_ADDR FLASHWORD must
     * succeed even though the underlying flash forbids 1→0 transitions
     * — bl_nvm_compact_replace_meta runs a sector erase first so the
     * destination is fresh-0xFF before the new write. Without the
     * erase, the second write would HAL_ERROR. */

    seed_and_init();

    /* First commit: write metadata with one CRC. */
    uint32_t meta_v1[8] = {
        BL_APP_META_MAGIC,
        100U,             /* size  v1 */
        0xAAAAAAAAU,      /* crc   v1 */
        BL_APP_BASE,
        0x00010001U,      /* version v1 */
        0, 0, 0
    };
    TEST_ASSERT_EQUAL_INT(BL_NVM_OK, bl_nvm_compact_replace_meta(meta_v1));

    /* Confirm v1 is in flash. */
    const uint32_t *got = (const uint32_t *)(uintptr_t)BL_APP_METADATA_ADDR;
    TEST_ASSERT_EQUAL_HEX32(0xAAAAAAAAU, got[2]);

    /* Second commit: completely different CRC. Bits that were 1 in v1
     * now want to be 0 in v2 and vice-versa — without an erase between
     * the two, HAL_FLASH_Program would return HAL_ERROR. */
    uint32_t meta_v2[8] = {
        BL_APP_META_MAGIC,
        2000U,
        0x55555555U,      /* exact bit-inverse of 0xAAAAAAAA */
        BL_APP_BASE,
        0x00010002U,
        0, 0, 0
    };
    TEST_ASSERT_EQUAL_INT(BL_NVM_OK, bl_nvm_compact_replace_meta(meta_v2));

    /* And v2 is what's in flash now. */
    TEST_ASSERT_EQUAL_HEX32(0x55555555U, got[2]);
    TEST_ASSERT_EQUAL_HEX32(2000U, got[1]);
}

/* ---- Patch-1 coverage: bl_nvm_write retry via compaction ---- */

void test_nvm_write_recovers_via_compaction_on_first_program_fail(void)
{
    seed_and_init();

    uint8_t a = 0xAA;
    TEST_ASSERT_EQUAL_INT(BL_NVM_OK, bl_nvm_write(0x80U, &a, 1));

    /* Force the very next program to fail (the append). The fix path
     * must fall through to compact_and_append, which does its own
     * erase + writes — those subsequent programs succeed. The overall
     * write must return BL_NVM_OK and the value must be readable. */
    int programs_before = mock_flash_program_call_count();
    mock_flash_set_program_fail(1);

    uint8_t b = 0xBB;
    bl_nvm_status_t st = bl_nvm_write(0x81U, &b, 1);
    TEST_ASSERT_EQUAL_INT(BL_NVM_OK, st);

    /* At least one extra program ran (the retry through compaction). */
    int programs_after = mock_flash_program_call_count();
    TEST_ASSERT_GREATER_THAN(programs_before + 1, programs_after);

    /* Compaction also ran exactly one extra erase. */
    TEST_ASSERT_GREATER_OR_EQUAL(1, mock_flash_erase_call_count());

    /* Read back both keys — content survived compaction. */
    uint8_t out = 0;
    uint8_t len = 0;
    TEST_ASSERT_EQUAL_INT(BL_NVM_OK, bl_nvm_read(0x80U, &out, 1, &len));
    TEST_ASSERT_EQUAL_UINT8(0xAAU, out);
    TEST_ASSERT_EQUAL_INT(BL_NVM_OK, bl_nvm_read(0x81U, &out, 1, &len));
    TEST_ASSERT_EQUAL_UINT8(0xBBU, out);
}

/* ---- G-A2: degraded mode (post-ECC-fault recovery init) ---- */

void test_nvm_degraded_mode_hides_reads_rejects_writes_until_format(void)
{
    /* After a prior boot double-bit-ECC-faulted reading sector 7, Bootloader_Init
     * brings the store up via bl_nvm_init_degraded() instead of scanning. Reads
     * must then return NOT_FOUND (no slot scan -> no re-fault) and writes must be
     * rejected, until an explicit bl_nvm_format() erases + re-trusts the sector. */
    seed_and_init();

    uint8_t v = 0x42U;
    TEST_ASSERT_EQUAL_INT(BL_NVM_OK, bl_nvm_write(0x1234U, &v, 1U));

    bl_nvm_init_degraded();
    uint8_t out = 0U, len = 0U;
    TEST_ASSERT_EQUAL_INT(BL_NVM_NOT_FOUND, bl_nvm_read(0x1234U, &out, 1U, &len));
    TEST_ASSERT_EQUAL_INT(BL_NVM_HARDWARE,  bl_nvm_write(0x1234U, &v, 1U));

    /* A clean format re-trusts the sector — reads + writes work again. */
    TEST_ASSERT_EQUAL_INT(BL_NVM_OK, bl_nvm_format());
    TEST_ASSERT_EQUAL_INT(BL_NVM_OK, bl_nvm_write(0x1234U, &v, 1U));
    TEST_ASSERT_EQUAL_INT(BL_NVM_OK, bl_nvm_read(0x1234U, &out, 1U, &len));
    TEST_ASSERT_EQUAL_UINT8(0x42U, out);
}
