#ifndef BL_FDCAN_H
#define BL_FDCAN_H

/*
 * Bootloader FDCAN abstraction (issue #120 Phase A).
 *
 * The BL hosts its CAN-side protocol on exactly one FDCAN peripheral
 * at a time. Until #120 the choice was hardwired to FDCAN2 across
 * `main.c`, `stm32h7xx_hal_msp.c`, `stm32h7xx_it.c` and `bl_proto.c`.
 * This module concentrates every site that cared about "which
 * peripheral" so a per-board build flag (`-DBL_FDCAN_INSTANCE=N`)
 * picks the right instance, pin map, clock + NVIC vector — all the
 * other TUs just call `bl_fdcan_get_handle()` and stay
 * instance-agnostic.
 *
 * Phase A scope (this iteration): compile-time selection only. The
 * default (`BL_FDCAN_INSTANCE = 2`) preserves pre-#120 behaviour
 * bit-for-bit on the wire and aims for size parity with v1.3.1.
 *
 * Phase B (next): NVM-backed runtime override under
 * `BL_NVM_KEY_FDCAN_INSTANCE` resolved before the peripheral init.
 * `bl_fdcan_get_handle()` stays the same; under the hood it'll
 * resolve to a cached value chosen at boot rather than at link time.
 *
 * See `bl_config.h::BL_FDCAN_INSTANCE` for the build flag, and the
 * `pin_map_for_instance` table in `bl_fdcan.c` for per-instance GPIO
 * defaults.
 */

#include "stm32h7xx_hal.h"

#include <stdint.h>

/* The handle every other TU uses for HAL_FDCAN_* calls. The backing
 * storage lives in `bl_fdcan.c` and isn't exposed as an extern — all
 * accesses go through this accessor. Trivial body, so the compiler
 * inlines it at -O2 / -Os and the call overhead vs the legacy
 * direct-`&hfdcan2` shape is in the noise (4–8 bytes site-by-site,
 * far below the firmware-size-delta gate). */
FDCAN_HandleTypeDef *bl_fdcan_get_handle(void);

/* Replaces the CubeMX-generated `MX_FDCAN2_Init` body. Loads the
 * bit-timing + FIFO sizing + filter-table sizing into
 * `bl_fdcan_handle` and calls `HAL_FDCAN_Init` against the right
 * peripheral pointer for `BL_FDCAN_INSTANCE`. Calls `Error_Handler`
 * on failure. */
void bl_fdcan_mx_init(void);

/* Replaces the body of `HAL_FDCAN_MspInit` — clock enable + GPIO
 * pin/AF config + NVIC priority for whichever instance was selected.
 * Called by HAL during `bl_fdcan_mx_init()` via the standard MSP
 * callback indirection. */
void bl_fdcan_msp_init(FDCAN_HandleTypeDef *hfdcan);

/* Replaces the body of `HAL_FDCAN_MspDeInit` — disables clock, GPIO
 * and NVIC for the selected instance. Called by HAL during
 * `HAL_FDCAN_DeInit` (which the BL→APP jump path invokes). */
void bl_fdcan_msp_deinit(FDCAN_HandleTypeDef *hfdcan);

/* Configures the two FIFO0 filters (unicast to `node_id` + broadcast
 * to `0xF`) on the resolved FDCAN instance. Replaces the body of the
 * old `main.c::Bootloader_ConfigFdcanFilters`. Returns HAL_OK on
 * success; the caller (BL init) treats anything else as fatal. */
HAL_StatusTypeDef bl_fdcan_configure_filters(uint8_t node_id);

/* Returns the resolved instance number (1, 2 or 3). Useful for log
 * messages + the future health-record `fdcan_instance` field. Phase A
 * just returns `BL_FDCAN_INSTANCE` verbatim. */
uint8_t bl_fdcan_get_instance_number(void);

#endif /* BL_FDCAN_H */
