#ifndef BL_FAULT_H
#define BL_FAULT_H

/*
 * bl_fault — turn terminal hangs into auto-recovering reboots
 * (issue #125 H6, "reset-on-spin" half).
 *
 * The bootloader's prime directive is to stay reachable over CAN so an
 * ECU can always be reflashed without opening its enclosure. Any
 * `while(1)` spin (a CPU fault handler, Error_Handler, a failed
 * peripheral init) violates that — the board goes permanently dead
 * until a power-cycle, and a power-cycle may land in the same spin.
 *
 * `bl_fault_reboot()` replaces those spins with a controlled
 * NVIC_SystemReset, so a TRANSIENT fault (an ECC glitch, a stray
 * access from a one-off condition) self-recovers: the reboot re-inits
 * the BL and reopens a CAN listen window. A DETERMINISTIC fault still
 * needs SWD, but the bounded dwell before reset (see bl_fault.c) keeps
 * the board SWD-attachable and leaves a CAN window between reboots
 * instead of a MHz reset storm.
 *
 * This is the app-safe half of H6: it never enables a watchdog, so it
 * cannot affect the application after a BL→app jump. The full IWDG
 * (which persists into the app and so needs the app to refresh it) is
 * tracked separately, pending the app-team handoff.
 *
 * The reboot reason is stashed in a `.noinit` RAM breadcrumb that
 * survives the system reset (RAM is not cleared by NVIC_SystemReset,
 * only by power loss) and is NOT zeroed by startup. After the reboot,
 * Bootloader_Init calls bl_fault_take_pending() and logs a
 * BL_DTC_CPU_FAULT so the cause is visible via `diagnose dtc`.
 */

#include <stdbool.h>
#include <stdint.h>

/* Reboot-reason codes (context_data of the BL_DTC_CPU_FAULT logged
 * after recovery). */
#define BL_FAULT_NONE           0x00U
#define BL_FAULT_HARDFAULT      0x01U
#define BL_FAULT_NMI            0x02U
#define BL_FAULT_MEMMANAGE      0x03U
#define BL_FAULT_BUSFAULT       0x04U
#define BL_FAULT_USAGEFAULT     0x05U
#define BL_FAULT_ERROR_HANDLER  0x06U  /* HAL Error_Handler (e.g. clock cfg) */
#define BL_FAULT_FDCAN_INIT     0x07U  /* FDCAN filter/start failed at boot   */

/* Record `reason` in a reset-surviving breadcrumb, dwell briefly so a
 * deterministic fault loops slowly (not a reset storm), then
 * NVIC_SystemReset. Never returns. Safe from a fault/exception
 * handler: writes only plain .noinit RAM — no clocks, no backup
 * domain, no HAL calls before the reset. */
void bl_fault_reboot(uint8_t reason) __attribute__((noreturn));

/* If the last boot ended via bl_fault_reboot(), return true and write
 * the reason to *out_reason, then clear the breadcrumb (one-shot).
 * Returns false on a normal (non-fault) boot. Call once early in
 * Bootloader_Init, after bl_dtc_init so the DTC can be logged. */
bool bl_fault_take_pending(uint8_t *out_reason);

#endif /* BL_FAULT_H */
