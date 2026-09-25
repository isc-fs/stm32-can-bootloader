#ifndef BL_PROVISION_H
#define BL_PROVISION_H

/*
 * bl_provision — one-shot SWD provisioning seed (#183).
 *
 * An SWD tool programs a single 32-byte FLASHWORD at BL_PROVISION_SEED_ADDR
 * during the bootloader burn, carrying the board's node ID. On the next boot
 * the bootloader validates it and, if no node-id is stored in NVM yet,
 * translates it into a proper BL_NVM_KEY_NODE_ID entry via bl_nvm_write — so a
 * bare board is commissioned in the same SWD step, with no CAN round-trip and
 * no boot required first.
 *
 * The host writes only this fixed struct; it never needs to know the
 * log-structured KV record format (private to bl_nvm, and evolving). The seed
 * is the stable host<->BL provisioning contract.
 *
 * One-shot + idempotent: once a node-id is in NVM (from this seed, a later
 * `cf provision` over CAN, or anything else) the seed is ignored. The stale
 * seed word is wiped on the next sector-7 compaction/erase.
 *
 * SAFETY: the seed read runs pre-CAN. The caller MUST invoke
 * bl_provision_consume_seed() inside the bl_appcheck ECC guard — the same
 * window that wraps bl_nvm_init + bl_node_id_init in Bootloader_Init — so a
 * corrupt / half-programmed seed word that double-bit-ECC-faults leaves a
 * recovery breadcrumb instead of reboot-looping the chip unreachable
 * (#166 / G-A2). Do NOT call it on the degraded-recovery path.
 */

#include <stdint.h>

#include "bl_memmap.h"

/* On-flash seed record — exactly BL_PROVISION_SEED_SIZE (one FLASHWORD).
 * Host-writable; the BL only ever reads it. `crc32` covers the first 8 bytes
 * (everything before the crc field), little-endian, IEEE-802.3. */
typedef struct {
    uint32_t magic;          /* 0   BL_PROVISION_SEED_MAGIC (LE)             */
    uint8_t  node_id;        /* 4   0x1..0xE                                 */
    uint8_t  node_id_check;  /* 5   ~node_id  (node_id ^ node_id_check==0xFF)*/
    uint16_t reserved;       /* 6   0xFFFF                                   */
    uint32_t crc32;          /* 8   CRC32 over bytes [0..8)                  */
    uint8_t  padding[20];    /* 12  0xFF                                     */
} bl_provision_seed_t;

_Static_assert(sizeof(bl_provision_seed_t) == BL_PROVISION_SEED_SIZE,
               "bl_provision_seed_t must be exactly one FLASHWORD");

/* Outcome of a consume attempt — informational; the boot path does not branch
 * on it (a missing/invalid seed is the normal case). */
typedef enum {
    BL_PROVISION_NONE     = 0,  /* no valid seed present (erased / malformed)  */
    BL_PROVISION_APPLIED  = 1,  /* seed valid -> node-id written to NVM         */
    BL_PROVISION_SKIPPED  = 2,  /* seed valid but NVM already had a node-id     */
    BL_PROVISION_NVM_FAIL = 3,  /* seed valid but the NVM write failed          */
} bl_provision_result_t;

/* Read + validate the seed FLASHWORD; if it is valid AND no node-id is stored
 * in NVM yet, write the node-id into NVM (key BL_NVM_KEY_NODE_ID). Safe to call
 * every boot. MUST run inside the bl_appcheck ECC guard, after bl_nvm_init and
 * before bl_node_id_init_from_nvm, on the NORMAL boot path only. */
bl_provision_result_t bl_provision_consume_seed(void);

#endif /* BL_PROVISION_H */
