/*
 * test_bl_proto_dispatch.c — bl_proto_dispatch entry-gate coverage.
 *
 * The dispatcher is the bootloader's primary contact with the host —
 * everything that arrives over CAN flows through it. This file covers
 * the four early-return gates that decide which frames even make it
 * into the ISO-TP reassembler:
 *
 *   1. direction must be HOST_TO_NODE (silent drop on NODE_TO_HOST)
 *   2. dst must be us (silent drop on a different node)
 *   3. length must be > 0 (silent drop on empty frame)
 *   4. PCI nibble must be one of SF/FF/CF/FC (NACK on anything else
 *      — the *only* gate that emits a reply, since the earlier gates
 *      already filtered out everything not meant for us)
 *
 * Gate 4 is the regression target for issue #60 — it used to silently
 * drop, which left hosts and fuzzers unable to distinguish "the frame
 * reached the BL but was malformed" from "the frame never arrived".
 * PR #63 changed it to emit BL_NACK_TRANSPORT_ERROR; this test pins
 * that behaviour down so a future refactor can't quietly regress it.
 *
 * Tests below exercise the dispatcher end-to-end against the host
 * FDCAN capture mock: every successful HAL_FDCAN_AddMessageToTxFifoQ
 * call appends a frame to a ring, and we inspect that ring to assert
 * which frames the BL emitted in response. Captures are wiped in
 * mock_flash_reset() (called from setUp), so every test starts with
 * an empty TX ring.
 */

#include "bl_isotp.h"
#include "bl_proto.h"
#include "stm32h7xx_hal.h"   /* mock_fdcan_* */
#include "unity.h"

#include <stdint.h>
#include <string.h>

/* Build a host-to-us dispatcher id. `BL_NODE_ID` is the compile-time
 * node identity (default 0x1, see bl_config.h). */
static bl_proto_id_t host_to_us(void)
{
    bl_proto_id_t id = { BL_PROTO_DIRECTION_HOST_TO_NODE, 0x1U /* BL_NODE_ID */ };
    return id;
}

/* ---- Gate 1: direction ---- */

void test_dispatch_node_to_host_direction_is_silently_dropped(void)
{
    /* Frames the BL might receive on a loopback or simulator harness
     * with the wrong direction bit set must never reach the
     * reassembler or emit anything. The FDCAN hardware filter usually
     * catches these, but the software-side gate is defence-in-depth. */
    bl_proto_id_t id = { BL_PROTO_DIRECTION_NODE_TO_HOST, 0x1U };
    uint8_t frame[8] = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07 };

    bl_proto_dispatch(&id, frame, 8);

    TEST_ASSERT_EQUAL_INT(0, mock_fdcan_tx_count());
}


/* ---- Gate 2: dst ---- */

void test_dispatch_wrong_destination_is_silently_dropped(void)
{
    /* A frame addressed to a different node ID — not us, not the
     * broadcast — must be ignored. No reply, the bus belongs to
     * whoever owns 0x05. */
    bl_proto_id_t id = { BL_PROTO_DIRECTION_HOST_TO_NODE, 0x05U };
    uint8_t frame[8] = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07 };

    bl_proto_dispatch(&id, frame, 8);

    TEST_ASSERT_EQUAL_INT(0, mock_fdcan_tx_count());
}


/* ---- Gate 3: length ---- */

void test_dispatch_zero_length_frame_is_silently_dropped(void)
{
    /* The reassembler reads data[0] for the PCI byte and would dereference
     * out of bounds on a length-0 frame. Gate ensures we never get there. */
    bl_proto_id_t id = host_to_us();
    uint8_t frame[8] = { 0 };

    bl_proto_dispatch(&id, frame, 0);

    TEST_ASSERT_EQUAL_INT(0, mock_fdcan_tx_count());
}


/* ---- Gate 4: PCI — the #60 regression ---- */

