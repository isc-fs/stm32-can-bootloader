#ifndef BL_FWINFO_H
#define BL_FWINFO_H

/*
 * bl_fwinfo — application firmware-info record.
 *
 * The record is a 64-byte struct the **application** exports at a fixed
 * offset inside its image. The bootloader reads it (never writes it)
 * to answer host-side identity queries such as GET_FW_INFO and — in
 * later phases — richer DISCOVER replies.
 *
 * Location:  BL_APP_BASE + BL_FWINFO_OFFSET  =  0x08020400
 *
 * Apps emit the record by placing a `const bl_fwinfo_t` into a named
 * `.firmware_info` section and pinning that section at the right offset
 * in their linker script:
 *
 *     const bl_fwinfo_t __firmware_info
 *         __attribute__((section(".firmware_info"), used)) = {
 *         .magic            = BL_FWINFO_MAGIC,
 *         .record_version   = BL_FWINFO_RECORD_VERSION,
 *         .fw_version_major = 1,
 *         .fw_version_minor = 2,
 *         .fw_version_patch = 3,
 *         .mcu_id           = 0x00000450,          // STM32H7x3
 *         .git_hash         = { 0xAA, 0xBB, … },    // first 8 bytes of SHA1
 *         .build_timestamp  = 1734567890ULL,        // unix seconds
 *         .product_name     = "IFS08-CE-ECU",
 *         .reserved         = { 0, 0 },
 *     };
 *
 *     // Linker script (app-side):
 *     .firmware_info BL_APP_BASE + 0x400 : { KEEP(*(.firmware_info)) } > FLASH
 *
 * The offset is 1 KB past BL_APP_BASE, which clears an H7-sized vector
 * table with margin to spare. App code typically starts at 0x500 or
 * later after reserving this header region.
 *
 * Forward-compat policy: the high 16 bits of `record_version` are the
 * major version; the low 16 bits are the minor. The bootloader accepts
 * any record whose magic matches AND whose major version is >= 1, so
 * apps built against future minor revisions still identify themselves
 * correctly.
 */

#include <stdbool.h>
#include <stdint.h>

#include "bl_memmap.h"

#define BL_FWINFO_MAGIC             0xF14F1B00U   /* "FW INFO" reversed, distinct from BL_APP_META_MAGIC */
#define BL_FWINFO_RECORD_VERSION    0x00010000U   /* 1.0 — first record shape */
#define BL_FWINFO_OFFSET            0x400U
#define BL_FWINFO_ADDR              (BL_APP_BASE + BL_FWINFO_OFFSET)
#define BL_FWINFO_SIZE              64U
#define BL_FWINFO_PRODUCT_NAME_LEN  16U

/* The on-flash layout. Natural alignment is satisfied — the struct fits
 * in exactly two 32-byte FLASHWORDs — so no __attribute__((packed)) is
 * needed. If you extend this, either consume bytes from `reserved` or
 * bump the major version. */
typedef struct {
    uint32_t magic;                                   /* offset 0  — BL_FWINFO_MAGIC */
    uint32_t record_version;                          /* offset 4  — 0x00MM.mmmm (major/minor) */
    uint32_t fw_version_major;                        /* offset 8 */
    uint32_t fw_version_minor;                        /* offset 12 */
    uint32_t fw_version_patch;                        /* offset 16 */
    uint32_t mcu_id;                                  /* offset 20 — DBGMCU->IDCODE baked into the image */
    uint8_t  git_hash[8];                             /* offset 24 — first 8 bytes of SHA-1 */
    uint64_t build_timestamp;                         /* offset 32 — unix seconds, little-endian */
    char     product_name[BL_FWINFO_PRODUCT_NAME_LEN];/* offset 40 — ASCII, NUL-padded */
    uint32_t reserved[2];                             /* offset 56 — zero-pad to 64 bytes */
} bl_fwinfo_t;

/* Compile-time sanity check: if this fires, the layout above drifted
 * from 64 bytes and the wire format is broken. */
_Static_assert(sizeof(bl_fwinfo_t) == BL_FWINFO_SIZE,
               "bl_fwinfo_t must be exactly 64 bytes");

/*
 * Returns true when the record at BL_FWINFO_ADDR has the right magic
 * and an acceptable major version.
 */
bool bl_fwinfo_is_present(void);

/*
 * Returns a pointer to the record at BL_FWINFO_ADDR if it's present
 * and valid, NULL otherwise. The returned pointer is into flash and
 * must not be written; treat the target as const.
 */
const bl_fwinfo_t *bl_fwinfo_get(void);

#endif /* BL_FWINFO_H */
