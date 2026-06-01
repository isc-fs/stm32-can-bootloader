/*
 * bl_fdcan.c — FDCAN-instance selector (issue #120).
 *
 * The BL hosts its protocol on exactly one FDCAN peripheral. Which one is
 * resolved at boot from two sources, in priority order:
 *
 *   1. NVM override:   a 1-byte entry under BL_NVM_KEY_FDCAN_INSTANCE
 *      (sector 7). When present + well-formed (value 1, 2 or 3) it picks
 *      the instance, so one firmware image can be provisioned to boards
 *      that wire CAN to different FDCANs — no rebuild (Phase B).
 *   2. Compile-time:   BL_FDCAN_INSTANCE from bl_config.h (default 2).
 *
 * bl_fdcan_init_from_nvm() does the resolution once at boot (after
 * bl_nvm_init, alongside bl_node_id_init_from_nvm) and caches the result;
 * bl_fdcan_get_handle() / bl_fdcan_get_instance_number() return it. The
 * CubeMX-generated MX_FDCAN{1,2,3}_Init (main.c) + HAL_FDCAN_MspInit
 * (stm32h7xx_hal_msp.c) bring up all three peripherals regardless, so the
 * resolved one is always initialised and ready — this module just selects
 * among the CubeMX hfdcanN handles. Selecting (rather than owning a private
 * handle) keeps the module regeneration-proof.
 *
 * Validation mirrors bl_node_id: a malformed / out-of-range / wrong-length
 * NVM byte silently falls back to the compile-time default rather than
 * refusing to boot — a corrupt override must never brick a board.
 */

#include "bl_fdcan.h"

#include "bl_config.h"
#include "bl_nvm.h"            /* bl_nvm_read + BL_NVM_KEY_FDCAN_INSTANCE */
#include "bl_proto.h"          /* BL_PROTO_NODE_MASK / NODE_BROADCAST / ID_VALID_MASK */
#include "stm32h7xx_hal.h"

#include <stdint.h>

/* The CubeMX-generated per-peripheral handles, defined + initialised in
 * main.c by MX_FDCAN{1,2,3}_Init. */
extern FDCAN_HandleTypeDef hfdcan1;
extern FDCAN_HandleTypeDef hfdcan2;
extern FDCAN_HandleTypeDef hfdcan3;

/* Resolved instance (1, 2 or 3), cached at boot by
 * bl_fdcan_init_from_nvm(). Initialised to the compile-time default so a
 * caller reaching bl_fdcan_get_handle() *before* init still gets a sane
 * handle (matches pre-Phase-B behaviour). */
static uint8_t s_fdcan_instance = BL_FDCAN_INSTANCE;

void bl_fdcan_init_from_nvm(void)
{
    /* Reset to the compile-time default first so the call is idempotent
     * and a previously-cached NVM value can't leak forward. */
    s_fdcan_instance = BL_FDCAN_INSTANCE;

    uint8_t value = 0U;
    uint8_t value_len = 0U;
    bl_nvm_status_t st = bl_nvm_read(BL_NVM_KEY_FDCAN_INSTANCE,
                                     &value, sizeof(value), &value_len);
    if (st != BL_NVM_OK) {
        /* NOT_FOUND is the normal "no override stored" case; any other
         * status is a read failure — either way the compile-time default
         * is the right answer. */
        return;
    }

    /* Validate the override byte; anything malformed keeps the default. */
    if (value_len != 1U) {
        return;
    }
    if ((value != 1U) && (value != 2U) && (value != 3U)) {
        return;
    }

    s_fdcan_instance = value;
}

FDCAN_HandleTypeDef *bl_fdcan_get_handle(void)
{
    switch (s_fdcan_instance) {
    case 1U:  return &hfdcan1;
    case 3U:  return &hfdcan3;
    case 2U:
    default:  return &hfdcan2;
    }
}

uint8_t bl_fdcan_get_instance_number(void)
{
    return s_fdcan_instance;
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
