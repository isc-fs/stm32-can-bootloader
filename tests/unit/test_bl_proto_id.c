/*
 * test_bl_proto_id.c — wire-format ID parsing/building tests.
 *
 * `bl_proto_build_id` and `bl_proto_parse_id` are `static inline` in
 * bl_proto.h. They drive every CAN frame the bootloader emits and the
 * filter logic the dispatcher applies on RX, so getting them wrong
 * silently corrupts the entire bus. We don't need to link bl_proto.o
 * — the inlines compile straight into the test TU.
 *
 * ID layout recap (from bl_proto.h):
 *   bits 10..5 = 0          (must be zero on valid frames)
 *   bit   4    = direction   0 = host→node, 1 = node→host
 *   bits  3..0 = "other end" node ID
 *                host→node: dst (0x0..0xE unicast, 0xF broadcast)
 *                node→host: src (0x1..0xE; 0x0 + 0xF reserved)
 *
 * Coverage targets:
 *   - Build emits the right bit pattern in all three quadrants
 *     (host→unicast, host→broadcast, node→host).
 *   - Parse accepts every well-formed ID and rejects every malformed
 *     one — reserved high bits set, or node→host with a reserved src
 *     (0x0 = host, 0xF = broadcast can't be a source).
 *   - parse(build(d, n)) is the identity for every valid (d, n).
 */

#include "bl_proto.h"
#include "unity.h"

#include <stdbool.h>
#include <stdint.h>

/* ---- bl_proto_build_id ---- */

void test_proto_build_host_to_node_unicast(void)
{
    /* bit 4 clear, node in low nibble. */
    TEST_ASSERT_EQUAL_UINT32(
        0x05U, bl_proto_build_id(BL_PROTO_DIRECTION_HOST_TO_NODE, 0x5U));
}

void test_proto_build_host_to_node_broadcast(void)
{
    TEST_ASSERT_EQUAL_UINT32(
        0x0FU, bl_proto_build_id(BL_PROTO_DIRECTION_HOST_TO_NODE,
                                 BL_PROTO_NODE_BROADCAST));
}

void test_proto_build_node_to_host_sets_dir_bit(void)
{
    /* direction = node→host → bit 4 set; node=3 → low nibble 0x3. */
    TEST_ASSERT_EQUAL_UINT32(
        0x13U, bl_proto_build_id(BL_PROTO_DIRECTION_NODE_TO_HOST, 0x3U));
}

void test_proto_build_masks_node_id_to_low_nibble(void)
{
    /* If the caller hands us a node ID with bits set above bit 3 the
     * builder must strip them — otherwise we'd silently emit a frame
     * with reserved bits set, which the dispatcher would then reject
     * on the other side. */
    TEST_ASSERT_EQUAL_UINT32(
        0x05U, bl_proto_build_id(BL_PROTO_DIRECTION_HOST_TO_NODE, 0xF5U));
    TEST_ASSERT_EQUAL_UINT32(
        0x10U, bl_proto_build_id(BL_PROTO_DIRECTION_NODE_TO_HOST, 0xF0U));
}

/* ---- bl_proto_parse_id ---- */

void test_proto_parse_valid_host_to_node_unicast(void)
{
    bl_proto_id_t out = {0};
    TEST_ASSERT_TRUE(bl_proto_parse_id(0x05U, &out));
    TEST_ASSERT_EQUAL_INT(BL_PROTO_DIRECTION_HOST_TO_NODE, out.direction);
    TEST_ASSERT_EQUAL_UINT8(0x05U, out.node);
}

void test_proto_parse_valid_host_to_node_broadcast(void)
{
    bl_proto_id_t out = {0};
    TEST_ASSERT_TRUE(bl_proto_parse_id(0x0FU, &out));
    TEST_ASSERT_EQUAL_INT(BL_PROTO_DIRECTION_HOST_TO_NODE, out.direction);
    TEST_ASSERT_EQUAL_UINT8(BL_PROTO_NODE_BROADCAST, out.node);
}

void test_proto_parse_valid_node_to_host(void)
{
    bl_proto_id_t out = {0};
    TEST_ASSERT_TRUE(bl_proto_parse_id(0x13U, &out));
    TEST_ASSERT_EQUAL_INT(BL_PROTO_DIRECTION_NODE_TO_HOST, out.direction);
    TEST_ASSERT_EQUAL_UINT8(0x03U, out.node);
}

