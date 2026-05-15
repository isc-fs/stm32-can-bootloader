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

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
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


/* ---- Issue #94: multi-frame WRITE_CHUNK should NOT emit TRANSPORT_ERROR ---- */

/* Helper: returns true iff the captured frame is a NACK with the given
 * code byte. A node→host SF NACK reaches the wire as:
 *   data[0] = SF PCI with len-nibble = 3   (0x03)
 *   data[1] = msg_type = BL_MSG_NACK       (0x02)
 *   data[2] = rejected_opcode
 *   data[3] = nack_code
 * Anything that doesn't match the SF NACK shape returns false. */
static bool frame_is_nack_with_code(const mock_fdcan_frame_t *f, uint8_t code)
{
    if (f == (const mock_fdcan_frame_t *)0) return false;
    if ((f->data[0] & 0xF0U) != 0x00U)      return false; /* not SF */
    if (f->data[1] != (uint8_t)BL_MSG_NACK) return false;
    return f->data[3] == code;
}

void test_dispatch_write_chunk_19cf_sequence_does_not_emit_transport_error(void)
{
    /* Regression test for issue #94. The bench-side flasher emitted a
     * multi-frame WRITE_CHUNK as `FF + 19 CFs` with the candump-
     * observed PCI sequence `10 → 21..2F → 20..23`, and the BL replied
     * with NACK(opcode=0xFF, code=BL_NACK_TRANSPORT_ERROR=0x0A). That
     * NACK signature pinpoints bl_proto.c:1166's default arm of the
     * switch on bl_isotp_rx_feed's return — i.e. the reassembler
     * itself returned one of BAD_PCI / BAD_SEQ / OVERFLOW / NO_FF for
     * a frame in this sequence.
     *
     * Static analysis (issue thread) couldn't pin down which frame in
     * the sequence trips the BL. This test feeds the exact wire shape
     * and asserts that no captured TX frame is a NACK with the
     * TRANSPORT_ERROR code. If it passes, the regression isn't in the
     * BL's reassembler logic for a clean wire trace — the bug lives in
     * the bench bridge / driver / host framing. If it fails, the
     * failing frame index pinpoints the BL-side defect.
     *
     * The reassembled payload at the dispatcher level is 134 bytes:
     *   [0]      msg_type = BL_MSG_CMD        (0x00)
     *   [1]      opcode   = BL_CMD_FLASH_WRITE (0x11)
     *   [2..5]   addr     = 0x08020000        (LE)
     *   [6..133] chunk    = 128 bytes of arbitrary data
     *
     * After the FF the BL emits FC(CTS). After every CF except the
     * last it stays quiet. After CF19 it dispatches to handle_flash_
     * write; without a prior CONNECT the handler emits
     * NACK(BAD_SESSION) — that's expected (the test isn't about
     * session state) and is tolerated by the assertion (we only flag
     * TRANSPORT_ERROR). */
    bl_proto_id_t id = host_to_us();

    /* === FF: PCI=0x10, total=134 (0x86), first 6 payload bytes === */
    uint8_t ff[8];
    ff[0] = 0x10U;                      /* FF PCI, length-high-nibble = 0 */
    ff[1] = 0x86U;                      /* length low byte = 134          */
    ff[2] = (uint8_t)BL_MSG_CMD;
    ff[3] = (uint8_t)BL_CMD_FLASH_WRITE;
    ff[4] = 0x00U;                      /* addr LE byte 0                 */
    ff[5] = 0x00U;                      /* addr LE byte 1                 */
    ff[6] = 0x02U;                      /* addr LE byte 2                 */
    ff[7] = 0x08U;                      /* addr LE byte 3 → 0x08020000    */
    bl_proto_dispatch(&id, ff, 8U);

    /* === 18 full CFs: PCI 0x21..0x2F, then 0x20..0x22 (seq 1..15, 0..2) === */
    for (uint8_t i = 0U; i < 18U; i++) {
        uint8_t seq = (uint8_t)((i + 1U) & 0x0FU);  /* 1..15, 0, 1, 2 */
        uint8_t cf[8];
        cf[0] = (uint8_t)(0x20U | seq);
        for (uint8_t j = 1U; j < 8U; j++) {
            cf[j] = (uint8_t)(0xA0U + i);            /* arbitrary payload */
        }
        bl_proto_dispatch(&id, cf, 8U);
    }

    /* === 19th CF: PCI 0x23 (seq=3), 2 data bytes (DLC=3) === */
    uint8_t cf_last[3];
    cf_last[0] = 0x23U;
    cf_last[1] = 0xDEU;
    cf_last[2] = 0xADU;
    bl_proto_dispatch(&id, cf_last, 3U);

    /* Assert: no captured frame is a NACK with TRANSPORT_ERROR.
     * A NACK with a DIFFERENT code (e.g. BAD_SESSION from
     * handle_flash_write after MSG_COMPLETE dispatched) is fine —
     * that isn't the regression we're hunting. */
    int n = mock_fdcan_tx_count();
    for (int i = 0; i < n; i++) {
        const mock_fdcan_frame_t *f = mock_fdcan_get(i);
        TEST_ASSERT_NOT_NULL(f);
        if (frame_is_nack_with_code(f, (uint8_t)BL_NACK_TRANSPORT_ERROR)) {
            char detail[160];
            (void)snprintf(detail, sizeof(detail),
                "BL emitted NACK(TRANSPORT_ERROR) at TX frame %d/%d "
                "(rejected_opcode=0x%02X). Issue #94 regression — "
                "bl_isotp_rx_feed returned an error inside the "
                "WRITE_CHUNK reassembly for a wire shape that should "
                "reassemble cleanly.",
                i, n, f->data[2]);
            TEST_FAIL_MESSAGE(detail);
        }
    }
}


