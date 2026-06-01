/*
 * bl_proto.c — frame parse/build helpers + dispatch + opcode handlers.
 *
 * Owns the software side of the protocol plumbing:
 *
 *   - addressed-to-us check (defence in depth behind the FDCAN filters)
 *   - a single in-flight ISO-TP reassembly via bl_isotp
 *   - TX helpers for ACK / NACK / FC, all wrapped in ISO-TP framing
 *   - the reassembly timeout tick
 *   - opcode handlers for the Phase 2 set:
 *       CONNECT / DISCONNECT / DISCOVER          — identity & session
 *       FLASH_ERASE / FLASH_WRITE /
 *       FLASH_READ_CRC / FLASH_VERIFY            — programming (via bl_flash)
 *       RESET / JUMP                              — hand-off
 *   - a session-active latch, set by CONNECT and cleared by DISCONNECT.
 *     FLASH_* opcodes refuse to run without it; the remaining opcodes
 *     are session-agnostic so a host can always reach them.
 *
 * HAL dependencies are confined to this file and bl_flash; bl_isotp is
 * HAL-free.
 */

#include "bl_proto.h"

#include "bl_config.h"
#include "bl_dtc.h"
#include "bl_flash.h"
#include "bl_fwinfo.h"
#include "bl_health.h"
#include "bl_isotp.h"
#include "bl_live.h"
#include "bl_log.h"
#include "bl_memmap.h"
#include "bl_node_id.h"
#include "bl_nvm.h"
#include "bl_obyte.h"
#include "bl_fdcan.h"
#include "main.h"
#include "stm32h7xx_hal.h"

#include <string.h>

/* ---- State ---- */

static bl_isotp_rx_t g_rx;

/* Session latch. Set by CONNECT, cleared by DISCONNECT, by an MCU
 * reset, or by the session watchdog. FLASH_* opcodes require this to
 * be true. */
static bool g_session_active = false;

/* Session watchdog. The last-activity timestamp is updated on two
 * kinds of event:
 *
 *   - every addressed frame that passes the FDCAN filter + TYPE/PCI
 *     check (bl_proto_dispatch), so host-side retries and even
 *     rejected commands count as "still alive"
 *   - every ACK we transmit (send_ack), so a multi-second blocking
 *     handler like FLASH_ERASE re-arms the watchdog once it finally
 *     ACKs. Without this, any operation longer than
 *     BL_SESSION_TIMEOUT_MS would trip the watchdog on the very next
 *     main-loop iteration.
 *
 * The value is only consulted while g_session_active; idle bootloaders
 * are not watchdogged. */
static uint32_t g_session_last_activity_ms = 0U;

/* #125 C2: set once a FLASH_ERASE or FLASH_WRITE has run in the
 * current session — i.e. the installed app image may now be
 * partially overwritten. While set, a session timeout must NOT
 * auto-jump into the app: the old metadata can still pass
 * CheckApplication (it's only rewritten on the host's explicit
 * FLASH_VERIFY), so a mid-flash link drop would otherwise launch a
 * half-erased / half-written binary — and recovering from that needs
 * SWD (open the sealed enclosure). Cleared on CONNECT (fresh session)
 * and DISCONNECT. */
static bool g_session_flash_dirty = false;

static void session_touch_activity(void)
{
    g_session_last_activity_ms = HAL_GetTick();
}

static void session_timeout(void)
{
    /* Record the event before tearing the session down — bl_dtc_log
     * reads the current session state (via bl_health_flags) so the
     * timeline in the health record and any NOTIFY_DTC carries the
     * right "was in session" semantics. */
    bl_dtc_log(BL_DTC_SESSION_TIMEOUT, BL_DTC_SEV_WARN, 0U);
    bl_log_warn("session timeout, tearing down");

    bool was_flash_dirty = g_session_flash_dirty;

    g_session_active = false;
    g_session_flash_dirty = false;
    bl_isotp_rx_init(&g_rx);

    /* #125 C2: if a flash mutation happened this session, the host
     * abandoned us mid-reflash. Do NOT jump — the app region may be
     * half-programmed even though stale metadata still validates.
     * Stay in listen mode so a reconnecting host can finish the
     * flash. A clean unattended boot still auto-jumps via the
     * cold-boot window in main.c; this only suppresses the
     * post-interrupted-flash jump.
     *
     * For a pure-diagnostic session (CONNECT + health/discover, no
     * flash), the original behaviour is preserved: hand control to a
     * valid app so a one-off poke doesn't strand the board in BL. */
    if (was_flash_dirty) {
        bl_log_warn("flash-dirty session timed out; staying in bootloader");
        return;
    }

    /* Host abandoned us. If a valid application is installed, hand
     * control over; otherwise stay in listen mode for a future
     * reconnect. No NACK is emitted — silence matches what the host
     * has been doing. */
    if (Bootloader_CheckApplication() == 0U) {
        Bootloader_JumpToApplication();
    }
}

/* ---- Low-level TX plumbing ---- */

bool bl_proto_addressed_to_us(uint8_t dst)
{
    return (dst == bl_node_id_get()) || (dst == BL_PROTO_NODE_BROADCAST);
}

/* Map a 0..8 byte count onto the `FDCAN_DLC_BYTES_N` encoding the
 * HAL expects in `FDCAN_TxHeaderTypeDef::DataLength`.
 *
 * For classic CAN the HAL's `FDCAN_DLC_BYTES_N` constants are just
 * `N` (the byte count in the low nibble); the HAL itself shifts
 * that value into bit position 16 when it packs the message RAM
 * element (see `(pTxHeader->DataLength << 16U)` in the STM32H7 HAL
 * `PrepareTxElement`). Pre-shifting on our side produces a DLC of
 * `N << 16` which the peripheral reads as an out-of-range
 * FD-format length — the TX either silently fails or reads
 * garbage from `DLCtoBytes[N << 16]`.
 *
 * So just return the byte count directly (clamped to 8 for classic
 * CAN). */
static uint32_t dlc_bytes_to_fdcan(uint8_t bytes)
{
    return (bytes > 8U) ? 8U : (uint32_t)bytes;
}

/* TX-FIFO/Queue depth as configured by main.c::MX_FDCAN2_Init's
 * `Init.TxFifoQueueElmtsNbr = 16` setting. Used by wait_tx_drain
 * below; if MX_FDCAN2_Init is ever regenerated with a different
 * value, sync this constant or the drain wait will short-circuit
 * incorrectly. CubeMX preserves USER CODE blocks across regen, so
 * this constant in particular won't drift silently — but a check
 * here is the canonical fence. */
#define BL_FDCAN_TX_QUEUE_DEPTH        16U

/* Block until the FDCAN TX FIFO has drained or `max_ms` has elapsed.
 *
 * HAL_FDCAN_AddMessageToTxFifoQ returns the moment the frame is in
 * the controller's queue — actual transmission is asynchronous. The
 * old "let the ACK drain" pattern of `HAL_Delay(10U)` was hope, not
 * guarantee: under bus load (long ISO-TP CFs queued ahead, low CAN
 * bitrate, arbitration loss) 10 ms can be too short and the host
 * never sees the ACK before NVIC_SystemReset / OB_Launch fires.
 *
 * Free level equals the configured queue depth exactly when every
 * frame has been transmitted. NOTE (#125 M4): AutoRetransmission is
 * ENABLE on this BL (see MX_FDCAN2_Init / issue #94), so a frame that
 * keeps losing arbitration is retried indefinitely rather than
 * aborted — it WILL sit in the queue until it wins the bus or the
 * node faults. The `max_ms` fallback is therefore the real bound: a
 * timeout here means TX did NOT drain (bus contended or offline), not
 * "drained cleanly." We proceed anyway — better to reset / launch and
 * let the host detect via its session timeout than to hang the BL.
 * (The earlier comment here described the AutoRetransmission=DISABLE
 * behaviour, which is stale since the #94 fix flipped it to ENABLE.)
 *
 * Issue #68 ("HAL_Delay-based 'let ACK drain'" bullet). */
