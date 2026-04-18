#ifndef BL_FLASH_H
#define BL_FLASH_H

/*
 * bl_flash — flash manager for the bootloader.
 *
 * Encapsulates every interaction with the STM32H7 flash programmer so
 * the protocol handlers in bl_proto.c don't have to care about FLASHWORD
 * alignment, sector geometry or HAL_FLASH_* semantics.
 *
 * Writable range is strictly [BL_APP_BASE, BL_APP_METADATA_ADDR). The
 * bootloader's own sector 0, and the metadata FLASHWORD at 0x080FFFE0,
 * are off-limits to the FLASH_* opcodes — bl_flash refuses any range
 * that touches them with BL_FLASH_ERR_PROTECTED.
 *
 * The metadata FLASHWORD itself is written through a dedicated helper
 * (bl_flash_write_metadata) that the FLASH_VERIFY handler calls once
 * the image's CRC has been validated.
 */

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    BL_FLASH_OK              = 0,
    BL_FLASH_ERR_PROTECTED   = 1,  /* range hits sector 0 or the metadata word */
    BL_FLASH_ERR_OUT_OF_BOUNDS = 2,/* range outside the app region entirely     */
    BL_FLASH_ERR_UNALIGNED   = 3,  /* addr not FLASHWORD-aligned (write) or     *
                                    * sector-aligned (erase)                   */
    BL_FLASH_ERR_HARDWARE    = 4,  /* HAL returned non-OK                      */
} bl_flash_status_t;

/* Returns true when [start, start + length) lies entirely inside the
 * writable app range. A zero-length range is rejected. */
bool bl_flash_range_is_writable(uint32_t start, uint32_t length);

/*
 * Erase the flash sectors that cover [start, start + length).
 *
 *   - `start` must be sector-aligned (multiple of BL_FLASH_SECTOR_SIZE).
 *   - `length` must be a multiple of BL_FLASH_SECTOR_SIZE.
 *   - The resulting sector set must sit entirely inside sectors
 *     BL_APP_FIRST_SECTOR .. BL_APP_LAST_SECTOR.
 *
 * On success `*sectors_erased` receives the number of sectors that
 * were actually erased. Blocks for the full HAL_FLASHEx_Erase duration
 * (a 128 KB H7 sector takes ~1..4 s). The caller is responsible for
 * making sure no other flash operation is in flight.
 */
bl_flash_status_t bl_flash_erase(uint32_t start,
                                 uint32_t length,
                                 uint32_t *sectors_erased);

/*
 * Program `length` bytes from `data` starting at `addr`.
 *
 *   - `addr` must be FLASHWORD-aligned (32 bytes).
 *   - `length` may be any value in 1..BL_APP_SIZE; the final FLASHWORD
 *     is padded with 0xFF internally.
 *   - The target range must be fully writable per
 *     bl_flash_range_is_writable.
 *
 * Each FLASHWORD may only be programmed once between erases (ECC
 * constraint on H7); the caller must have erased the target sectors
 * first. On hardware error we unlock / lock and return HARDWARE —
 * the partial write may have committed some bytes already.
 */
bl_flash_status_t bl_flash_write(uint32_t addr,
                                 const uint8_t *data,
                                 uint32_t length);

/* CRC32 — IEEE 802.3 reflected polynomial 0xEDB88320, init and xor-
 * out both 0xFFFFFFFF. Computed directly over the memory-mapped flash
 * bytes; `length` may be up to BL_APP_SIZE. */
uint32_t bl_flash_crc32(uint32_t addr, uint32_t length);

/*
 * Write the application metadata record at BL_APP_METADATA_ADDR with
 * the given size, CRC32 and version. The FLASHWORD layout mirrors the
 * one documented in ARCHITECTURE.md:
 *
 *   [0] = BL_APP_META_MAGIC
 *   [1] = size
 *   [2] = crc32
 *   [3] = BL_APP_BASE
 *   [4] = version
 *   [5..7] = 0
 *
 * The metadata FLASHWORD must have been erased (typically by a prior
 * FLASH_ERASE that covered sector 7) before calling this.
 */
bl_flash_status_t bl_flash_write_metadata(uint32_t size,
                                          uint32_t crc,
                                          uint32_t version);

#endif /* BL_FLASH_H */
