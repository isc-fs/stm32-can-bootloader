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
#include "bl_flash.h"
#include "bl_isotp.h"
#include "bl_memmap.h"
#include "main.h"
#include "stm32h7xx_hal.h"

#include <string.h>

extern FDCAN_HandleTypeDef hfdcan2;

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

static void session_touch_activity(void)
{
    g_session_last_activity_ms = HAL_GetTick();
}

static void session_timeout(void)
{
    g_session_active = false;
    bl_isotp_rx_init(&g_rx);

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
    return (dst == BL_NODE_ID) || (dst == BL_PROTO_NODE_BROADCAST);
}

/* Map a 0..8 byte count onto the FDCAN_DLC_BYTES_N encoding. */
static uint32_t dlc_bytes_to_fdcan(uint8_t bytes)
{
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
 * yet — the bootloader emits back-to-back CFs and trusts the host's
 * permissive CTS. */
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

/* Send a positive ack. `payload[0]` is the opcode being acked;
 * `payload[1..length-1]` is opcode-specific response data. Re-arms
 * the session watchdog so long blocking handlers (FLASH_ERASE) don't
 * appear dead the instant they finally ACK. */
static void send_ack(uint8_t dst, const uint8_t *payload, uint16_t length)
{
    send_message(BL_PROTO_TYPE_ACK, dst, payload, length);
    session_touch_activity();
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

/* ---- Opcode handlers ---- */

/* CMD_CONNECT: [major, minor]. Major must match exactly; minor is a
 * hint, carried for diagnostics but not validated. On success the
 * session latch goes high and the bootloader echoes back its own
 * (major, minor). */
static void handle_connect(uint8_t peer, const uint8_t *args, uint16_t args_len)
{
    if (args_len < 2U) {
        send_nack(peer, BL_CMD_CONNECT, BL_NACK_UNSUPPORTED);
        return;
    }

    uint8_t host_major = args[0];
    /* args[1] is host_minor — unused today, reserved. */

    if (host_major != BL_PROTO_VERSION_MAJOR) {
        send_nack(peer, BL_CMD_CONNECT, BL_NACK_PROTOCOL_VERSION);
        return;
    }

    g_session_active = true;
    session_touch_activity();  /* arm the watchdog starting now */

    uint8_t resp[3] = {
        BL_CMD_CONNECT,
        BL_PROTO_VERSION_MAJOR,
        BL_PROTO_VERSION_MINOR,
    };
    send_ack(peer, resp, (uint16_t)sizeof(resp));
}

/* CMD_DISCONNECT: no args. Clears the session latch and acks. */
static void handle_disconnect(uint8_t peer, uint16_t args_len)
{
    (void)args_len;
    g_session_active = false;
    uint8_t resp[1] = { BL_CMD_DISCONNECT };
    send_ack(peer, resp, (uint16_t)sizeof(resp));
}

/* CMD_DISCOVER: no args. Reply as TYPE=DISCOVER with this node's ID
 * and the bootloader's protocol version. Additional identity fields
 * (UID, HW rev, FW version, WRP status, …) arrive as later phases
 * populate them. */
static void handle_discover(uint8_t peer)
{
    uint8_t resp[4] = {
        BL_CMD_DISCOVER,
        BL_NODE_ID,
        BL_PROTO_VERSION_MAJOR,
        BL_PROTO_VERSION_MINOR,
    };
    send_message(BL_PROTO_TYPE_DISCOVER, peer, resp, (uint16_t)sizeof(resp));
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
        send_nack(peer, BL_CMD_RESET, BL_NACK_UNSUPPORTED);
        return;
    }

    uint8_t mode = args[0];
    if (mode > 3U) {
        send_nack(peer, BL_CMD_RESET, BL_NACK_UNSUPPORTED);
        return;
    }

    if (mode == 3U && Bootloader_CheckApplication() != 0U) {
        send_nack(peer, BL_CMD_RESET, BL_NACK_NO_VALID_APP);
        return;
    }

    uint8_t resp[1] = { BL_CMD_RESET };
    send_ack(peer, resp, (uint16_t)sizeof(resp));

    /* Let the ACK drain onto the wire before we yank the MCU. */
    HAL_Delay(10U);

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
        send_nack(peer, BL_CMD_FLASH_ERASE, BL_NACK_BAD_SESSION);
        return;
    }
    if (args_len < 8U) {
        send_nack(peer, BL_CMD_FLASH_ERASE, BL_NACK_UNSUPPORTED);
        return;
    }

    uint32_t start  = read_le32(&args[0]);
    uint32_t length = read_le32(&args[4]);

    uint32_t sectors = 0U;
    bl_flash_status_t st = bl_flash_erase(start, length, &sectors);
    if (st != BL_FLASH_OK) {
        send_nack(peer, BL_CMD_FLASH_ERASE, flash_status_to_nack(st));
        return;
    }

    uint8_t resp[1] = { BL_CMD_FLASH_ERASE };
    send_ack(peer, resp, (uint16_t)sizeof(resp));
}