static void wait_tx_drain(uint32_t max_ms)
{
    uint32_t start = HAL_GetTick();
    while (HAL_FDCAN_GetTxFifoFreeLevel(bl_fdcan_get_handle()) < BL_FDCAN_TX_QUEUE_DEPTH) {
        if ((HAL_GetTick() - start) >= max_ms) {
            return;  /* timeout — proceed anyway */
        }
    }
}

/* Send one raw CAN frame — always from us (the node) back to the host.
 * The ID is `BL_PROTO_DIR_NODE_TO_HOST | <id>`, where <id> is the
 * resolved node ID (NVM override or compile-time default — see
 * `bl_node_id_get()`). The new wire format has no concept of
 * per-peer node→host addressing (there's exactly one host, and its
 * reserved ID is 0x0 which doesn't show up in any node→host ID).
 * `data` holds `length` valid bytes. Any TX-queue full condition is
 * swallowed silently — the host's session timeout is the backstop. */
static void send_raw(const uint8_t *data, uint8_t length)
{
    FDCAN_TxHeaderTypeDef tx = { 0 };
    tx.Identifier          = bl_proto_build_id(BL_PROTO_DIRECTION_NODE_TO_HOST,
                                               bl_node_id_get());
    tx.IdType              = FDCAN_STANDARD_ID;
    tx.TxFrameType         = FDCAN_DATA_FRAME;
    tx.DataLength          = dlc_bytes_to_fdcan(length);
    tx.ErrorStateIndicator = FDCAN_ESI_ACTIVE;
    tx.BitRateSwitch       = FDCAN_BRS_OFF;
    tx.FDFormat            = FDCAN_CLASSIC_CAN;
    tx.TxEventFifoControl  = FDCAN_NO_TX_EVENTS;
    tx.MessageMarker       = 0;

    /* Count every outgoing CAN frame for live-data telemetry. If the
     * HAL call fails the frame isn't actually on the wire — but we
     * still count the attempt; the host sees the attempt rate. */
    bl_live_frame_tx();

    /* #125 H4: don't silently swallow a TX enqueue failure. A full TX
     * FIFO (depth 16) under bus contention means a frame — possibly an
     * FC(CTS) or an ACK — never reached the wire, which can wedge an
     * in-flight transfer (the host waits for a reply that never comes).
     * We deliberately do NOT add a blocking backpressure wait here: this
     * runs in the main loop, and blocking would stall the RX drain — the
     * exact #123 failure mode. Instead we surface it, edge-triggered
     * (once per contiguous run of failures, re-armed on the next
     * success) so a wedge is diagnosable in the log stream without
     * spamming. A real backpressure mechanism + a drop counter/DTC is a
     * bench-gated follow-up.
     *
     * #120: routed through bl_fdcan_get_handle() so the selected instance
     * (BL_FDCAN_INSTANCE) is used rather than a hardcoded hfdcan2. */
    static uint8_t s_tx_full = 0U;
    if (HAL_FDCAN_AddMessageToTxFifoQ(bl_fdcan_get_handle(), &tx, (uint8_t *)data) != HAL_OK) {
        if (s_tx_full == 0U) {
            s_tx_full = 1U;
            bl_log_warn("FDCAN TX FIFO full — frame dropped (FC/ACK may be lost)");
        }
    } else {
        s_tx_full = 0U;
    }
}

/* Send a logical message to the host. Prepends the `msg_type` byte
 * to `payload` so SF/FF frames carry it as byte[1] (right after the
 * PCI byte) per the fix/12 wire format. CFs inherit the type from
 * their parent FF by virtue of sharing its CAN ID + ISO-TP
 * reassembly state — they never carry msg_type explicitly.
 *
 * The framed buffer is a file-scope static rather than stack-allocated
 * (at 1024 B it would dwarf the main-loop stack budget). Single-
 * threaded bootloader: the main loop drives both RX dispatch and all
 * TX, so there's no reentrancy risk. */
static void send_message(bl_msg_type_t msg_type,
                         const uint8_t *payload,
                         uint16_t length)
{
    static uint8_t framed[BL_ISOTP_MAX_MSG];
    bl_isotp_tx_t  tx;
    uint8_t        frame[8];
    uint8_t        dlc;

    /* Guard against a caller that somehow builds a bigger payload
     * than fits in our framed buffer (minus the one-byte msg_type
     * prefix). Silently drop — matches send_raw's "never block
     * forever on queue pressure" contract. */
    if ((uint32_t)length + 1U > sizeof(framed)) {
        return;
    }
    framed[0] = (uint8_t)msg_type;
    if (length > 0U) {
        memcpy(&framed[1], payload, length);
    }
    uint16_t framed_len = (uint16_t)(length + 1U);

    bl_isotp_tx_init(&tx, framed, framed_len);
    while (bl_isotp_tx_next(&tx, frame, &dlc)) {
        send_raw(frame, dlc);
    }
}

/* Forward declarations for helpers defined further down. */
static uint32_t read_le32(const uint8_t *p);

/* Send a positive ack. `payload[0]` is the opcode being acked;
 * `payload[1..length-1]` is opcode-specific response data. Re-arms
 * the session watchdog so long blocking handlers (FLASH_ERASE) don't
 * appear dead the instant they finally ACK. */
static void send_ack(const uint8_t *payload, uint16_t length)
{
    send_message(BL_MSG_ACK, payload, length);
    session_touch_activity();
}

/* Send a 2-byte NACK payload wrapped in an ISO-TP SF. */
static void send_nack(uint8_t rejected_opcode, uint8_t code)
{
    uint8_t payload[2] = { rejected_opcode, code };
    bl_live_nack_sent();
    send_message(BL_MSG_NACK, payload, (uint16_t)sizeof(payload));
}

/* Log an ISO-TP reassembly error to the DTC table + log stream.
 *
 * Packs four bytes of diagnostic context — see BL_DTC_ISOTP_ERROR in
 * bl_dtc.h for the encoding. The FDCAN RX_FIFO0 fill level in the low
 * byte is the smoking gun for issue #94: if it consistently reads
 * "near full" (≥ depth-2) right when BAD_SEQ fires, host CFs are
 * being dropped at the controller because the BL's main-loop drain
 * rate can't keep up — fix is on the BL side (faster drain or move to
 * ISR-driven RX). If it reads "near empty" with a real seq gap, the
 * drop happened upstream (bridge / driver / cabling). Either case is
 * actionable; the BAD_PCI / OVERFLOW / NO_FF cases each get their own
 * useful context bytes too (see bl_dtc.h). */
