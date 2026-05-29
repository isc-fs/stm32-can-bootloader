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
#include "bl_nvm.h"          /* bl_nvm_init / bl_nvm_read / bl_nvm_write
                                for NVM round-trip dispatcher tests   */
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
            char detail[256];
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
            char detail[256];
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


/* ============================================================
 * Opcode-handler coverage. The gate tests above pin frame-level
 * filtering. This block covers what happens AFTER the gates —
 * each test sends a single-frame command and asserts the
 * dispatcher landed on the right `handle_*` function with the
 * right session / arg / error semantics.
 *
 * Cross-test contamination risk: `g_session_active` is a static
 * inside `bl_proto.c` and persists across tests. We can't read
 * or clear it directly, so every test that cares starts with an
 * explicit DISCONNECT (which always clears the latch and ACKs
 * once) plus `mock_fdcan_reset()` to start with an empty TX
 * ring. The convention is intentionally noisy at the top of
 * each test rather than hidden in a tearDown — easier to reason
 * about what state each test is in just from reading it.
 * ============================================================ */

/* Build an SF frame for a `BL_MSG_CMD` with the given opcode and
 * args, and feed it through the dispatcher. The SF payload-length
 * nibble is `2 + args_len` (msg_type + opcode + args). */
static void send_cmd_sf(uint8_t opcode, const uint8_t *args, uint8_t args_len)
{
    bl_proto_id_t id = host_to_us();
    uint8_t frame[8] = { 0 };
    uint8_t payload_len = (uint8_t)(2U + args_len);
    /* SF can carry at most 7 payload bytes — enforced upstream by
     * the caller, but be defensive so a typo doesn't silently send
     * a truncated request. */
    TEST_ASSERT_LESS_OR_EQUAL_UINT8(7U, payload_len);
    frame[0] = (uint8_t)(BL_ISOTP_PCI_SF | (payload_len & 0x07U));
    frame[1] = (uint8_t)BL_MSG_CMD;
    frame[2] = opcode;
    for (uint8_t i = 0U; i < args_len; i++) {
        frame[3U + i] = args[i];
    }
    bl_proto_dispatch(&id, frame, 8U);
}

/* Reset session latch + clear TX ring. Safe to call when there's
 * no active session (DISCONNECT is idempotent). After this returns
 * the BL is in {session=false, ring=empty}. */
static void reset_session_and_tx(void)
{
    send_cmd_sf((uint8_t)BL_CMD_DISCONNECT, NULL, 0U);
    mock_fdcan_reset();
}

/* Prime an active session with a valid CONNECT, then clear the TX
 * ring so the test's first assertion sees only its own response.
 * Caller must verify the session actually took effect by some
 * downstream observation. */
static void prime_session(void)
{
    reset_session_and_tx();
    uint8_t args[2] = {
        (uint8_t)BL_PROTO_VERSION_MAJOR,
        (uint8_t)BL_PROTO_VERSION_MINOR,
    };
    send_cmd_sf((uint8_t)BL_CMD_CONNECT, args, 2U);
    /* Sanity: the CONNECT ACK should have landed on the wire. */
    TEST_ASSERT_GREATER_OR_EQUAL_INT(1, mock_fdcan_tx_count());
    mock_fdcan_reset();
}

/* ---- CONNECT ---- */

