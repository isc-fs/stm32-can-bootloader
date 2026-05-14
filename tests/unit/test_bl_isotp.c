/*
 * test_bl_isotp.c — ISO-TP RX reassembly and TX segmentation tests.
 *
 * bl_isotp is pure data manipulation with no HAL surface, so these run
 * end-to-end without any mocks beyond Unity itself. They lock down the
 * framing edge cases the bootloader has historically gotten wrong on
 * untrusted input: bad PCI bytes, sequence-number mismatches, declared
 * lengths over the buffer cap, and the FF + CF chain integrity.
 */

#include "bl_isotp.h"
#include "stm32h7xx_hal.h"   /* for mock_set_tick / mock_advance_tick */
#include "unity.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* Test message type — value isn't enforced by the reassembler,
 * we just need something. */
#define TYPE_CMD    0x00U
#define PEER_HOST   0x01U

/* ---- Helpers ---- */

static void make_sf(uint8_t *frame, const uint8_t *payload, uint8_t len)
{
    frame[0] = (uint8_t)(BL_ISOTP_PCI_SF | (len & 0x0FU));
    memcpy(&frame[1], payload, len);
}

static void make_ff(uint8_t *frame, uint16_t total_len,
                    const uint8_t *first_chunk, uint8_t first_chunk_len)
{
    frame[0] = (uint8_t)(BL_ISOTP_PCI_FF | ((total_len >> 8) & 0x0FU));
    frame[1] = (uint8_t)(total_len & 0xFFU);
    memcpy(&frame[2], first_chunk, first_chunk_len);
}

static void make_cf(uint8_t *frame, uint8_t seq,
                    const uint8_t *chunk, uint8_t chunk_len)
{
    frame[0] = (uint8_t)(BL_ISOTP_PCI_CF | (seq & 0x0FU));
    memcpy(&frame[1], chunk, chunk_len);
}

/* ---- RX tests ---- */

/* Arbitrary fixed tick for tests that don't care about timing — keeps
 * the rx_feed call sites compact and signals "any now_ms is fine here". */
#define NOW_DONT_CARE   0U

void test_isotp_rx_single_frame_completes_in_one_feed(void)
{
    bl_isotp_rx_t rx;
    bl_isotp_rx_init(&rx);

    uint8_t payload[7] = {0xDE,0xAD,0xBE,0xEF,0x01,0x02,0x03};
    uint8_t frame[8] = {0};
    make_sf(frame, payload, 7);

    bool send_fc = false;
    bl_isotp_rx_status_t st = bl_isotp_rx_feed(&rx, TYPE_CMD, PEER_HOST,
                                               NOW_DONT_CARE,
                                               frame, 8, &send_fc);
    TEST_ASSERT_EQUAL_INT(BL_ISOTP_MSG_COMPLETE, st);
    TEST_ASSERT_EQUAL_UINT16(7, rx.received);
    TEST_ASSERT_EQUAL_MEMORY(payload, rx.buf, 7);
    TEST_ASSERT_FALSE(send_fc);
}

void test_isotp_rx_first_frame_starts_assembly(void)
{
    bl_isotp_rx_t rx;
    bl_isotp_rx_init(&rx);

    /* Declare a 12-byte total: FF has 6 data bytes after PCI(2). */
    uint8_t chunk[6] = {0xAA,0xBB,0xCC,0xDD,0xEE,0xFF};
    uint8_t frame[8] = {0};
    make_ff(frame, 12, chunk, 6);

    bool send_fc = false;
    /* Use a non-zero now_ms so we can confirm the deadline was armed
     * to now_ms + TIMEOUT — proves the fix/14 invariant landed. */
    bl_isotp_rx_status_t st = bl_isotp_rx_feed(&rx, TYPE_CMD, PEER_HOST,
                                               500U,
                                               frame, 8, &send_fc);
    TEST_ASSERT_EQUAL_INT(BL_ISOTP_OK, st);
    TEST_ASSERT_TRUE(send_fc);                    /* must reply CTS */
    TEST_ASSERT_EQUAL_UINT16(6, rx.received);
    TEST_ASSERT_EQUAL_INT(BL_ISOTP_STATE_WAIT_CF, rx.state);
    TEST_ASSERT_EQUAL_UINT8(1, rx.next_seq);
    TEST_ASSERT_EQUAL_UINT32(500U + BL_ISOTP_TIMEOUT_MS, rx.deadline_ms);
}

