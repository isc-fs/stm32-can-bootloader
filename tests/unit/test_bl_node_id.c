/*
 * test_bl_node_id.c — NVM-backed override accessor for the bootloader
 * node ID.
 *
 * The bootloader's node identity has two sources: a compile-time
 * `BL_NODE_ID` (the build-time default — host stub CMake doesn't
 * `-D`-set it, so it falls through to `0x1U`) and an NVM-backed
 * override under key `BL_NVM_KEY_NODE_ID = 0x0001`. The accessor
 * `bl_node_id_get()` returns the NVM value when present and valid,
 * else the compile-time default.
 *
 * These tests exhaustively cover the validation rules so an operator
 * provisioning a board via `can-flasher config nvm write 0x0001 …`
 * can never accidentally brick a node by writing nonsense.
 *
 * Validation rules under test:
 *   - empty NVM                → fall back
 *   - valid byte (0x1..0xE)    → override
 *   - 0x00 (host ID reserved)  → fall back
 *   - 0x0F (broadcast pseudo)  → fall back
 *   - 0x10..0xFF (out-of-range)→ fall back
 *   - wrong length (≠ 1 byte)  → fall back
 *
 * Plus two flow tests:
 *   - init_from_nvm is idempotent (same NVM → same cached value
 *     across back-to-back calls)
 *   - re-init picks up NVM changes (so the operator's "write then
 *     reboot" mental model holds)
 */

#include "bl_config.h"   /* BL_NODE_ID default                            */
#include "bl_node_id.h"
#include "bl_nvm.h"
#include "stm32h7xx_hal.h"

#include "unity.h"

#include <stdint.h>

/* Convenience: write a single byte to BL_NVM_KEY_NODE_ID with the
 * real NVM API. Going through bl_nvm_write here (rather than poking
 * the fake-flash buffer directly) is deliberate — it exercises the
 * same path the host's CMD_NVM_WRITE would take, so the test
 * coverage matches the operator's real provisioning workflow. */
static void nvm_seed_node_id(uint8_t value)
{
    bl_nvm_init();
    bl_nvm_status_t st = bl_nvm_write(BL_NVM_KEY_NODE_ID, &value, 1U);
    TEST_ASSERT_EQUAL_INT(BL_NVM_OK, st);
}

/* Seed a multi-byte value under the node-id key. Used to verify that
 * a future caller who misuses the key (writes 2 bytes instead of 1)
 * gets the fall-back rather than a randomly-truncated override. */
static void nvm_seed_node_id_with_length(const uint8_t *bytes, uint8_t len)
{
    bl_nvm_init();
    bl_nvm_status_t st = bl_nvm_write(BL_NVM_KEY_NODE_ID, bytes, len);
    TEST_ASSERT_EQUAL_INT(BL_NVM_OK, st);
}

/* ---- fallback path ---- */

void test_node_id_falls_back_to_compile_time_on_empty_nvm(void)
{
    /* setUp() cleared the fake flash; bl_nvm_init() will scan zero
     * entries. bl_node_id_init_from_nvm() should find nothing and
     * keep the compile-time default. */
    bl_nvm_init();
    bl_node_id_init_from_nvm();
    TEST_ASSERT_EQUAL_UINT8(BL_NODE_ID, bl_node_id_get());
}

void test_node_id_falls_back_on_zero_host_value(void)
{
    /* 0x00 is the host's reserved ID. A node claiming it would
     * collide with the host on the wire — must be rejected. */
    nvm_seed_node_id(0x00U);
    bl_node_id_init_from_nvm();
    TEST_ASSERT_EQUAL_UINT8(BL_NODE_ID, bl_node_id_get());
}

void test_node_id_falls_back_on_broadcast_value(void)
{
    /* 0x0F is the broadcast pseudo-node. A node responding from it
     * would race every reply on the bus — must be rejected. */
    nvm_seed_node_id(0x0FU);
    bl_node_id_init_from_nvm();
    TEST_ASSERT_EQUAL_UINT8(BL_NODE_ID, bl_node_id_get());
}