void test_handle_connect_valid_version_acks_and_latches_session(void)
{
    reset_session_and_tx();

    uint8_t args[2] = {
        (uint8_t)BL_PROTO_VERSION_MAJOR,
        (uint8_t)BL_PROTO_VERSION_MINOR,
    };
    send_cmd_sf((uint8_t)BL_CMD_CONNECT, args, 2U);

    /* One TX: ACK with [BL_CMD_CONNECT, MAJOR, MINOR] payload. */
    TEST_ASSERT_EQUAL_INT(1, mock_fdcan_tx_count());
    const mock_fdcan_frame_t *tx = mock_fdcan_get(0);
    TEST_ASSERT_NOT_NULL(tx);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)(BL_ISOTP_PCI_SF | 0x04U), tx->data[0]);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)BL_MSG_ACK,           tx->data[1]);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)BL_CMD_CONNECT,       tx->data[2]);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)BL_PROTO_VERSION_MAJOR, tx->data[3]);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)BL_PROTO_VERSION_MINOR, tx->data[4]);

    /* Latch verification: a session-gated op should now respond with
     * something OTHER than BAD_SESSION. NVM_READ is the cleanest
     * probe because its 2-byte arg fits in a single SF and, with
     * NVM empty, the post-session reply is NACK(NVM_NOT_FOUND) — a
     * distinct code that can't be confused with the BAD_SESSION
     * we're guarding against. */
    mock_fdcan_reset();
    uint8_t probe_key[2] = { 0xCDU, 0xABU };  /* arbitrary unused key */
    send_cmd_sf((uint8_t)BL_CMD_NVM_READ, probe_key, 2U);
    TEST_ASSERT_EQUAL_INT(1, mock_fdcan_tx_count());
    const mock_fdcan_frame_t *reply = mock_fdcan_get(0);
    TEST_ASSERT_NOT_NULL(reply);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)BL_MSG_NACK, reply->data[1]);
    /* Distinguishes "session live, key missing" from "session not
     * active" — exactly the latch state we just asserted. */
    TEST_ASSERT_EQUAL_UINT8((uint8_t)BL_NACK_NVM_NOT_FOUND, reply->data[3]);
}

void test_handle_connect_wrong_major_nacks_protocol_version(void)
{
    reset_session_and_tx();

    /* Send major = MAJOR + 1 so it can't ever match the BL's
     * compile-time MAJOR regardless of what we bump that to. */
    uint8_t args[2] = {
        (uint8_t)(BL_PROTO_VERSION_MAJOR + 1U),
        (uint8_t)BL_PROTO_VERSION_MINOR,
    };
    send_cmd_sf((uint8_t)BL_CMD_CONNECT, args, 2U);

    TEST_ASSERT_EQUAL_INT(1, mock_fdcan_tx_count());
    const mock_fdcan_frame_t *tx = mock_fdcan_get(0);
    TEST_ASSERT_NOT_NULL(tx);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)BL_MSG_NACK, tx->data[1]);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)BL_CMD_CONNECT, tx->data[2]);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)BL_NACK_PROTOCOL_VERSION, tx->data[3]);

    /* And the session latch must NOT have gone high — a follow-up
     * session-gated op should still NACK with BAD_SESSION. */
    mock_fdcan_reset();
    send_cmd_sf((uint8_t)BL_CMD_NVM_READ,
                (const uint8_t[]){ 0x01U, 0x00U }, 2U);
    TEST_ASSERT_EQUAL_INT(1, mock_fdcan_tx_count());
    const mock_fdcan_frame_t *reply = mock_fdcan_get(0);
    TEST_ASSERT_NOT_NULL(reply);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)BL_MSG_NACK, reply->data[1]);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)BL_NACK_BAD_SESSION, reply->data[3]);
}

void test_handle_connect_short_args_nacks_unsupported(void)
{
    /* CONNECT requires 2 args (major + minor). A 1-arg payload must
     * be rejected — the handler's `args_len < 2` guard. */
    reset_session_and_tx();

    uint8_t args[1] = { (uint8_t)BL_PROTO_VERSION_MAJOR };
    send_cmd_sf((uint8_t)BL_CMD_CONNECT, args, 1U);

    TEST_ASSERT_EQUAL_INT(1, mock_fdcan_tx_count());
    const mock_fdcan_frame_t *tx = mock_fdcan_get(0);
    TEST_ASSERT_NOT_NULL(tx);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)BL_MSG_NACK, tx->data[1]);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)BL_CMD_CONNECT, tx->data[2]);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)BL_NACK_UNSUPPORTED, tx->data[3]);
}

/* ---- DISCONNECT ---- */

