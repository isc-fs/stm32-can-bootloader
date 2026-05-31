/*
 * bl_flash.c — flash manager implementation.
 *
 * Every HAL_FLASH_* call in the bootloader goes through this module.
 * Protocol handlers in bl_proto.c never touch HAL flash directly; they
 * speak bl_flash's status codes and let bl_flash enforce the writable-
 * range, FLASHWORD-alignment and sector-alignment invariants.
 */

#include "bl_flash.h"

#include "bl_health.h"
#include "bl_iwdg.h"
#include "bl_memmap.h"
#include "bl_nvm.h"
#include "stm32h7xx_hal.h"

#include <stdbool.h>
#include <string.h>

#define BL_FLASHWORD_BYTES        32U

/* ---- Internal helpers ---- */

/* True when [start, start + length) lies within the writable app range
 * [BL_APP_BASE, BL_APP_END + 1). Zero-length ranges and overflow are
 * rejected. This excludes sector 0 (bootloader, WRP), sector 7 (NVM +
 * metadata — bl_nvm owns that), and anything outside the flash map. */
bool bl_flash_range_is_writable(uint32_t start, uint32_t length)
{
    if (length == 0U) {
        return false;
    }
    uint32_t end = start + length;
    if (end < start) {  /* overflow */
        return false;
    }
    return (start >= BL_APP_BASE) && (end <= (BL_APP_END + 1U));
}

/* Classify a range error — returns the right status when the caller
 * already knows the range isn't writable. Separates "protected because
 * it touches the BL / NVM / metadata" from "just plain out of range". */
static bl_flash_status_t classify_range_error(uint32_t start, uint32_t length)
{
    /* Fully outside the whole flash (or zero-length / overflow) →
     * out of bounds. */
    uint32_t end = start + length;
    if (length == 0U || end < start
        || start < BL_FLASH_BASE
        || end > (BL_FLASH_BASE + BL_FLASH_SIZE)) {
        return BL_FLASH_ERR_OUT_OF_BOUNDS;
    }
    /* Overlaps sector 0 (bootloader) or sector 7 (NVM + metadata) —
     * either way, the app-writable range check rejected it. Callers
     * see this as protected. */
    return BL_FLASH_ERR_PROTECTED;
}


/* ---- Erase ---- */

