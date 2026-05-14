#ifndef BL_PROTO_H
#define BL_PROTO_H

/*
 * Bootloader CAN protocol — frame layout + dispatch entry point.
 *
 * Classic CAN (11-bit standard ID, 8-byte payload) on FDCAN2.
 *
 * ID layout (Proposal A — fix/12):
 *
 *   bits 10..5 = 0          (reserved; every valid ID ≤ 0x01F)
 *   bit   4    = direction   0 = host→node, 1 = node→host
 *   bits  3..0 = other-end node ID
 *                host→node: dst (0x0..0xE unicast, 0xF broadcast)
 *                node→host: src (0x1..0xE; 0x0 and 0xF reserved)
 *
 * Keeping every ID ≤ 0x01F gives the bootloader protocol the highest
 * arbitration priority on any shared CAN bus (lower IDs win), and
 * reserves `0x020..0x7FF` for future protocol extensions or
 * application-owned traffic that should sit below BL priority.
 *
 * Message type is NOT encoded in the ID any more — it rides as
 * payload byte 1 of every SF and FF (byte 0 is the ISO-TP PCI byte).
 * CF and FC frames do not carry a message-type byte; they inherit
 * semantic type from their parent FF, and ISO-TP PCI is sufficient
 * to identify them. See `bl_msg_type_t` below for the enumeration.
 *
 * Opcode handlers are introduced across the rest of Phase 2:
 *   feat/6-isotp              — multi-frame segmentation / reassembly
 *   feat/7-core-opcodes       — CONNECT / DISCOVER / RESET / JUMP + NACK
 *   feat/8-flash-opcodes      — ERASE / WRITE / READ_CRC / VERIFY
 *   feat/9-session-timeout    — session watchdog + keepalive
 *
 * Until those land every received frame gets a generic NACK back with
 * code BL_NACK_UNSUPPORTED.
 */

#include <stdbool.h>
#include <stdint.h>

/* ---- ID field layout ---- */
#define BL_PROTO_DIRECTION_BIT      0x10U   /* bit 4 of the 11-bit ID */
#define BL_PROTO_NODE_MASK          0x0FU   /* low nibble */
#define BL_PROTO_ID_VALID_MASK      0x1FU   /* bits 4:0 only; everything else must be zero */

/* ---- Direction constants ---- */
#define BL_PROTO_DIR_HOST_TO_NODE   0x00U
#define BL_PROTO_DIR_NODE_TO_HOST   0x10U

/* ---- Reserved node IDs ---- */
#define BL_PROTO_NODE_HOST          0x0U
#define BL_PROTO_NODE_BROADCAST     0xFU

/* ---- Message type byte (payload[1] of every SF/FF) ----
 * Matches `MessageType` in the flasher's `protocol/ids.rs`. Keep in sync. */
typedef enum {
    BL_MSG_CMD              = 0x00U,  /* host → device, command                      */
    BL_MSG_ACK              = 0x01U,  /* device → host, positive ack                 */
    BL_MSG_NACK             = 0x02U,  /* device → host, negative ack                 */
    BL_MSG_NOTIFY           = 0x03U,  /* device → host, unsolicited event            */
    BL_MSG_DISCOVER_REQUEST = 0x04U,  /* host → broadcast, discovery ping            */
    BL_MSG_DISCOVER_REPLY   = 0x05U,  /* device → host, discovery reply              */
    BL_MSG_APP_CTRL         = 0x06U,  /* host → node, application-level traffic      */
                                      /* (BL silently drops; app firmware handles)   */
} bl_msg_type_t;

