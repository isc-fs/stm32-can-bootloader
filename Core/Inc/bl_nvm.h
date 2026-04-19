#ifndef BL_NVM_H
#define BL_NVM_H

/*
 * bl_nvm — log-structured key-value store in flash sector 7.
 *
 * Storage layout:
 *
 *     0x080E0000 .. 0x080FFFDF     (BL_NVM_SIZE bytes = 128 KB − 32 B)
 *         bl_nvm_entry_t[] — 32-byte slots, packed back-to-back.
 *         Writable one slot at a time (one FLASHWORD per slot).
 *
 *     0x080FFFE0 .. 0x080FFFFF     (BL_APP_METADATA_SIZE = 32 B)
 *         App metadata FLASHWORD. Owned by bl_flash_write_metadata,
 *         but NVM compaction must save + restore it across its sector
 *         erase (they share sector 7).
 *
 * Writes are append-only: every new value lands in the next erased
 * slot, tagged with a monotonically-increasing `seq`. Reads are a
 * linear scan of the sector looking for the highest `seq` for a
 * given key. When the sector fills, compaction gathers the latest
 * live entries into RAM, erases sector 7, rewrites them at the head,
 * and restores the metadata word.
 *
 * The sector holds up to BL_NVM_SIZE / BL_NVM_ENTRY_SIZE = 4095
 * entries before needing compaction. Unique live keys in any given
 * compaction are capped at BL_NVM_MAX_LIVE_ENTRIES; beyond that,
 * writes fail with BL_NVM_FULL.
 *
 * Value size: up to BL_NVM_MAX_VALUE_LEN bytes per entry. Larger
 * values must wait for a multi-entry extension (not in this branch).
 *
 * Power-loss behaviour: a power loss during compaction can leave
 * the sector erased but not yet rewritten. The app metadata word
 * is the last thing compaction programs, so losing power mid-
 * compaction typically drops NVM but preserves the path through
 * bootloader → valid-app check → auto-jump for subsequent boots.
 * Acceptable for Phase 4; a two-sector ping-pong scheme could be
 * added later if stronger guarantees matter.
 */

#include <stdbool.h>
#include <stdint.h>

#include "bl_memmap.h"

/* ---- Entry geometry ---- */
#define BL_NVM_ENTRY_SIZE        32U                  /* one FLASHWORD */
#define BL_NVM_ENTRY_MAGIC       0xABCDU
#define BL_NVM_MAX_VALUE_LEN     20U

/* Max unique live keys a single compaction can handle. Bumping this
 * widens the temporary stack-local buffer in bl_nvm_write during
 * compaction (128 × 32 B = 4 KB). Increase if you actually need more
 * distinct keys; most deployments won't. */
#define BL_NVM_MAX_LIVE_ENTRIES  128U

/* ---- Well-known keys (bootloader use) ----
 * User / app keys should start at 0x1000 to stay clear of the
 * bootloader's reserved range. */
#define BL_NVM_KEY_NODE_ID        0x0001U   /* 1-byte override for compile-time BL_NODE_ID */
#define BL_NVM_KEY_CAN_BITRATE    0x0002U   /* future — CAN bitrate preference             */

/* ---- Status codes ---- */
typedef enum {
    BL_NVM_OK        = 0,
    BL_NVM_NOT_FOUND = 1,   /* no live entry for this key                    */
    BL_NVM_FULL      = 2,   /* sector full and compaction also can't fit it  */
    BL_NVM_HARDWARE  = 3,   /* HAL_FLASH_Program / HAL_FLASHEx_Erase failed  */
    BL_NVM_BAD_ARG   = 4,   /* value length > BL_NVM_MAX_VALUE_LEN           */
} bl_nvm_status_t;

/* ---- Entry layout (must be exactly BL_NVM_ENTRY_SIZE = 32 bytes,
 *      naturally aligned) ---- */
typedef struct {
    uint16_t magic;                           /* BL_NVM_ENTRY_MAGIC (live) or else (stale / erased) */
    uint16_t key;                             /* vendor identifier                                    */
    uint8_t  len;                             /* 0..BL_NVM_MAX_VALUE_LEN; 0 = tombstone              */
    uint8_t  reserved_a;
    uint16_t reserved_b;                      /* zero-pad for alignment                               */
    uint32_t seq;                             /* monotonic; highest-seq wins per key                 */
    uint8_t  value[BL_NVM_MAX_VALUE_LEN];     /* up to 20 bytes                                       */
} bl_nvm_entry_t;

_Static_assert(sizeof(bl_nvm_entry_t) == BL_NVM_ENTRY_SIZE,
               "bl_nvm_entry_t must be exactly 32 bytes");


/* ---- API ---- */

/* Scan sector 7, find the highest seq and the first empty slot.
 * Runs after bl_dtc_init / bl_log_init in Bootloader_Init. */
void bl_nvm_init(void);

/* Read the current value for `key` into `out`. On success `*actual_len`
 * receives the value length. BL_NVM_NOT_FOUND is returned when the key
 * has no live entry (never written, all writes tombstoned, or the
 * value is larger than `max_len`). */
bl_nvm_status_t bl_nvm_read(uint16_t key,
                            void *out,
                            uint8_t max_len,
                            uint8_t *actual_len);

/* Append a write for `key` with `len` bytes from `value`. `len == 0`
 * is a tombstone: subsequent reads for `key` return NOT_FOUND until
 * a non-zero-len write lands again. `len > BL_NVM_MAX_VALUE_LEN`
 * returns BAD_ARG; sector full even after compaction returns FULL;
 * underlying flash errors return HARDWARE. */
bl_nvm_status_t bl_nvm_write(uint16_t key,
                             const void *value,
                             uint8_t len);

/* Number of live (non-tombstoned) unique keys currently in the store.
 * Linear scan; not a hot path. */
uint32_t bl_nvm_live_count(void);

#endif /* BL_NVM_H */
