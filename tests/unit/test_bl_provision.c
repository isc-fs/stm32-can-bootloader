/*
 * test_bl_provision.c — the #183 one-shot SWD provisioning seed.
 *
 * Plants a seed FLASHWORD directly in g_fake_flash (simulating the SWD tool's
 * external write) and checks bl_provision_consume_seed() translates a VALID
 * seed into an NVM node-id entry, rejects every malformed seed, and never
 * overrides a node-id already present in NVM (NVM always wins; one-shot).
 */

#include "bl_provision.h"
#include "bl_nvm.h"
#include "bl_node_id.h"
#include "bl_memmap.h"
#include "stm32h7xx_hal.h"
#include "unity.h"

#include <stdint.h>
#include <string.h>

/* Same IEEE-802.3 reflected CRC32 the firmware uses — lets the test compute a
 * valid seed CRC (and a deliberately-wrong one). */
static uint32_t test_crc32(const uint8_t *data, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFU;
    for (uint32_t i = 0U; i < len; i++) {
        crc ^= data[i];
        for (uint32_t b = 0U; b < 8U; b++) {
            crc = ((crc & 1U) != 0U) ? ((crc >> 1) ^ 0xEDB88320U) : (crc >> 1);
        }
    }
    return ~crc;
}

/* Build a seed in RAM with correct magic + check + crc for `node_id`. */
static bl_provision_seed_t make_valid_seed(uint8_t node_id)
{
    bl_provision_seed_t s;
    memset(&s, 0xFF, sizeof(s));
    s.magic = BL_PROVISION_SEED_MAGIC;
    s.node_id = node_id;
    s.node_id_check = (uint8_t)~node_id;
    s.reserved = 0xFFFFU;
    s.crc32 = test_crc32((const uint8_t *)&s, 8U);
    return s;
}

/* Write a seed into the fake-flash seed slot (simulates the SWD-side write). */
static void plant_seed(const bl_provision_seed_t *s)
{
    (void)memcpy((void *)BL_PROVISION_SEED_ADDR, s, sizeof(*s));
}

/* Node-id currently stored in NVM, or 0 if none. */
static uint8_t nvm_node_id_or_zero(void)
{
    uint8_t v = 0U;
    uint8_t len = 0U;
    if (bl_nvm_read(BL_NVM_KEY_NODE_ID, &v, sizeof(v), &len) == BL_NVM_OK && len == 1U) {
        return v;
    }
    return 0U;
}

/* ---- valid seed ---- */

void test_provision_valid_seed_writes_node_id(void)
{
    bl_nvm_init();                                  /* erased sector, no node-id */
    bl_provision_seed_t s = make_valid_seed(0x2U);
    plant_seed(&s);

    TEST_ASSERT_EQUAL_INT(BL_PROVISION_APPLIED, bl_provision_consume_seed());
    TEST_ASSERT_EQUAL_UINT8(0x2U, nvm_node_id_or_zero());
}

void test_provision_seeded_id_resolves_via_node_id(void)
{
    /* End-to-end: seed -> consume -> bl_node_id_init_from_nvm -> get(). */
    bl_nvm_init();
    bl_provision_seed_t s = make_valid_seed(0x7U);
    plant_seed(&s);

    (void)bl_provision_consume_seed();
    bl_node_id_init_from_nvm();
    TEST_ASSERT_EQUAL_UINT8(0x7U, bl_node_id_get());
}

/* ---- no / malformed seed: every reject path returns NONE, writes nothing ---- */

void test_provision_erased_flash_is_none(void)
{
    bl_nvm_init();                                  /* seed slot is 0xFF (erased) */
    TEST_ASSERT_EQUAL_INT(BL_PROVISION_NONE, bl_provision_consume_seed());
    TEST_ASSERT_EQUAL_UINT8(0U, nvm_node_id_or_zero());
}

void test_provision_bad_magic_rejected(void)
{
    bl_nvm_init();
    bl_provision_seed_t s = make_valid_seed(0x3U);
    s.magic ^= 0x1U;                                /* corrupt magic */
    plant_seed(&s);
    TEST_ASSERT_EQUAL_INT(BL_PROVISION_NONE, bl_provision_consume_seed());
    TEST_ASSERT_EQUAL_UINT8(0U, nvm_node_id_or_zero());
}

void test_provision_bad_check_byte_rejected(void)
{
    bl_nvm_init();
    bl_provision_seed_t s = make_valid_seed(0x3U);
    s.node_id_check ^= 0x1U;                         /* no longer ~node_id ... */
    s.crc32 = test_crc32((const uint8_t *)&s, 8U);   /* ...keep crc valid so CHECK is the reject */
    plant_seed(&s);
    TEST_ASSERT_EQUAL_INT(BL_PROVISION_NONE, bl_provision_consume_seed());
}

void test_provision_node_id_zero_rejected(void)
{
    bl_nvm_init();
    bl_provision_seed_t s = make_valid_seed(0x0U);   /* 0 is the host's reserved id */
    plant_seed(&s);
    TEST_ASSERT_EQUAL_INT(BL_PROVISION_NONE, bl_provision_consume_seed());
}

void test_provision_node_id_broadcast_rejected(void)
{
    bl_nvm_init();
    bl_provision_seed_t s = make_valid_seed(0xFU);   /* 0xF is broadcast */
    plant_seed(&s);
    TEST_ASSERT_EQUAL_INT(BL_PROVISION_NONE, bl_provision_consume_seed());
}

void test_provision_bad_crc_rejected(void)
{
    bl_nvm_init();
    bl_provision_seed_t s = make_valid_seed(0x4U);
    s.crc32 ^= 0xDEADBEEFU;                          /* magic+check valid, crc wrong */
    plant_seed(&s);
    TEST_ASSERT_EQUAL_INT(BL_PROVISION_NONE, bl_provision_consume_seed());
}

/* ---- NVM wins / one-shot ---- */

void test_provision_skipped_when_nvm_already_has_node_id(void)
{
    bl_nvm_init();
    uint8_t existing = 0x5U;
    bl_nvm_write(BL_NVM_KEY_NODE_ID, &existing, 1U);     /* already provisioned over CAN */

    bl_provision_seed_t s = make_valid_seed(0x2U);
    plant_seed(&s);

    TEST_ASSERT_EQUAL_INT(BL_PROVISION_SKIPPED, bl_provision_consume_seed());
    TEST_ASSERT_EQUAL_UINT8(0x5U, nvm_node_id_or_zero());   /* NVM unchanged — NVM wins */
}

void test_provision_is_one_shot_idempotent(void)
{
    bl_nvm_init();
    bl_provision_seed_t s = make_valid_seed(0x6U);
    plant_seed(&s);

    TEST_ASSERT_EQUAL_INT(BL_PROVISION_APPLIED, bl_provision_consume_seed());
    /* Second boot: seed still in flash, but NVM now holds the id -> ignored. */
    TEST_ASSERT_EQUAL_INT(BL_PROVISION_SKIPPED, bl_provision_consume_seed());
    TEST_ASSERT_EQUAL_UINT8(0x6U, nvm_node_id_or_zero());
}