void test_node_id_falls_back_on_out_of_range_value(void)
{
    /* Anything ≥ 0x10 has bits set that the 4-bit node field can't
     * carry — reject so the FDCAN filter masking doesn't quietly
     * truncate to an unrelated low nibble. */
    nvm_seed_node_id(0x80U);
    bl_node_id_init_from_nvm();
    TEST_ASSERT_EQUAL_UINT8(BL_NODE_ID, bl_node_id_get());
}

void test_node_id_falls_back_on_multi_byte_value(void)
{
    /* The override is a 1-byte key by contract. If a caller writes
     * 2 bytes the accessor must NOT silently use byte 0 — there's
     * no way to know which byte they meant. Fall back instead. */
    const uint8_t two_bytes[2] = { 0x05U, 0x07U };
    nvm_seed_node_id_with_length(two_bytes, 2U);
    bl_node_id_init_from_nvm();
    TEST_ASSERT_EQUAL_UINT8(BL_NODE_ID, bl_node_id_get());
}

/* ---- override path ---- */

void test_node_id_uses_valid_nvm_override(void)
{
    /* 0x05 is a plain unicast ID. Should appear from
     * bl_node_id_get() after init. */
    nvm_seed_node_id(0x05U);
    bl_node_id_init_from_nvm();
    TEST_ASSERT_EQUAL_UINT8(0x05U, bl_node_id_get());
}

void test_node_id_accepts_min_unicast(void)
{
    /* Lower boundary of the valid range. */
    nvm_seed_node_id(0x01U);
    bl_node_id_init_from_nvm();
    TEST_ASSERT_EQUAL_UINT8(0x01U, bl_node_id_get());
}

void test_node_id_accepts_max_unicast(void)
{
    /* Upper boundary — 0x0E is the last legal unicast ID before
     * the 0x0F broadcast pseudo-node. */
    nvm_seed_node_id(0x0EU);
    bl_node_id_init_from_nvm();
    TEST_ASSERT_EQUAL_UINT8(0x0EU, bl_node_id_get());
}

/* ---- flow / idempotence ---- */

void test_node_id_init_is_idempotent(void)
{
    /* Back-to-back init calls against the same NVM state must yield
     * the same cached value. Guards against the accessor accumulating
     * state across calls (which would make per-test setUp ordering
     * matter — bad). */
    nvm_seed_node_id(0x07U);
    bl_node_id_init_from_nvm();
    bl_node_id_init_from_nvm();
    bl_node_id_init_from_nvm();
    TEST_ASSERT_EQUAL_UINT8(0x07U, bl_node_id_get());
}

void test_node_id_reinit_picks_up_nvm_changes(void)
{
    /* Models the operator workflow: provision → reboot → BL re-reads
     * NVM and the new ID takes effect. Here we simulate "reboot" by
     * calling init_from_nvm() a second time after rewriting the key. */
    nvm_seed_node_id(0x03U);
    bl_node_id_init_from_nvm();
    TEST_ASSERT_EQUAL_UINT8(0x03U, bl_node_id_get());

    /* Operator writes a new value (same key, latest-seq wins). */
    bl_nvm_status_t st;
    uint8_t new_value = 0x09U;
    st = bl_nvm_write(BL_NVM_KEY_NODE_ID, &new_value, 1U);
    TEST_ASSERT_EQUAL_INT(BL_NVM_OK, st);

    /* Simulated reboot — re-resolve. */
    bl_node_id_init_from_nvm();
    TEST_ASSERT_EQUAL_UINT8(0x09U, bl_node_id_get());
}

void test_node_id_reinit_after_tombstone_restores_default(void)
{
    /* If the operator clears the override (writes a zero-length
     * tombstone via CMD_NVM_WRITE with no value), the next reboot
     * must return to the compile-time default — not stay stuck on
     * the previously-overridden value. */
    nvm_seed_node_id(0x0AU);
    bl_node_id_init_from_nvm();
    TEST_ASSERT_EQUAL_UINT8(0x0AU, bl_node_id_get());

    /* Tombstone the key (len=0 is the bl_nvm convention for delete). */
    bl_nvm_status_t st = bl_nvm_write(BL_NVM_KEY_NODE_ID, NULL, 0U);
    TEST_ASSERT_EQUAL_INT(BL_NVM_OK, st);

    bl_node_id_init_from_nvm();
    TEST_ASSERT_EQUAL_UINT8(BL_NODE_ID, bl_node_id_get());
}