void test_isotp_rx_ff_plus_cf_chain_completes(void)
{
    bl_isotp_rx_t rx;
    bl_isotp_rx_init(&rx);

    /* 12-byte message: FF carries first 6, one CF(seq=1) carries the rest. */
    uint8_t ff_chunk[6] = {0x01,0x02,0x03,0x04,0x05,0x06};
    uint8_t cf_chunk[6] = {0x07,0x08,0x09,0x0A,0x0B,0x0C};

    uint8_t ff[8] = {0};
    make_ff(ff, 12, ff_chunk, 6);
    bool send_fc = false;
    TEST_ASSERT_EQUAL_INT(BL_ISOTP_OK,
        bl_isotp_rx_feed(&rx, TYPE_CMD, PEER_HOST, NOW_DONT_CARE,
                         ff, 8, &send_fc));

    uint8_t cf[8] = {0};
    make_cf(cf, 1, cf_chunk, 6);
    bl_isotp_rx_status_t st = bl_isotp_rx_feed(&rx, TYPE_CMD, PEER_HOST,
                                               NOW_DONT_CARE,
                                               cf, 7, &send_fc);
    TEST_ASSERT_EQUAL_INT(BL_ISOTP_MSG_COMPLETE, st);
    TEST_ASSERT_EQUAL_UINT16(12, rx.received);

    uint8_t expected[12] = {0x01,0x02,0x03,0x04,0x05,0x06,
                            0x07,0x08,0x09,0x0A,0x0B,0x0C};
    TEST_ASSERT_EQUAL_MEMORY(expected, rx.buf, 12);
}

void test_isotp_rx_cf_with_wrong_sequence_errors(void)
{
    bl_isotp_rx_t rx;
    bl_isotp_rx_init(&rx);

    uint8_t ff_chunk[6] = {1,2,3,4,5,6};
    uint8_t ff[8] = {0};
    make_ff(ff, 14, ff_chunk, 6);
    bool send_fc;
    TEST_ASSERT_EQUAL_INT(BL_ISOTP_OK,
        bl_isotp_rx_feed(&rx, TYPE_CMD, PEER_HOST, NOW_DONT_CARE,
                         ff, 8, &send_fc));

    /* Send CF with seq=2 instead of expected 1. */
    uint8_t cf_chunk[7] = {7,8,9,10,11,12,13};
    uint8_t cf[8] = {0};
    make_cf(cf, 2, cf_chunk, 7);
    bl_isotp_rx_status_t st = bl_isotp_rx_feed(&rx, TYPE_CMD, PEER_HOST,
                                               NOW_DONT_CARE,
                                               cf, 8, &send_fc);
    TEST_ASSERT_EQUAL_INT(BL_ISOTP_ERR_BAD_SEQ, st);
}

void test_isotp_rx_cf_without_ff_errors(void)
{
    bl_isotp_rx_t rx;
    bl_isotp_rx_init(&rx);

    uint8_t chunk[7] = {1,2,3,4,5,6,7};
    uint8_t cf[8] = {0};
    make_cf(cf, 1, chunk, 7);
    bool send_fc;
    bl_isotp_rx_status_t st = bl_isotp_rx_feed(&rx, TYPE_CMD, PEER_HOST,
                                               NOW_DONT_CARE,
                                               cf, 8, &send_fc);
    TEST_ASSERT_EQUAL_INT(BL_ISOTP_ERR_NO_FF, st);
}

void test_isotp_rx_overflow_too_long_message_rejects(void)
{
    bl_isotp_rx_t rx;
    bl_isotp_rx_init(&rx);

    uint8_t chunk[6] = {0};
    uint8_t ff[8] = {0};
    /* Declare a total length larger than BL_ISOTP_MAX_MSG. */
    make_ff(ff, (uint16_t)(BL_ISOTP_MAX_MSG + 1U), chunk, 6);

    bool send_fc;
    bl_isotp_rx_status_t st = bl_isotp_rx_feed(&rx, TYPE_CMD, PEER_HOST,
                                               NOW_DONT_CARE,
                                               ff, 8, &send_fc);
    TEST_ASSERT_EQUAL_INT(BL_ISOTP_ERR_OVERFLOW, st);
}