bl_flash_status_t bl_flash_erase(uint32_t start,
                                 uint32_t length,
                                 uint32_t *sectors_erased)
{
    if (sectors_erased != (uint32_t *)0) {
        *sectors_erased = 0U;
    }

    if (!bl_flash_range_is_writable(start, length)) {
        return classify_range_error(start, length);
    }

    /* Require sector-aligned start and length — sub-sector erase doesn't
     * exist on H7 flash. */
    if (((start - BL_FLASH_BASE) % BL_FLASH_SECTOR_SIZE) != 0U
        || (length % BL_FLASH_SECTOR_SIZE) != 0U) {
        return BL_FLASH_ERR_UNALIGNED;
    }

    uint32_t first_sector = (start - BL_FLASH_BASE) / BL_FLASH_SECTOR_SIZE;
    uint32_t end_inclusive = start + length - 1U;
    uint32_t last_sector = (end_inclusive - BL_FLASH_BASE) / BL_FLASH_SECTOR_SIZE;

    /* Defensive — should never trip given range_is_writable + alignment. */
    if (first_sector < BL_APP_FIRST_SECTOR || last_sector > BL_APP_LAST_SECTOR) {
        return BL_FLASH_ERR_PROTECTED;
    }

    if (HAL_FLASH_Unlock() != HAL_OK) {
        return BL_FLASH_ERR_HARDWARE;
    }

    /* #125 H6: erase ONE sector per HAL call, kicking the IWDG between
     * sectors.
     *
     * The H733 is single-bank, so erasing a sector stalls the CPU's
     * instruction fetch from flash for the WHOLE erase (no read-while-
     * write within a bank). The watchdog therefore CANNOT be refreshed
     * mid-erase — only between sectors. A single multi-sector
     * HAL_FLASHEx_Erase (what this used to do) would stall for the SUM of
     * every sector — up to 6 x ~1.8 s ≈ 10.6 s for a full app erase — in
     * one un-kickable call, tripping the ~8 s IWDG mid-erase and bricking
     * the unit over CAN. One sector at a time bounds each stall to a
     * single ~1.8 s erase, comfortably inside the period.
     *
     * Timing uses DWT->CYCCNT, NOT HAL_GetTick: during the stall SysTick
     * starves and the tick barely advances (the original bench read of
     * "1 ms" for a 128 KB erase). CYCCNT is clock-driven and counts
     * through the stall; it is enabled once at boot in Bootloader_Init.
     * The 32-bit subtraction is wrap-safe. bl_health_record_flash_op_ms
     * keeps the per-boot MAX, so recording per sector captures the true
     * worst-case single-sector duration that sizes the IWDG period. */
    uint32_t cyc_per_ms = SystemCoreClock / 1000U;
    uint32_t erased     = 0U;

    for (uint32_t sector = first_sector; sector <= last_sector; sector++) {
        /* Fresh full period immediately before this sector's stall. */
        bl_iwdg_refresh();

        FLASH_EraseInitTypeDef eraseInit = { 0 };
        eraseInit.TypeErase = FLASH_TYPEERASE_SECTORS;
        eraseInit.Banks     = FLASH_BANK_1;
        eraseInit.Sector    = sector;
        eraseInit.NbSectors = 1U;

        uint32_t page_error = 0U;
        uint32_t cyc0 = DWT->CYCCNT;
        HAL_StatusTypeDef st = HAL_FLASHEx_Erase(&eraseInit, &page_error);
        uint32_t cyc_elapsed = DWT->CYCCNT - cyc0;

        /* cycles → ms. Recorded even on a failed erase — it still
         * consumed time and is informative for sizing the IWDG. */
        if (cyc_per_ms > 0U) {
            bl_health_record_flash_op_ms(cyc_elapsed / cyc_per_ms);
        }

        if (st != HAL_OK) {
            HAL_FLASH_Lock();
            if (sectors_erased != (uint32_t *)0) {
                *sectors_erased = erased;  /* sectors completed before the fault */
            }
            return BL_FLASH_ERR_HARDWARE;
        }
        erased++;
    }

    /* The final sector's erase stalled the CPU for up to ~1.8 s; kick once
     * more so the caller's ACK and the next main-loop refresh start with a
     * full period of headroom. */
    bl_iwdg_refresh();

    HAL_FLASH_Lock();
    if (sectors_erased != (uint32_t *)0) {
        *sectors_erased = erased;
    }
    /* Count successful erases for the persistent flash_write_count
     * field in the health record. Once per call, not per sector —
     * matches the bl_flash_write convention below. */
    bl_health_record_flash_write();
    return BL_FLASH_OK;
}


/* ---- Program ---- */

