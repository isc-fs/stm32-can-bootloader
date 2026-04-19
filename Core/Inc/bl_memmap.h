#ifndef BL_MEMMAP_H
#define BL_MEMMAP_H

/*
 * Canonical flash memory map for the STM32H733 CAN bootloader.
 * Every hard-coded bootloader / application address or magic value lives
 * here; main.c and future protocol code must reference these symbols
 * instead of redefining literals locally.
 *
 * See ARCHITECTURE.md in the repository root for the full memory map,
 * boot flow, and firmware contract.
 *
 * Layout (post-Phase-4):
 *
 *   sector 0       bootloader         128 KB  WRP-protected
 *   sectors 1..6   application        768 KB
 *   sector 7       NVM + app metadata 128 KB  log-structured KV + 32 B meta word
 *
 * The app metadata FLASHWORD at 0x080FFFE0 (last 32 B of sector 7)
 * continues to describe the installed image. NVM entries pack into
 * the rest of the sector. A sector erase during NVM compaction wipes
 * both — bl_nvm is responsible for saving + restoring the metadata
 * across that cycle.
 */

#include <stdint.h>

/* ---- Flash geometry (STM32H733ZGT6, bank 1 only) ---- */
#define BL_FLASH_BASE            0x08000000U
#define BL_FLASH_SIZE            (1024U * 1024U)       /* 1 MB total */
#define BL_FLASH_SECTOR_SIZE     (128U * 1024U)        /* 128 KB each */
#define BL_FLASH_SECTOR_COUNT    8U

/* ---- Bootloader region — sector 0, WRP-protected in production ---- */
#define BL_BOOT_BASE             BL_FLASH_BASE
#define BL_BOOT_SIZE             BL_FLASH_SECTOR_SIZE  /* 128 KB */
#define BL_BOOT_END              (BL_BOOT_BASE + BL_BOOT_SIZE - 1U)

/* ---- Application region — sectors 1..6 (Phase 4 shrank this from
 *      sectors 1..7 to make room for NVM in sector 7) ---- */
#define BL_APP_BASE              0x08020000U
#define BL_APP_END               0x080DFFFFU
#define BL_APP_FIRST_SECTOR      1U
#define BL_APP_LAST_SECTOR       6U
#define BL_APP_SIZE              (BL_APP_END - BL_APP_BASE + 1U)  /* 0x000C0000 = 768 KB */

/* ---- NVM + metadata region — sector 7 ---- */
#define BL_NVM_SECTOR            7U
#define BL_NVM_BASE              0x080E0000U
#define BL_NVM_SIZE              (BL_FLASH_SECTOR_SIZE - BL_APP_METADATA_SIZE)  /* 128 KB - 32 B */
#define BL_NVM_END               (BL_NVM_BASE + BL_NVM_SIZE - 1U)               /* 0x080FFFDF */

/* Metadata FLASHWORD lives in the last 32 bytes of sector 7 (the
 * address is unchanged from earlier phases; only its neighbourhood
 * moved). Apps and host tools that compute the metadata address from
 * BL_APP_BASE + some offset need updating once, but the absolute
 * address is stable. */
#define BL_APP_METADATA_ADDR     0x080FFFE0U
#define BL_APP_METADATA_SIZE     32U
#define BL_APP_META_MAGIC        0xB007C0DEU

/* ---- Boot-request handshake (RTC->BKP0R) ---- */
#define BL_BOOT_REQ_MAGIC        0xB00710ADU

#endif /* BL_MEMMAP_H */
