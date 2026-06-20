/*
 * bl_provision — consume the one-shot SWD provisioning seed (#183).
 *
 * See bl_provision.h for the contract and the ECC-guard safety requirement.
 */

#include "bl_provision.h"

#include "bl_nvm.h"
#include "bl_proto.h"   /* BL_PROTO_NODE_BROADCAST */

#include <string.h>

/* IEEE-802.3 reflected CRC32 (poly 0xEDB88320, init/xor-out 0xFFFFFFFF) over a
 * RAM buffer. Same algorithm as bl_flash_crc32, reimplemented here so the seed
 * check stays self-contained and host-testable — bl_flash is a linker stub in
 * the unit suite, so reusing bl_flash_crc32 would make the CRC-reject path
 * untestable. */
static uint32_t crc32_buf(const uint8_t *data, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFU;
    for (uint32_t i = 0U; i < len; i++) {
        crc ^= data[i];
        for (uint32_t b = 0U; b < 8U; b++) {
            if ((crc & 1U) != 0U) {
                crc = (crc >> 1) ^ 0xEDB88320U;
            } else {
                crc >>= 1;
            }
        }
    }
    return ~crc;
}

bl_provision_result_t bl_provision_consume_seed(void)
{
    /* Read the seed FLASHWORD into RAM. THIS is the access that can ECC-fault
     * on a half-programmed seed; the caller runs us inside the bl_appcheck
     * guard so such a fault recovers (breadcrumb -> next boot skips sector 7)
     * instead of reboot-looping unreachable. */
    bl_provision_seed_t seed;
    (void)memcpy(&seed, (const void *)BL_PROVISION_SEED_ADDR, sizeof(seed));

    /* Magic gate — an erased word (all 0xFF) or any non-seed content bails
     * fast, before the more expensive checks. */
    if (seed.magic != BL_PROVISION_SEED_MAGIC) {
        return BL_PROVISION_NONE;
    }

    /* Integrity: the check byte is the bitwise complement of the node id. */
    if ((uint8_t)(seed.node_id ^ seed.node_id_check) != 0xFFU) {
        return BL_PROVISION_NONE;
    }

    /* Range: 0x1..0xE only — 0x0 is the host's reserved id, 0xF is broadcast;
     * either as a per-board id would corrupt the bus. Matches the NVM-override
     * validation in bl_node_id_init_from_nvm. */
    if ((seed.node_id == 0U) || (seed.node_id >= BL_PROTO_NODE_BROADCAST)) {
        return BL_PROVISION_NONE;
    }

    /* CRC32 over bytes [0..8) — magic + node_id + node_id_check + reserved. */
    if (crc32_buf((const uint8_t *)&seed, 8U) != seed.crc32) {
        return BL_PROVISION_NONE;
    }

    /* One-shot: if a node-id is already in NVM (this seed already consumed on a
     * prior boot, or a later `cf provision` over CAN), the seed is stale — leave
     * it; the next sector-7 compaction wipes it. NVM always wins over the seed. */
    uint8_t cur = 0U;
    uint8_t cur_len = 0U;
    if (bl_nvm_read(BL_NVM_KEY_NODE_ID, &cur, sizeof(cur), &cur_len) == BL_NVM_OK) {
        return BL_PROVISION_SKIPPED;
    }

    /* Translate the seed into a proper NVM node-id entry. bl_node_id_init_from_nvm
     * (called next, still under the guard) then reads it back as the live id. */
    if (bl_nvm_write(BL_NVM_KEY_NODE_ID, &seed.node_id, 1U) != BL_NVM_OK) {
        return BL_PROVISION_NVM_FAIL;
    }
    return BL_PROVISION_APPLIED;
}