void test_dispatch_bad_pci_emits_nack_transport_error(void)
{
    /* Regression for issue #60. A frame addressed to us with a PCI
     * byte whose high nibble isn't 0x0 (SF) / 0x1 (FF) / 0x2 (CF) /
     * 0x3 (FC) used to be silently dropped — the dispatcher looked
     * like a black hole on malformed input, and hosts/fuzzers had no
     * way to tell a malformed frame from a missed one. The fix in
     * PR #63 added a send_nack(0x00, BL_NACK_TRANSPORT_ERROR) before
     * the return; this test asserts the NACK actually shows up on
     * the wire with the right framing. */
    bl_proto_id_t id = host_to_us();
    /* 0x40 lives in the reserved upper range (PCI bits 7..4 = 0x4). */
    uint8_t frame[8] = { 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

    bl_proto_dispatch(&id, frame, 8);

    /* Exactly one TX: the NACK SF. */
    TEST_ASSERT_EQUAL_INT(1, mock_fdcan_tx_count());

    const mock_fdcan_frame_t *tx = mock_fdcan_get(0);
    TEST_ASSERT_NOT_NULL(tx);

    /* Reply is node→host, src = BL_NODE_ID = 0x1. Use the production
     * builder so the test stays correct if the ID layout ever shifts. */
    uint32_t expected_id =
        bl_proto_build_id(BL_PROTO_DIRECTION_NODE_TO_HOST, 0x1U);
    TEST_ASSERT_EQUAL_UINT32(expected_id, tx->identifier);

    /* DLC = 1 PCI + msg_type + rejected_opcode + nack_code = 4 bytes. */
    TEST_ASSERT_EQUAL_UINT8(4U, tx->dlc_bytes);

    /* ISO-TP SF with payload length 3. */
    TEST_ASSERT_EQUAL_UINT8((uint8_t)(BL_ISOTP_PCI_SF | 0x03U), tx->data[0]);
    /* msg_type = NACK (0x02). */
    TEST_ASSERT_EQUAL_UINT8(0x02U, tx->data[1]);
    /* rejected_opcode = 0 — we failed at the PCI level, before
     * reassembly exposed an opcode. */
    TEST_ASSERT_EQUAL_UINT8(0x00U, tx->data[2]);
    /* NACK code. The cast to uint8_t silences the macro-expands-to-
     * unsigned-literal warning some compilers throw on a TEST_ASSERT
     * arg with mixed widths. */
    TEST_ASSERT_EQUAL_UINT8((uint8_t)BL_NACK_TRANSPORT_ERROR, tx->data[3]);
}


/* ---- Sanity: valid SF still passes through the gate ---- */

void test_dispatch_valid_sf_pci_passes_pci_gate(void)
{
    /* Negative control: a frame whose PCI nibble IS a valid SF must
     * not trip the bad-PCI gate. We don't fully validate downstream
     * dispatch here (that's bigger tests in a follow-up PR); we just
     * confirm the bad-PCI NACK doesn't fire for a well-formed PCI.
     *
     * The reassembled SF will carry msg_type=0 / opcode=0 which the
     * handler tree treats as BL_CMD unknown opcode → NACK(UNSUPPORTED).
     * That's fine for this test: we just want to assert the emitted
     * NACK is NOT the TRANSPORT_ERROR one. */
    bl_proto_id_t id = host_to_us();
    /* SF PCI 0x01, then msg_type=0 + opcode=0. Length 2 bytes after PCI. */
    uint8_t frame[8] = { (uint8_t)(BL_ISOTP_PCI_SF | 0x02U),
                         0x00, 0x00, 0, 0, 0, 0, 0 };

    bl_proto_dispatch(&id, frame, 8);

    /* The handler emitted SOMETHING (likely a NACK(UNSUPPORTED) for
     * the zero opcode), but importantly not the TRANSPORT_ERROR NACK
     * that the bad-PCI gate would emit. */
    TEST_ASSERT_GREATER_OR_EQUAL_INT(1, mock_fdcan_tx_count());
    const mock_fdcan_frame_t *tx = mock_fdcan_get(0);
    TEST_ASSERT_NOT_NULL(tx);
    /* If this is a NACK, byte 3 holds the code; assert it's not
     * TRANSPORT_ERROR. (For an ACK or any other reply, the assertion
     * is still meaningful — it just trivially passes.) */
    TEST_ASSERT_NOT_EQUAL((uint8_t)BL_NACK_TRANSPORT_ERROR, tx->data[3]);
}
