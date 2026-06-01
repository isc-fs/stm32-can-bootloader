#ifndef BL_FDCAN_H
#define BL_FDCAN_H

/*
 * Bootloader FDCAN abstraction (issue #120).
 *
 * The BL serves its CAN-side protocol on ALL THREE FDCAN peripherals at
 * once (FDCAN1, FDCAN2, FDCAN3). A host may connect on whichever bus the
 * board happens to wire CAN to — the BL listens on every one, and answers
 * a request back out the same bus it arrived on. No build flag, no
 * provisioning: one image works regardless of which FDCAN the hardware
 * uses.
 *
 * The CubeMX-generated MX_FDCAN{1,2,3}_Init + HAL_FDCAN_MspInit bring up
 * all three peripherals; this module configures their RX filters, starts
 * them, and routes each reply to the originating bus. main.c iterates the
 * buses for boot bring-up / teardown / RX polling via bl_fdcan_bus(); the
 * RX drain calls bl_fdcan_set_active() per frame so bl_proto's TX path
 * (which calls bl_fdcan_get_handle()) replies on the right bus without
 * having to know there is more than one.
 *
 * Per-instance GPIO/AF/NVIC setup lives in stm32h7xx_hal_msp.c::
 * HAL_FDCAN_MspInit; the .ioc keeps all three in sync (notably
 * AutoRetransmission=ENABLE — the issue #94 fix).
 */

#include "stm32h7xx_hal.h"

#include <stdint.h>

/* How many FDCAN buses the BL serves concurrently. The H733 exposes
 * FDCAN1..3; all three are live. Used to size the per-bus loops + state in
 * main.c. */
#define BL_FDCAN_BUS_COUNT  3U

/* Handle for bus `index` (0 -> FDCAN1, 1 -> FDCAN2, 2 -> FDCAN3). For the
 * boot bring-up / teardown / per-bus RX-poll loops. An out-of-range index
 * returns bus 0 rather than NULL so callers can't dereference garbage. */
FDCAN_HandleTypeDef *bl_fdcan_bus(uint8_t index);

/* The active reply target: the bus the in-flight request arrived on.
 * main.c's RX drain calls bl_fdcan_set_active() for each received frame
 * before dispatching it; bl_proto's TX path calls bl_fdcan_get_handle() to
 * answer on that same bus. Defaults to bus 0 until the first RX (the BL
 * never transmits before it has received a request). */
FDCAN_HandleTypeDef *bl_fdcan_get_handle(void);
void                 bl_fdcan_set_active(FDCAN_HandleTypeDef *handle);

/* Configure the two FIFO0 RX filters (unicast to `node_id` + broadcast to
 * 0xF) plus a reject-everything-else global filter on EVERY bus. Must run
 * while the peripherals are still in Init mode (before bl_fdcan_start_all).
 * Returns HAL_OK only if all buses configured; the first failure's status
 * otherwise. The caller (BL init) treats anything but HAL_OK as fatal. */
HAL_StatusTypeDef bl_fdcan_configure_filters(uint8_t node_id);

/* HAL_FDCAN_Start every bus. Returns HAL_OK only if all started; the first
 * failure's status otherwise. */
HAL_StatusTypeDef bl_fdcan_start_all(void);

#endif /* BL_FDCAN_H */