/* CMD_FLASH_WRITE: [addr_le32, data...]. Programs `args_len - 4` bytes
 * starting at `addr`. `addr` must be FLASHWORD-aligned; trailing
 * partial FLASHWORD is padded with 0xFF. Requires an active session. */
static void handle_flash_write(uint8_t peer, const uint8_t *args, uint16_t args_len)
{
    if (!g_session_active) {
        send_nack(peer, BL_CMD_FLASH_WRITE, BL_NACK_BAD_SESSION);
        return;
    }
    if (args_len < 5U) {
        /* Need at least the address plus one data byte. */
        send_nack(peer, BL_CMD_FLASH_WRITE, BL_NACK_UNSUPPORTED);
        return;
    }

    uint32_t addr = read_le32(&args[0]);
    const uint8_t *data = &args[4];
    uint16_t       data_len = (uint16_t)(args_len - 4U);

    bl_flash_status_t st = bl_flash_write(addr, data, data_len);
    if (st != BL_FLASH_OK) {
        send_nack(peer, BL_CMD_FLASH_WRITE, flash_status_to_nack(st));
        return;
    }

    uint8_t resp[1] = { BL_CMD_FLASH_WRITE };
    send_ack(peer, resp, (uint16_t)sizeof(resp));
}

/* CMD_FLASH_READ_CRC: [addr_le32, length_le32]. Returns CRC32 over the
 * range. Read-only but still gated behind the session — "any FLASH_*
 * requires CONNECT" is a cleaner invariant. */
