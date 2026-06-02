/*
 * bl_fdcan.c — multi-bus FDCAN front-end (issue #120).
 *
 * The BL serves its protocol on ALL THREE FDCAN peripherals simultaneously
 * (FDCAN1, FDCAN2, FDCAN3), so a host on whichever bus the board wires CAN
 * to is heard. The CubeMX-generated MX_FDCAN{1,2,3}_Init (main.c) +
 * HAL_FDCAN_MspInit bring up all three handles; this module:
 *
 *   - bl_fdcan_bus(i)            — exposes handle i for the boot / teardown
 *                                 / RX-poll loops in main.c;
 *   - bl_fdcan_configure_filters — installs the RX filters on every bus;
 *   - bl_fdcan_start_all         — starts every bus;
 *   - bl_fdcan_set_active /      — track which bus the in-flight request
 *     bl_fdcan_get_handle          arrived on, so the reply (sent via
 *                                 bl_proto, which calls get_handle) goes
 *                                 back out that same bus.
 *
 * Selecting CubeMX's hfdcanN handles (rather than owning private ones)
 * keeps the module regeneration-proof: a .ioc regen rewrites MX_FDCAN*_Init
 * but this file never holds an init it could leave stale.
 */

#include "bl_fdcan.h"

#include "bl_proto.h"          /* BL_PROTO_NODE_MASK / NODE_BROADCAST / ID_VALID_MASK */
#include "stm32h7xx_hal.h"

#include <stdint.h>

/* The CubeMX-generated per-peripheral handles, defined + initialised in
 * main.c by MX_FDCAN{1,2,3}_Init. */
extern FDCAN_HandleTypeDef hfdcan1;
extern FDCAN_HandleTypeDef hfdcan2;
extern FDCAN_HandleTypeDef hfdcan3;

/* Bus index -> handle. Order is FDCAN1, FDCAN2, FDCAN3. */
static FDCAN_HandleTypeDef *const s_buses[BL_FDCAN_BUS_COUNT] = {
    &hfdcan1,
    &hfdcan2,
    &hfdcan3,
};

/* The reply target — the bus the current request arrived on. Defaults to
 * bus 0; the BL never transmits before its first RX, so any started bus is
 * a safe default. (Initialised to &hfdcan1 directly: a static initialiser
 * can't index s_buses[], but the address constant is identical.) */
static FDCAN_HandleTypeDef *s_active = &hfdcan1;

FDCAN_HandleTypeDef *bl_fdcan_bus(uint8_t index)
{
    if (index >= BL_FDCAN_BUS_COUNT) {
        return s_buses[0];
    }
    return s_buses[index];
}

FDCAN_HandleTypeDef *bl_fdcan_get_handle(void)
{
    return s_active;
}

void bl_fdcan_set_active(FDCAN_HandleTypeDef *handle)
{
    if (handle != (FDCAN_HandleTypeDef *)0) {
        s_active = handle;
    }
}

/* ---- bl_fdcan_configure_filters ---------------------------------- *
 *
 * The filter geometry is identical on every bus — same exact-match mask
 * (#154: full 11-bit, so aliases like the charger's 0x101 can't slip in as
 * a node-1 unicast), same two entries (unicast + broadcast), same
 * reject-everything-else global filter. Install it on all three so each bus
 * only ever forwards frames addressed to this node (or broadcast) into
 * RX FIFO0. */
HAL_StatusTypeDef bl_fdcan_configure_filters(uint8_t node_id)
{
    for (uint8_t b = 0U; b < BL_FDCAN_BUS_COUNT; b++) {
        FDCAN_HandleTypeDef *h = s_buses[b];

        FDCAN_FilterTypeDef filter = { 0 };
        filter.IdType       = FDCAN_STANDARD_ID;
        filter.FilterType   = FDCAN_FILTER_MASK;
        filter.FilterConfig = FDCAN_FILTER_TO_RXFIFO0;
        filter.FilterID2    = BL_PROTO_ID_FILTER_MASK;   /* #154: exact 11-bit match, no aliases */

        /* Unicast: host -> this node. */
        filter.FilterIndex = 0U;
        filter.FilterID1   = (uint32_t)node_id & BL_PROTO_NODE_MASK;
        HAL_StatusTypeDef st = HAL_FDCAN_ConfigFilter(h, &filter);
        if (st != HAL_OK) {
            return st;
        }

        /* Broadcast: host -> 0xF. */
        filter.FilterIndex = 1U;
        filter.FilterID1   = BL_PROTO_NODE_BROADCAST;
        st = HAL_FDCAN_ConfigFilter(h, &filter);
        if (st != HAL_OK) {
            return st;
        }

        /* Reject everything that doesn't match one of the two filters;
         * reject remote frames outright — the protocol has no use for them. */
        st = HAL_FDCAN_ConfigGlobalFilter(h,
                                          FDCAN_REJECT,
                                          FDCAN_REJECT,
                                          FDCAN_REJECT_REMOTE,
                                          FDCAN_REJECT_REMOTE);
        if (st != HAL_OK) {
            return st;
        }
    }
    return HAL_OK;
}

HAL_StatusTypeDef bl_fdcan_start_all(void)
{
    for (uint8_t b = 0U; b < BL_FDCAN_BUS_COUNT; b++) {
        HAL_StatusTypeDef st = HAL_FDCAN_Start(s_buses[b]);
        if (st != HAL_OK) {
            return st;
        }
    }
    return HAL_OK;
}