static void log_isotp_error(bl_isotp_rx_status_t st)
{
    uint32_t fifo_fill =
        HAL_FDCAN_GetRxFifoFillLevel(bl_fdcan_get_handle(), FDCAN_RX_FIFO0);

    /* v2 encoding: drop fifo_fill from the DTC context (already known
     * to be near-zero from the first bench-replay — see #94) and use
     * that byte slot for the raw data[0] of the failing frame.
     * fifo_fill stays in the log-ring message for completeness. */
    uint32_t ctx =
        ((uint32_t)(uint8_t)st              << 24) |
        ((uint32_t)g_rx.last_err_raw        << 16) |
        ((uint32_t)g_rx.last_err_expected   <<  8) |
        ((uint32_t)g_rx.last_err_observed        );

    bl_dtc_log(BL_DTC_ISOTP_ERROR, BL_DTC_SEV_WARN, ctx);
    bl_log_warn("isotp err st=%u raw=0x%02X exp=0x%02X got=0x%02X fifo=%u",
                (unsigned int)st,
                (unsigned int)g_rx.last_err_raw,
                (unsigned int)g_rx.last_err_expected,
                (unsigned int)g_rx.last_err_observed,
                (unsigned int)fifo_fill);
}

/* Send an FC(CTS, BS=0, STmin=0) back to the host.
 *
 * FCs are pure ISO-TP plumbing — they carry no msg_type byte. They
 * share the same node→host ID as our SF/FF replies; the host's ISO-TP
 * reassembler keys on the PCI nibble (0x3 = FC) to route them. */
static void send_fc_cts(void)
{
    uint8_t frame[3] = {
        (uint8_t)(BL_ISOTP_PCI_FC | BL_ISOTP_FC_CTS),
        BL_ISOTP_FC_BS_DEFAULT,
        BL_ISOTP_FC_STMIN_DEFAULT,
    };
    send_raw(frame, 3U);
}

/* ---- Opcode handlers ---- */

/* CMD_CONNECT: [major, minor]. Major must match exactly; minor is a
 * hint, carried for diagnostics but not validated. On success the
 * session latch goes high and the bootloader echoes back its own
 * (major, minor). */
static void handle_connect(uint8_t peer, const uint8_t *args, uint16_t args_len)
{
    if (args_len < 2U) {
        send_nack(BL_CMD_CONNECT, BL_NACK_UNSUPPORTED);
        return;
    }

    uint8_t host_major = args[0];
    /* args[1] is host_minor — unused today, reserved. */

    if (host_major != BL_PROTO_VERSION_MAJOR) {
        send_nack(BL_CMD_CONNECT, BL_NACK_PROTOCOL_VERSION);
        return;
    }

    g_session_active = true;
    g_session_flash_dirty = false;  /* fresh session: clear flash-dirty latch (#125 C2) */
    session_touch_activity();  /* arm the watchdog starting now */

    bl_log_info("CONNECT from 0x%X (host proto %u.%u)",
                (unsigned int)peer,
                (unsigned int)host_major,
                (unsigned int)args[1]);

    uint8_t resp[3] = {
        BL_CMD_CONNECT,
        BL_PROTO_VERSION_MAJOR,
        BL_PROTO_VERSION_MINOR,
    };
    send_ack(resp, (uint16_t)sizeof(resp));
}

/* CMD_DISCONNECT: no args. Clears the session latch and acks. */
static void handle_disconnect(uint8_t peer, uint16_t args_len)
{
    (void)args_len;
    g_session_active = false;
    g_session_flash_dirty = false;  /* clean teardown: clear flash-dirty latch (#125 C2) */
    uint8_t resp[1] = { BL_CMD_DISCONNECT };
    send_ack(resp, (uint16_t)sizeof(resp));
}

/* CMD_DISCOVER: no args. Reply as TYPE=DISCOVER with this node's ID
 * and the bootloader's protocol version. Richer identity (product
 * name, fw version, build stamp, …) is fetched via CMD_GET_FW_INFO
 * once a host has picked the node it wants to talk to. */
static void handle_discover(uint8_t peer)
{
    (void)peer;  /* there's only ever one peer (the host); kept for
                  * signature symmetry with the other handlers */
    uint8_t resp[4] = {
        BL_CMD_DISCOVER,
        bl_node_id_get(),
        BL_PROTO_VERSION_MAJOR,
        BL_PROTO_VERSION_MINOR,
    };
    send_message(BL_MSG_DISCOVER_REPLY, resp, (uint16_t)sizeof(resp));
}

/* CMD_GET_FW_INFO: no args. Returns the 64-byte __firmware_info record
 * the application publishes at BL_FWINFO_ADDR. The ACK is 65 bytes of
 * payload (1 opcode + 64 record bytes), so it goes out as an ISO-TP
 * First Frame plus nine Consecutive Frames — the first real exercise
 * of multi-frame TX.
 *
 * Errors:
 *   - BL_NACK_NO_VALID_APP   — no valid application is installed
 *   - BL_NACK_UNSUPPORTED    — app is present but has no firmware-info
 *                              record (missing / wrong magic / bad
 *                              record-version). Treated as a host-side
 *                              "this image pre-dates the convention". */
static void handle_get_fw_info(uint8_t peer)
{
    if (Bootloader_CheckApplication() != 0U) {
        send_nack(BL_CMD_GET_FW_INFO, BL_NACK_NO_VALID_APP);
        return;
    }

    const bl_fwinfo_t *info = bl_fwinfo_get();
    if (info == (const bl_fwinfo_t *)0) {
        send_nack(BL_CMD_GET_FW_INFO, BL_NACK_UNSUPPORTED);
        return;
    }

    uint8_t resp[1U + BL_FWINFO_SIZE];
    resp[0] = BL_CMD_GET_FW_INFO;
    memcpy(&resp[1], info, BL_FWINFO_SIZE);
    send_ack(resp, (uint16_t)sizeof(resp));
}

/* CMD_GET_HEALTH: no args. Returns the 32-byte health record built by
 * bl_health. Not session-gated — hosts should be able to poll health
 * at any time (e.g. while deciding whether to open a session). ACK
 * payload is 33 bytes (opcode + record) → one FF + four CFs. */
static void handle_get_health(uint8_t peer)
{
    bl_health_record_t record;
    bl_health_fill_record(&record);

    uint8_t resp[1U + BL_HEALTH_RECORD_SIZE];
    resp[0] = BL_CMD_GET_HEALTH;
    memcpy(&resp[1], &record, BL_HEALTH_RECORD_SIZE);
    send_ack(resp, (uint16_t)sizeof(resp));
}

/* CMD_LOG_STREAM_START: [min_severity]. Session-gated — log streams
 * are push-y and want session discipline. `min_severity` filters out
 * lower-priority entries at the drain boundary. */
static void handle_log_stream_start(uint8_t peer, const uint8_t *args, uint16_t args_len)
{
    if (!g_session_active) {
        send_nack(BL_CMD_LOG_STREAM_START, BL_NACK_BAD_SESSION);
        return;
    }
    uint8_t min_severity = (args_len >= 1U) ? args[0] : BL_LOG_SEV_INFO;
    bl_log_stream_start(min_severity);

    uint8_t resp[1] = { BL_CMD_LOG_STREAM_START };
    send_ack(resp, (uint16_t)sizeof(resp));
}

/* CMD_LOG_STREAM_STOP: no args. Session-gated. The ring contents are
 * left intact — a subsequent START replays whatever is still buffered. */
static void handle_log_stream_stop(uint8_t peer)
{
    if (!g_session_active) {
        send_nack(BL_CMD_LOG_STREAM_STOP, BL_NACK_BAD_SESSION);
        return;
    }
    bl_log_stream_stop();

    uint8_t resp[1] = { BL_CMD_LOG_STREAM_STOP };
    send_ack(resp, (uint16_t)sizeof(resp));
}