void test_isotp_rx_timeout_invalidates_partial_assembly(void)
{
    /* Regression for issue #54: prior to fix/14, handle_ff() called
     * reset_idle() (which zeroes rx->deadline_ms) but no code path in
     * bl_isotp_rx_feed armed the deadline afterwards. Production papered
     * over this by stamping g_rx.deadline_ms in bl_proto.c right after
     * the feed returned send_fc=true — but any caller that didn't do
     * that (test harnesses, loopback simulators) saw the reassembly
     * time out the instant after FF receipt.
     *
     * Fix: thread now_ms into bl_isotp_rx_feed; handle_ff arms the
     * deadline before returning, so the invariant lives inside the
     * module. This test now exercises the full path. */
    bl_isotp_rx_t rx;
    bl_isotp_rx_init(&rx);

    uint8_t chunk[6] = {1,2,3,4,5,6};
    uint8_t ff[8] = {0};
    make_ff(ff, 14, chunk, 6);
    bool send_fc;
    /* FF at now_ms = 0 → deadline armed to BL_ISOTP_TIMEOUT_MS. */
    TEST_ASSERT_EQUAL_INT(BL_ISOTP_OK,
        bl_isotp_rx_feed(&rx, TYPE_CMD, PEER_HOST, 0U, ff, 8, &send_fc));
    TEST_ASSERT_EQUAL_UINT32(BL_ISOTP_TIMEOUT_MS, rx.deadline_ms);

    uint8_t peer_out = 0;
    TEST_ASSERT_FALSE(bl_isotp_rx_tick(&rx, BL_ISOTP_TIMEOUT_MS - 1U, &peer_out));

    TEST_ASSERT_TRUE(bl_isotp_rx_tick(&rx, BL_ISOTP_TIMEOUT_MS + 1U, &peer_out));
    TEST_ASSERT_EQUAL_UINT8(PEER_HOST, peer_out);
    TEST_ASSERT_EQUAL_INT(BL_ISOTP_STATE_IDLE, rx.state);

    /* tick() reset the state — a second call must not re-fire. */
    TEST_ASSERT_FALSE(bl_isotp_rx_tick(&rx, BL_ISOTP_TIMEOUT_MS + 2U, &peer_out));
}

void test_isotp_rx_timeout_handles_tick_wraparound(void)
{
    /* Belt-and-suspenders for the signed-diff path in bl_isotp_rx_tick:
     * when an FF arrives near the top of the 32-bit tick range, the
     * deadline math wraps around 0. The tick() function uses
     * (int32_t)(now - deadline) >= 0 specifically to handle this case.
     *
     * Scenario: FF at now_ms = 0xFFFFFF00. With TIMEOUT_MS = 1000
     * (0x3E8) the deadline lands at 0x000002E8 after unsigned wrap.
     * tick() must NOT fire at 0xFFFFFFFF (still inside the window) and
     * MUST fire at 0x00000400 (past the wrapped deadline). */
    bl_isotp_rx_t rx;
    bl_isotp_rx_init(&rx);

    uint8_t chunk[6] = {1,2,3,4,5,6};
    uint8_t ff[8] = {0};
    make_ff(ff, 14, chunk, 6);
    bool send_fc;
    TEST_ASSERT_EQUAL_INT(BL_ISOTP_OK,
        bl_isotp_rx_feed(&rx, TYPE_CMD, PEER_HOST,
                         0xFFFFFF00U, ff, 8, &send_fc));
    /* Confirm the deadline wrapped through zero. */
    TEST_ASSERT_EQUAL_UINT32(
        (uint32_t)(0xFFFFFF00U + BL_ISOTP_TIMEOUT_MS), rx.deadline_ms);

    uint8_t peer_out = 0;
    /* Still inside the window — about to wrap but not past the deadline. */
    TEST_ASSERT_FALSE(bl_isotp_rx_tick(&rx, 0xFFFFFFFFU, &peer_out));
    /* Past the wrapped deadline — must fire. */
    TEST_ASSERT_TRUE(bl_isotp_rx_tick(&rx, 0x00000400U, &peer_out));
    TEST_ASSERT_EQUAL_UINT8(PEER_HOST, peer_out);
}

void test_isotp_rx_cf_with_no_payload_is_rejected(void)
{
    /* Regression for the first bullet of audit follow-ups (#68): a CF
     * with length == 1 (PCI byte only, no payload data) used to be
     * silently accepted — handle_cf computed frame_avail = 0,
     * memcpy'd 0 bytes, but still advanced rx->next_seq. The protocol
     * then proceeded with a sequence-vs-content invariant violation:
     * every subsequent CF consumed a sequence slot without adding to
     * rx->received, so rx->total_len was never reached and the
     * reassembler stalled until the 1-second deadline fired.
     *
     * The fix rejects zero-payload CFs at the PCI level so the host
     * gets immediate diagnostic feedback (NACK(TRANSPORT_ERROR) via
     * the dispatcher) instead of waiting for the timeout. */
    bl_isotp_rx_t rx;
    bl_isotp_rx_init(&rx);

    /* Start a multi-frame reassembly so we're in WAIT_CF state. */
    uint8_t ff_chunk[6] = {1, 2, 3, 4, 5, 6};
    uint8_t ff[8] = {0};
    make_ff(ff, 14, ff_chunk, 6);
    bool send_fc;
    TEST_ASSERT_EQUAL_INT(BL_ISOTP_OK,
        bl_isotp_rx_feed(&rx, TYPE_CMD, PEER_HOST, NOW_DONT_CARE,
                         ff, 8, &send_fc));

    /* Now feed a CF with only the PCI byte (length=1). */
    uint8_t cf_pci_only[1] = { (uint8_t)(BL_ISOTP_PCI_CF | 0x01U) };
    bl_isotp_rx_status_t st = bl_isotp_rx_feed(&rx, TYPE_CMD, PEER_HOST,
                                               NOW_DONT_CARE,
                                               cf_pci_only, 1, &send_fc);

    /* The fix rejects it as malformed PCI rather than stalling. */
    TEST_ASSERT_EQUAL_INT(BL_ISOTP_ERR_BAD_PCI, st);

    /* And critically, rx->next_seq was NOT advanced — so if the host
     * recovers and resends a well-formed CF, the protocol picks up
     * where it left off (seq=1) instead of getting stuck at seq=2
     * with no path to total_len. */
    TEST_ASSERT_EQUAL_UINT8(1U, rx.next_seq);
}

