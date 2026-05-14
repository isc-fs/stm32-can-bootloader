#ifndef STM32H7XX_HAL_H
#define STM32H7XX_HAL_H

/*
 * Host-test stub for the STM32H7 HAL. Provides just enough types and
 * function declarations for the production sources we exercise (bl_nvm
 * principally) to compile on a developer machine.
 *
 * The actual HAL_FLASH_* implementations live in mocks/hal_stubs.c
 * and target the g_fake_flash[] buffer redirected by mocks/bl_memmap.h.
 * They enforce the real STM32 1→0-only programming rule and sector-
 * erase semantics, so tests see the same failure modes the chip does.
 */

#include <stdint.h>

/* ---- Status type ---- */
typedef enum {
    HAL_OK      = 0x00,
    HAL_ERROR   = 0x01,
    HAL_BUSY    = 0x02,
    HAL_TIMEOUT = 0x03,
} HAL_StatusTypeDef;

/* ---- Flash programming + erase types (subset) ---- */
#define FLASH_TYPEPROGRAM_FLASHWORD     0x03U   /* 32-byte word write */

#define FLASH_TYPEERASE_SECTORS         0x00U
#define FLASH_BANK_1                    0x01U

typedef struct {
    uint32_t TypeErase;
    uint32_t Banks;
    uint32_t Sector;
    uint32_t NbSectors;
    uint32_t VoltageRange;
} FLASH_EraseInitTypeDef;

/* ---- Public HAL flash surface used by the bootloader ----
 *
 * Note: the real ST HAL declares FlashAddress and DataAddress as
 * `uint32_t`. We widen them to `uintptr_t` here so the host build
 * (where pointers are 64-bit) can pass real pointers in without the
 * compiler complaining about narrowing-cast. On the STM32 the two
 * types are the same width so no production code needs to change
 * behaviourally — the production call sites use `(uintptr_t)ptr`
 * which is a no-op cast on Cortex-M. */
HAL_StatusTypeDef HAL_FLASH_Unlock(void);
HAL_StatusTypeDef HAL_FLASH_Lock(void);
HAL_StatusTypeDef HAL_FLASH_Program(uint32_t TypeProgram, uintptr_t FlashAddress, uintptr_t DataAddress);
HAL_StatusTypeDef HAL_FLASHEx_Erase(FLASH_EraseInitTypeDef *pEraseInit, uint32_t *PageError);

uint32_t HAL_GetTick(void);
void     HAL_Delay(uint32_t ms);

/* ---- Test-only helpers (defined alongside the stubs) ---- */
void mock_flash_reset(void);            /* fill the 1-MB buffer with 0xFF */
void mock_flash_set_program_fail(int n);/* next N program calls return HAL_ERROR */
void mock_flash_set_erase_fail(int n);  /* next N erase calls return HAL_ERROR */
int  mock_flash_program_call_count(void);
int  mock_flash_erase_call_count(void);
void mock_set_tick(uint32_t t);
void mock_advance_tick(uint32_t dt);

#endif /* STM32H7XX_HAL_H */