/* CMD_LIVE_DATA_START: [rate_hz]. Session-gated. Validates the rate
 * against BL_LIVE_{MIN,MAX}_RATE_HZ; out-of-range earns
 * NACK(UNSUPPORTED). On success the bl_live_tick emitter starts
 * shipping NOTIFY_LIVE_DATA at the chosen cadence. */
static void handle_live_data_start(uint8_t peer, const uint8_t *args, uint16_t args_len)
{
    if (!g_session_active) {
        send_nack(BL_CMD_LIVE_DATA_START, BL_NACK_BAD_SESSION);
        return;
    }
    if (args_len < 1U) {
        send_nack(BL_CMD_LIVE_DATA_START, BL_NACK_UNSUPPORTED);
        return;
    }
    if (!bl_live_stream_start(args[0])) {
        send_nack(BL_CMD_LIVE_DATA_START, BL_NACK_UNSUPPORTED);
        return;
    }

    uint8_t resp[1] = { BL_CMD_LIVE_DATA_START };
    send_ack(resp, (uint16_t)sizeof(resp));
}

/* CMD_LIVE_DATA_STOP: no args. Session-gated. */
static void handle_live_data_stop(uint8_t peer)
{
    if (!g_session_active) {
        send_nack(BL_CMD_LIVE_DATA_STOP, BL_NACK_BAD_SESSION);
        return;
    }
    bl_live_stream_stop();

    uint8_t resp[1] = { BL_CMD_LIVE_DATA_STOP };
    send_ack(resp, (uint16_t)sizeof(resp));
}

/* CMD_DTC_READ: no args. Returns the full DTC table as
 *   [opcode, count_le16, entry_0, entry_1, …]
 * Up to 32 entries × 20 bytes + 3 bytes of header = 643 bytes worst
 * case, well inside the 1024 B reassembly buffer. Not session-gated —
 * diagnostic reads should always be possible, even before CONNECT. */
static void handle_dtc_read(uint8_t peer)
{
    uint8_t resp[1U + 2U + BL_DTC_MAX_ENTRIES * sizeof(bl_dtc_entry_t)];
    resp[0] = BL_CMD_DTC_READ;

    uint16_t payload_len = 0U;
    bl_dtc_serialize(&resp[1], &payload_len);

    send_ack(resp, (uint16_t)(1U + payload_len));
}

/* CMD_DTC_CLEAR: no args. Wipes the DTC table. Session-gated because
 * erasing fault history is destructive. */
static void handle_dtc_clear(uint8_t peer)
{
    if (!g_session_active) {
        send_nack(BL_CMD_DTC_CLEAR, BL_NACK_BAD_SESSION);
        return;
    }
    bl_dtc_clear();

    uint8_t resp[1] = { BL_CMD_DTC_CLEAR };
    send_ack(resp, (uint16_t)sizeof(resp));
}

/* CMD_NVM_READ: [key_le16]. Session-gated — NVM is bootloader
 * administration, not something random hosts should poke. Replies
 * ACK [opcode, len, value…] on hit, NACK(NVM_NOT_FOUND) otherwise. */
static void handle_nvm_read(uint8_t peer, const uint8_t *args, uint16_t args_len)
{
    if (!g_session_active) {
        send_nack(BL_CMD_NVM_READ, BL_NACK_BAD_SESSION);
        return;
    }
    if (args_len < 2U) {
        send_nack(BL_CMD_NVM_READ, BL_NACK_UNSUPPORTED);
        return;
    }

    uint16_t key = (uint16_t)args[0] | ((uint16_t)args[1] << 8);

    uint8_t value[BL_NVM_MAX_VALUE_LEN];
    uint8_t value_len = 0U;
    bl_nvm_status_t st = bl_nvm_read(key, value, sizeof(value), &value_len);
    if (st == BL_NVM_NOT_FOUND) {
        send_nack(BL_CMD_NVM_READ, BL_NACK_NVM_NOT_FOUND);
        return;
    }
    if (st != BL_NVM_OK) {
        send_nack(BL_CMD_NVM_READ, BL_NACK_UNSUPPORTED);
        return;
    }

    /* Reply layout: [opcode, len, value…] */
    uint8_t resp[2U + BL_NVM_MAX_VALUE_LEN];
    resp[0] = BL_CMD_NVM_READ;
    resp[1] = value_len;
    memcpy(&resp[2], value, value_len);
    send_ack(resp, (uint16_t)(2U + value_len));
}

/* CMD_NVM_WRITE: [key_le16, value…]. Session-gated. `len == 0` (no
 * value bytes) is a tombstone — subsequent NVM_READs for `key`
 * return NVM_NOT_FOUND. Flash hardware errors are logged as a DTC
 * so host-side dashboards catch the trend. */
static void handle_nvm_write(uint8_t peer, const uint8_t *args, uint16_t args_len)
{
    if (!g_session_active) {
        send_nack(BL_CMD_NVM_WRITE, BL_NACK_BAD_SESSION);
        return;
    }
    if (args_len < 2U) {
        send_nack(BL_CMD_NVM_WRITE, BL_NACK_UNSUPPORTED);
        return;
    }

    uint16_t key = (uint16_t)args[0] | ((uint16_t)args[1] << 8);
    uint16_t value_len = (uint16_t)(args_len - 2U);
    if (value_len > BL_NVM_MAX_VALUE_LEN) {
        send_nack(BL_CMD_NVM_WRITE, BL_NACK_UNSUPPORTED);
        return;
    }

    bl_nvm_status_t st = bl_nvm_write(key, &args[2], (uint8_t)value_len);
    if (st == BL_NVM_FULL) {
        send_nack(BL_CMD_NVM_WRITE, BL_NACK_NVM_FULL);
        return;
    }
    if (st == BL_NVM_HARDWARE) {
        bl_dtc_log(BL_DTC_FLASH_HW, BL_DTC_SEV_ERROR, BL_NVM_BASE);
        bl_log_error("NVM write hw fail key=0x%04X", (unsigned int)key);
        send_nack(BL_CMD_NVM_WRITE, BL_NACK_FLASH_HW);
        return;
    }
    if (st != BL_NVM_OK) {
        send_nack(BL_CMD_NVM_WRITE, BL_NACK_UNSUPPORTED);
        return;
    }

    uint8_t resp[1] = { BL_CMD_NVM_WRITE };
    send_ack(resp, (uint16_t)sizeof(resp));
}

/* CMD_NVM_FORMAT: [token_le32]. Session-gated and token-gated — wipes
 * sector 7 entirely (every NVM key/value AND the app metadata
 * FLASHWORD). After format, the BL boot path sees no_valid_app and a
 * fresh flash session is required to install a runnable image.
 *
 * Intended for operator-driven recovery when the sector has wedged
 * (e.g. metadata word stuck in a non-erased state and the automatic
 * compact-replace path is itself failing) — for routine use bl_nvm
 * handles this transparently via compaction. */