void test_proto_parse_rejects_reserved_bit_5_set(void)
{
    /* 0x20 has bit 5 set, which lives outside the 5-bit protocol window.
     * Spec says: ignore anything with non-zero bits in 10..5. */
    bl_proto_id_t out = {0};
    TEST_ASSERT_FALSE(bl_proto_parse_id(0x20U, &out));
}

void test_proto_parse_rejects_reserved_bit_high_set(void)
{
    /* Any of bits 5..10 being set must reject — try the top one. */
    bl_proto_id_t out = {0};
    TEST_ASSERT_FALSE(bl_proto_parse_id(0x400U, &out));
}

void test_proto_parse_rejects_node_to_host_src_zero(void)
{
    /* src=0 is the host's own ID; a node can't legitimately claim that
     * as its source. Bit 4 set + low nibble zero → 0x10. */
    bl_proto_id_t out = {0};
    TEST_ASSERT_FALSE(bl_proto_parse_id(0x10U, &out));
}

void test_proto_parse_rejects_node_to_host_src_broadcast(void)
{
    /* src=0xF is the broadcast pseudo-node; can't appear as a real
     * source. Bit 4 set + low nibble 0xF → 0x1F. */
    bl_proto_id_t out = {0};
    TEST_ASSERT_FALSE(bl_proto_parse_id(0x1FU, &out));
}

void test_proto_parse_accepts_node_to_host_src_max_unicast(void)
{
    /* src=0xE is the last legal unicast source — must accept. */
    bl_proto_id_t out = {0};
    TEST_ASSERT_TRUE(bl_proto_parse_id(0x1EU, &out));
    TEST_ASSERT_EQUAL_INT(BL_PROTO_DIRECTION_NODE_TO_HOST, out.direction);
    TEST_ASSERT_EQUAL_UINT8(0x0EU, out.node);
}

void test_proto_parse_accepts_host_to_node_zero(void)
{
    /* dst=0 (host as destination) is the dispatcher's reply target —
     * device-to-host responses come in on this ID. Must accept. */
    bl_proto_id_t out = {0};
    TEST_ASSERT_TRUE(bl_proto_parse_id(0x00U, &out));
    TEST_ASSERT_EQUAL_INT(BL_PROTO_DIRECTION_HOST_TO_NODE, out.direction);
    TEST_ASSERT_EQUAL_UINT8(BL_PROTO_NODE_HOST, out.node);
}

/* ---- build+parse round-trip ---- */

void test_proto_build_parse_roundtrip_all_valid_ids(void)
{
    /* Iterate every well-formed (direction, node) pair and confirm
     * parse(build(d, n)) recovers the inputs and returns true. */
    for (uint8_t node = 0x0U; node <= 0x0FU; node++) {
        /* host→node accepts every node 0x0..0xF (incl. broadcast). */
        uint32_t id_h = bl_proto_build_id(BL_PROTO_DIRECTION_HOST_TO_NODE, node);
        bl_proto_id_t out = {0};
        TEST_ASSERT_TRUE_MESSAGE(bl_proto_parse_id(id_h, &out),
            "host→node should accept every node 0x0..0xF");
        TEST_ASSERT_EQUAL_INT(BL_PROTO_DIRECTION_HOST_TO_NODE, out.direction);
        TEST_ASSERT_EQUAL_UINT8(node, out.node);

        /* node→host rejects 0x0 + 0xF; accepts 0x1..0xE. */
        uint32_t id_n = bl_proto_build_id(BL_PROTO_DIRECTION_NODE_TO_HOST, node);
        bool expected_ok = (node != BL_PROTO_NODE_HOST)
                        && (node != BL_PROTO_NODE_BROADCAST);
        bool got = bl_proto_parse_id(id_n, &out);
        TEST_ASSERT_EQUAL_INT(expected_ok ? 1 : 0, got ? 1 : 0);
        if (expected_ok) {
            TEST_ASSERT_EQUAL_INT(BL_PROTO_DIRECTION_NODE_TO_HOST, out.direction);
            TEST_ASSERT_EQUAL_UINT8(node, out.node);
        }
    }
}