void test_handle_disconnect_clears_session_latch(void)
{
    /* CONNECT, verify latch is high (FLASH_ERASE doesn't NACK with
     * BAD_SESSION), DISCONNECT, verify latch is low (FLASH_ERASE
     * now NACKs with BAD_SESSION). The before/after symmetry is the
     * test point. */
    prime_session();

    /* NVM_READ used as the latch probe (same rationale as the
     * connect-latch test above — SF-fitting args, distinct NACK
     * code when session is live but key is missing). */
    uint8_t probe_key[2] = { 0xCDU, 0xABU };
    send_cmd_sf((uint8_t)BL_CMD_NVM_READ, probe_key, 2U);
    TEST_ASSERT_EQUAL_INT(1, mock_fdcan_tx_count());
    {
        const mock_fdcan_frame_t *r = mock_fdcan_get(0);
        TEST_ASSERT_NOT_NULL(r);
        /* Session live → NACK with NVM_NOT_FOUND, never BAD_SESSION. */
        TEST_ASSERT_EQUAL_UINT8((uint8_t)BL_MSG_NACK, r->data[1]);
        TEST_ASSERT_EQUAL_UINT8((uint8_t)BL_NACK_NVM_NOT_FOUND, r->data[3]);
    }

    mock_fdcan_reset();
    send_cmd_sf((uint8_t)BL_CMD_DISCONNECT, NULL, 0U);
    TEST_ASSERT_EQUAL_INT(1, mock_fdcan_tx_count());
    {
        const mock_fdcan_frame_t *r = mock_fdcan_get(0);
        TEST_ASSERT_NOT_NULL(r);
        TEST_ASSERT_EQUAL_UINT8((uint8_t)BL_MSG_ACK, r->data[1]);
        TEST_ASSERT_EQUAL_UINT8((uint8_t)BL_CMD_DISCONNECT, r->data[2]);
    }

    /* Post-disconnect: same NVM_READ now NACKs BAD_SESSION. */
    mock_fdcan_reset();
    send_cmd_sf((uint8_t)BL_CMD_NVM_READ, probe_key, 2U);
    TEST_ASSERT_EQUAL_INT(1, mock_fdcan_tx_count());
    {
        const mock_fdcan_frame_t *r = mock_fdcan_get(0);
        TEST_ASSERT_NOT_NULL(r);
        TEST_ASSERT_EQUAL_UINT8((uint8_t)BL_MSG_NACK, r->data[1]);
        TEST_ASSERT_EQUAL_UINT8((uint8_t)BL_NACK_BAD_SESSION, r->data[3]);
    }
}

/* ---- DISCOVER ---- */

void test_handle_discover_uses_discover_reply_type_not_ack(void)
{
    /* DISCOVER is session-agnostic (no CONNECT first) and is the
     * only command that replies with msg_type = DISCOVER_REPLY
     * (0x05) instead of ACK (0x01). This pins both the routing AND
     * the payload shape: [opcode, node_id, ver_major, ver_minor]. */
    reset_session_and_tx();

    send_cmd_sf((uint8_t)BL_CMD_DISCOVER, NULL, 0U);

    TEST_ASSERT_EQUAL_INT(1, mock_fdcan_tx_count());
    const mock_fdcan_frame_t *tx = mock_fdcan_get(0);
    TEST_ASSERT_NOT_NULL(tx);
    /* PCI = SF with 5-byte payload: msg_type + opcode + node_id +
     * MAJOR + MINOR. (`send_message` prepends msg_type to the 4-byte
     * resp[] the handler builds.) */
    TEST_ASSERT_EQUAL_UINT8((uint8_t)(BL_ISOTP_PCI_SF | 0x05U), tx->data[0]);
    /* msg_type = DISCOVER_REPLY, NOT ACK. */
    TEST_ASSERT_EQUAL_UINT8((uint8_t)BL_MSG_DISCOVER_REPLY, tx->data[1]);
    /* Payload: opcode echo, node id, proto major, proto minor. */
    TEST_ASSERT_EQUAL_UINT8((uint8_t)BL_CMD_DISCOVER,        tx->data[2]);
    TEST_ASSERT_EQUAL_UINT8(0x01U,                            tx->data[3]); /* default BL_NODE_ID = 0x1 */
    TEST_ASSERT_EQUAL_UINT8((uint8_t)BL_PROTO_VERSION_MAJOR,  tx->data[4]);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)BL_PROTO_VERSION_MINOR,  tx->data[5]);
}