static void handle_nvm_format(uint8_t peer, const uint8_t *args, uint16_t args_len)
{
    (void)peer;
    if (!g_session_active) {
        send_nack(BL_CMD_NVM_FORMAT, BL_NACK_BAD_SESSION);
        return;
    }
    if (args_len < 4U) {
        send_nack(BL_CMD_NVM_FORMAT, BL_NACK_NVM_WRONG_TOKEN);
        return;
    }

    uint32_t token = read_le32(&args[0]);
    if (token != BL_NVM_FORMAT_TOKEN) {
        bl_log_warn("NVM_FORMAT bad token 0x%08X", (unsigned int)token);
        send_nack(BL_CMD_NVM_FORMAT, BL_NACK_NVM_WRONG_TOKEN);
        return;
    }

    bl_log_warn("NVM_FORMAT erasing sector 7 (NVM + app metadata)");

    bl_nvm_status_t st = bl_nvm_format();
    if (st != BL_NVM_OK) {
        bl_dtc_log(BL_DTC_FLASH_HW, BL_DTC_SEV_ERROR, BL_NVM_BASE);
        bl_log_error("NVM_FORMAT failed (status=%d)", (int)st);
        send_nack(BL_CMD_NVM_FORMAT, BL_NACK_FLASH_HW);
        return;
    }

    uint8_t resp[1] = { BL_CMD_NVM_FORMAT };
    send_ack(resp, (uint16_t)sizeof(resp));
}

/* CMD_RESET: [mode]. Modes:
 *   0 = hard reset via NVIC_SystemReset
 *   1 = soft reset — same as 0 on this family, no distinction in HW
 *   2 = reset and re-enter the bootloader listen loop (RTC->BKP0R magic)
 *   3 = jump directly to the installed application (no reset)
 *
 * Mode 3 is validated before ACK: if the installed app fails
 * Bootloader_CheckApplication the host gets BL_NACK_NO_VALID_APP and
 * the bootloader stays in listen mode. Modes 0..2 always ACK and then
 * trigger the reset. */
static void handle_reset(uint8_t peer, const uint8_t *args, uint16_t args_len)
{
    if (args_len < 1U) {
        send_nack(BL_CMD_RESET, BL_NACK_UNSUPPORTED);
        return;
    }

    uint8_t mode = args[0];
    if (mode > 3U) {
        send_nack(BL_CMD_RESET, BL_NACK_UNSUPPORTED);
        return;
    }

    if (mode == 3U && Bootloader_CheckApplication() != 0U) {
        send_nack(BL_CMD_RESET, BL_NACK_NO_VALID_APP);
        return;
    }

    uint8_t resp[1] = { BL_CMD_RESET };
    send_ack(resp, (uint16_t)sizeof(resp));

    /* Wait for the ACK to actually leave the TX FIFO before resetting
     * the MCU. See wait_tx_drain() comment for the rationale; 50 ms
     * is comfortably more than the time to transmit a 4-byte SF at
     * 125 kbps (the slowest classic-CAN rate we're likely to see)
     * with 16 frames already queued ahead. */
    wait_tx_drain(50U);

    switch (mode) {
        case 0U:
        case 1U:
            NVIC_SystemReset();
            break;

        case 2U: {
            /* Set the boot-request magic in backup register 0 so the
             * next bootloader start-up holds in listen mode instead of
             * auto-jumping. Mirrors the setup Bootloader_IsBootRequestActive
             * performs at boot so this works even before the RTC has
             * been touched. */
            HAL_PWR_EnableBkUpAccess();
            if ((RCC->BDCR & RCC_BDCR_RTCEN) == 0U) {
                __HAL_RCC_RTC_ENABLE();
            }
            RTC->BKP0R = BL_BOOT_REQ_MAGIC;
            NVIC_SystemReset();
            break;
        }

        case 3U:
            Bootloader_JumpToApplication();
            break;

        default:
            break;  /* unreachable — guarded above */
    }
}

/* Small helpers for reading little-endian 32-bit fields out of an
 * ISO-TP args buffer. */
static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

/* Map a bl_flash status to an appropriate NACK code. */
static uint8_t flash_status_to_nack(bl_flash_status_t st)
{
    switch (st) {
        case BL_FLASH_ERR_PROTECTED:     return BL_NACK_PROTECTED_ADDR;
        case BL_FLASH_ERR_OUT_OF_BOUNDS: return BL_NACK_OUT_OF_BOUNDS;
        case BL_FLASH_ERR_UNALIGNED:     return BL_NACK_UNSUPPORTED;
        case BL_FLASH_ERR_HARDWARE:      return BL_NACK_FLASH_HW;
        default:                          return BL_NACK_UNSUPPORTED;
    }
}

/* CMD_FLASH_ERASE: [start_le32, length_le32]. Sector-aligned erase of
 * the range. Requires an active session. Synchronous: this handler
 * blocks for the full HAL erase duration (~1..4 s per 128 KB sector)
 * before replying. */
static void handle_flash_erase(uint8_t peer, const uint8_t *args, uint16_t args_len)
{
    if (!g_session_active) {
        send_nack(BL_CMD_FLASH_ERASE, BL_NACK_BAD_SESSION);
        return;
    }
    /* #125 C2: a flash mutation is now in progress — the app image may
     * become inconsistent. Mark the session so a subsequent timeout
     * won't auto-jump into a half-flashed binary. */
    g_session_flash_dirty = true;
    if (args_len < 8U) {
        send_nack(BL_CMD_FLASH_ERASE, BL_NACK_UNSUPPORTED);
        return;
    }

    uint32_t start  = read_le32(&args[0]);
    uint32_t length = read_le32(&args[4]);
    bl_live_note_flash_addr(start);

    uint32_t sectors = 0U;
    bl_log_info("FLASH_ERASE start=0x%08X len=0x%X",
                (unsigned int)start, (unsigned int)length);
    /* #125 H6: the erase duration is timed INSIDE bl_flash_erase (via
     * DWT — HAL_GetTick can't measure it: the single-bank H733 stalls
     * the CPU on instruction fetch for the whole erase, starving
     * SysTick) and recorded into the health record's max_flash_op_ms.
     * Don't time it here — HAL_GetTick would read ~1 ms regardless. */
    bl_flash_status_t st = bl_flash_erase(start, length, &sectors);
    if (st != BL_FLASH_OK) {
        if (st == BL_FLASH_ERR_HARDWARE) {
            bl_dtc_log(BL_DTC_FLASH_HW, BL_DTC_SEV_ERROR, start);
            bl_log_error("FLASH_ERASE hw fail at 0x%08X", (unsigned int)start);
        } else {
            bl_log_warn("FLASH_ERASE rejected (status=%d)", (int)st);
        }
        send_nack(BL_CMD_FLASH_ERASE, flash_status_to_nack(st));
        return;
    }
    bl_log_info("FLASH_ERASE ok (%u sectors); see health.max_flash_op_ms",
                (unsigned int)sectors);

    uint8_t resp[1] = { BL_CMD_FLASH_ERASE };
    send_ack(resp, (uint16_t)sizeof(resp));
}

/* CMD_FLASH_WRITE: [addr_le32, data...]. Programs `args_len - 4` bytes
 * starting at `addr`. `addr` must be FLASHWORD-aligned; trailing
 * partial FLASHWORD is padded with 0xFF. Requires an active session. */
