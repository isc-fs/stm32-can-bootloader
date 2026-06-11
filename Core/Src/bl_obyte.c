/*
 * bl_obyte.c — option-byte read + WRP apply.
 *
 * Keeps all HAL option-byte knowledge in one file so the protocol
 * handlers can speak a clean bl_obyte API. Write-protection-on-launch
 * triggers a system reset, so bl_obyte_apply_wrp does not return on
 * success — callers must ACK beforehand.
 */

#include "bl_obyte.h"

#include "stm32h7xx_hal.h"

#include <string.h>


/* ---- Read ---- */

bl_ob_rc_t bl_obyte_read(bl_ob_status_t *out)
{
    memset(out, 0, sizeof(*out));

    FLASH_OBProgramInitTypeDef ob = { 0 };
    /* Tell the HAL which bank's WRP we want back. Single-bank part —
     * bank 1 only. */
    ob.Banks = FLASH_BANK_1;
    HAL_FLASHEx_OBGetConfig(&ob);

    out->wrp_sector_mask = ob.WRPSector;
    out->user_config     = ob.USERConfig;
    out->rdp_level       = (uint8_t)(ob.RDPLevel & 0xFFU);
    out->bor_level       = (uint8_t)(ob.BORLevel & 0xFFU);
    return BL_OB_OK;
}


/* ---- WRP query ---- */

bool bl_obyte_is_sector_wrp_protected(uint8_t sector)
{
    if (sector > 7U) {
        return false;
    }
    bl_ob_status_t st;
    if (bl_obyte_read(&st) != BL_OB_OK) {
        return false;
    }
    return (st.wrp_sector_mask & (1UL << sector)) != 0UL;
}


/* ---- Apply ---- */

bl_ob_rc_t bl_obyte_apply_wrp(uint32_t sector_bitmap)
{
    /* G-B5: guard the IRREVERSIBLE option-byte write at its own layer (the
     * bl_proto caller already forces a sector-0 mask, but WRP is the one op
     * that can permanently un-flash a sector, so don't trust a single guard).
     * Only sector 0 — the bootloader — may be write-protected; protecting any
     * app or NVM sector (1..7) would make it impossible to erase/reflash, i.e.
     * a self-inflicted brick. Refuse such a bitmap rather than commit + reset
     * on it. (This path sets OPTIONBYTE_WRP only, so RDP / BOR / BOOT_ADD are
     * structurally never touched — the catastrophic OB fields are out of reach
     * here; the remaining risk is exactly this over-protection direction.) */
    if ((sector_bitmap & ~0x01U) != 0U) {
        return BL_OB_ERR_HARDWARE;   /* would WRP-protect a non-bootloader sector */
    }

    FLASH_OBProgramInitTypeDef ob = { 0 };
    ob.OptionType = OPTIONBYTE_WRP;
    ob.WRPState   = OB_WRPSTATE_ENABLE;
    ob.WRPSector  = sector_bitmap;
    ob.Banks      = FLASH_BANK_1;

    if (HAL_FLASH_Unlock() != HAL_OK) {
        return BL_OB_ERR_HARDWARE;
    }
    if (HAL_FLASH_OB_Unlock() != HAL_OK) {
        (void)HAL_FLASH_Lock();
        return BL_OB_ERR_HARDWARE;
    }

    HAL_StatusTypeDef st = HAL_FLASHEx_OBProgram(&ob);
    if (st != HAL_OK) {
        (void)HAL_FLASH_OB_Lock();
        (void)HAL_FLASH_Lock();
        return BL_OB_ERR_HARDWARE;
    }

    /* Launch writes the programmed bytes from the shadow area into the
     * active option-byte area and triggers a system reset. Execution
     * does not return on success. On the outside chance it ever does
     * (documented as "may fail silently on some families"), we fall
     * through and report HARDWARE. */
    (void)HAL_FLASH_OB_Launch();
    (void)HAL_FLASH_OB_Lock();
    (void)HAL_FLASH_Lock();
    return BL_OB_ERR_HARDWARE;
}
