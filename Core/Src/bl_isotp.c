/*
 * bl_isotp.c — ISO-TP-style multi-frame transport for the bootloader
 * CAN protocol. See Core/Inc/bl_isotp.h for the wire format and the
 * design rationale.
 *
 * This module is deliberately HAL-free: the RX path consumes raw frame
 * payloads passed in by the dispatcher, and the TX path is a stateless
 * iterator that yields successive frame payloads for the caller to
 * transmit. bl_proto.c owns everything CAN-related (IDs, headers,
 * HAL_FDCAN_AddMessageToTxFifoQ).
 */

#include "bl_isotp.h"

#include <string.h>

/* ---- Internal helpers ---- */

static void reset_idle(bl_isotp_rx_t *rx)
{
    rx->state        = BL_ISOTP_STATE_IDLE;
    rx->peer         = 0U;
    rx->initial_type = 0U;
    rx->total_len    = 0U;
    rx->received     = 0U;
    rx->next_seq     = 0U;
    rx->deadline_ms  = 0U;
    /* rx->buf is left untouched — consumers must read from it using
     * rx->received before the caller resets state. */
}

/* ---- RX ---- */

void bl_isotp_rx_init(bl_isotp_rx_t *rx)
{
    reset_idle(rx);
}

static bl_isotp_rx_status_t handle_sf(bl_isotp_rx_t *rx,
                                      uint8_t type,
                                      uint8_t peer,
                                      const uint8_t *data,
                                      uint8_t length)
{
    /* SF: byte 0 = 0x0L with 1 <= L <= 7, and there must be at least
     *     L + 1 bytes in the frame. */
    uint8_t len = data[0] & BL_ISOTP_PCI_MASK_LO;

    if ((len == 0U) || (len > BL_ISOTP_SF_MAX_LEN) || ((uint8_t)(len + 1U) > length)) {
        return BL_ISOTP_ERR_BAD_PCI;
    }

    /* Any in-flight reassembly is clobbered by a new SF — consistent
     * with standard ISO-TP behaviour for an unexpected SF. */
    reset_idle(rx);

    rx->peer         = peer;
    rx->initial_type = type;
    rx->total_len    = len;
    rx->received     = len;
    memcpy(rx->buf, &data[1], len);

    return BL_ISOTP_MSG_COMPLETE;
}

static bl_isotp_rx_status_t handle_ff(bl_isotp_rx_t *rx,
                                      uint8_t type,
                                      uint8_t peer,
                                      const uint8_t *data,
                                      uint8_t length,
                                      bool *send_fc)
{
    if (length < 8U) {
        /* An FF always fills all 8 bytes; a shorter DLC is malformed. */
        return BL_ISOTP_ERR_BAD_PCI;
    }

    /* Total length = 12 bits across byte 0 low nibble + byte 1.
     * If both are zero this is the ISO-TP 32-bit escape FF — we don't
     * support that in the bootloader. */
    uint16_t total = (uint16_t)(((uint16_t)(data[0] & BL_ISOTP_PCI_MASK_LO) << 8) | data[1]);

    if (total == 0U) {
        return BL_ISOTP_ERR_BAD_PCI;
    }

    if (total > BL_ISOTP_MAX_MSG) {
        return BL_ISOTP_ERR_OVERFLOW;
    }

    /* 6 bytes of payload follow (bytes 2..7). */
    reset_idle(rx);
    rx->peer         = peer;
    rx->initial_type = type;
    rx->total_len    = total;
    rx->next_seq     = 1U;  /* first CF has sequence 1 */
    rx->state        = BL_ISOTP_STATE_WAIT_CF;

    uint16_t first_chunk = (total < 6U) ? total : 6U;
    memcpy(rx->buf, &data[2], first_chunk);
    rx->received = first_chunk;

    /* Caller must send FC(CTS) to keep the host flowing. */
    *send_fc = true;

    /* If the FF already declared a length of <= 6 the message is
     * complete right here (pathological but well-defined). */
    if (rx->received >= rx->total_len) {
        return BL_ISOTP_MSG_COMPLETE;
    }
    return BL_ISOTP_OK;
}

