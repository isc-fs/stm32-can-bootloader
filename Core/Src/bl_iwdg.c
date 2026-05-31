/*
 * bl_iwdg.c — independent watchdog (IWDG1), register-level.
 *
 * See bl_iwdg.h for the why: this is the BL's hardware backstop against an
 * un-refreshable hang, sized so a single 128 KB sector erase (the worst
 * uninterruptible stall) always clears the period.
 *
 * Not built host-side — it pokes IWDG1 registers directly. The host test
 * harness stubs bl_iwdg_refresh() in mocks/bl_peer_stubs.c.
 */

#include "bl_iwdg.h"

#include "bl_config.h"
#include "stm32h7xx_hal.h"   /* IWDG_TypeDef, IWDG1, IWDG_PR_PR */

#ifndef BL_IWDG_ENABLE
#define BL_IWDG_ENABLE 1
#endif

#if BL_IWDG_ENABLE

/* IWDG key-register commands (RM0468 §IWDG). */
#define BL_IWDG_KEY_RELOAD   0x0000AAAAU  /* reload counter from RLR        */
#define BL_IWDG_KEY_UNLOCK   0x00005555U  /* enable write access to PR/RLR  */
#define BL_IWDG_KEY_START    0x0000CCCCU  /* start IWDG (also starts LSI)   */

/* Prescaler /256 → PR field = 0b110 = 6. /256 is the maximum divider the
 * H7 IWDG supports (PR is 3 bits, 0..6 map /4../256). At LSI ~32 kHz the
 * tick is 256/32000 = 8 ms, so BL_IWDG_RELOAD counts of 8 ms set the
 * period. See bl_config.h for the reload value + period math. */
#define BL_IWDG_PRESCALER_DIV256   0x06U

/* Bound the post-config sync wait so a stuck SR can never hang the BL.
 * The PVU/RVU flags clear within a handful of LSI cycles (a few hundred µs
 * at 32 kHz); at the CPU clock this loop count is comfortably longer than
 * that yet still a hard ceiling of a few ms. */
#define BL_IWDG_SYNC_SPINS         200000U

void bl_iwdg_start(void)
{
    /* Order mirrors ST's HAL_IWDG_Init: START first (no-op if already
     * running, e.g. inherited from a prior app), then unlock + program
     * PR/RLR, wait for the values to be absorbed, then reload.
     *
     * On a cold boot START arms the IWDG with its reset-default period
     * (~0.5 s) for the few microseconds until our RLR commits — far longer
     * than the configuration below takes, so the default can never fire.
     * On an app->BL reset it re-periods the inherited ~100 ms IWDG to the
     * BL's ~8 s, which is why this must run first in Bootloader_Init. */
    IWDG1->KR  = BL_IWDG_KEY_START;
    IWDG1->KR  = BL_IWDG_KEY_UNLOCK;
    IWDG1->PR  = BL_IWDG_PRESCALER_DIV256;
    IWDG1->RLR = (uint32_t)BL_IWDG_RELOAD;

    /* Wait for PR/RLR to be absorbed (SR clears) before reloading — a
     * reload issued while RVU is set is ignored and the new period would
     * not take effect. Bounded: if SR never clears we proceed anyway
     * rather than spin forever; a degraded watchdog beats a hung BL. */
    uint32_t spins = BL_IWDG_SYNC_SPINS;
    while ((IWDG1->SR != 0U) && (spins != 0U)) {
        spins--;
    }

    IWDG1->KR = BL_IWDG_KEY_RELOAD;
}

void bl_iwdg_refresh(void)
{
    /* Bare reload-key write — deliberately NOT gated on "did start
     * succeed", because the key is ignored by hardware until the IWDG is
     * actually running, so a stray refresh is always safe. */
    IWDG1->KR = BL_IWDG_KEY_RELOAD;
}

#else  /* BL_IWDG_ENABLE == 0 — watchdog compiled out */

void bl_iwdg_start(void)   { /* no-op: IWDG disabled at build time */ }
void bl_iwdg_refresh(void) { /* no-op: IWDG disabled at build time */ }

#endif /* BL_IWDG_ENABLE */