static void handle_flash_write(uint8_t peer, const uint8_t *args, uint16_t args_len)
{
    if (!g_session_active) {
        send_nack(BL_CMD_FLASH_WRITE, BL_NACK_BAD_SESSION);
        return;
    }
    /* #125 C2: mark the app image as in-flux for the rest of this
     * session (see handle_flash_erase + session_timeout). */
    g_session_flash_dirty = true;
    if (args_len < 5U) {
        /* Need at least the address plus one data byte. */
        send_nack(BL_CMD_FLASH_WRITE, BL_NACK_UNSUPPORTED);
        return;
    }

    uint32_t addr = read_le32(&args[0]);
    const uint8_t *data = &args[4];
    uint16_t       data_len = (uint16_t)(args_len - 4U);
    bl_live_note_flash_addr(addr);

    bl_flash_status_t st = bl_flash_write(addr, data, data_len);
    if (st != BL_FLASH_OK) {
        if (st == BL_FLASH_ERR_HARDWARE) {
            bl_dtc_log(BL_DTC_FLASH_HW, BL_DTC_SEV_ERROR, addr);
        }
        send_nack(BL_CMD_FLASH_WRITE, flash_status_to_nack(st));
        return;
    }

    uint8_t resp[1] = { BL_CMD_FLASH_WRITE };
    send_ack(resp, (uint16_t)sizeof(resp));
}

/* CMD_FLASH_READ_CRC: [addr_le32, length_le32]. Returns CRC32 over the
 * range. Read-only but still gated behind the session — "any FLASH_*
 * requires CONNECT" is a cleaner invariant. */
static void handle_flash_read_crc(uint8_t peer, const uint8_t *args, uint16_t args_len)
{
    if (!g_session_active) {
        send_nack(BL_CMD_FLASH_READ_CRC, BL_NACK_BAD_SESSION);
        return;
    }
    if (args_len < 8U) {
        send_nack(BL_CMD_FLASH_READ_CRC, BL_NACK_UNSUPPORTED);
        return;
    }

    uint32_t addr   = read_le32(&args[0]);
    uint32_t length = read_le32(&args[4]);

    /* CRC is read-only but we still enforce the writable range — a host
     * CRC'ing the bootloader itself would be a protocol abuse. */
    if (!bl_flash_range_is_writable(addr, length)) {
        send_nack(BL_CMD_FLASH_READ_CRC, BL_NACK_OUT_OF_BOUNDS);
        return;
    }

    uint32_t crc = bl_flash_crc32(addr, length);

    uint8_t resp[5] = {
        BL_CMD_FLASH_READ_CRC,
        (uint8_t)(crc        & 0xFFU),
        (uint8_t)((crc >>  8) & 0xFFU),
        (uint8_t)((crc >> 16) & 0xFFU),
        (uint8_t)((crc >> 24) & 0xFFU),
    };
    send_ack(resp, (uint16_t)sizeof(resp));
}

/* CMD_FLASH_VERIFY: [expected_crc_le32, expected_size_le32,
 *                    expected_version_le32]. Recomputes CRC32 over
 * [BL_APP_BASE, BL_APP_BASE + expected_size). On match, stamps the
 * application metadata FLASHWORD so next boot will auto-jump. */
static void handle_flash_verify(uint8_t peer, const uint8_t *args, uint16_t args_len)
{
    if (!g_session_active) {
        send_nack(BL_CMD_FLASH_VERIFY, BL_NACK_BAD_SESSION);
        return;
    }
    if (args_len < 12U) {
        send_nack(BL_CMD_FLASH_VERIFY, BL_NACK_UNSUPPORTED);
        return;
    }

    uint32_t expected_crc     = read_le32(&args[0]);
    uint32_t expected_size    = read_le32(&args[4]);
    uint32_t expected_version = read_le32(&args[8]);

    if (expected_size == 0U || expected_size > BL_APP_SIZE) {
        send_nack(BL_CMD_FLASH_VERIFY, BL_NACK_OUT_OF_BOUNDS);
        return;
    }

    uint32_t computed = bl_flash_crc32(BL_APP_BASE, expected_size);
    if (computed != expected_crc) {
        send_nack(BL_CMD_FLASH_VERIFY, BL_NACK_CRC_MISMATCH);
        return;
    }

    bl_flash_status_t st = bl_flash_write_metadata(expected_size,
                                                    expected_crc,
                                                    expected_version);
    if (st != BL_FLASH_OK) {
        if (st == BL_FLASH_ERR_HARDWARE) {
            bl_dtc_log(BL_DTC_FLASH_HW, BL_DTC_SEV_ERROR, BL_APP_METADATA_ADDR);
        }
        send_nack(BL_CMD_FLASH_VERIFY, flash_status_to_nack(st));
        return;
    }

    uint8_t resp[1] = { BL_CMD_FLASH_VERIFY };
    send_ack(resp, (uint16_t)sizeof(resp));
}

/* CMD_JUMP: [addr_le32]. Phase-2 policy: the address must equal
 * BL_APP_BASE, and the installed app must pass the integrity check.
 * Host tools with a use for jumping elsewhere will land that in a
 * later phase. */
static void handle_jump(uint8_t peer, const uint8_t *args, uint16_t args_len)
{
    if (args_len < 4U) {
        send_nack(BL_CMD_JUMP, BL_NACK_UNSUPPORTED);
        return;
    }

    uint32_t addr = (uint32_t)args[0]
                  | ((uint32_t)args[1] << 8)
                  | ((uint32_t)args[2] << 16)
                  | ((uint32_t)args[3] << 24);

    if (addr != BL_APP_BASE) {
        send_nack(BL_CMD_JUMP, BL_NACK_OUT_OF_BOUNDS);
        return;
    }

    if (Bootloader_CheckApplication() != 0U) {
        send_nack(BL_CMD_JUMP, BL_NACK_NO_VALID_APP);
        return;
    }

    uint8_t resp[1] = { BL_CMD_JUMP };
    send_ack(resp, (uint16_t)sizeof(resp));

    /* Wait for the ACK to leave the TX FIFO before Bootloader_JumpToApp
     * deinitialises FDCAN and yanks the MCU into the app's vector
     * table. Same rationale + timeout as in handle_reset above. */
    wait_tx_drain(50U);
    Bootloader_JumpToApplication();  /* never returns on success */
}

/* CMD_OB_READ: no args. Returns a 16-byte bl_ob_status_t snapshot. Not
 * session-gated — hosts need to be able to probe the current WRP mask
 * before opening a session so the provisioning UI can decide whether
 * OB_APPLY_WRP is needed. ACK payload is 17 bytes (opcode + record) →
 * one FF + two CFs over ISO-TP. */
static void handle_ob_read(uint8_t peer)
{
    bl_ob_status_t status;
    if (bl_obyte_read(&status) != BL_OB_OK) {
        send_nack(BL_CMD_OB_READ, BL_NACK_FLASH_HW);
        return;
    }

    uint8_t resp[1U + BL_OB_STATUS_SIZE];
    resp[0] = BL_CMD_OB_READ;
    memcpy(&resp[1], &status, BL_OB_STATUS_SIZE);
    send_ack(resp, (uint16_t)sizeof(resp));
}

/* CMD_OB_APPLY_WRP: [token_le32, sector_bitmap_le32?]. Session-gated.
 *
 * This is a destructive, non-reversible operation on recent H7 silicon
 * (WRP is cleared only by a full chip erase via external debugger) so
 * the handler insists on the 4-byte BL_OB_APPLY_TOKEN confirmation
 * prefix. A stray frame without the token earns NACK(OB_WRONG_TOKEN),
 * never causes an accidental brick.
 *
 * The sector bitmap is optional. When omitted, the default is 0x01 —
 * protect the bootloader's own sector only, which is the common case
 * during provisioning. Bit N maps to sector N (H7x3 single-bank, 8
 * sectors).
 *
 * On success bl_obyte_apply_wrp calls HAL_FLASH_OB_Launch which resets
 * the MCU and never returns. The ACK is sent first with a short drain
 * delay; if the launch ever does return (documented edge case) we emit
 * NACK(FLASH_HW) so the host isn't left waiting. */
