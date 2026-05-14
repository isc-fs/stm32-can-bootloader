/*
 * test_bl_log.c — bl_log_drain / evict_oldest defence-in-depth tests.
 *
 * Background: the BL log ring lives in BKPSRAM, which is reachable by
 * application firmware running before us and survives soft resets.
 * Any byte we read out of it must be treated as untrusted in the same
 * way we treat host CAN traffic. Issue #65 was about the consumer side
 * (bl_log_drain) trusting the entry-length byte without a bound — a
 * corrupt ent_len=0xFF asked for HEADER+255 bytes into a HEADER+120-
 * byte stack scratch[].
 *
 * The fix adds an entry_consistent() bound check on both lifters
 * (bl_log_drain's loop + evict_oldest) and a reset_ring_corrupt() that
 * zeros the ring on detection. These tests pin that behaviour down by
 * planting deliberately-bogus state directly into the BKPSRAM-resident
 * struct via the BL_LOG_RING_ADDR mock override, then exercising the
 * consumer paths and asserting nothing overflows + the ring ends up in
 * a clean state.
 */

#include "bl_log.h"
#include "bl_memmap.h"   /* BL_LOG_RING_ADDR (mock override) */
#include "bl_stubs.h"
#include "unity.h"

#include <stdint.h>
#include <string.h>

/* Convenience pointer at the ring. Both production and tests reach the
 * ring through this address — same surface, no special back-doors. */
static bl_log_ring_t *fake_ring(void)
{
    return (bl_log_ring_t *)BL_LOG_RING_ADDR;
}

/* Plant a clean, empty ring with the magic set. After this, bl_log_init
 * would be a no-op; we skip calling it because the explicit state-write
 * makes it obvious what each test starts from. */
static void make_empty_clean_ring(void)
{
    bl_log_ring_t *r = fake_ring();
    memset(r, 0, sizeof(*r));
    r->magic = BL_LOG_MAGIC;
}

/* Plant a single, well-formed entry of `text_len` bytes at the start
 * of the ring (read_pos=0, write_pos and unread_bytes set accordingly).
 * Returns the total entry size (header + text). */
static size_t plant_one_entry(uint8_t severity, uint8_t text_len, uint8_t fill_byte)
{
    bl_log_ring_t *r = fake_ring();
    r->magic = BL_LOG_MAGIC;
    r->read_pos = 0U;
    r->buffer[0] = text_len;
    r->buffer[1] = severity;
    r->buffer[2] = 0U;
    r->buffer[3] = 0U;
    r->buffer[4] = 0U;
    r->buffer[5] = 0U;
    memset(&r->buffer[BL_LOG_ENTRY_HEADER_SIZE], fill_byte, text_len);
    size_t total = (size_t)BL_LOG_ENTRY_HEADER_SIZE + text_len;
    r->write_pos = (uint32_t)total;
    r->unread_bytes = (uint32_t)total;
    r->dropped_bytes = 0U;
    return total;
}


/* ---- The smoking gun: corrupt ent_len doesn't overflow scratch[] ---- */

void test_log_drain_clamps_oversized_ent_len(void)
{
    /* Plant a fake "entry" where the length byte claims 0xFF (255) but
     * unread_bytes is set to the full ring so the secondary bound
     * (ent_total > unread_bytes) doesn't fire first. The fix's
     * primary clamp — ent_len > BL_LOG_MAX_ENTRY_TEXT — is what we're
     * actually exercising here.
     *
     * Without the fix, bl_log_drain would ring_read(scratch, 261) into
     * a 126-byte scratch buffer = stack smash. With the fix, the
     * consumer must short-circuit and leave the ring in a clean state. */
    make_empty_clean_ring();
    bl_log_ring_t *r = fake_ring();
    r->buffer[0] = 0xFFU;                        /* bogus ent_len */
    r->buffer[1] = BL_LOG_SEV_INFO;
    r->unread_bytes = BL_LOG_RING_BYTES;         /* lots of "data" so
                                                  * the unread bound
                                                  * passes (we want the
                                                  * length-byte clamp
                                                  * to fire, not the
                                                  * unread guard) */

    uint8_t out[BL_LOG_DRAIN_BUDGET];
    size_t written = bl_log_drain(out, sizeof(out), BL_LOG_SEV_INFO);

    /* Drain emits nothing for a corrupt entry. */
    TEST_ASSERT_EQUAL_UINT32(0U, (uint32_t)written);

    /* And the ring is back to a known-clean state. */
    TEST_ASSERT_EQUAL_HEX32(BL_LOG_MAGIC, r->magic);
    TEST_ASSERT_EQUAL_UINT32(0U, r->unread_bytes);
    TEST_ASSERT_EQUAL_UINT32(0U, r->read_pos);
    TEST_ASSERT_EQUAL_UINT32(0U, r->write_pos);
}

