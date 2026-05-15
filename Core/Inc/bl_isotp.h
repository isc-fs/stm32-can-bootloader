#ifndef BL_ISOTP_H
#define BL_ISOTP_H

/*
 * ISO-TP-style multi-frame transport over classic CAN.
 *
 * Every protocol frame (regardless of message TYPE) carries a PCI byte
 * as byte 0 of its 8-byte payload. The high nibble of that byte picks
 * one of four frame kinds:
 *
 *   Single Frame    0x0L   L = payload length 1..7
 *   First Frame     0x1H   H = high nibble of 12-bit total length
 *                           byte 1 = low byte of total length
 *   Consecutive     0x2N   N = sequence number 0..15 (wraps)
 *   Flow Control    0x3S   S = 0 CTS / 1 Wait / 2 Overflow
 *                           byte 1 = BlockSize, byte 2 = SeparationTime
 *
 * Convention on this bus (matches the spec):
 *   - A multi-frame message's FF keeps the original message TYPE
 *     (CMD / NOTIFY / ACK / NACK / …).
 *   - Subsequent CFs and the receiver's FC reply use TYPE = DATA.
 *
 * The bootloader's RX reassembler is state-based and doesn't check
 * TYPE on CFs — it binds a reassembly by (peer, state) and tolerates
 * either convention on the wire.
 *
 * Feature scope in feat/6-isotp:
 *   - RX reassembly of SF / FF + CF, bounded by BL_ISOTP_MAX_MSG bytes.
 *   - Receiver sends FC(CTS, BS=0, STmin=0) immediately on FF. No real
 *     backpressure — adding FC(Wait) during flash erase is feat/8 work.
 *   - Total reassembly timeout of BL_ISOTP_TIMEOUT_MS (1 s by default),
 *     collapsing ISO 15765's N_Bs / N_Cr sub-timers for simplicity.
 *   - TX segmentation: a stateless iterator that yields successive
 *     8-byte frame payloads (SF, or FF followed by CFs) from a caller-
 *     owned buffer. The caller wraps each frame in the right CAN header
 *     and transmits it — bl_isotp has no HAL dependency.
 *
 * Not in this branch:
 *   - ISO-TP 32-bit escape FF (messages > 4095 bytes).
 *   - Parallel reassemblies / multi-session.
 *   - Sender-side FC handling (we ignore received FC frames).
 */

#include <stdbool.h>
#include <stdint.h>

/* ---- Maximum reassembled message size (bytes) ---- */
#ifndef BL_ISOTP_MAX_MSG
#define BL_ISOTP_MAX_MSG            1024U
#endif

/* ---- PCI field layout ---- */
#define BL_ISOTP_PCI_MASK_HI        0xF0U
#define BL_ISOTP_PCI_MASK_LO        0x0FU

#define BL_ISOTP_PCI_SF             0x00U
#define BL_ISOTP_PCI_FF             0x10U
#define BL_ISOTP_PCI_CF             0x20U
#define BL_ISOTP_PCI_FC             0x30U

/* ---- Single-frame payload limit (bytes 1..7 of an 8-byte CAN frame) ---- */
#define BL_ISOTP_SF_MAX_LEN         7U

/* ---- FC status codes (low nibble of PCI byte) ---- */
#define BL_ISOTP_FC_CTS             0x00U
#define BL_ISOTP_FC_WAIT            0x01U
#define BL_ISOTP_FC_OVERFLOW        0x02U

/* ---- Defaults this bootloader advertises in every FC it emits ---- */
#define BL_ISOTP_FC_BS_DEFAULT      0x00U   /* 0 = send every CF without another FC */
#define BL_ISOTP_FC_STMIN_DEFAULT   0x00U   /* 0 = no minimum inter-CF gap */

/* ---- Reassembly timeout (milliseconds) between FF and COMPLETE ---- */
#ifndef BL_ISOTP_TIMEOUT_MS
#define BL_ISOTP_TIMEOUT_MS         1000U
#endif

/* ---- Status returned by bl_isotp_rx_feed ---- */
typedef enum {
    BL_ISOTP_OK            = 0,  /* frame consumed, no completed message  */
    BL_ISOTP_MSG_COMPLETE  = 1,  /* rx->buf now holds a full message      */
    BL_ISOTP_ERR_BAD_PCI   = 2,  /* malformed PCI byte or length          */
    BL_ISOTP_ERR_BAD_SEQ   = 3,  /* CF arrived with wrong sequence number */
    BL_ISOTP_ERR_OVERFLOW  = 4,  /* declared length > BL_ISOTP_MAX_MSG    */
    BL_ISOTP_ERR_NO_FF     = 5,  /* CF arrived with no FF active          */
    BL_ISOTP_ERR_TIMEOUT   = 6,  /* reassembly timed out (from _tick)     */
} bl_isotp_rx_status_t;

