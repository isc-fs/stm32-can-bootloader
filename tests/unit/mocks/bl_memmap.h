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

/* ---- NVM + provisioning-seed + metadata region — sector 7 ----
 * Mirrors the production bl_memmap.h: KV | seed | metadata. */
#define BL_NVM_SECTOR            7U
#define BL_NVM_BASE              (BL_FLASH_BASE + (uintptr_t)0x000E0000U)
#define BL_NVM_SIZE              (BL_FLASH_SECTOR_SIZE - BL_APP_METADATA_SIZE - BL_PROVISION_SEED_SIZE)
#define BL_NVM_END               (BL_NVM_BASE + BL_NVM_SIZE - 1U)

/* #183 provisioning seed — one FLASHWORD below the metadata word. */
#define BL_PROVISION_SEED_ADDR   (BL_FLASH_BASE + (uintptr_t)0x000FFFC0U)
#define BL_PROVISION_SEED_SIZE   32U
#define BL_PROVISION_SEED_MAGIC  0xB0070D1DU

#define BL_APP_METADATA_ADDR     (BL_FLASH_BASE + (uintptr_t)0x000FFFE0U)
#define BL_APP_METADATA_SIZE     32U
#define BL_APP_META_MAGIC        0xB007C0DEU

/* ---- Boot-request handshake (irrelevant on host but kept for parity) ---- */
#define BL_BOOT_REQ_MAGIC        0xB00710ADU
#define BL_BOOT_APP_MAGIC        0xB0070A99U   /* #142: boot-app-via-reset */

/* ---- bl_log ring placement override ----
 *
 * Production bl_log.h points BL_LOG_RING_ADDR at the chip's BKPSRAM
 * slot (0x38800400). On host that's a wild address; redirect it to a
 * process-local buffer so test cases can plant arbitrary state in it
 * by writing through `(bl_log_ring_t *)BL_LOG_RING_ADDR` directly.
 *
 * Size = 32-byte header + BL_LOG_RING_BYTES (1024) = 1056. We don't
 * include bl_log.h here to keep the dependency direction one-way; the
 * literal is checked by bl_log.h's own _Static_assert on the chip side
 * (which still compiles in the host build) so any size drift fails at
 * the host compile step. */
extern uint8_t g_fake_log_ring[1056];
#define BL_LOG_RING_ADDR    ((uintptr_t)g_fake_log_ring)

#endif /* BL_MEMMAP_H */
