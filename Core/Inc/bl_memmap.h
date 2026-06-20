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

/* ---- Sector 7: NVM KV | provisioning seed | app metadata ----
 *
 * Top-down within sector 7:
 *   app metadata     0x080FFFE0 .. 0x080FFFFF   32 B  installed-image record
 *   provision seed   0x080FFFC0 .. 0x080FFFDF   32 B  #183 one-shot SWD node-id seed
 *   NVM KV store     0x080E0000 .. 0x080FFFBF   rest  log-structured key/value
 *
 * BL_NVM_SIZE is reduced by BOTH the metadata and the seed FLASHWORDs so the
 * KV log can never pack into either. */
#define BL_NVM_SECTOR            7U
#define BL_NVM_BASE              0x080E0000U
#define BL_NVM_SIZE              (BL_FLASH_SECTOR_SIZE - BL_APP_METADATA_SIZE - BL_PROVISION_SEED_SIZE)  /* 128 KB - 64 B */
#define BL_NVM_END               (BL_NVM_BASE + BL_NVM_SIZE - 1U)               /* 0x080FFFBF */

/* #183 — provisioning seed FLASHWORD, one 256-bit word just below the
 * metadata. An SWD tool programs it during the bootloader burn; the BL
 * consumes it into a proper NVM node-id entry on first boot (bl_provision.c).
 * Reserving it here keeps it out of the KV packing range above. */
#define BL_PROVISION_SEED_ADDR   0x080FFFC0U
#define BL_PROVISION_SEED_SIZE   32U
#define BL_PROVISION_SEED_MAGIC  0xB0070D1DU   /* "boot node-id" — any non-erased value */

/* Metadata FLASHWORD lives in the last 32 bytes of sector 7 (the
 * address is unchanged from earlier phases; only its neighbourhood
 * moved). Apps and host tools that compute the metadata address from
 * BL_APP_BASE + some offset need updating once, but the absolute
 * address is stable. */
#define BL_APP_METADATA_ADDR     0x080FFFE0U
#define BL_APP_METADATA_SIZE     32U
#define BL_APP_META_MAGIC        0xB007C0DEU

/* Sector 7 is exactly KV | seed | metadata, contiguous + non-overlapping. */
_Static_assert(BL_PROVISION_SEED_ADDR == BL_NVM_BASE + BL_NVM_SIZE,
               "provisioning seed must sit immediately above the NVM KV region");
_Static_assert(BL_APP_METADATA_ADDR == BL_PROVISION_SEED_ADDR + BL_PROVISION_SEED_SIZE,
               "app metadata must sit immediately above the provisioning seed");
_Static_assert(BL_APP_METADATA_ADDR + BL_APP_METADATA_SIZE
                   == BL_FLASH_BASE + ((BL_NVM_SECTOR + 1U) * BL_FLASH_SECTOR_SIZE),
               "KV + seed + metadata must exactly fill sector 7");

/* ---- Boot-request handshake (RTC->BKP0R) ---- */
#define BL_BOOT_REQ_MAGIC        0xB00710ADU   /* one-shot: stay in bootloader */
/* #142: one-shot "boot the app via a clean reset" — set by handle_jump /
 * handle_reset when a flash write happened this session, so a write-then-
 * jump reaches the app through a reset (cold-equivalent flash state) rather
 * than a direct warm jump that can leave the freshly-written app stuck.
 * Distinct value from BL_BOOT_REQ_MAGIC; shares BKP0R (mutually exclusive,
 * both one-shot). */
#define BL_BOOT_APP_MAGIC        0xB0070A99U

#endif /* BL_MEMMAP_H */
