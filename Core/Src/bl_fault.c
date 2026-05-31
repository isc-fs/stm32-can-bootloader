/*
 * bl_fault.c — controlled reboot on terminal faults. See bl_fault.h.
 *
 * Firmware-only TU (uses NVIC_SystemReset + a CMSIS .noinit section);
 * not part of the host unit-test build.
 */

#include "bl_fault.h"

#include "stm32h7xx_hal.h"   /* NVIC_SystemReset, __DSB */

/* Sentinel proving the breadcrumb was written by us (vs. random RAM
 * contents on a cold power-up, where .noinit is undefined). */
#define BL_FAULT_MAGIC   0xFA017A11U

/* Reset-surviving breadcrumb.
 *
 * `.noinit` RAM is NOT zeroed by the startup code and its contents
 * survive a system reset (NVIC_SystemReset resets the core +
 * peripherals but leaves SRAM intact — only a power-on reset clears
 * it). Plain RAM is writable in ANY context, so this is safe to set
 * from inside a HardFault where clocks / the backup domain may be in
 * an unknown state. The companion .noinit section lives in
 * STM32H733_boot.ld. */
static volatile uint32_t g_fault_magic  __attribute__((section(".noinit")));
static volatile uint32_t g_fault_reason __attribute__((section(".noinit")));

void bl_fault_reboot(uint8_t reason)
{
    g_fault_reason = (uint32_t)reason;
    g_fault_magic  = BL_FAULT_MAGIC;

    /* Bounded dwell before the reset (~tens to a few hundred ms).
     * Purpose: if the fault is deterministic, this loops the board
     * slowly instead of resetting at MHz — which (a) keeps an SWD
     * probe able to attach, and (b) leaves a CAN listen window each
     * boot so a fixed image can still be flashed. A fixed NOP count
     * rather than HAL_Delay because SysTick may be dead in a fault,
     * and the clock may be wrong (e.g. an Error_Handler from a failed
     * clock config) — exact timing doesn't matter, only the order of
     * magnitude. `volatile` keeps the compiler from eliding it. */
    for (volatile uint32_t i = 0U; i < 48000000U; i++) {
        __asm volatile ("nop");
    }

    __DSB();
    NVIC_SystemReset();

    /* NVIC_SystemReset does not return; satisfy noreturn. */
    for (;;) {
        /* unreachable */
    }
}

bool bl_fault_take_pending(uint8_t *out_reason)
{
    if (g_fault_magic != BL_FAULT_MAGIC) {
        return false;
    }
    if (out_reason != (uint8_t *)0) {
        *out_reason = (uint8_t)g_fault_reason;
    }
    g_fault_magic = 0U;   /* one-shot: clear so a later normal boot reads clean */
    return true;
}