void test_log_drain_clamps_when_unread_smaller_than_declared_entry(void)
{
    /* Second arm of the entry_consistent check: ent_len is within the
     * legal 0..120 range, but unread_bytes is smaller than HEADER+len.
     * Production code would still ring_read past the end of legitimate
     * data; the fix must catch this too. */
    make_empty_clean_ring();
    bl_log_ring_t *r = fake_ring();
    r->buffer[0] = 100U;                         /* legal length */
    r->buffer[1] = BL_LOG_SEV_INFO;
    r->unread_bytes = 50U;                       /* but only 50 bytes
                                                  * really exist */

    uint8_t out[BL_LOG_DRAIN_BUDGET];
    size_t written = bl_log_drain(out, sizeof(out), BL_LOG_SEV_INFO);

    TEST_ASSERT_EQUAL_UINT32(0U, (uint32_t)written);
    TEST_ASSERT_EQUAL_UINT32(0U, r->unread_bytes);
}


/* ---- Round-trip: a well-formed entry still drains correctly ---- */

void test_log_drain_well_formed_entry_passes_through(void)
{
    /* Negative control: the corruption guard must not regress the
     * happy path. Plant a single 5-byte INFO entry, drain it, get the
     * header + 5 bytes back verbatim. */
    make_empty_clean_ring();
    plant_one_entry(BL_LOG_SEV_INFO, 5U, 0xAA);

    uint8_t out[BL_LOG_DRAIN_BUDGET] = {0};
    size_t written = bl_log_drain(out, sizeof(out), BL_LOG_SEV_INFO);

    size_t expected = (size_t)BL_LOG_ENTRY_HEADER_SIZE + 5U;
    TEST_ASSERT_EQUAL_UINT32((uint32_t)expected, (uint32_t)written);
    TEST_ASSERT_EQUAL_UINT8(5U, out[0]);                      /* len */
    TEST_ASSERT_EQUAL_UINT8(BL_LOG_SEV_INFO, out[1]);         /* sev */
    for (size_t i = 0; i < 5U; i++) {
        TEST_ASSERT_EQUAL_UINT8(0xAAU, out[BL_LOG_ENTRY_HEADER_SIZE + i]);
    }
    TEST_ASSERT_EQUAL_UINT32(0U, fake_ring()->unread_bytes);  /* consumed */
}

void test_log_drain_severity_filter_below_threshold_is_skipped(void)
{
    /* Another regression guard: severity filtering still works after
     * the bound-check refactor. INFO entry, drain at WARN threshold,
     * expect nothing emitted but the entry still consumed from the
     * ring (the "once you start streaming, anything below is gone for
     * good" policy in bl_log.h). */
    make_empty_clean_ring();
    plant_one_entry(BL_LOG_SEV_INFO, 7U, 0x55);

    uint8_t out[BL_LOG_DRAIN_BUDGET];
    size_t written = bl_log_drain(out, sizeof(out), BL_LOG_SEV_WARN);

    TEST_ASSERT_EQUAL_UINT32(0U, (uint32_t)written);
    /* Skipped, not retained. */
    TEST_ASSERT_EQUAL_UINT32(0U, fake_ring()->unread_bytes);
}


/* ---- bl_log_init catches a corrupt ring at boot ---- */

void test_log_init_zeros_ring_when_magic_is_wrong(void)
{
    /* Pre-existing bl_log_init contract: invalid state at boot → wipe
     * the ring. This is the upstream layer of the same defence-in-
     * depth and worth pinning down. */
    bl_log_ring_t *r = fake_ring();
    r->magic = 0xDEADBEEFU;     /* not BL_LOG_MAGIC */
    r->write_pos = 0xFFFFFFFFU; /* out of range */
    r->read_pos = 0xFFFFFFFFU;
    r->unread_bytes = 0xFFFFFFFFU;

    bl_log_init();

    TEST_ASSERT_EQUAL_HEX32(BL_LOG_MAGIC, r->magic);
    TEST_ASSERT_EQUAL_UINT32(0U, r->unread_bytes);
    TEST_ASSERT_EQUAL_UINT32(0U, r->read_pos);
    TEST_ASSERT_EQUAL_UINT32(0U, r->write_pos);
}