void test_dispatch_write_chunk_262byte_37cf_padded_last_cf(void)
{
    /* Higher-fidelity reproducer for #94 using the EXACT candump from
     * the operator's bench session. WRITE_CHUNK with declared total
     * 262 bytes (PCI `11 06`), 37 CFs at DLC=8, last CF padded with
     * three 0x00 bytes (real data ends at offset 4 of the 7-byte
     * data area).
     *
     * Operator's hypothesis: BL credits 7 bytes per CF regardless of
     * declared remainder, so 6 + 37*7 = 265 > 262 = OVERFLOW. The
     * code-side `take = min(remaining, frame_avail)` says it
     * shouldn't, but bench evidence is concrete — this test settles
     * the question deterministically. If it FAILS we have the bug;
     * if it PASSES the failure happens elsewhere (bench-side
     * framing, bridge mangling some specific frame, etc.). */
    bl_proto_id_t id = host_to_us();

    /* === FF: PCI=0x11 06 → total=262, 6 bytes of payload === */
    uint8_t ff[8];
    ff[0] = 0x11U;                       /* FF PCI, length high nibble=1 */
    ff[1] = 0x06U;                       /* length low byte = 0x06 → total 0x106 = 262 */
    ff[2] = (uint8_t)BL_MSG_CMD;
    ff[3] = (uint8_t)BL_CMD_FLASH_WRITE;
    ff[4] = 0x00U;                       /* addr LE → 0x08020000 */
    ff[5] = 0x00U;
    ff[6] = 0x02U;
    ff[7] = 0x08U;
    bl_proto_dispatch(&id, ff, 8U);

    /* === 36 full CFs: DLC=8, 7 data bytes each.
     *
     * Sequence pattern from the trace:
     *   CFs 1..15  → seq 1..15
     *   CFs 16..31 → seq 0..15
     *   CFs 32..36 → seq 0..4
     * All DLC=8. */
    for (uint8_t i = 0U; i < 36U; i++) {
        uint8_t seq = (uint8_t)((i + 1U) & 0x0FU);
        uint8_t cf[8];
        cf[0] = (uint8_t)(0x20U | seq);
        for (uint8_t j = 1U; j < 8U; j++) {
            cf[j] = (uint8_t)(0xA0U + (i & 0x3FU));
        }
        bl_proto_dispatch(&id, cf, 8U);
    }

    /* === 37th CF: PCI 0x25 (seq=5, the next after CF36's seq=4),
     *   DLC=8 with last 3 bytes zero-padded (real data ends at index 4).
     * Total real data after FF = 36*7 + 4 = 252 + 4 = 256 bytes.
     * Total message = 6 + 256 = 262 = declared. === */
    uint8_t cf37[8];
    cf37[0] = 0x25U;
    cf37[1] = 0xF1U; cf37[2] = 0x10U; cf37[3] = 0x02U; cf37[4] = 0x08U;
    cf37[5] = 0x00U; cf37[6] = 0x00U; cf37[7] = 0x00U;  /* padding */
    bl_proto_dispatch(&id, cf37, 8U);

    /* Assert: no captured frame is a NACK with TRANSPORT_ERROR. */
    int n = mock_fdcan_tx_count();
    for (int i = 0; i < n; i++) {
        const mock_fdcan_frame_t *f = mock_fdcan_get(i);
        TEST_ASSERT_NOT_NULL(f);
        if (frame_is_nack_with_code(f, (uint8_t)BL_NACK_TRANSPORT_ERROR)) {
            char detail[200];
            (void)snprintf(detail, sizeof(detail),
                "BL emitted NACK(TRANSPORT_ERROR) at TX frame %d/%d "
                "(rejected_opcode=0x%02X). Issue #94 regression: the "
                "262-byte / 37-CF WRITE_CHUNK shape from the operator's "
                "bench trace fails reassembly in unit test.",
                i, n, f->data[2]);
            TEST_FAIL_MESSAGE(detail);
        }
    }
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