void test_isotp_rx_sf_zero_length_is_rejected(void)
{
    bl_isotp_rx_t rx;
    bl_isotp_rx_init(&rx);

    /* SF with L=0 (PCI = 0x00) is not legal — represents an empty
     * single frame which the protocol forbids. */
    uint8_t frame[8] = {0};
    frame[0] = (uint8_t)BL_ISOTP_PCI_SF | 0x00U;
    bool send_fc;
    bl_isotp_rx_status_t st = bl_isotp_rx_feed(&rx, TYPE_CMD, PEER_HOST,
                                               NOW_DONT_CARE,
                                               frame, 8, &send_fc);
    TEST_ASSERT_EQUAL_INT(BL_ISOTP_ERR_BAD_PCI, st);
}

/* ---- TX tests ---- */

void test_isotp_tx_single_frame_below_8_bytes(void)
{
    uint8_t payload[5] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE};
    bl_isotp_tx_t tx;
    bl_isotp_tx_init(&tx, payload, sizeof(payload));

    uint8_t frame[8] = {0};
    uint8_t dlc = 0;
    TEST_ASSERT_TRUE(bl_isotp_tx_next(&tx, frame, &dlc));
    /* SF PCI carries len in low nibble. */
    TEST_ASSERT_EQUAL_UINT8((uint8_t)(BL_ISOTP_PCI_SF | 5U), frame[0]);
    TEST_ASSERT_EQUAL_MEMORY(payload, &frame[1], 5);
    TEST_ASSERT_EQUAL_UINT8(6U, dlc);    /* 1 PCI + 5 data */

    /* Only one frame for SF; second call returns false. */
    TEST_ASSERT_FALSE(bl_isotp_tx_next(&tx, frame, &dlc));
}

void test_isotp_tx_multi_frame_yields_ff_then_cfs(void)
{
    uint8_t payload[20] = {0};
    for (uint8_t i = 0; i < 20; i++) payload[i] = (uint8_t)(0x10 + i);

    bl_isotp_tx_t tx;
    bl_isotp_tx_init(&tx, payload, sizeof(payload));

    uint8_t frame[8] = {0};
    uint8_t dlc = 0;

    /* First call: FF, declared length = 20. */
    TEST_ASSERT_TRUE(bl_isotp_tx_next(&tx, frame, &dlc));
    TEST_ASSERT_EQUAL_UINT8(BL_ISOTP_PCI_FF | 0x00U, frame[0]);  /* high nibble of 20 = 0 */
    TEST_ASSERT_EQUAL_UINT8(20U, frame[1]);
    TEST_ASSERT_EQUAL_MEMORY(&payload[0], &frame[2], 6);
    TEST_ASSERT_EQUAL_UINT8(8U, dlc);

    /* Second call: CF seq=1, 7 bytes. */
    TEST_ASSERT_TRUE(bl_isotp_tx_next(&tx, frame, &dlc));
    TEST_ASSERT_EQUAL_UINT8(BL_ISOTP_PCI_CF | 1U, frame[0]);
    TEST_ASSERT_EQUAL_MEMORY(&payload[6], &frame[1], 7);
    TEST_ASSERT_EQUAL_UINT8(8U, dlc);

    /* Third call: CF seq=2, remaining 7 bytes. */
    TEST_ASSERT_TRUE(bl_isotp_tx_next(&tx, frame, &dlc));
    TEST_ASSERT_EQUAL_UINT8(BL_ISOTP_PCI_CF | 2U, frame[0]);
    TEST_ASSERT_EQUAL_MEMORY(&payload[13], &frame[1], 7);
    TEST_ASSERT_EQUAL_UINT8(8U, dlc);

    /* Iterator done. */
    TEST_ASSERT_FALSE(bl_isotp_tx_next(&tx, frame, &dlc));
}
