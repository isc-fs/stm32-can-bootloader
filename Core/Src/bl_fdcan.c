/*
 * bl_fdcan.c — FDCAN-instance selector (issue #120).
 *
 * The BL hosts its protocol on exactly one FDCAN peripheral, chosen by
 * BL_FDCAN_INSTANCE (1, 2 or 3 — see bl_config.h). The CubeMX-generated
 * MX_FDCAN{1,2,3}_Init (main.c) + HAL_FDCAN_MspInit (stm32h7xx_hal_msp.c)
 * own the actual peripheral / GPIO / clock / NVIC bring-up for all three
 * instances. This module is the thin selector that hands the rest of the
 * BL the *selected* instance's handle (bl_fdcan_get_handle) and configures
 * its RX filters (bl_fdcan_configure_filters). Every other TU stays
 * instance-agnostic.
 *
 * Why select among the CubeMX hfdcanN handles rather than own a private
 * one: CubeMX rewrites MX_FDCANx_Init / HAL_FDCAN_MspInit unconditionally
 * on every "Generate Code", so anything that tried to *replace* them would
 * silently desync on the next regeneration (the bootloader would end up
 * driving an uninitialised handle). Selecting hfdcanN keeps this module
 * regeneration-proof.
 *
 * Phase B (next): NVM-backed runtime override under
 * BL_NVM_KEY_FDCAN_INSTANCE — bl_fdcan_get_handle() resolves to a value
 * cached at boot rather than at link time; its callers don't change.
 */

#include "bl_fdcan.h"

#include "bl_config.h"
#include "bl_proto.h"          /* BL_PROTO_NODE_MASK / NODE_BROADCAST / ID_VALID_MASK */
#include "stm32h7xx_hal.h"

#include <stdint.h>

/* The CubeMX-generated per-peripheral handles, defined + initialised in
 * main.c by MX_FDCAN{1,2,3}_Init. The abstraction points at the selected
 * one rather than maintaining a separate (and easily-desynced) handle. */
extern FDCAN_HandleTypeDef hfdcan1;
extern FDCAN_HandleTypeDef hfdcan2;
extern FDCAN_HandleTypeDef hfdcan3;

FDCAN_HandleTypeDef *bl_fdcan_get_handle(void)
{
#if BL_FDCAN_INSTANCE == 1
    return &hfdcan1;
#elif BL_FDCAN_INSTANCE == 2
    return &hfdcan2;
#else /* BL_FDCAN_INSTANCE == 3 — value validated in bl_config.h */
    return &hfdcan3;
#endif
}

uint8_t bl_fdcan_get_instance_number(void)
{
    return (uint8_t)BL_FDCAN_INSTANCE;
}

/* ---- bl_fdcan_configure_filters ---------------------------------- *
 *
 * The filter geometry doesn't depend on the FDCAN instance — same
 * 5-low-bit mask, same two filter entries (unicast + broadcast), same
 * reject-everything-else global filter. The only piece that varies is the
 * handle it operates on, which is exactly what bl_fdcan_get_handle()
 * resolves. */
HAL_StatusTypeDef bl_fdcan_configure_filters(uint8_t node_id)
{
    FDCAN_HandleTypeDef *h = bl_fdcan_get_handle();

    FDCAN_FilterTypeDef filter = { 0 };
    filter.IdType       = FDCAN_STANDARD_ID;
    filter.FilterType   = FDCAN_FILTER_MASK;
    filter.FilterConfig = FDCAN_FILTER_TO_RXFIFO0;
    filter.FilterID2    = BL_PROTO_ID_VALID_MASK;

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

    /* Reject everything that doesn't match one of the two filters; reject
     * remote frames outright — the protocol has no use for them. */
    return HAL_FDCAN_ConfigGlobalFilter(h,
                                        FDCAN_REJECT,
                                        FDCAN_REJECT,
                                        FDCAN_REJECT_REMOTE,
                                        FDCAN_REJECT_REMOTE);
}