static void handle_ob_apply_wrp(uint8_t peer, const uint8_t *args, uint16_t args_len)
{
    if (!g_session_active) {
        send_nack(BL_CMD_OB_APPLY_WRP, BL_NACK_BAD_SESSION);
        return;
    }
    if (args_len < 4U) {
        send_nack(BL_CMD_OB_APPLY_WRP, BL_NACK_OB_WRONG_TOKEN);
        return;
    }

    uint32_t token = read_le32(&args[0]);
    if (token != BL_OB_APPLY_TOKEN) {
        bl_log_warn("OB_APPLY_WRP bad token 0x%08X", (unsigned int)token);
        send_nack(BL_CMD_OB_APPLY_WRP, BL_NACK_OB_WRONG_TOKEN);
        return;
    }

    uint32_t sector_bitmap = 0x01U;  /* default: protect bootloader sector */
    if (args_len >= 8U) {
        sector_bitmap = read_le32(&args[4]);
    }

    /* #125 C4: this BL's WRP policy protects exactly the bootloader
     * sector (0) and nothing else. WRP'ing an app sector (1..6) would
     * make the ECU un-reflashable over CAN; WRP'ing the NVM sector (7)
     * would break NVM compaction — and on recent H7 silicon WRP clears
     * only via an external-debugger full chip erase (open the sealed
     * enclosure). So we refuse any mask that asks to protect a
     * non-bootloader sector rather than silently bricking
     * re-flashability, and we always force bit 0 on so a host that
     * passes 0x00 can't ship an unprotected bootloader. A future
     * multi-sector provisioning need should be an explicit, separate
     * opcode — never a free-form bitmap on the one-way WRP path. */
    if ((sector_bitmap & ~0x01U) != 0U) {
        bl_log_warn("OB_APPLY_WRP refused unsafe mask=0x%08X (only sector 0 permitted)",
                    (unsigned int)sector_bitmap);
        send_nack(BL_CMD_OB_APPLY_WRP, BL_NACK_UNSUPPORTED);
        return;
    }
    sector_bitmap |= 0x01U;

    bl_log_warn("OB_APPLY_WRP committing mask=0x%08X (reset imminent)",
                (unsigned int)sector_bitmap);

    uint8_t resp[1] = { BL_CMD_OB_APPLY_WRP };
    send_ack(resp, (uint16_t)sizeof(resp));

    /* Wait for the ACK to leave the TX FIFO before HAL_FLASH_OB_Launch
     * (inside bl_obyte_apply_wrp) yanks the MCU into a system reset.
     * Same rationale + timeout as the other terminal opcodes above. */
    wait_tx_drain(50U);

    bl_ob_rc_t rc = bl_obyte_apply_wrp(sector_bitmap);
    /* This point is only reachable if OB_Launch unexpectedly returned —
     * usually means HAL_FLASH_OB_Unlock or HAL_FLASHEx_OBProgram failed
     * before the launch ever happened, so the option bytes weren't
     * touched. We log a DTC + bl_log_error for post-mortem visibility,
     * then fall through silently.
     *
     * Previously this path also emitted send_nack(FLASH_HW). That was a
     * protocol-invariant violation: the host already received a positive
     * ACK above, and a contradictory NACK arriving afterwards would
     * tear the session down on hosts that key off either. The host
     * detects the no-op via session timeout (the BL stops responding
     * because we're hung here, OR — more typically — the bench operator
     * reboots and queries OB_READ to verify nothing changed). #68 OB
     * bullet. */
    bl_dtc_log(BL_DTC_FLASH_HW, BL_DTC_SEV_ERROR, (uint32_t)rc);
    bl_log_error("OB_APPLY_WRP HAL failure rc=%d (no NACK — session "
                 "watchdog will surface the failure)", (int)rc);
}

/* Dispatch a completed ISO-TP message. byte 0 = opcode, remaining bytes
 * = opcode-specific args. Anything unknown earns NACK(UNSUPPORTED). */
static void handle_message(uint8_t peer,
                           uint8_t opcode,
                           const uint8_t *args,
                           uint16_t args_len)
{
    bl_live_note_opcode(opcode);

    switch (opcode) {
        case BL_CMD_CONNECT:
            handle_connect(peer, args, args_len);
            break;
        case BL_CMD_DISCONNECT:
            handle_disconnect(peer, args_len);
            break;
        case BL_CMD_DISCOVER:
            handle_discover(peer);
            break;
        case BL_CMD_GET_FW_INFO:
            handle_get_fw_info(peer);
            break;
        case BL_CMD_GET_HEALTH:
            handle_get_health(peer);
            break;
        case BL_CMD_FLASH_ERASE:
            handle_flash_erase(peer, args, args_len);
            break;
        case BL_CMD_FLASH_WRITE:
            handle_flash_write(peer, args, args_len);
            break;
        case BL_CMD_FLASH_READ_CRC:
            handle_flash_read_crc(peer, args, args_len);
            break;
        case BL_CMD_FLASH_VERIFY:
            handle_flash_verify(peer, args, args_len);
            break;
        case BL_CMD_LOG_STREAM_START:
            handle_log_stream_start(peer, args, args_len);
            break;
        case BL_CMD_LOG_STREAM_STOP:
            handle_log_stream_stop(peer);
            break;
        case BL_CMD_LIVE_DATA_START:
            handle_live_data_start(peer, args, args_len);
            break;
        case BL_CMD_LIVE_DATA_STOP:
            handle_live_data_stop(peer);
            break;
        case BL_CMD_DTC_READ:
            handle_dtc_read(peer);
            break;
        case BL_CMD_DTC_CLEAR:
            handle_dtc_clear(peer);
            break;
        case BL_CMD_NVM_READ:
            handle_nvm_read(peer, args, args_len);
            break;
        case BL_CMD_NVM_WRITE:
            handle_nvm_write(peer, args, args_len);
            break;
        case BL_CMD_NVM_FORMAT:
            handle_nvm_format(peer, args, args_len);
            break;
        case BL_CMD_OB_READ:
            handle_ob_read(peer);
            break;
        case BL_CMD_OB_APPLY_WRP:
            handle_ob_apply_wrp(peer, args, args_len);
            break;
        case BL_CMD_RESET:
            handle_reset(peer, args, args_len);
            break;
        case BL_CMD_JUMP:
            handle_jump(peer, args, args_len);
            break;
        default:
            send_nack(opcode, BL_NACK_UNSUPPORTED);
            break;
    }
}

/* ---- Public API ---- */

