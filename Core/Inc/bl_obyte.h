#ifndef BL_OBYTE_H
#define BL_OBYTE_H

/*
 * bl_obyte — option-byte read + WRP apply.
 *
 * Thin wrapper around HAL_FLASHEx_OBGetConfig / HAL_FLASHEx_OBProgram
 * that exposes just enough for the bootloader's Phase 4 needs:
 *
 *   - Read current WRP mask, RDP level, BOR level and user-config
 *     word, serialised into a fixed 16-byte record.
 *   - Test whether a given sector is currently WRP-protected, for the
 *     boot-time self-check + health/live-data flags byte.
 *   - Apply WRP to a caller-chosen sector bitmap. Calls HAL_FLASH_OB_Launch
 *     which triggers an MCU reset to latch the new OBs — the function
 *     does not return on success.
 *
 * Apply is gated behind a 4-byte confirmation token at the protocol
 * layer (see BL_OB_APPLY_TOKEN below) so a stray CMD_OB_APPLY_WRP
 * during development can't accidentally brick the part.
 *
 * WRP encoding note (H7 HAL convention): the returned `wrp_sector_mask`
 * has bit N = 1 when sector N is write-protected. The HAL inverts the
 * underlying FLASH_WPSN_CUR1R register (where a clear bit means
 * protected) before handing back the mask, so the higher-level "bit
 * set = protected" reading is stable across phases.
 */

#include <stdbool.h>
#include <stdint.h>

/* ---- Confirmation token for OB_APPLY_WRP (ASCII "WRP\0", LE) ---- */
#define BL_OB_APPLY_TOKEN         0x00505257U

/* ---- Serialised status record (16 bytes) ---- */
#define BL_OB_STATUS_SIZE         16U

typedef struct {
    uint32_t wrp_sector_mask;     /* offset 0  — bit N set = sector N WRP-protected */
    uint32_t user_config;         /* offset 4  — raw H7 user-config word             */
    uint8_t  rdp_level;           /* offset 8  — raw OB_RDP_LEVEL_* byte             */
    uint8_t  bor_level;           /* offset 9  — raw OB_BOR_LEVEL_* byte             */
    uint8_t  reserved[2];         /* offset 10                                         */
    uint32_t reserved_ext;        /* offset 12                                         */
} bl_ob_status_t;

_Static_assert(sizeof(bl_ob_status_t) == BL_OB_STATUS_SIZE,
               "bl_ob_status_t must be exactly 16 bytes");

typedef enum {
    BL_OB_OK           = 0,
    BL_OB_ERR_HARDWARE = 1,
} bl_ob_rc_t;

/* Populate `out` with the current option-byte snapshot. Always
 * succeeds on H7 (HAL_FLASHEx_OBGetConfig is a pure read) but kept
 * as a status-returning API so future chips with failure modes don't
 * break the shape. */
bl_ob_rc_t bl_obyte_read(bl_ob_status_t *out);

/* True when sector N is currently WRP-protected. `sector` is 0..7
 * (H7x3 has a single 8-sector bank). */
bool bl_obyte_is_sector_wrp_protected(uint8_t sector);

/*
 * Apply WRP to every sector whose bit is set in `sector_bitmap`, then
 * trigger HAL_FLASH_OB_Launch. The launch resets the MCU — the
 * function does not return on success. Failure (HAL programming
 * error before the launch) returns BL_OB_ERR_HARDWARE.
 *
 * Typical production use: `sector_bitmap = 0x01` (protect sector 0 —
 * the bootloader itself).
 */
bl_ob_rc_t bl_obyte_apply_wrp(uint32_t sector_bitmap);

#endif /* BL_OBYTE_H */