/* ---- Session gating ---- */

/* Each session-gated opcode must NACK with BL_NACK_BAD_SESSION when
 * called from {session=false}. We assert one opcode per test so a
 * regression points at the specific handler whose gate dropped. */

static void assert_nack_bad_session_for_opcode(uint8_t opcode,
                                               const uint8_t *args,
                                               uint8_t args_len)
{
    reset_session_and_tx();
    send_cmd_sf(opcode, args, args_len);
    TEST_ASSERT_EQUAL_INT(1, mock_fdcan_tx_count());
    const mock_fdcan_frame_t *tx = mock_fdcan_get(0);
    TEST_ASSERT_NOT_NULL(tx);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)BL_MSG_NACK, tx->data[1]);
    TEST_ASSERT_EQUAL_UINT8(opcode,                tx->data[2]);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)BL_NACK_BAD_SESSION, tx->data[3]);
}

void test_session_gate_flash_erase_without_connect_nacks_bad_session(void)
{
    /* The session check runs BEFORE the args_len check in
     * handle_flash_erase, so a zero-arg payload is enough to
     * exercise the gate. Realistic 8-byte FLASH_ERASE args wouldn't
     * fit in an SF anyway (would need FF+CF). */
    assert_nack_bad_session_for_opcode((uint8_t)BL_CMD_FLASH_ERASE, NULL, 0U);
}

void test_session_gate_flash_write_without_connect_nacks_bad_session(void)
{
    /* Same shortcut as FLASH_ERASE: session check first, args
     * irrelevant for the gate. */
    assert_nack_bad_session_for_opcode((uint8_t)BL_CMD_FLASH_WRITE, NULL, 0U);
}

void test_session_gate_nvm_read_without_connect_nacks_bad_session(void)
{
    /* NVM_READ takes a 2-byte LE key. */
    uint8_t args[2] = { 0x01U, 0x00U };
    assert_nack_bad_session_for_opcode((uint8_t)BL_CMD_NVM_READ, args, 2U);
}

void test_session_gate_log_stream_start_without_connect_nacks_bad_session(void)
{
    uint8_t args[1] = { 0x02U };  /* optional min-severity argument */
    assert_nack_bad_session_for_opcode((uint8_t)BL_CMD_LOG_STREAM_START, args, 1U);
}

/* ---- NVM round-trip via the dispatcher ---- */

void test_handle_nvm_read_returns_value_for_pre_written_key(void)
{
    /* Pre-program a known key/value via the real bl_nvm API (the
     * host test build links the real bl_nvm.c). Then CONNECT and
     * issue NVM_READ over the wire; assert the ACK carries the
     * exact stored value. This is the host-visible round-trip the
     * operator workflow (`cf config nvm read 0x0001`) depends on. */
    bl_nvm_init();
    const uint16_t key = 0x4567U;
    const uint8_t  expected[3] = { 0xDEU, 0xADU, 0xBEU };
    TEST_ASSERT_EQUAL_INT(BL_NVM_OK,
        bl_nvm_write(key, expected, sizeof(expected)));

    prime_session();

    /* NVM_READ args = key_le16 (2 bytes). */
    uint8_t args[2] = { (uint8_t)(key & 0xFFU), (uint8_t)((key >> 8) & 0xFFU) };
    send_cmd_sf((uint8_t)BL_CMD_NVM_READ, args, 2U);

    /* Reply payload: [opcode, len, value...]. With len=3 the
     * handler builds a 5-byte resp[] and `send_ack` prepends the
     * msg_type byte → 6 wire bytes total: msg_type + opcode + len
     * + 3 value bytes. The whole thing fits in one SF (≤7). */
    TEST_ASSERT_EQUAL_INT(1, mock_fdcan_tx_count());
    const mock_fdcan_frame_t *tx = mock_fdcan_get(0);
    TEST_ASSERT_NOT_NULL(tx);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)(BL_ISOTP_PCI_SF | 0x06U), tx->data[0]);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)BL_MSG_ACK,           tx->data[1]);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)BL_CMD_NVM_READ,      tx->data[2]);
    TEST_ASSERT_EQUAL_UINT8(0x03U,                          tx->data[3]); /* len */
    TEST_ASSERT_EQUAL_UINT8(expected[0],                    tx->data[4]);
    TEST_ASSERT_EQUAL_UINT8(expected[1],                    tx->data[5]);
    TEST_ASSERT_EQUAL_UINT8(expected[2],                    tx->data[6]);
}