/* ---- RX state ---- */
typedef enum {
    BL_ISOTP_STATE_IDLE,
    BL_ISOTP_STATE_WAIT_CF,
} bl_isotp_rx_state_t;

typedef struct {
    bl_isotp_rx_state_t state;
    uint8_t  peer;                          /* node ID we're reassembling from */
    uint8_t  initial_type;                  /* TYPE captured from FF           */
    uint16_t total_len;                     /* declared by FF, bytes           */
    uint16_t received;                      /* bytes filled into buf so far    */
    uint8_t  next_seq;                      /* expected CF sequence 0..15      */
    uint32_t deadline_ms;                   /* HAL_GetTick() target            */
    /* Diagnostics carried OUT of bl_isotp_rx_feed when a frame is
     * rejected, so the dispatcher can DTC-log the failure context
     * without bl_isotp depending on bl_dtc. All three reset to 0 by
     * reset_idle() so stale values from a prior reassembly cannot
     * bleed into the current error's DTC entry. Issue #94. */
    uint8_t  last_err_expected;             /* BAD_SEQ: expected next_seq      */
    uint8_t  last_err_observed;             /* BAD_SEQ: got_seq (masked low nib);
                                             * BAD_PCI: 0                      */
    uint8_t  last_err_raw;                  /* full data[0] byte of the frame
                                             * that triggered the reject
                                             * (added in v2 so the host can
                                             * tell when the BL saw a frame
                                             * different from the one a clean
                                             * candump suggests was sent)     */
    uint8_t  buf[BL_ISOTP_MAX_MSG];
} bl_isotp_rx_t;

/* Reset the RX state to IDLE and clear all bookkeeping. */
void bl_isotp_rx_init(bl_isotp_rx_t *rx);

/*
 * Feed one received frame's payload into the reassembler.
 *
 *   type     = message TYPE from the CAN ID (used to stamp reassembly)
 *   peer     = source node ID from the CAN ID
 *   now_ms   = current millisecond tick (HAL_GetTick()); used to arm
 *              the reassembly deadline when an FF is accepted, so
 *              bl_isotp_rx_tick() can later observe a real timeout
 *   data     = CAN frame payload (starts with PCI byte)
 *   length   = number of valid bytes in `data` (1..8)
 *   send_fc  = out-param, set to true if the caller should reply with
 *              FC(CTS) after this feed returned (fires on FF consumed)
 *
 * Returns BL_ISOTP_MSG_COMPLETE when rx->buf / rx->received hold a full
 * message ready for the dispatcher, BL_ISOTP_OK for partial progress,
 * or one of the error codes. Error codes never leave partial state in
 * the buffer — the caller should still invoke bl_isotp_rx_init after
 * handling the error.
 *
 * Encapsulation note: prior to fix/14 the deadline was armed externally
 * by the dispatcher after every FF. That worked but left the invariant
 * dependent on caller discipline — any future caller (test harness,
 * loopback simulator, secondary transport) that forgot the stamp would
 * see the reassembler time out instantly. Threading now_ms in here
 * makes the lifetime correct by construction.
 */
bl_isotp_rx_status_t bl_isotp_rx_feed(bl_isotp_rx_t *rx,
                                      uint8_t type,
                                      uint8_t peer,
                                      uint32_t now_ms,
                                      const uint8_t *data,
                                      uint8_t length,
                                      bool *send_fc);

/*
 * Tick the reassembly timeout. Call periodically from the main loop with
 * the current millisecond tick. Returns true exactly once when an
 * in-flight reassembly has just timed out; the caller should NACK the
 * peer (stored in `*peer_out`) and the state has already been reset
 * internally.
 */
bool bl_isotp_rx_tick(bl_isotp_rx_t *rx, uint32_t now_ms, uint8_t *peer_out);


/* ---- TX segmenter ---- */

typedef struct {
    const uint8_t *payload;
    uint16_t length;
    uint16_t sent;
    uint8_t  seq;
} bl_isotp_tx_t;

/* Initialise a TX iterator over `length` bytes of `payload`. length
 * must be in 1..4095. */
void bl_isotp_tx_init(bl_isotp_tx_t *tx,
                      const uint8_t *payload,
                      uint16_t length);

/*
 * Emit the next CAN-frame payload. Writes between 1 and 8 valid bytes
 * into `frame` and sets `*dlc` to the byte count. Returns true if a
 * frame was produced, false when the iterator is done.
 *
 * The caller owns the CAN framing — it chooses the 11-bit ID (type +
 * src + dst) for each frame and transmits it.
 */
bool bl_isotp_tx_next(bl_isotp_tx_t *tx, uint8_t frame[8], uint8_t *dlc);

#endif /* BL_ISOTP_H */
