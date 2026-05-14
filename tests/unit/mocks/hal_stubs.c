/*
 * hal_stubs.c — host-side simulation of the STM32H7 HAL flash + tick.
 *
 * Backs the bootloader's HAL_FLASH_* calls with a 1-MB g_fake_flash[]
 * buffer and enforces:
 *
 *   - The 1→0-only programming rule. STM32 flash can clear bits in a
 *     programmed word but never set them. If any bit position needs a
 *     0→1 transition, HAL_FLASH_Program returns HAL_ERROR. This is
 *     the exact behaviour the bl_flash_write_metadata bug-fix targets,
 *     and the test suite uses it to keep that fix from regressing.
 *
 *   - Sector-aligned erase. HAL_FLASHEx_Erase wipes the requested
 *     sectors to 0xFF; sector-bitmap math matches the production code.
 *
 *   - Address-range checking: any address outside the 1-MB buffer
 *     returns HAL_ERROR instead of segfaulting.
 *
 * Test-only knobs (declared in mocks/stm32h7xx_hal.h):
 *
 *   mock_flash_reset()              — full re-init: buffer to 0xFF,
 *                                     fault counters cleared.
 *   mock_flash_set_program_fail(N)  — next N program calls return
 *                                     HAL_ERROR even on legal writes.
 *   mock_flash_set_erase_fail(N)    — same for erases.
 *   mock_flash_program_call_count() — observed by tests that need to
 *                                     assert "exactly N writes happened".
 *   mock_flash_erase_call_count()   — same for erases.
 *   mock_set_tick(t) / mock_advance_tick(dt)
 *                                   — drive HAL_GetTick() deterministically.
 */

#include "stm32h7xx_hal.h"
#include "bl_memmap.h"

#include <stdint.h>
#include <string.h>

#define FLASHWORD_BYTES 32U

uint8_t g_fake_flash[BL_FLASH_SIZE];

static int      g_program_fail_remaining = 0;
static int      g_erase_fail_remaining   = 0;
static int      g_program_calls          = 0;
static int      g_erase_calls            = 0;
static uint32_t g_tick                   = 0;

/* ---- Test-only knobs ---- */

void mock_flash_reset(void)
{
    memset(g_fake_flash, 0xFF, sizeof(g_fake_flash));
    g_program_fail_remaining = 0;
    g_erase_fail_remaining   = 0;
    g_program_calls          = 0;
    g_erase_calls            = 0;
    g_tick                   = 0;
}

void mock_flash_set_program_fail(int n) { g_program_fail_remaining = n; }
void mock_flash_set_erase_fail(int n)   { g_erase_fail_remaining = n;   }
int  mock_flash_program_call_count(void){ return g_program_calls;       }
int  mock_flash_erase_call_count(void)  { return g_erase_calls;         }
void mock_set_tick(uint32_t t)          { g_tick = t;                   }
void mock_advance_tick(uint32_t dt)     { g_tick += dt;                 }

/* ---- HAL surface ---- */

HAL_StatusTypeDef HAL_FLASH_Unlock(void) { return HAL_OK; }
HAL_StatusTypeDef HAL_FLASH_Lock(void)   { return HAL_OK; }

HAL_StatusTypeDef HAL_FLASH_Program(uint32_t type, uintptr_t addr, uintptr_t data_addr)
{
    g_program_calls++;

    if (g_program_fail_remaining > 0) {
        g_program_fail_remaining--;
        return HAL_ERROR;
    }

    /* Only the FLASHWORD program mode is used by the bootloader. */
    if (type != FLASH_TYPEPROGRAM_FLASHWORD) {
        return HAL_ERROR;
    }
    /* Range check. Reject writes whose 32-byte window leaves the buffer. */
    if (addr < BL_FLASH_BASE) {
        return HAL_ERROR;
    }
    uintptr_t end = addr + FLASHWORD_BYTES;
    if (end < addr || end > BL_FLASH_BASE + BL_FLASH_SIZE) {
        return HAL_ERROR;
    }

    uint8_t       *dst = g_fake_flash + (addr - BL_FLASH_BASE);
    const uint8_t *src = (const uint8_t *)data_addr;

    /* Enforce 1→0-only: if any source bit needs to go from 0 → 1
     * relative to current flash content, the chip would return
     * HAL_ERROR with a PGSERR / WRPERR. Surface the same here. */
    for (uint32_t i = 0; i < FLASHWORD_BYTES; i++) {
        uint8_t needs_set = (uint8_t)(src[i] & (uint8_t)~dst[i]);
        if (needs_set != 0U) {
            return HAL_ERROR;
        }
    }
    /* All transitions are 1→0; commit via bitwise AND. */
    for (uint32_t i = 0; i < FLASHWORD_BYTES; i++) {
        dst[i] = (uint8_t)(dst[i] & src[i]);
    }
    return HAL_OK;
}

HAL_StatusTypeDef HAL_FLASHEx_Erase(FLASH_EraseInitTypeDef *pEraseInit, uint32_t *PageError)
{
    g_erase_calls++;

    if (g_erase_fail_remaining > 0) {
        g_erase_fail_remaining--;
        if (PageError != (uint32_t *)0) {
            *PageError = pEraseInit->Sector;
        }
        return HAL_ERROR;
    }

    if (pEraseInit->TypeErase != FLASH_TYPEERASE_SECTORS) {
        return HAL_ERROR;
    }
    if (pEraseInit->Sector >= BL_FLASH_SECTOR_COUNT) {
        return HAL_ERROR;
    }
    if (pEraseInit->NbSectors == 0U
        || pEraseInit->Sector + pEraseInit->NbSectors > BL_FLASH_SECTOR_COUNT) {
        return HAL_ERROR;
    }

    uint8_t *base = g_fake_flash + (uint32_t)pEraseInit->Sector * BL_FLASH_SECTOR_SIZE;
    memset(base, 0xFF, (size_t)pEraseInit->NbSectors * BL_FLASH_SECTOR_SIZE);

    if (PageError != (uint32_t *)0) {
        *PageError = 0xFFFFFFFFU;   /* per HAL: no fault */
    }
    return HAL_OK;
}

uint32_t HAL_GetTick(void) { return g_tick; }
void     HAL_Delay(uint32_t ms) { g_tick += ms; }

void HAL_PWR_EnableBkUpAccess(void) { /* no-op on host */ }