void test_handle_nvm_read_unknown_key_nacks_not_found(void)
{
    /* setUp() already wiped flash; NVM is empty. A read against any
     * key must therefore NACK with NVM_NOT_FOUND. */
    bl_nvm_init();
    prime_session();

    uint8_t args[2] = { 0xCDU, 0xABU };  /* arbitrary unused key */
    send_cmd_sf((uint8_t)BL_CMD_NVM_READ, args, 2U);

    TEST_ASSERT_EQUAL_INT(1, mock_fdcan_tx_count());
    const mock_fdcan_frame_t *tx = mock_fdcan_get(0);
    TEST_ASSERT_NOT_NULL(tx);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)BL_MSG_NACK,           tx->data[1]);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)BL_CMD_NVM_READ,       tx->data[2]);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)BL_NACK_NVM_NOT_FOUND, tx->data[3]);
}

void test_handle_nvm_write_persists_value_visible_to_nvm_read_api(void)
{
    /* Send NVM_WRITE for a known key over the wire, then read it
     * back through the direct bl_nvm_read API to confirm the value
     * actually landed in flash. This is the symmetric half of the
     * read round-trip — together they cover the host's full
     * provisioning loop (write key, reboot, confirm via read). */
    bl_nvm_init();
    prime_session();

    /* Args: key_le16 + value bytes. Use key 0x5000 (user-range, not
     * a reserved BL key) so the test can't accidentally trip the
     * node-id resolver in another test. */
    uint8_t args[4] = { 0x00U, 0x50U, 0x11U, 0x22U };
    send_cmd_sf((uint8_t)BL_CMD_NVM_WRITE, args, 4U);

    /* ACK arrived. */
    TEST_ASSERT_EQUAL_INT(1, mock_fdcan_tx_count());
    const mock_fdcan_frame_t *tx = mock_fdcan_get(0);
    TEST_ASSERT_NOT_NULL(tx);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)BL_MSG_ACK,        tx->data[1]);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)BL_CMD_NVM_WRITE,  tx->data[2]);

    /* Value persisted at the right key — read it back directly. */
    uint8_t value[BL_NVM_MAX_VALUE_LEN] = { 0 };
    uint8_t value_len = 0U;
    bl_nvm_status_t st = bl_nvm_read(0x5000U, value, sizeof(value), &value_len);
    TEST_ASSERT_EQUAL_INT(BL_NVM_OK, st);
    TEST_ASSERT_EQUAL_UINT8(2U, value_len);
    TEST_ASSERT_EQUAL_UINT8(0x11U, value[0]);
    TEST_ASSERT_EQUAL_UINT8(0x22U, value[1]);
}

/* ---- Unknown opcodes ---- */

