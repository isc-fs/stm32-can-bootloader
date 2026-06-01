#ifndef BL_FDCAN_H
#define BL_FDCAN_H

/*
 * Bootloader FDCAN abstraction (issue #120).
 *
 * The BL hosts its CAN-side protocol on exactly one FDCAN peripheral at a
 * time. Which one is chosen by BL_FDCAN_INSTANCE (1, 2 or 3 — see
 * bl_config.h). The CubeMX-generated MX_FDCAN{1,2,3}_Init + the
 * HAL_FDCAN_MspInit dispatch bring up all three peripherals; this module is
 * the thin selector that gives the rest of the BL the *selected* instance's
 * handle, so main.c / bl_proto.c stay instance-agnostic — they call
 * bl_fdcan_get_handle() instead of referencing a specific hfdcanN.
 *
 * The instance is resolved at boot by bl_fdcan_init_from_nvm(): a valid
 * 1-byte NVM override under BL_NVM_KEY_FDCAN_INSTANCE (1, 2 or 3) wins,
 * otherwise the compile-time BL_FDCAN_INSTANCE default. bl_fdcan_get_handle()
 * returns a value cached at boot, so its callers stay instance-agnostic
 * regardless of how the choice was made.
 *
 * See bl_config.h::BL_FDCAN_INSTANCE for the build flag, and the per-
 * instance GPIO/AF/NVIC setup in stm32h7xx_hal_msp.c::HAL_FDCAN_MspInit.
 */

#include "stm32h7xx_hal.h"

#include <stdint.h>

/* Resolve which FDCAN instance the BL uses and cache it. Prefers a valid
 * 1-byte NVM override (BL_NVM_KEY_FDCAN_INSTANCE = 1/2/3) over the
 * compile-time BL_FDCAN_INSTANCE default; a malformed override silently
 * falls back. Call once at boot, after bl_nvm_init() and before
 * bl_fdcan_configure_filters() / the first bl_fdcan_get_handle(). */
void bl_fdcan_init_from_nvm(void);

/* The handle every other TU uses for HAL_FDCAN_* calls — the CubeMX
 * hfdcanN for the resolved instance. Trivial body, so the compiler inlines
 * it at -O2 / -Os. */
FDCAN_HandleTypeDef *bl_fdcan_get_handle(void);

/* Configures the two FIFO0 filters (unicast to `node_id` + broadcast to
 * `0xF`) on the resolved FDCAN instance, plus a reject-everything-else
 * global filter. Replaces the old `main.c::Bootloader_ConfigFdcanFilters`.
 * Returns HAL_OK on success; the caller (BL init) treats anything else as
 * fatal. */
HAL_StatusTypeDef bl_fdcan_configure_filters(uint8_t node_id);

/* Returns the resolved instance number (1, 2 or 3). Useful for log
 * messages + the future health-record `fdcan_instance` field. */
uint8_t bl_fdcan_get_instance_number(void);

#endif /* BL_FDCAN_H */
