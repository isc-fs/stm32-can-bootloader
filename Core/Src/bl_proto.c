/*
 * bl_proto.c — frame parse/build helpers + dispatch.
 *
 * Owns the software side of the protocol plumbing:
 *
 *   - addressed-to-us check (defence in depth behind the FDCAN filters)
 *   - a single in-flight ISO-TP reassembly via bl_isotp
 *   - handoff of completed messages to a single-frame handler (still a
 *     stub in feat/6 — opcode handlers land in feat/7 and feat/8)
 *   - NACK / FC transmitters, both wrapped in ISO-TP SF / FC framing
 *   - periodic tick that fires the reassembly timeout
 *
 * HAL dependencies are confined to this file; bl_isotp is HAL-free.
 */

#include "bl_proto.h"

#include "bl_config.h"
#include "bl_isotp.h"
#include "main.h"
#include "stm32h7xx_hal.h"

#include <string.h>

extern FDCAN_HandleTypeDef hfdcan2;

/* ---- Single in-flight reassembly ---- */
static bl_isotp_rx_t g_rx;

/* ---- Internal helpers ---- */

bool bl_proto_addressed_to_us(uint8_t dst)
{
    return (dst == BL_NODE_ID) || (dst == BL_PROTO_NODE_BROADCAST);
}

/* Map a 0..8 byte count onto the FDCAN_DLC_BYTES_N encoding. */
static uint32_t dlc_bytes_to_fdcan(uint8_t bytes)
{
    /* FDCAN_DLC_BYTES_N is (N << 16) for N in 0..8. */
    uint32_t n = (bytes > 8U) ? 8U : (uint32_t)bytes;
    return n << 16;
}

/* Send one raw CAN frame. The 11-bit ID is built from (type, BL_NODE_ID,
 * dst); `data` holds `length` valid bytes. Any TX-queue full condition
 * is swallowed silently — the host's session timeout is the backstop. */
static void send_raw(uint8_t type,
                     uint8_t dst,
                     const uint8_t *data,
                     uint8_t length)
{
    FDCAN_TxHeaderTypeDef tx = { 0 };
    tx.Identifier          = bl_proto_build_id((bl_proto_type_t)type,
                                               BL_NODE_ID,
                                               dst);
    tx.IdType              = FDCAN_STANDARD_ID;
    tx.TxFrameType         = FDCAN_DATA_FRAME;
    tx.DataLength          = dlc_bytes_to_fdcan(length);
    tx.ErrorStateIndicator = FDCAN_ESI_ACTIVE;
    tx.BitRateSwitch       = FDCAN_BRS_OFF;
    tx.FDFormat            = FDCAN_CLASSIC_CAN;
    tx.TxEventFifoControl  = FDCAN_NO_TX_EVENTS;
    tx.MessageMarker       = 0;

    (void)HAL_FDCAN_AddMessageToTxFifoQ(&hfdcan2, &tx, (uint8_t *)data);
}

/* Send a logical message to `dst`. If the payload fits in a single
 * frame (<= 7 bytes) one SF frame is emitted; otherwise an FF followed
 * by CFs. FC-based flow control on the sender side is not implemented
 * — the bootloader emits back-to-back CFs and trusts the host's
 * permissive CTS. That's fine for Phase 2 since the only multi-frame
 * TX path the bootloader takes today is "none". */
static void send_message(bl_proto_type_t type,
                         uint8_t dst,
                         const uint8_t *payload,
                         uint16_t length)
{
    bl_isotp_tx_t tx;
    uint8_t       frame[8];
    uint8_t       dlc;

    bl_isotp_tx_init(&tx, payload, length);
    while (bl_isotp_tx_next(&tx, frame, &dlc)) {
        /* SF and FF keep the original message TYPE; CFs and FCs go
         * out with TYPE = DATA per the spec. */
        uint8_t pci_hi     = frame[0] & BL_ISOTP_PCI_MASK_HI;
        uint8_t frame_type = (pci_hi == BL_ISOTP_PCI_SF || pci_hi == BL_ISOTP_PCI_FF)
                                ? (uint8_t)type
                                : (uint8_t)BL_PROTO_TYPE_DATA;
        send_raw(frame_type, dst, frame, dlc);
    }
}