void test_dispatch_unknown_opcode_nacks_unsupported(void)
{
    /* The catch-all in the dispatch table must NACK rather than
     * silently drop — the same #60 principle as the bad-PCI gate.
     * Pick an opcode that's not in the table (0x99 is in the
     * reserved gap between 0x82 NVM_FORMAT and the next future
     * range). */
    reset_session_and_tx();

    send_cmd_sf(0x99U, NULL, 0U);

    TEST_ASSERT_EQUAL_INT(1, mock_fdcan_tx_count());
    const mock_fdcan_frame_t *tx = mock_fdcan_get(0);
    TEST_ASSERT_NOT_NULL(tx);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)BL_MSG_NACK,       tx->data[1]);
    TEST_ASSERT_EQUAL_UINT8(0x99U,                       tx->data[2]);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)BL_NACK_UNSUPPORTED, tx->data[3]);
}

/* ---- #123 (general fix): NOTIFY suppression during reassembly ---- */

void test_send_notify_suppressed_while_reassembly_in_flight(void)
{
    /* Regression guard for audit finding H1 / issue #123. While an
     * inbound multi-frame transfer is mid-flight (g_rx in WAIT_CF), an
     * unsolicited NOTIFY (heartbeat / live-data / log / DTC — all route
     * through bl_proto_send_notify) must NOT be emitted: dropping a
     * multi-frame NOTIFY onto the bus mid-WRITE_CHUNK strands the
     * host's transfer (BlockSize=0 → no pacing) → 1 s timeout → NACK
     * 0x09. This is exactly the failure the bench saw. */
    reset_session_and_tx();

    /* Feed a First Frame so the reassembler enters WAIT_CF. total=134
     * (0x86) means it now expects CFs. The FF itself triggers one
     * FC(CTS) reply — clear it so the assertion sees only what the
     * NOTIFY does (or doesn't) emit. */
    bl_proto_id_t id = host_to_us();
    uint8_t ff[8] = { 0x10U, 0x86U, (uint8_t)BL_MSG_CMD,
                      (uint8_t)BL_CMD_FLASH_WRITE, 0x00U, 0x00U, 0x02U, 0x08U };
    bl_proto_dispatch(&id, ff, 8U);
    TEST_ASSERT_EQUAL_UINT16(6U, bl_proto_isotp_rx_progress()); /* WAIT_CF, 6 bytes in */
    mock_fdcan_reset();

    /* A heartbeat-shaped NOTIFY while mid-reassembly → suppressed. */
    uint8_t notify[7] = { 0xF0U, 0x01U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U };
    bl_proto_send_notify(notify, (uint16_t)sizeof(notify));
    TEST_ASSERT_EQUAL_INT(0, mock_fdcan_tx_count());
}

void test_send_notify_emitted_when_idle(void)
{
    /* Control: with no reassembly in flight, the same NOTIFY DOES go
     * out — confirms the gate only suppresses during WAIT_CF and
     * doesn't silence heartbeats on an idle link.
     *
     * Note the 7-byte heartbeat payload becomes an 8-byte message
     * (send_message prepends msg_type) which exceeds the 7-byte SF
     * limit → it ships as FF + CF (2 frames). That multi-frame shape
     * is precisely why an unsolicited heartbeat mid-transfer was so
     * destructive in #123 — so the control asserts "emitted at all"
     * rather than a specific frame count, keeping it robust to
     * ISO-TP framing details covered by other tests. */
    reset_session_and_tx();
    TEST_ASSERT_EQUAL_UINT16(0U, bl_proto_isotp_rx_progress());

    uint8_t notify[7] = { 0xF0U, 0x01U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U };
    bl_proto_send_notify(notify, (uint16_t)sizeof(notify));

    TEST_ASSERT_GREATER_OR_EQUAL_INT(1, mock_fdcan_tx_count());
    /* First frame is the ISO-TP FF: data[0]=FF PCI, data[1]=len low
     * byte, data[2]=msg_type. Confirm it's a NOTIFY. */
    const mock_fdcan_frame_t *tx = mock_fdcan_get(0);
    TEST_ASSERT_NOT_NULL(tx);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)BL_ISOTP_PCI_FF, (uint8_t)(tx->data[0] & 0xF0U));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)BL_MSG_NOTIFY,   tx->data[2]);
}

/* ---- #125 C2: session-timeout must not auto-jump after a flash ---- */