/* ---- Command opcodes ---- */
#define BL_CMD_CONNECT              0x01U  /* start a session, exchange protocol version */
#define BL_CMD_DISCONNECT           0x02U  /* end the session */
#define BL_CMD_DISCOVER             0x03U  /* broadcast ping; device returns short identity */
#define BL_CMD_GET_FW_INFO          0x04U  /* return the application's __firmware_info record */
#define BL_CMD_GET_HEALTH           0x05U  /* return the 32-byte health record */
#define BL_CMD_FLASH_ERASE          0x10U  /* erase sectors covering [start, start+length) */
#define BL_CMD_FLASH_WRITE          0x11U  /* program bytes at addr (FLASHWORD-aligned) */
#define BL_CMD_FLASH_READ_CRC       0x12U  /* CRC32 over [addr, addr+length) */
#define BL_CMD_FLASH_VERIFY         0x13U  /* verify app + commit metadata record */
#define BL_CMD_LOG_STREAM_START     0x30U  /* start emitting NOTIFY_LOG frames */
#define BL_CMD_LOG_STREAM_STOP      0x31U  /* stop emitting NOTIFY_LOG frames */
#define BL_CMD_LIVE_DATA_START      0x32U  /* start periodic NOTIFY_LIVE_DATA */
#define BL_CMD_LIVE_DATA_STOP       0x33U  /* stop NOTIFY_LIVE_DATA stream */
#define BL_CMD_DTC_READ             0x40U  /* return the full DTC table */
#define BL_CMD_DTC_CLEAR            0x41U  /* erase every DTC entry */
#define BL_CMD_OB_READ              0x50U  /* read current option-byte snapshot */
#define BL_CMD_OB_APPLY_WRP         0x51U  /* apply WRP; triggers system reset */
#define BL_CMD_RESET                0x60U  /* reset MCU in one of four modes */
#define BL_CMD_JUMP                 0x61U  /* jump directly to the installed application */
#define BL_CMD_NVM_READ             0x80U  /* read a value from the NVM store */
#define BL_CMD_NVM_WRITE            0x81U  /* append/update/tombstone a value in the NVM store */
#define BL_CMD_NVM_FORMAT           0x82U  /* erase sector 7 (wipes NVM + app metadata); token-gated */

/* ---- Unsolicited notifications (msg_type = BL_MSG_NOTIFY, dst = HOST) ---- */
#define BL_NOTIFY_HEARTBEAT         0xF0U  /* 1 Hz periodic alive-plus-state ping */
#define BL_NOTIFY_DTC               0xF1U  /* new DTC recorded; not emitted on dedupe */
#define BL_NOTIFY_LOG               0xF2U  /* batched log entries from the bl_log ring */
#define BL_NOTIFY_LIVE_DATA         0xF3U  /* 1..50 Hz 32-byte bootloader snapshot */

/* ---- Protocol version advertised in CONNECT / DISCOVER replies ---- */
#define BL_PROTO_VERSION_MAJOR      0U
#define BL_PROTO_VERSION_MINOR      1U

/* ---- Session watchdog ----
 * A session that stays silent for this long is considered dead: the
 * bootloader clears the session latch, resets ISO-TP reassembly state,
 * and auto-jumps to the installed application if one is valid. The
 * watchdog only runs while a session is active — idle bootloaders
 * never time out. */
#ifndef BL_SESSION_TIMEOUT_MS
#define BL_SESSION_TIMEOUT_MS       30000U
#endif

/* ---- NACK error codes ----
 * Codes 0x01..0x08 are allocated by the project's top-level protocol
 * spec (see host-tool requirements); 0x09..0x7F are reserved for the
 * bootloader's own extensions; 0xFE..0xFF are generic fallbacks. */
#define BL_NACK_PROTECTED_ADDR      0x01U  /* write into WRP-protected region */
#define BL_NACK_OUT_OF_BOUNDS       0x02U  /* address outside allowed region */
#define BL_NACK_CRC_MISMATCH        0x03U  /* FLASH_VERIFY: computed CRC != expected */
#define BL_NACK_BAD_SESSION         0x06U  /* flash op issued without an active session */
#define BL_NACK_FLASH_HW            0x07U  /* HAL flash erase / program returned error */
#define BL_NACK_BUSY                0x08U  /* previous operation not complete */
#define BL_NACK_TRANSPORT_TIMEOUT   0x09U  /* ISO-TP reassembly timed out */
#define BL_NACK_TRANSPORT_ERROR     0x0AU  /* ISO-TP framing error (bad PCI/seq/overflow) */
#define BL_NACK_PROTOCOL_VERSION    0x0BU  /* host and device disagree on protocol major */
#define BL_NACK_NO_VALID_APP        0x0CU  /* tried to jump / reset-to-app but no valid image */
#define BL_NACK_NVM_NOT_FOUND       0x0DU  /* NVM_READ for a key with no live value */
#define BL_NACK_NVM_FULL            0x0EU  /* NVM store can't accept another write (post-compaction) */
#define BL_NACK_OB_WRONG_TOKEN      0x0FU  /* OB_APPLY_WRP missing/incorrect confirmation token */
#define BL_NACK_NVM_WRONG_TOKEN     0x10U  /* NVM_FORMAT missing/incorrect confirmation token */
#define BL_NACK_UNSUPPORTED         0xFEU  /* unknown opcode, unknown arg, or bad arg length */

/* ---- Decoded frame ID ---- */
typedef enum {
    BL_PROTO_DIRECTION_HOST_TO_NODE = 0,
    BL_PROTO_DIRECTION_NODE_TO_HOST = 1,
} bl_proto_direction_t;

