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
    /* FDCAN capture also lives in this file (see further below) — wipe
     * it here too so every test sees an empty TX ring even if it does
     * its own ring setup later. Avoids cross-test leaks when a NACK
     * fires unexpectedly. Defined as `mock_fdcan_reset()` further down
     * but we just inline a forward call here to keep setUp() simple. */
    extern void mock_fdcan_reset(void);
    mock_fdcan_reset();
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

HAL_StatusTypeDef HAL_FLASH_OB_Launch(void)
{
    /* On the chip this never returns (triggers system reset). On host
     * we report OK so the post-launch dead-code in handle_ob_apply_wrp
     * runs deterministically if tests reach it. */
    return HAL_OK;
}

/* ---- NVIC + RCC + RTC stub storage ---- */
void NVIC_SystemReset(void) { /* no-op on host — real chip would not return */ }

static RCC_TypeDef g_mock_rcc = { .BDCR = 0U };
RCC_TypeDef *const RCC = &g_mock_rcc;

static RTC_TypeDef g_mock_rtc = { .BKP0R = 0U };
RTC_TypeDef *const RTC = &g_mock_rtc;


/* ---- FDCAN TX capture ----
 *
 * Every successful HAL_FDCAN_AddMessageToTxFifoQ call records the
 * frame into a ring. Tests assert against this to verify which
 * frames the BL emits in response to a given dispatch input. */
/* Storage + accessor that back the production `bl_fdcan_get_handle()`
 * in the host-test build (issue #120 Phase A). In production both
 * live in bl_fdcan.c, but tests don't link that TU because
 * HAL_FDCAN_Init / HAL_FDCAN_ConfigFilter / HAL_FDCAN_Start aren't
 * stubbed in the host harness. The field contents are never read by
 * tests — only the FDCAN TX capture mock matters — so a zero-init
 * handle is fine. */
FDCAN_HandleTypeDef bl_fdcan_handle;

FDCAN_HandleTypeDef *bl_fdcan_get_handle(void)
{
    return &bl_fdcan_handle;
}

static mock_fdcan_frame_t g_fdcan_frames[MOCK_FDCAN_CAPTURE_DEPTH];
static int                g_fdcan_count = 0;

void mock_fdcan_reset(void)
{
    g_fdcan_count = 0;
    memset(g_fdcan_frames, 0, sizeof(g_fdcan_frames));
}

int mock_fdcan_tx_count(void) { return g_fdcan_count; }

const mock_fdcan_frame_t *mock_fdcan_get(int index)
{
    if (index < 0 || index >= g_fdcan_count) {
        return (const mock_fdcan_frame_t *)0;
    }
    return &g_fdcan_frames[index];
}

uint32_t HAL_FDCAN_GetRxFifoFillLevel(FDCAN_HandleTypeDef *h, uint32_t fifo)
{
    (void)h;
    (void)fifo;
    /* Host tests don't simulate RX FIFO occupancy — return 0 so the
     * dispatcher's log_isotp_error packs a "FIFO empty" byte into the
     * DTC context. Tests that want to assert on the instrumentation's
     * encoding under a non-empty FIFO can wrap this with a smarter
     * fake. */
    return 0U;
}

uint32_t HAL_FDCAN_GetTxFifoFreeLevel(FDCAN_HandleTypeDef *h)
{
    (void)h;
    /* "Always fully drained." Matches BL_FDCAN_TX_QUEUE_DEPTH (16) in
     * bl_proto.c — when that constant changes, this mock must agree
     * or wait_tx_drain will spin in tests. */
    return 16U;
}

HAL_StatusTypeDef HAL_FDCAN_AddMessageToTxFifoQ(FDCAN_HandleTypeDef *h,
                                                FDCAN_TxHeaderTypeDef *hdr,
                                                uint8_t *data)
{
    (void)h;
    if (g_fdcan_count >= (int)MOCK_FDCAN_CAPTURE_DEPTH) {
        return HAL_ERROR;
    }
    mock_fdcan_frame_t *slot = &g_fdcan_frames[g_fdcan_count++];
    slot->identifier = hdr->Identifier;
    /* bl_proto's dlc_bytes_to_fdcan() (see Core/Src/bl_proto.c) returns
     * the byte count directly — NOT shifted, because the ST HAL itself
     * does the bit-16 shift internally during message-RAM packing.
     * So DataLength is just the 0..8 byte count, clamped. */
    slot->dlc_bytes = (uint8_t)(hdr->DataLength > 8U ? 8U : hdr->DataLength);
    memcpy(slot->data, data, slot->dlc_bytes);
    return HAL_OK;
}
