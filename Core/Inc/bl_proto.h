#ifndef BL_PROTO_H
#define BL_PROTO_H

/*
 * Bootloader CAN protocol — frame layout + dispatch entry point.
 *
 * Classic CAN (11-bit standard ID, 8-byte payload) on FDCAN2. The 11-bit
 * identifier encodes three fields:
 *
 *   bits 10:8  — message type (3 bits, `bl_proto_type_t`)
 *   bits 7:4   — source node ID (4 bits, 0x0 = host)
 *   bits 3:0   — destination node ID (4 bits, 0xF = broadcast)
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
#define BL_PROTO_TYPE_SHIFT        8U
#define BL_PROTO_SRC_SHIFT         4U
#define BL_PROTO_DST_SHIFT         0U

#define BL_PROTO_TYPE_MASK         0x7U
#define BL_PROTO_SRC_MASK          0xFU
#define BL_PROTO_DST_MASK          0xFU

/* ---- Reserved node IDs ---- */
#define BL_PROTO_NODE_HOST         0x0U
#define BL_PROTO_NODE_BROADCAST    0xFU

/* ---- Message type (bits 10:8 of the ID) ---- */
typedef enum {
    BL_PROTO_TYPE_CMD      = 0x0U,  /* host → device, command */
    BL_PROTO_TYPE_ACK      = 0x1U,  /* device → host, positive ack */
    BL_PROTO_TYPE_NACK     = 0x2U,  /* device → host, negative ack */
    BL_PROTO_TYPE_DATA     = 0x3U,  /* bidirectional multi-frame payload */
    BL_PROTO_TYPE_NOTIFY   = 0x4U,  /* device → host, unsolicited event */
    BL_PROTO_TYPE_DISCOVER = 0x7U,  /* broadcast, discovery ping/response */
} bl_proto_type_t;

/* ---- NACK error codes — subset used in Phase 2 ---- */
#define BL_NACK_BUSY               0x08U  /* previous operation not complete */
#define BL_NACK_UNSUPPORTED        0xFEU  /* unknown opcode or message type */

/* ---- Decoded frame ID ---- */
typedef struct {
    bl_proto_type_t type;
    uint8_t src;
    uint8_t dst;
} bl_proto_id_t;

/* Build an 11-bit ID from its components. */
static inline uint32_t bl_proto_build_id(bl_proto_type_t type,
                                         uint8_t src,
                                         uint8_t dst) {
    return ((uint32_t)((uint32_t)type & BL_PROTO_TYPE_MASK) << BL_PROTO_TYPE_SHIFT)
         | ((uint32_t)((uint32_t)src  & BL_PROTO_SRC_MASK)  << BL_PROTO_SRC_SHIFT)
         | ((uint32_t)((uint32_t)dst  & BL_PROTO_DST_MASK)  << BL_PROTO_DST_SHIFT);
}

/* Parse an 11-bit ID into its components. */
static inline void bl_proto_parse_id(uint32_t id, bl_proto_id_t *out) {
    out->type = (bl_proto_type_t)((id >> BL_PROTO_TYPE_SHIFT) & BL_PROTO_TYPE_MASK);
    out->src  = (uint8_t)((id >> BL_PROTO_SRC_SHIFT) & BL_PROTO_SRC_MASK);
    out->dst  = (uint8_t)((id >> BL_PROTO_DST_SHIFT) & BL_PROTO_DST_MASK);
}

/* Returns true if the frame is addressed to us — unicast to BL_NODE_ID
 * or broadcast (0xF). Kept as a software-side check even though the
 * FDCAN filters already reject non-matching frames, so unit tests and
 * any future software-routed paths stay correct. */
bool bl_proto_addressed_to_us(uint8_t dst);

/* Dispatch a received frame. `data` must point to `length` bytes of
 * payload (0..8 for classic CAN). Phase 2 placeholder: every frame is
 * answered with a NACK until the handler branches land. */
void bl_proto_dispatch(const bl_proto_id_t *id,
                       const uint8_t *data,
                       uint8_t length);

#endif /* BL_PROTO_H */