typedef struct {
    bl_proto_direction_t direction;
    /* The "other end" node, interpreted per direction:
     *   HostToNode: dst (0x0..0xE, or 0xF broadcast)
     *   NodeToHost: src (0x1..0xE; 0x0 and 0xF are reserved and a
     *                     well-formed frame will never carry them)  */
    uint8_t node;
} bl_proto_id_t;

/* Build an 11-bit ID from its components. */
static inline uint32_t bl_proto_build_id(bl_proto_direction_t direction,
                                         uint8_t node) {
    uint32_t dir_bit = (direction == BL_PROTO_DIRECTION_NODE_TO_HOST)
                       ? BL_PROTO_DIRECTION_BIT
                       : 0U;
    return dir_bit | ((uint32_t)node & BL_PROTO_NODE_MASK);
}

/* Parse an 11-bit ID into its components.
 *
 * Returns true if the ID is well-formed for this protocol (bits 10..5
 * clear, and — for node→host frames — node is not 0x0/0xF). Returns
 * false for any ID outside the protocol's 5-bit space or with a
 * reserved node ID on the node→host direction. In the false case
 * `out` is still populated but callers should treat the frame as
 * malformed. */
static inline bool bl_proto_parse_id(uint32_t id, bl_proto_id_t *out) {
    out->direction = (id & BL_PROTO_DIRECTION_BIT)
                     ? BL_PROTO_DIRECTION_NODE_TO_HOST
                     : BL_PROTO_DIRECTION_HOST_TO_NODE;
    out->node = (uint8_t)(id & BL_PROTO_NODE_MASK);

    if ((id & ~(uint32_t)BL_PROTO_ID_VALID_MASK) != 0U) {
        return false;
    }
    if (out->direction == BL_PROTO_DIRECTION_NODE_TO_HOST
        && (out->node == BL_PROTO_NODE_HOST
            || out->node == BL_PROTO_NODE_BROADCAST)) {
        return false;
    }
    return true;
}

/* Returns true if the frame is addressed to us — unicast to BL_NODE_ID
 * or broadcast (0xF). Kept as a software-side check even though the
 * FDCAN filters already reject non-matching frames, so unit tests and
 * any future software-routed paths stay correct. */
bool bl_proto_addressed_to_us(uint8_t dst);

/* Dispatch a received frame. `data` must point to `length` bytes of
 * payload (0..8 for classic CAN). Frames are fed into the ISO-TP
 * reassembler; a completed message's first payload byte is decoded as
 * the message type (`bl_msg_type_t`) and the rest (opcode + args) is
 * routed to an internal handler. ISO-TP framing errors elicit a NACK
 * with BL_NACK_TRANSPORT_ERROR; unknown message types are silently
 * dropped (so APP_CTRL traffic transits cleanly while the BL is
 * running without provoking spurious NACKs). */
void bl_proto_dispatch(const bl_proto_id_t *id,
                       const uint8_t *data,
                       uint8_t length);

/* Periodic tick — call from the main loop. Drives the ISO-TP
 * reassembly timeout; if an in-flight reassembly expires the peer
 * gets NACK(BL_NACK_TRANSPORT_TIMEOUT) and the state machine resets. */
void bl_proto_tick(uint32_t now_ms);

/* True while a session is active (between a successful CONNECT and
 * the next DISCONNECT / watchdog timeout / MCU reset). Exposed so
 * other modules — e.g. bl_health for heartbeat gating — can read
 * the latch without owning it. */
bool bl_proto_session_active(void);

/* Emit an unsolicited msg_type=NOTIFY message to the host (dst=0x0).
 * Short payloads go as SF, longer as FF+CFs. Does NOT refresh the
 * session watchdog — notifications are device-initiated and don't
 * prove the host is still talking. */
void bl_proto_send_notify(const uint8_t *payload, uint16_t length);

/* Milliseconds since the last session-activity event, or 0 when no
 * session is active. `now_ms` is expected to be HAL_GetTick() from
 * the caller — passing it in keeps bl_live's snapshot self-consistent
 * (all time-based fields read from the same tick). */
uint32_t bl_proto_session_age_ms(uint32_t now_ms);

/* Bytes buffered so far in the current ISO-TP reassembly, or 0 when
 * the reassembler is idle. Cheap accessor; intended for live-data
 * dashboards. */
uint16_t bl_proto_isotp_rx_progress(void);

#endif /* BL_PROTO_H */
