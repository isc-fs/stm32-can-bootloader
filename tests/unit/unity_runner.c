/*
 * Unity entry point. Add new test functions here as the suite grows.
 *
 * Convention: one forward-declaration block per test_<module>.c file,
 * one `RUN_TEST(...)` per test inside `main`.
 */

#include "unity.h"

/* Required by Unity: per-test setup / teardown. We re-init the fake
 * flash to all-erased state and reset the simulated tick before every
 * test, so test ordering can't introduce hidden coupling through
 * stale flash contents. */
#include "stm32h7xx_hal.h"

void setUp(void)
{
    mock_flash_reset();
}

void tearDown(void)
{
    /* nothing — mock_flash_reset() in setUp covers the next test */
}

/* ---- test_bl_isotp.c ---- */
void test_isotp_rx_single_frame_completes_in_one_feed(void);
void test_isotp_rx_first_frame_starts_assembly(void);
void test_isotp_rx_ff_plus_cf_chain_completes(void);
void test_isotp_rx_cf_with_wrong_sequence_errors(void);
void test_isotp_rx_cf_without_ff_errors(void);
void test_isotp_rx_overflow_too_long_message_rejects(void);
void test_isotp_rx_timeout_invalidates_partial_assembly(void);
void test_isotp_rx_sf_zero_length_is_rejected(void);
void test_isotp_tx_single_frame_below_8_bytes(void);
void test_isotp_tx_multi_frame_yields_ff_then_cfs(void);

/* ---- test_bl_fwinfo.c ---- */
void test_fwinfo_absent_returns_false_on_erased_flash(void);
void test_fwinfo_valid_magic_and_major1_is_present(void);
void test_fwinfo_wrong_magic_rejected(void);
void test_fwinfo_major_version_zero_rejected(void);
void test_fwinfo_future_minor_accepted(void);

/* ---- test_bl_nvm.c ---- */
void test_nvm_init_on_erased_sector_has_zero_live(void);
void test_nvm_write_then_read_round_trip(void);
void test_nvm_latest_seq_wins_on_repeated_key(void);
void test_nvm_tombstone_hides_previous_value(void);
void test_nvm_live_count_tracks_unique_keys(void);
void test_nvm_value_too_long_rejects(void);
void test_nvm_format_wipes_sector_and_resets_pointers(void);
void test_nvm_compact_replace_meta_preserves_live_entries(void);
void test_nvm_compact_replace_meta_writes_caller_metadata(void);
void test_nvm_compact_replace_meta_erases_old_metadata(void);
void test_nvm_write_recovers_via_compaction_on_first_program_fail(void);

int main(void)
{
    UNITY_BEGIN();

    /* test_bl_isotp.c */
    RUN_TEST(test_isotp_rx_single_frame_completes_in_one_feed);
    RUN_TEST(test_isotp_rx_first_frame_starts_assembly);
    RUN_TEST(test_isotp_rx_ff_plus_cf_chain_completes);
    RUN_TEST(test_isotp_rx_cf_with_wrong_sequence_errors);
    RUN_TEST(test_isotp_rx_cf_without_ff_errors);
    RUN_TEST(test_isotp_rx_overflow_too_long_message_rejects);
    RUN_TEST(test_isotp_rx_timeout_invalidates_partial_assembly);
    RUN_TEST(test_isotp_rx_sf_zero_length_is_rejected);
    RUN_TEST(test_isotp_tx_single_frame_below_8_bytes);
    RUN_TEST(test_isotp_tx_multi_frame_yields_ff_then_cfs);

    /* test_bl_fwinfo.c */
    RUN_TEST(test_fwinfo_absent_returns_false_on_erased_flash);
    RUN_TEST(test_fwinfo_valid_magic_and_major1_is_present);
    RUN_TEST(test_fwinfo_wrong_magic_rejected);
    RUN_TEST(test_fwinfo_major_version_zero_rejected);
    RUN_TEST(test_fwinfo_future_minor_accepted);

    /* test_bl_nvm.c */
    RUN_TEST(test_nvm_init_on_erased_sector_has_zero_live);
    RUN_TEST(test_nvm_write_then_read_round_trip);
    RUN_TEST(test_nvm_latest_seq_wins_on_repeated_key);
    RUN_TEST(test_nvm_tombstone_hides_previous_value);
    RUN_TEST(test_nvm_live_count_tracks_unique_keys);
    RUN_TEST(test_nvm_value_too_long_rejects);
    RUN_TEST(test_nvm_format_wipes_sector_and_resets_pointers);
    RUN_TEST(test_nvm_compact_replace_meta_preserves_live_entries);
    RUN_TEST(test_nvm_compact_replace_meta_writes_caller_metadata);
    RUN_TEST(test_nvm_compact_replace_meta_erases_old_metadata);
    RUN_TEST(test_nvm_write_recovers_via_compaction_on_first_program_fail);

    return UNITY_END();
}
