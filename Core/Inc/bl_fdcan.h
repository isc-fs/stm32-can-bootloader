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
 * Phase B (next): NVM-backed runtime override under
 * BL_NVM_KEY_FDCAN_INSTANCE resolved at boot. bl_fdcan_get_handle() stays
 * the same; under the hood it resolves to a value cached at boot rather
 * than a link-time constant, so its callers don't have to track the change.
 *
 * See bl_config.h::BL_FDCAN_INSTANCE for the build flag, and the per-
 * instance GPIO/AF/NVIC setup in stm32h7xx_hal_msp.c::HAL_FDCAN_MspInit.
 */

#include "stm32h7xx_hal.h"

#include <stdint.h>

/* The handle every other TU uses for HAL_FDCAN_* calls — the CubeMX
 * hfdcanN for the selected BL_FDCAN_INSTANCE. Trivial body, so the
 * compiler inlines it at -O2 / -Os and the call overhead vs the legacy
 * direct-`&hfdcan2` shape is in the noise. */
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
