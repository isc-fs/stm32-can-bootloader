/*
 * bl_proto.c — frame parse/build helpers + dispatch skeleton.
 *
 * This file is intentionally minimal in feat/5-frame-layout. It owns
 * the software side of the protocol plumbing (addressed-to-us check,
 * NACK sender, dispatcher entry point) but has no opcode handlers yet.
 * Handlers land in later Phase 2 branches (see bl_proto.h).
 */

#include "bl_proto.h"

#include "bl_config.h"
#include "main.h"
#include "stm32h7xx_hal.h"

extern FDCAN_HandleTypeDef hfdcan2;

bool bl_proto_addressed_to_us(uint8_t dst) {
    return (dst == BL_NODE_ID) || (dst == BL_PROTO_NODE_BROADCAST);
}

/* Send a 2-byte NACK back to the frame's source.
 *   tx ID   = (NACK, src=BL_NODE_ID, dst=received.src)
 *   payload = [rejected_opcode, nack_code]
 *
 * If the TX FIFO is full or the call otherwise fails we silently drop
 * the reply — the host-side session timeout will catch it.
 */
static void bl_proto_send_nack(uint8_t dst,
                               uint8_t rejected_opcode,
                               uint8_t code) {
    FDCAN_TxHeaderTypeDef tx = { 0 };
    uint8_t payload[2];

    tx.Identifier          = bl_proto_build_id(BL_PROTO_TYPE_NACK,
                                               BL_NODE_ID,
                                               dst);
    tx.IdType              = FDCAN_STANDARD_ID;
    tx.TxFrameType         = FDCAN_DATA_FRAME;
    tx.DataLength          = FDCAN_DLC_BYTES_2;
    tx.ErrorStateIndicator = FDCAN_ESI_ACTIVE;
    tx.BitRateSwitch       = FDCAN_BRS_OFF;
    tx.FDFormat            = FDCAN_CLASSIC_CAN;
    tx.TxEventFifoControl  = FDCAN_NO_TX_EVENTS;
    tx.MessageMarker       = 0;

    payload[0] = rejected_opcode;
    payload[1] = code;

    (void)HAL_FDCAN_AddMessageToTxFifoQ(&hfdcan2, &tx, payload);
}

void bl_proto_dispatch(const bl_proto_id_t *id,
                       const uint8_t *data,
                       uint8_t length) {
    /* FDCAN filters already reject frames not addressed to us; keep a
     * software check so future software-routed paths stay honest. */
    if (!bl_proto_addressed_to_us(id->dst)) {
        return;
    }

    /* Phase 2 placeholder — no opcodes implemented yet. Reply to every
     * frame with a generic UNSUPPORTED NACK so hosts get fast feedback
     * while the handler branches land. */
    uint8_t opcode = (length > 0U) ? data[0] : 0xFFU;
    bl_proto_send_nack(id->src, opcode, BL_NACK_UNSUPPORTED);
}