static bl_isotp_rx_status_t handle_cf(bl_isotp_rx_t *rx,
                                      const uint8_t *data,
                                      uint8_t length)
{
    if (rx->state != BL_ISOTP_STATE_WAIT_CF) {
        return BL_ISOTP_ERR_NO_FF;
    }

    uint8_t seq = data[0] & BL_ISOTP_PCI_MASK_LO;
    if (seq != rx->next_seq) {
        return BL_ISOTP_ERR_BAD_SEQ;
    }

    /* CFs carry up to 7 payload bytes (bytes 1..7). The last CF may be
     * short; trust the total_len from FF to decide how many bytes of
     * the frame are meaningful. */
    uint16_t remaining = (uint16_t)(rx->total_len - rx->received);
    uint8_t  frame_avail = (length > 1U) ? (uint8_t)(length - 1U) : 0U;
    uint16_t take = (remaining < frame_avail) ? remaining : frame_avail;

    memcpy(&rx->buf[rx->received], &data[1], take);
    rx->received = (uint16_t)(rx->received + take);
    rx->next_seq = (uint8_t)((rx->next_seq + 1U) & 0x0FU);

    if (rx->received >= rx->total_len) {
        return BL_ISOTP_MSG_COMPLETE;
    }
    return BL_ISOTP_OK;
}

bl_isotp_rx_status_t bl_isotp_rx_feed(bl_isotp_rx_t *rx,
                                      uint8_t type,
                                      uint8_t peer,
                                      const uint8_t *data,
                                      uint8_t length,
                                      bool *send_fc)
{
    *send_fc = false;

    if (length == 0U) {
        return BL_ISOTP_ERR_BAD_PCI;
    }

    uint8_t pci_hi = data[0] & BL_ISOTP_PCI_MASK_HI;

    switch (pci_hi) {
        case BL_ISOTP_PCI_SF:
            return handle_sf(rx, type, peer, data, length);

        case BL_ISOTP_PCI_FF:
            return handle_ff(rx, type, peer, data, length, send_fc);

        case BL_ISOTP_PCI_CF:
            return handle_cf(rx, data, length);

        case BL_ISOTP_PCI_FC:
            /* We never throttle incoming messages, so any FC we receive
             * is a host → device FC that doesn't apply to us. Drop it. */
            return BL_ISOTP_OK;

        default:
            return BL_ISOTP_ERR_BAD_PCI;
    }
}

bool bl_isotp_rx_tick(bl_isotp_rx_t *rx, uint32_t now_ms, uint8_t *peer_out)
{
    if (rx->state != BL_ISOTP_STATE_WAIT_CF) {
        return false;
    }

    /* Signed diff handles tick wrap-around. */
    if ((int32_t)(now_ms - rx->deadline_ms) < 0) {
        return false;
    }

    if (peer_out != (uint8_t *)0) {
        *peer_out = rx->peer;
    }
    reset_idle(rx);
    return true;
}


/* ---- TX ---- */

void bl_isotp_tx_init(bl_isotp_tx_t *tx,
                      const uint8_t *payload,
                      uint16_t length)
{
    tx->payload = payload;
    tx->length  = length;
    tx->sent    = 0U;
    tx->seq     = 1U;  /* first CF is sequence 1 */
}

bool bl_isotp_tx_next(bl_isotp_tx_t *tx, uint8_t frame[8], uint8_t *dlc)
{
    if (tx->sent >= tx->length) {
        return false;
    }

    uint16_t remaining = (uint16_t)(tx->length - tx->sent);

    if (tx->sent == 0U) {
        /* First frame of the message: SF if it fits, FF otherwise. */
        if (tx->length <= BL_ISOTP_SF_MAX_LEN) {
            frame[0] = (uint8_t)(BL_ISOTP_PCI_SF | (tx->length & BL_ISOTP_PCI_MASK_LO));
            memcpy(&frame[1], tx->payload, tx->length);
            *dlc = (uint8_t)(1U + tx->length);
            tx->sent = tx->length;
            return true;
        }

        /* FF: 12-bit total length across byte 0 low nibble + byte 1. */
        frame[0] = (uint8_t)(BL_ISOTP_PCI_FF | ((tx->length >> 8) & BL_ISOTP_PCI_MASK_LO));
        frame[1] = (uint8_t)(tx->length & 0xFFU);
        memcpy(&frame[2], tx->payload, 6U);
        *dlc = 8U;
        tx->sent = 6U;
        return true;
    }

    /* Consecutive frames: up to 7 bytes each, sequence wraps 0..15. */
    uint8_t chunk = (remaining < 7U) ? (uint8_t)remaining : 7U;
    frame[0] = (uint8_t)(BL_ISOTP_PCI_CF | (tx->seq & BL_ISOTP_PCI_MASK_LO));
    memcpy(&frame[1], &tx->payload[tx->sent], chunk);
    *dlc = (uint8_t)(1U + chunk);
    tx->sent = (uint16_t)(tx->sent + chunk);
    tx->seq  = (uint8_t)((tx->seq + 1U) & 0x0FU);
    return true;
}