bl_flash_status_t bl_flash_write(uint32_t addr,
                                 const uint8_t *data,
                                 uint32_t length)
{
    if (!bl_flash_range_is_writable(addr, length)) {
        return classify_range_error(addr, length);
    }

    /* Write address must be FLASHWORD-aligned; length can be anything,
     * the last partial FLASHWORD gets padded with 0xFF. */
    if ((addr & (BL_FLASHWORD_BYTES - 1U)) != 0U) {
        return BL_FLASH_ERR_UNALIGNED;
    }

    if (HAL_FLASH_Unlock() != HAL_OK) {
        return BL_FLASH_ERR_HARDWARE;
    }

    uint8_t fw[BL_FLASHWORD_BYTES];
    uint32_t offset = 0U;
    while (offset < length) {
        uint32_t remaining = length - offset;
        uint32_t chunk = (remaining >= BL_FLASHWORD_BYTES)
                             ? BL_FLASHWORD_BYTES
                             : remaining;
        memcpy(fw, data + offset, chunk);
        if (chunk < BL_FLASHWORD_BYTES) {
            memset(fw + chunk, 0xFFU, BL_FLASHWORD_BYTES - chunk);
        }

        /* `(uintptr_t)` rather than `(uint32_t)`: identity on STM32,
         * lossless on a 64-bit host (unit tests). */
        HAL_StatusTypeDef st = HAL_FLASH_Program(FLASH_TYPEPROGRAM_FLASHWORD,
                                                 addr + offset,
                                                 (uintptr_t)fw);
        if (st != HAL_OK) {
            HAL_FLASH_Lock();
            return BL_FLASH_ERR_HARDWARE;
        }
        offset += BL_FLASHWORD_BYTES;
    }

    HAL_FLASH_Lock();
    /* Count one program op for the persistent counter — not per
     * FLASHWORD, so a single host WRITE command of N bytes ticks the
     * counter once. */
    bl_health_record_flash_write();
    return BL_FLASH_OK;
}


/* ---- CRC32 (IEEE 802.3 reflected) ---- */

uint32_t bl_flash_crc32(uint32_t addr, uint32_t length)
{
    uint32_t crc = 0xFFFFFFFFU;
    const uint8_t *ptr = (const uint8_t *)addr;

    for (uint32_t i = 0U; i < length; i++) {
        crc ^= ptr[i];
        for (uint32_t bit = 0U; bit < 8U; bit++) {
            if ((crc & 1U) != 0U) {
                crc = (crc >> 1) ^ 0xEDB88320U;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc ^ 0xFFFFFFFFU;
}


/* ---- Metadata record ---- */

bl_flash_status_t bl_flash_write_metadata(uint32_t size,
                                          uint32_t crc,
                                          uint32_t version)
{
    uint32_t meta[8] = { 0U };
    meta[0] = BL_APP_META_MAGIC;
    meta[1] = size;
    meta[2] = crc;
    meta[3] = BL_APP_BASE;
    meta[4] = version;
    /* meta[5..7] reserved (zeroed above) */

    /* STM32 flash only permits 1→0 transitions per program cycle; once
     * any bit at BL_APP_METADATA_ADDR is set, an in-place rewrite with
     * different content will return HAL_ERROR. Check whether the word
     * is in erased state (all 0xFF); if not, hand off to
     * bl_nvm_compact_replace_meta() which erases sector 7 (preserving
     * live NVM entries) and writes the new metadata in one pass. */
    const uint32_t *cur = (const uint32_t *)BL_APP_METADATA_ADDR;
    bool erased = true;
    for (uint32_t i = 0U; i < (BL_APP_METADATA_SIZE / 4U); i++) {
        if (cur[i] != 0xFFFFFFFFU) {
            erased = false;
            break;
        }
    }

    if (!erased) {
        bl_nvm_status_t ns = bl_nvm_compact_replace_meta(meta);
        return (ns == BL_NVM_OK) ? BL_FLASH_OK : BL_FLASH_ERR_HARDWARE;
    }

    /* Fast path: metadata word is erased, just program it. */
    if (HAL_FLASH_Unlock() != HAL_OK) {
        return BL_FLASH_ERR_HARDWARE;
    }

    /* `(uintptr_t)` rather than `(uint32_t)`: identity on STM32,
     * lossless on a 64-bit host (unit tests). */
    HAL_StatusTypeDef st = HAL_FLASH_Program(FLASH_TYPEPROGRAM_FLASHWORD,
                                             BL_APP_METADATA_ADDR,
                                             (uintptr_t)meta);
    HAL_FLASH_Lock();

    return (st == HAL_OK) ? BL_FLASH_OK : BL_FLASH_ERR_HARDWARE;
}