void test_session_timeout_after_flash_does_not_jump(void)
{
    /* The nightmare path: host starts reflashing (FLASH_WRITE), then
     * the link drops. 30 s later the session watchdog fires. The OLD
     * app metadata still validates (it's only rewritten on the host's
     * explicit FLASH_VERIFY), so pre-fix the BL would auto-jump into a
     * half-written binary — recoverable only by SWD. The flash-dirty
     * latch must suppress that jump and keep the BL in listen mode. */
    mock_set_tick(1000U);
    prime_session();                       /* CONNECT at t=1000 */

    /* A FLASH_WRITE marks the session flash-dirty (addr = BL_APP_BASE
     * 0x08020000 LE + one data byte). */
    uint8_t wargs[5] = { 0x00U, 0x00U, 0x02U, 0x08U, 0xAAU };
    send_cmd_sf((uint8_t)BL_CMD_FLASH_WRITE, wargs, 5U);
    mock_fdcan_reset();

    /* Host vanishes past the session timeout, then the watchdog ticks. */
    mock_advance_tick(31000U);
    bl_proto_tick(HAL_GetTick());

    TEST_ASSERT_EQUAL_INT(0, mock_bootloader_jump_count());   /* did NOT jump */
    TEST_ASSERT_FALSE(bl_proto_session_active());             /* session torn down */
}

void test_session_timeout_clean_diagnostic_session_still_jumps(void)
{
    /* Regression guard the other way: a pure CONNECT + (no flash)
     * session that times out SHOULD still hand control to a valid app,
     * so a one-off diagnostic poke doesn't strand the board in the
     * bootloader. Only flash-dirty sessions suppress the jump. */
    mock_set_tick(1000U);
    prime_session();                       /* CONNECT only, no flash */

    mock_advance_tick(31000U);
    bl_proto_tick(HAL_GetTick());

    TEST_ASSERT_EQUAL_INT(1, mock_bootloader_jump_count());   /* jumped to valid app */
    TEST_ASSERT_FALSE(bl_proto_session_active());
}

/* ---- #125 C4: OB_APPLY_WRP sector-bitmap validation ---- */

/* Frame [BL_MSG_CMD, opcode, args...] as an ISO-TP FF + one CF and
 * dispatch both. Covers 8..13-byte messages (FF carries 6 bytes, CF
 * up to 7) — enough for the 10-byte OB_APPLY_WRP (token + bitmap)
 * which doesn't fit a single SF. Not a general segmenter. */
static void send_cmd_multiframe(uint8_t opcode, const uint8_t *args, uint8_t args_len)
{
    bl_proto_id_t id = host_to_us();
    uint8_t total = (uint8_t)(2U + args_len);   /* msg_type + opcode + args */
    TEST_ASSERT_LESS_OR_EQUAL_UINT8(13U, total);

    uint8_t msg[16] = { 0 };
    msg[0] = (uint8_t)BL_MSG_CMD;
    msg[1] = opcode;
    for (uint8_t i = 0U; i < args_len; i++) {
        msg[2U + i] = args[i];
    }

    /* FF: PCI high nibble = 0x1, 12-bit total length, then 6 bytes. */
    uint8_t ff[8];
    ff[0] = (uint8_t)(BL_ISOTP_PCI_FF | ((total >> 8) & 0x0FU));
    ff[1] = (uint8_t)(total & 0xFFU);
    for (uint8_t i = 0U; i < 6U; i++) {
        ff[2U + i] = msg[i];
    }
    bl_proto_dispatch(&id, ff, 8U);

    /* CF seq 1: the remaining bytes. */
    uint8_t rem = (uint8_t)(total - 6U);
    uint8_t cf[8];
    cf[0] = (uint8_t)(BL_ISOTP_PCI_CF | 0x01U);
    for (uint8_t i = 0U; i < rem; i++) {
        cf[1U + i] = msg[6U + i];
    }
    bl_proto_dispatch(&id, cf, (uint8_t)(1U + rem));
}