static void handle_flash_read_crc(uint8_t peer, const uint8_t *args, uint16_t args_len)
{
    if (!g_session_active) {
        send_nack(peer, BL_CMD_FLASH_READ_CRC, BL_NACK_BAD_SESSION);
        return;
    }
    if (args_len < 8U) {
        send_nack(peer, BL_CMD_FLASH_READ_CRC, BL_NACK_UNSUPPORTED);
        return;
    }

    uint32_t addr   = read_le32(&args[0]);
    uint32_t length = read_le32(&args[4]);

    /* CRC is read-only but we still enforce the writable range — a host
     * CRC'ing the bootloader itself would be a protocol abuse. */
    if (!bl_flash_range_is_writable(addr, length)) {
        send_nack(peer, BL_CMD_FLASH_READ_CRC, BL_NACK_OUT_OF_BOUNDS);
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
    send_ack(peer, resp, (uint16_t)sizeof(resp));
}

/* CMD_FLASH_VERIFY: [expected_crc_le32, expected_size_le32,
 *                    expected_version_le32]. Recomputes CRC32 over
 * [BL_APP_BASE, BL_APP_BASE + expected_size). On match, stamps the
 * application metadata FLASHWORD so next boot will auto-jump. */
static void handle_flash_verify(uint8_t peer, const uint8_t *args, uint16_t args_len)
{
    if (!g_session_active) {
        send_nack(peer, BL_CMD_FLASH_VERIFY, BL_NACK_BAD_SESSION);
        return;
    }
    if (args_len < 12U) {
        send_nack(peer, BL_CMD_FLASH_VERIFY, BL_NACK_UNSUPPORTED);
        return;
    }

    uint32_t expected_crc     = read_le32(&args[0]);
    uint32_t expected_size    = read_le32(&args[4]);
    uint32_t expected_version = read_le32(&args[8]);

    if (expected_size == 0U || expected_size > BL_APP_SIZE) {
        send_nack(peer, BL_CMD_FLASH_VERIFY, BL_NACK_OUT_OF_BOUNDS);
        return;
    }

    uint32_t computed = bl_flash_crc32(BL_APP_BASE, expected_size);
    if (computed != expected_crc) {
        send_nack(peer, BL_CMD_FLASH_VERIFY, BL_NACK_CRC_MISMATCH);
        return;
    }

    bl_flash_status_t st = bl_flash_write_metadata(expected_size,
                                                    expected_crc,
                                                    expected_version);
    if (st != BL_FLASH_OK) {
        send_nack(peer, BL_CMD_FLASH_VERIFY, flash_status_to_nack(st));
        return;
    }

    uint8_t resp[1] = { BL_CMD_FLASH_VERIFY };
    send_ack(peer, resp, (uint16_t)sizeof(resp));
}

/* CMD_JUMP: [addr_le32]. Phase-2 policy: the address must equal
 * BL_APP_BASE, and the installed app must pass the integrity check.
 * Host tools with a use for jumping elsewhere will land that in a
 * later phase. */
static void handle_jump(uint8_t peer, const uint8_t *args, uint16_t args_len)
{
    if (args_len < 4U) {
        send_nack(peer, BL_CMD_JUMP, BL_NACK_UNSUPPORTED);
        return;
    }

    uint32_t addr = (uint32_t)args[0]
                  | ((uint32_t)args[1] << 8)
                  | ((uint32_t)args[2] << 16)
                  | ((uint32_t)args[3] << 24);

    if (addr != BL_APP_BASE) {
        send_nack(peer, BL_CMD_JUMP, BL_NACK_OUT_OF_BOUNDS);
        return;
    }

    if (Bootloader_CheckApplication() != 0U) {
        send_nack(peer, BL_CMD_JUMP, BL_NACK_NO_VALID_APP);
        return;
    }

    uint8_t resp[1] = { BL_CMD_JUMP };
    send_ack(peer, resp, (uint16_t)sizeof(resp));

    /* Let the ACK drain onto the wire before we deinit peripherals. */
    HAL_Delay(10U);
    Bootloader_JumpToApplication();  /* never returns on success */
}

/* Dispatch a completed ISO-TP message. byte 0 = opcode, remaining bytes
 * = opcode-specific args. Anything unknown earns NACK(UNSUPPORTED). */
static void handle_message(uint8_t peer,
                           uint8_t opcode,
                           const uint8_t *args,
                           uint16_t args_len)
{
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
        case BL_CMD_RESET:
            handle_reset(peer, args, args_len);
            break;
        case BL_CMD_JUMP:
            handle_jump(peer, args, args_len);
            break;
        default:
            send_nack(peer, opcode, BL_NACK_UNSUPPORTED);
            break;
    }
}

/* ---- Public API ---- */

void bl_proto_dispatch(const bl_proto_id_t *id,
                       const uint8_t *data,
                       uint8_t length)
{
    if (!bl_proto_addressed_to_us(id->dst) || length == 0U) {
        return;
    }

    /* Accepted TYPE/PCI combinations:
     *   - CMD      + SF / FF : host command (single or multi-frame)
     *   - DISCOVER + SF / FF : broadcast identity request
     *   - DATA     + CF / FC : continuation / flow control
     * Everything else is silently dropped. */
    uint8_t pci_hi = data[0] & BL_ISOTP_PCI_MASK_HI;
    bool initial = (id->type == BL_PROTO_TYPE_CMD || id->type == BL_PROTO_TYPE_DISCOVER)
                 && (pci_hi == BL_ISOTP_PCI_SF || pci_hi == BL_ISOTP_PCI_FF);
    bool continuation = (id->type == BL_PROTO_TYPE_DATA)
                     && (pci_hi == BL_ISOTP_PCI_CF || pci_hi == BL_ISOTP_PCI_FC);
    if (!initial && !continuation) {
        return;
    }

    /* Any frame that made it through the TYPE/PCI gate counts as session
     * activity — including frames that end up getting NACKed. A host
     * that's still transmitting, even unsuccessfully, is still alive. */
    session_touch_activity();

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
            break;

        case BL_ISOTP_ERR_TIMEOUT:
            /* Only _tick returns this; _feed never does. Handle
             * defensively anyway. */
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
