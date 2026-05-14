#ifndef BL_MEMMAP_H
#define BL_MEMMAP_H

/*
 * Host-test override of the real bl_memmap.h. The production layout is
 * preserved relative to BL_FLASH_BASE — the only thing that changes is
 * that BL_FLASH_BASE is the address of a 1-MB process-local buffer
 * (declared in mocks/hal_stubs.c) instead of the STM32's 0x08000000.
 *
 * The mock HAL flash stubs translate (addr - BL_FLASH_BASE) → byte
 * offset within that buffer and enforce the real STM32H7 programming
 * semantics — most importantly the 1→0-only bit-transition rule that
 * is exactly the trap our recent NVM fixes are guarding against. Tests
 * that exercise erase / program / metadata-rewrite paths therefore
 * see the same failure modes the chip would.
 */

#include <stdint.h>

extern uint8_t g_fake_flash[1024U * 1024U];

/* All BL_*_BASE / BL_*_ADDR macros below are `uintptr_t`-typed so the
 * production code's `(TYPE *)(BL_NVM_BASE + ...)` casts work on a
 * 64-bit host without truncating g_fake_flash's real pointer. On the
 * STM32 build, the real bl_memmap.h uses uint32_t literals — same
 * width as uintptr_t on Cortex-M, so there is no behavioural drift
 * between the two configurations. */

/* ---- Flash geometry ---- */
#define BL_FLASH_BASE            ((uintptr_t)g_fake_flash)
#define BL_FLASH_SIZE            (1024U * 1024U)
#define BL_FLASH_SECTOR_SIZE     (128U * 1024U)
#define BL_FLASH_SECTOR_COUNT    8U

/* ---- Bootloader region — sector 0 ---- */
#define BL_BOOT_BASE             BL_FLASH_BASE
#define BL_BOOT_SIZE             BL_FLASH_SECTOR_SIZE
#define BL_BOOT_END              (BL_BOOT_BASE + BL_BOOT_SIZE - 1U)

/* ---- Application region — sectors 1..6 ---- */
#define BL_APP_BASE              (BL_FLASH_BASE + (uintptr_t)0x00020000U)
#define BL_APP_END               (BL_FLASH_BASE + (uintptr_t)0x000DFFFFU)
#define BL_APP_FIRST_SECTOR      1U
#define BL_APP_LAST_SECTOR       6U
#define BL_APP_SIZE              (BL_APP_END - BL_APP_BASE + 1U)

/* ---- NVM + metadata region — sector 7 ---- */
#define BL_NVM_SECTOR            7U
#define BL_NVM_BASE              (BL_FLASH_BASE + (uintptr_t)0x000E0000U)
#define BL_NVM_SIZE              (BL_FLASH_SECTOR_SIZE - BL_APP_METADATA_SIZE)
#define BL_NVM_END               (BL_NVM_BASE + BL_NVM_SIZE - 1U)

#define BL_APP_METADATA_ADDR     (BL_FLASH_BASE + (uintptr_t)0x000FFFE0U)
#define BL_APP_METADATA_SIZE     32U
#define BL_APP_META_MAGIC        0xB007C0DEU

/* ---- Boot-request handshake (irrelevant on host but kept for parity) ---- */
#define BL_BOOT_REQ_MAGIC        0xB00710ADU

#endif /* BL_MEMMAP_H */