/* The valid OB_APPLY_WRP token (BL_OB_APPLY_TOKEN = 0x00505257, "WRP\0")
 * as little-endian wire bytes. */
#define OB_TOKEN_LE  0x57U, 0x52U, 0x50U, 0x00U

void test_ob_apply_wrp_rejects_non_bootloader_sector(void)
{
    /* The permanent-brick path: a host (bug or finger-trouble) asks to
     * WRP an app sector. Pre-fix the BL would WRP it and reset, after
     * which the ECU can never be reflashed over CAN. The validation
     * must NACK and the request must never reach the option-byte
     * layer. */
    prime_session();
    uint8_t args[8] = { OB_TOKEN_LE, 0x02U, 0x00U, 0x00U, 0x00U }; /* mask = sector 1 */
    send_cmd_multiframe((uint8_t)BL_CMD_OB_APPLY_WRP, args, 8U);

    int n = mock_fdcan_tx_count();
    TEST_ASSERT_GREATER_OR_EQUAL_INT(1, n);
    const mock_fdcan_frame_t *tx = mock_fdcan_get(n - 1);   /* last reply = the NACK */
    TEST_ASSERT_NOT_NULL(tx);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)BL_MSG_NACK,         tx->data[1]);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)BL_CMD_OB_APPLY_WRP, tx->data[2]);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)BL_NACK_UNSUPPORTED, tx->data[3]);
    TEST_ASSERT_EQUAL_INT(0, mock_ob_apply_wrp_calls());   /* never reached OB layer */
}

void test_ob_apply_wrp_accepts_bootloader_sector(void)
{
    /* The intended use: token only (SF) → default mask 0x01 → applies
     * to exactly sector 0. */
    prime_session();
    uint8_t args[4] = { OB_TOKEN_LE };
    send_cmd_sf((uint8_t)BL_CMD_OB_APPLY_WRP, args, 4U);

    TEST_ASSERT_EQUAL_INT(1, mock_ob_apply_wrp_calls());
    TEST_ASSERT_EQUAL_UINT32(0x01U, mock_ob_apply_wrp_last_mask());
}

void test_ob_apply_wrp_forces_sector0_when_mask_zero(void)
{
    /* A host that explicitly passes mask 0x00 must NOT end up shipping
     * an unprotected bootloader — bit 0 is forced on. */
    prime_session();
    uint8_t args[8] = { OB_TOKEN_LE, 0x00U, 0x00U, 0x00U, 0x00U }; /* mask = 0 */
    send_cmd_multiframe((uint8_t)BL_CMD_OB_APPLY_WRP, args, 8U);

    TEST_ASSERT_EQUAL_INT(1, mock_ob_apply_wrp_calls());
    TEST_ASSERT_EQUAL_UINT32(0x01U, mock_ob_apply_wrp_last_mask()); /* forced to sector 0 */
}

/* ---- #125 H6 probe: FLASH_ERASE reports its duration ---- */

void test_flash_erase_records_op_duration(void)
{
    /* The erase-duration probe (H6) brackets bl_flash_erase with
     * HAL_GetTick and reports the elapsed ms to bl_health, which
     * surfaces the per-boot max in the health record so the bench can
     * read the worst-case erase time (→ sizes the future IWDG period).
     * Host-side the duration is 0 (instant stub + frozen mock tick),
     * so we assert the plumbing fired: handle_flash_erase called the
     * recorder exactly once.
     *
     * FLASH_ERASE args = start_le32 + length_le32 = 8 bytes → the
     * message is 10 bytes, so it arrives as FF + CF (multi-frame). */
    prime_session();
    uint8_t eargs[8] = { 0x00U, 0x00U, 0x02U, 0x08U,   /* start = 0x08020000 */
                         0x00U, 0x00U, 0x02U, 0x00U };  /* length = 0x20000   */
    send_cmd_multiframe((uint8_t)BL_CMD_FLASH_ERASE, eargs, 8U);

    TEST_ASSERT_EQUAL_INT(1, mock_flash_op_ms_calls());
}