void bl_proto_dispatch(const bl_proto_id_t *id,
                       const uint8_t *data,
                       uint8_t length)
{
    /* Defence in depth behind the FDCAN filter: the filter already
     * rejects anything that isn't host→node and addressed to us, but
     * keep the software-level guard so unit tests and any future
     * software-routed code path (loopback harnesses, simulators)
     * stay correct. */
    if (id->direction != BL_PROTO_DIRECTION_HOST_TO_NODE
        || !bl_proto_addressed_to_us(id->node)
        || length == 0U) {
        return;
    }

    /* PCI gate: accept SF/FF (start of a new message) and CF/FC
     * (continuation / flow control of an in-flight one). Anything
     * else is malformed. Under the fix/12 wire format message type
     * lives in the reassembled payload, not the ID, so the PCI byte
     * is the only thing we key on at the frame level.
     *
     * On reject we emit NACK(TRANSPORT_ERROR) rather than silently
     * dropping — gives the host (or a fuzzer) the same diagnostic
     * visibility every other transport-level error path provides,
     * and prevents the dispatcher from looking like a black hole
     * under malformed input. The earlier gate (direction +
     * addressed_to_us + length>0) already filtered everything not
     * meant for us, so we know the host expected a reply. The
     * rejected_opcode is 0 because the frame failed before a
     * reassembled message exposed one. Issue #60. */
    uint8_t pci_hi = data[0] & BL_ISOTP_PCI_MASK_HI;
    if (pci_hi != BL_ISOTP_PCI_SF && pci_hi != BL_ISOTP_PCI_FF
        && pci_hi != BL_ISOTP_PCI_CF && pci_hi != BL_ISOTP_PCI_FC) {
        send_nack(0x00U, BL_NACK_TRANSPORT_ERROR);
        return;
    }

    /* Any frame that made it through the PCI gate counts as session
     * activity — including frames that end up getting NACKed. A host
     * that's still transmitting, even unsuccessfully, is still alive. */
    session_touch_activity();
    bl_live_frame_rx();

    /* `type` and `peer` args on bl_isotp_rx_feed are legacy
     * bookkeeping — the new wire format has exactly one peer (the
     * host, node 0x0) and no type-in-ID, so we pass constants. The
     * reassembler stores them for later inspection but no code path
     * in the new format actually reads them back.
     *
     * now_ms is passed through so the reassembler can arm its own
     * deadline on FF acceptance (fix/14 / issue #54). Before this the
     * dispatcher armed it externally right after send_fc_cts() — the
     * invariant lived in the caller, which left a footgun for any
     * future caller (e.g. unit tests, loopback simulators) that
     * forgot to stamp it. */
    bool                 send_fc = false;
    bl_isotp_rx_status_t st      = bl_isotp_rx_feed(&g_rx,
                                                     (uint8_t)BL_MSG_CMD,
                                                     BL_PROTO_NODE_HOST,
                                                     HAL_GetTick(),
                                                     data,
                                                     length,
                                                     &send_fc);

    if (send_fc) {
        send_fc_cts();
    }

    switch (st) {
        case BL_ISOTP_MSG_COMPLETE:
            /* Reassembled layout:
             *   g_rx.buf[0]   = msg_type
             *   g_rx.buf[1]   = opcode
             *   g_rx.buf[2..] = args
             *
             * Route by msg_type:
             *   CMD, DISCOVER_REQUEST → dispatch to handle_message
             *   APP_CTRL              → silently drop (BL transit)
             *   ACK / NACK / NOTIFY / DISCOVER_REPLY → silently drop
             *     (these are node→host types; seeing one here means
             *     either a malformed frame or a loopback harness
             *     feeding us our own TX — nothing actionable)
             *   unknown (>= 0x07)     → silently drop (reserved for
             *     future protocol extensions; keeping us quiet here
             *     lets the host adopt new types without breaking
             *     older BL firmware) */
            if (g_rx.received < 2U) {
                /* Need at least msg_type + opcode. Anything shorter
                 * is truncated; report it so the host can resync. */
                send_nack(0xFFU, BL_NACK_TRANSPORT_ERROR);
            } else {
                uint8_t msg_type_byte = g_rx.buf[0];
                uint8_t opcode        = g_rx.buf[1];
                const uint8_t *args   = &g_rx.buf[2];
                uint16_t       args_len = (uint16_t)(g_rx.received - 2U);

                switch (msg_type_byte) {
                    case BL_MSG_CMD:
                    case BL_MSG_DISCOVER_REQUEST:
                        handle_message(BL_PROTO_NODE_HOST,
                                       opcode,
                                       args,
                                       args_len);
                        break;

                    case BL_MSG_APP_CTRL:
                    case BL_MSG_ACK:
                    case BL_MSG_NACK:
                    case BL_MSG_NOTIFY:
                    case BL_MSG_DISCOVER_REPLY:
                    default:
                        /* Silent drop — see comment block above. */
                        break;
                }
            }
            bl_isotp_rx_init(&g_rx);
            break;

        case BL_ISOTP_OK:
            break;

        case BL_ISOTP_ERR_TIMEOUT:
            /* Only _tick returns this; _feed never does. Handle
             * defensively anyway. */
            log_isotp_error(st);
            send_nack(0xFFU, BL_NACK_TRANSPORT_TIMEOUT);
            bl_isotp_rx_init(&g_rx);
            break;

        default:
            log_isotp_error(st);
            send_nack(0xFFU, BL_NACK_TRANSPORT_ERROR);
            bl_isotp_rx_init(&g_rx);
            break;
    }
}

void bl_proto_tick(uint32_t now_ms)
{
    uint8_t peer = 0U;
    if (bl_isotp_rx_tick(&g_rx, now_ms, &peer)) {
        log_isotp_error(BL_ISOTP_ERR_TIMEOUT);
        send_nack(0xFFU, BL_NACK_TRANSPORT_TIMEOUT);
    }

    /* Session watchdog — only runs while a session is active. Unsigned
     * subtraction wraps around the 32-bit HAL_GetTick roll-over
     * correctly: it measures elapsed milliseconds since the last
     * activity, not an absolute deadline. */
    if (g_session_active) {
        uint32_t elapsed = now_ms - g_session_last_activity_ms;
        if (elapsed >= BL_SESSION_TIMEOUT_MS) {
            session_timeout();
        }
    }
}

bool bl_proto_session_active(void)
{
    return g_session_active;
}

void bl_proto_send_notify(const uint8_t *payload, uint16_t length)
{
    /* #123 (general fix): never inject an unsolicited NOTIFY while an
     * inbound multi-frame reassembly is in flight. The heartbeat is
     * not the only emitter — NOTIFY_LIVE_DATA (multi-frame, up to
     * 50 Hz), NOTIFY_LOG (multi-frame, up to 257 B) and NOTIFY_DTC
     * all reach here, and any of them dropped onto the bus mid-
     * WRITE_CHUNK strands the host's transfer exactly as the heartbeat
     * did: with ISO-TP Flow Control BlockSize=0 the host streams a
     * whole chunk without pausing, so a mid-transfer NOTIFY desyncs
     * the exchange → 1 s reassembly timeout → NACK 0x09.
     *
     * Suppressing here (the single choke point for every NOTIFY)
     * rather than per-emitter means a future NOTIFY source can't
     * reintroduce the bug. A suppressed periodic NOTIFY is simply
     * dropped — the next tick re-emits a fresh one once the bus is
     * idle. `bl_proto_isotp_rx_progress() != 0` is true exactly while
     * g_rx is in WAIT_CF (mid-reassembly). The heartbeat additionally
     * has its own quiet-link gate in bl_health_tick covering the
     * brief idle gaps between chunks. */
    if (bl_proto_isotp_rx_progress() != 0U) {
        return;
    }

    /* NOTIFY always goes to the host. Unsolicited — does not
     * refresh the session watchdog. */
    send_message(BL_MSG_NOTIFY, payload, length);
}

uint32_t bl_proto_session_age_ms(uint32_t now_ms)
{
    if (!g_session_active) {
        return 0U;
    }
    return now_ms - g_session_last_activity_ms;
}

uint16_t bl_proto_isotp_rx_progress(void)
{
    return (g_rx.state == BL_ISOTP_STATE_WAIT_CF) ? (uint16_t)g_rx.received : 0U;
}