/* Send a 2-byte NACK payload wrapped in an ISO-TP SF. */
static void send_nack(uint8_t dst, uint8_t rejected_opcode, uint8_t code)
{
    uint8_t payload[2] = { rejected_opcode, code };
    send_message(BL_PROTO_TYPE_NACK, dst, payload, (uint16_t)sizeof(payload));
}

/* Send an FC(CTS, BS=0, STmin=0) to `dst`. FC frames always travel
 * with TYPE = DATA per the spec. */
static void send_fc_cts(uint8_t dst)
{
    uint8_t frame[3] = {
        (uint8_t)(BL_ISOTP_PCI_FC | BL_ISOTP_FC_CTS),
        BL_ISOTP_FC_BS_DEFAULT,
        BL_ISOTP_FC_STMIN_DEFAULT,
    };
    send_raw((uint8_t)BL_PROTO_TYPE_DATA, dst, frame, 3U);
}

/* Phase 2 placeholder: once a complete message has been reassembled
 * this is where opcode dispatch happens. feat/7 replaces the body
 * with real handlers; until then every message gets NACKed. */
static void handle_message(uint8_t peer,
                           uint8_t opcode,
                           const uint8_t *args,
                           uint16_t args_len)
{
    (void)args;
    (void)args_len;
    send_nack(peer, opcode, BL_NACK_UNSUPPORTED);
}

/* ---- Public API ---- */

void bl_proto_dispatch(const bl_proto_id_t *id,
                       const uint8_t *data,
                       uint8_t length)
{
    if (!bl_proto_addressed_to_us(id->dst) || length == 0U) {
        return;
    }

    /* Only accept TYPE/PCI combinations the protocol actually uses:
     *   - CMD + SF : single-frame command
     *   - CMD + FF : start of a multi-frame command
     *   - DATA + CF: continuation of an in-flight message
     *   - DATA + FC: flow-control reply (ignored on RX today)
     * Everything else is silently dropped. */
    uint8_t pci_hi = data[0] & BL_ISOTP_PCI_MASK_HI;
    bool cmd_initial       = (id->type == BL_PROTO_TYPE_CMD)
                           && (pci_hi == BL_ISOTP_PCI_SF || pci_hi == BL_ISOTP_PCI_FF);
    bool data_continuation = (id->type == BL_PROTO_TYPE_DATA)
                           && (pci_hi == BL_ISOTP_PCI_CF || pci_hi == BL_ISOTP_PCI_FC);
    if (!cmd_initial && !data_continuation) {
        return;
    }

    bool                 send_fc = false;
    bl_isotp_rx_status_t st      = bl_isotp_rx_feed(&g_rx,
                                                     (uint8_t)id->type,
                                                     id->src,
                                                     data,
                                                     length,
                                                     &send_fc);

    if (send_fc) {
        /* Arm the reassembly deadline at the same instant we tell the
         * host it may proceed with CFs. */
        g_rx.deadline_ms = HAL_GetTick() + BL_ISOTP_TIMEOUT_MS;
        send_fc_cts(id->src);
    }

    switch (st) {
        case BL_ISOTP_MSG_COMPLETE:
            /* The first byte of the reassembled message is the opcode. */
            if (g_rx.received >= 1U) {
                handle_message(id->src,
                               g_rx.buf[0],
                               &g_rx.buf[1],
                               (uint16_t)(g_rx.received - 1U));
            } else {
                send_nack(id->src, 0xFFU, BL_NACK_TRANSPORT_ERROR);
            }
            bl_isotp_rx_init(&g_rx);
            break;

        case BL_ISOTP_OK:
            /* Still reassembling — nothing to emit. */
            break;

        case BL_ISOTP_ERR_TIMEOUT:
            /* Only _tick returns this; _feed never does. Still, handle
             * defensively: the timeout path resets state and NACKs. */
            send_nack(id->src, 0xFFU, BL_NACK_TRANSPORT_TIMEOUT);
            bl_isotp_rx_init(&g_rx);
            break;

        default:
            send_nack(id->src, 0xFFU, BL_NACK_TRANSPORT_ERROR);
            bl_isotp_rx_init(&g_rx);
            break;
    }
}

void bl_proto_tick(uint32_t now_ms)
{
    uint8_t peer = 0U;
    if (bl_isotp_rx_tick(&g_rx, now_ms, &peer)) {
        send_nack(peer, 0xFFU, BL_NACK_TRANSPORT_TIMEOUT);
    }
}
