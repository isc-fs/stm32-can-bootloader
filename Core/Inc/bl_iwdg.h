#ifndef BL_IWDG_H
#define BL_IWDG_H

/*
 * bl_iwdg — independent watchdog (IWDG1) management for the bootloader.
 *
 * The IWDG is the hardware backstop that guarantees the BL can never hang
 * indefinitely and become unflashable over CAN (issue #125 H6). It sits
 * BELOW the software reset-on-spin (bl_fault, #135) in the defence stack:
 * the fault handlers catch CPU faults and the Error_Handler promptly, but
 * anything that wedges the core with interrupts masked — or any infinite
 * loop the handlers don't cover — is only caught here.
 *
 * Driven by direct register access (KR/PR/RLR/SR), NOT the HAL: the HAL
 * IWDG driver isn't vendored in this tree and the module is disabled in
 * stm32h7xx_hal_conf.h. Four register writes are simpler to audit than a
 * driver dependency, and keep the watchdog fully self-contained.
 *
 * --- Ownership across the BL<->app handoff (IFS08-CE-AMS#280) ---
 *
 * The IWDG is one-way: once started it cannot be stopped, and its config +
 * count survive NVIC_SystemReset — only a power-on reset clears it. So
 * whoever starts it dictates the period until the next POR, and BOTH sides
 * must cooperate:
 *
 *   - BL -> app: the AMS app re-inits IWDG to its own ~100 ms period and
 *     refreshes it every 10 ms, reaching its re-init within ~50-70 ms of
 *     its reset — far inside the BL's ~8 s period. The BL also refreshes
 *     once right before the jump, handing the app a full fresh window.
 *
 *   - app -> BL (the `002` re-enter-BL reset): the BL inherits the app's
 *     tight ~100 ms IWDG, still running. bl_iwdg_start() re-inits the
 *     period to the BL's ~8 s and reloads the counter, so it MUST be
 *     called as the very first step of Bootloader_Init — within a few ms
 *     of reset, well before the inherited ~100 ms could fire.
 *
 * --- Period sizing ---
 *
 * The worst-case uninterruptible CPU stall in the BL is a single 128 KB
 * sector erase: the single-bank H733 stalls instruction fetch for the
 * whole erase (no read-while-write within a bank), so the IWDG cannot be
 * refreshed DURING an erase — only between sectors. bl_flash_erase()
 * therefore erases ONE sector per HAL call and kicks between sectors, and
 * bl_nvm's sector-7 erase kicks first too, so the period only has to clear
 * a SINGLE sector, never a multi-sector span.
 *
 * Measured single-sector erase: 1768 ms on the bench. The ~8 s nominal
 * period (BL_IWDG_RELOAD, see bl_config.h) clears that with >3x margin at
 * the nominal LSI corner and still ~1.8x at a deliberately pessimistic
 * fast-LSI corner, while a genuine hang self-recovers in <= ~15 s.
 *
 * Build knob BL_IWDG_ENABLE (default 1, see bl_config.h): build with
 * -DBL_IWDG_ENABLE=0 to compile the watchdog out entirely — an escape
 * hatch for bring-up / bisection if a period regression is ever suspected.
 * With it 0, both functions below are no-ops.
 */

/*
 * Configure IWDG1 to the BL's period and start (or re-period) it. Safe to
 * call when the IWDG is already running (inherited from a prior app): it
 * unlocks, rewrites PR/RLR, waits for the sync flags, and reloads. Never
 * blocks indefinitely and never resets on failure — a missing watchdog
 * must not itself make the BL unflashable. Call as the first step of
 * Bootloader_Init.
 */
void bl_iwdg_start(void);

/*
 * Reload (kick) the watchdog counter. A bare KR reload-key write, so it is
 * harmless even if bl_iwdg_start() was never called or failed — the key is
 * ignored by hardware until the IWDG is running. Call every main-loop
 * iteration, between flash-erase sectors, and once just before the app
 * jump.
 */
void bl_iwdg_refresh(void);

#endif /* BL_IWDG_H */
