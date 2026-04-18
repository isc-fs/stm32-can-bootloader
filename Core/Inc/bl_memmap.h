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

/* ---- Application region — sectors 1..7 ---- */
#define BL_APP_BASE              0x08020000U
#define BL_APP_END               0x080FFFFFU
#define BL_APP_FIRST_SECTOR      1U
#define BL_APP_LAST_SECTOR       7U

/* Metadata FLASHWORD lives in the last 32 bytes of the app region. */
#define BL_APP_METADATA_ADDR     0x080FFFE0U
#define BL_APP_METADATA_SIZE     32U
#define BL_APP_META_MAGIC        0xB007C0DEU

/* Maximum programmable image size (app region minus the metadata word). */
#define BL_APP_SIZE              (BL_APP_METADATA_ADDR - BL_APP_BASE)   /* 0x000DFFE0 */

/* ---- Boot-request handshake (RTC->BKP0R) ---- */
#define BL_BOOT_REQ_MAGIC        0xB00710ADU

#endif /* BL_MEMMAP_H */
