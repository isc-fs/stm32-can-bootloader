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
    mock_bootloader_reset();   /* clear jump counter + reset CheckApplication rv */
    mock_ob_apply_wrp_reset(); /* clear OB_APPLY_WRP mask/call record (#125 C4) */
    mock_flash_op_ms_reset();  /* clear flash-op-duration probe record (#125 H6) */
    mock_iwdg_refresh_reset(); /* clear IWDG-kick record (#125 H6) */
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
void test_isotp_rx_timeout_handles_tick_wraparound(void);
void test_isotp_rx_cf_with_no_payload_is_rejected(void);
void test_isotp_rx_sf_zero_length_is_rejected(void);
void test_isotp_tx_single_frame_below_8_bytes(void);
void test_isotp_tx_multi_frame_yields_ff_then_cfs(void);

/* ---- test_bl_proto_id.c ---- */
void test_proto_build_host_to_node_unicast(void);
void test_proto_build_host_to_node_broadcast(void);
void test_proto_build_node_to_host_sets_dir_bit(void);
void test_proto_build_masks_node_id_to_low_nibble(void);
void test_proto_parse_valid_host_to_node_unicast(void);
void test_proto_parse_valid_host_to_node_broadcast(void);
void test_proto_parse_valid_node_to_host(void);
void test_proto_parse_rejects_reserved_bit_5_set(void);
void test_proto_parse_rejects_reserved_bit_high_set(void);
void test_proto_parse_rejects_node_to_host_src_zero(void);
void test_proto_parse_rejects_node_to_host_src_broadcast(void);
void test_proto_parse_accepts_node_to_host_src_max_unicast(void);
void test_proto_parse_accepts_host_to_node_zero(void);
void test_proto_build_parse_roundtrip_all_valid_ids(void);

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
void test_nvm_erase_kicks_the_watchdog(void);
void test_nvm_compact_replace_meta_preserves_live_entries(void);
void test_nvm_compact_replace_meta_writes_caller_metadata(void);
void test_nvm_compact_replace_meta_erases_old_metadata(void);
void test_nvm_write_recovers_via_compaction_on_first_program_fail(void);

/* ---- test_bl_log.c ---- */
void test_log_drain_clamps_oversized_ent_len(void);
void test_log_drain_clamps_when_unread_smaller_than_declared_entry(void);
void test_log_drain_well_formed_entry_passes_through(void);
void test_log_drain_severity_filter_below_threshold_is_skipped(void);
void test_log_init_zeros_ring_when_magic_is_wrong(void);
void test_tx_idle_reflects_fifo_free(void);
void test_log_tick_skips_and_keeps_data_when_tx_busy(void);
void test_log_emission_fits_tx_fifo_when_idle(void);

/* ---- test_bl_proto_dispatch.c ---- */
void test_dispatch_node_to_host_direction_is_silently_dropped(void);
void test_dispatch_wrong_destination_is_silently_dropped(void);
void test_dispatch_zero_length_frame_is_silently_dropped(void);
void test_dispatch_bad_pci_emits_nack_transport_error(void);
void test_dispatch_write_chunk_19cf_sequence_does_not_emit_transport_error(void);
void test_dispatch_write_chunk_262byte_37cf_padded_last_cf(void);
void test_dispatch_valid_sf_pci_passes_pci_gate(void);
void test_handle_connect_valid_version_acks_and_latches_session(void);
void test_handle_connect_wrong_major_nacks_protocol_version(void);
void test_handle_connect_short_args_nacks_unsupported(void);
void test_handle_disconnect_clears_session_latch(void);
void test_handle_discover_uses_discover_reply_type_not_ack(void);
void test_session_gate_flash_erase_without_connect_nacks_bad_session(void);
void test_session_gate_flash_write_without_connect_nacks_bad_session(void);
void test_session_gate_nvm_read_without_connect_nacks_bad_session(void);
void test_session_gate_log_stream_start_without_connect_nacks_bad_session(void);
void test_handle_nvm_read_returns_value_for_pre_written_key(void);
void test_handle_nvm_read_unknown_key_nacks_not_found(void);
void test_handle_nvm_write_persists_value_visible_to_nvm_read_api(void);
void test_dispatch_unknown_opcode_nacks_unsupported(void);
void test_send_notify_suppressed_while_reassembly_in_flight(void);
void test_send_notify_emitted_when_idle(void);
void test_session_timeout_after_flash_does_not_jump(void);
void test_session_timeout_clean_diagnostic_session_still_jumps(void);
void test_jump_after_write_routes_through_reset_to_app(void);
void test_flash_ops_invalidate_app_check_cache(void);
void test_stay_in_bl_persists_in_nvm_and_clears_on_boot(void);
void test_ob_apply_wrp_rejects_non_bootloader_sector(void);
void test_ob_apply_wrp_accepts_bootloader_sector(void);
void test_ob_apply_wrp_forces_sector0_when_mask_zero(void);
void test_flash_erase_records_op_duration(void);

/* ---- test_bl_node_id.c ---- */
void test_node_id_falls_back_to_compile_time_on_empty_nvm(void);
void test_node_id_falls_back_on_zero_host_value(void);
void test_node_id_falls_back_on_broadcast_value(void);
void test_node_id_falls_back_on_out_of_range_value(void);
void test_node_id_falls_back_on_multi_byte_value(void);
void test_node_id_uses_valid_nvm_override(void);
void test_node_id_accepts_min_unicast(void);
void test_node_id_accepts_max_unicast(void);
void test_node_id_init_is_idempotent(void);
void test_node_id_reinit_picks_up_nvm_changes(void);
void test_node_id_reinit_after_tombstone_restores_default(void);

/* ---- test_bl_fdcan.c ---- */
void test_fdcan_bus_count_is_three(void);
void test_fdcan_bus_maps_index_to_handle(void);
void test_fdcan_bus_out_of_range_clamps_to_bus0(void);
void test_fdcan_set_active_routes_get_handle(void);
void test_fdcan_set_active_null_is_ignored(void);
void test_fdcan_configure_filters_returns_ok(void);
void test_fdcan_configure_filters_covers_all_buses(void);
void test_fdcan_configure_filters_propagates_failure(void);
void test_fdcan_filter_is_exact_match_not_aliasing(void);
void test_fdcan_start_all_returns_ok(void);
void test_fdcan_start_all_starts_every_bus(void);
void test_fdcan_start_all_propagates_failure(void);

/* ---- test_bl_app_validate.c ---- */
void test_app_stack_dtcm_base_accepted(void);
void test_app_stack_dtcm_interior_accepted(void);
void test_app_stack_dtcm_initial_msp_accepted(void);
void test_app_stack_just_past_dtcm_rejected(void);
void test_app_stack_d1_base_accepted(void);
void test_app_stack_d1_interior_accepted(void);
void test_app_stack_d1_initial_msp_accepted(void);
void test_app_stack_just_past_d1_rejected_regression(void);
void test_app_stack_far_past_d1_rejected(void);
void test_app_stack_gap_between_dtcm_and_d1_rejected(void);
void test_app_stack_flash_address_rejected(void);
void test_app_stack_zero_rejected(void);
void test_app_stack_top_of_address_space_rejected(void);

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
    RUN_TEST(test_isotp_rx_timeout_handles_tick_wraparound);
    RUN_TEST(test_isotp_rx_cf_with_no_payload_is_rejected);
    RUN_TEST(test_isotp_rx_sf_zero_length_is_rejected);
    RUN_TEST(test_isotp_tx_single_frame_below_8_bytes);
    RUN_TEST(test_isotp_tx_multi_frame_yields_ff_then_cfs);

    /* test_bl_proto_id.c */
    RUN_TEST(test_proto_build_host_to_node_unicast);
    RUN_TEST(test_proto_build_host_to_node_broadcast);
    RUN_TEST(test_proto_build_node_to_host_sets_dir_bit);
    RUN_TEST(test_proto_build_masks_node_id_to_low_nibble);
    RUN_TEST(test_proto_parse_valid_host_to_node_unicast);
    RUN_TEST(test_proto_parse_valid_host_to_node_broadcast);
    RUN_TEST(test_proto_parse_valid_node_to_host);
    RUN_TEST(test_proto_parse_rejects_reserved_bit_5_set);
    RUN_TEST(test_proto_parse_rejects_reserved_bit_high_set);
    RUN_TEST(test_proto_parse_rejects_node_to_host_src_zero);
    RUN_TEST(test_proto_parse_rejects_node_to_host_src_broadcast);
    RUN_TEST(test_proto_parse_accepts_node_to_host_src_max_unicast);
    RUN_TEST(test_proto_parse_accepts_host_to_node_zero);
    RUN_TEST(test_proto_build_parse_roundtrip_all_valid_ids);

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
    RUN_TEST(test_nvm_erase_kicks_the_watchdog);
    RUN_TEST(test_nvm_compact_replace_meta_preserves_live_entries);
    RUN_TEST(test_nvm_compact_replace_meta_writes_caller_metadata);
    RUN_TEST(test_nvm_compact_replace_meta_erases_old_metadata);
    RUN_TEST(test_nvm_write_recovers_via_compaction_on_first_program_fail);

    /* test_bl_log.c */
    RUN_TEST(test_log_drain_clamps_oversized_ent_len);
    RUN_TEST(test_log_drain_clamps_when_unread_smaller_than_declared_entry);
    RUN_TEST(test_log_drain_well_formed_entry_passes_through);
    RUN_TEST(test_log_drain_severity_filter_below_threshold_is_skipped);
    RUN_TEST(test_log_init_zeros_ring_when_magic_is_wrong);
    RUN_TEST(test_tx_idle_reflects_fifo_free);
    RUN_TEST(test_log_tick_skips_and_keeps_data_when_tx_busy);
    RUN_TEST(test_log_emission_fits_tx_fifo_when_idle);

    /* test_bl_proto_dispatch.c */
    RUN_TEST(test_dispatch_node_to_host_direction_is_silently_dropped);
    RUN_TEST(test_dispatch_wrong_destination_is_silently_dropped);
    RUN_TEST(test_dispatch_zero_length_frame_is_silently_dropped);
    RUN_TEST(test_dispatch_bad_pci_emits_nack_transport_error);
    RUN_TEST(test_dispatch_write_chunk_19cf_sequence_does_not_emit_transport_error);
    RUN_TEST(test_dispatch_write_chunk_262byte_37cf_padded_last_cf);
    RUN_TEST(test_dispatch_valid_sf_pci_passes_pci_gate);
    RUN_TEST(test_handle_connect_valid_version_acks_and_latches_session);
    RUN_TEST(test_handle_connect_wrong_major_nacks_protocol_version);
    RUN_TEST(test_handle_connect_short_args_nacks_unsupported);
    RUN_TEST(test_handle_disconnect_clears_session_latch);
    RUN_TEST(test_handle_discover_uses_discover_reply_type_not_ack);
    RUN_TEST(test_session_gate_flash_erase_without_connect_nacks_bad_session);
    RUN_TEST(test_session_gate_flash_write_without_connect_nacks_bad_session);
    RUN_TEST(test_session_gate_nvm_read_without_connect_nacks_bad_session);
    RUN_TEST(test_session_gate_log_stream_start_without_connect_nacks_bad_session);
    RUN_TEST(test_handle_nvm_read_returns_value_for_pre_written_key);
    RUN_TEST(test_handle_nvm_read_unknown_key_nacks_not_found);
    RUN_TEST(test_handle_nvm_write_persists_value_visible_to_nvm_read_api);
    RUN_TEST(test_dispatch_unknown_opcode_nacks_unsupported);
    RUN_TEST(test_send_notify_suppressed_while_reassembly_in_flight);
    RUN_TEST(test_send_notify_emitted_when_idle);
    RUN_TEST(test_session_timeout_after_flash_does_not_jump);
    RUN_TEST(test_session_timeout_clean_diagnostic_session_still_jumps);
    RUN_TEST(test_jump_after_write_routes_through_reset_to_app);
    RUN_TEST(test_flash_ops_invalidate_app_check_cache);
    RUN_TEST(test_stay_in_bl_persists_in_nvm_and_clears_on_boot);
    RUN_TEST(test_ob_apply_wrp_rejects_non_bootloader_sector);
    RUN_TEST(test_ob_apply_wrp_accepts_bootloader_sector);
    RUN_TEST(test_ob_apply_wrp_forces_sector0_when_mask_zero);
    RUN_TEST(test_flash_erase_records_op_duration);

    /* test_bl_node_id.c */
    RUN_TEST(test_node_id_falls_back_to_compile_time_on_empty_nvm);
    RUN_TEST(test_node_id_falls_back_on_zero_host_value);
    RUN_TEST(test_node_id_falls_back_on_broadcast_value);
    RUN_TEST(test_node_id_falls_back_on_out_of_range_value);
    RUN_TEST(test_node_id_falls_back_on_multi_byte_value);
    RUN_TEST(test_node_id_uses_valid_nvm_override);
    RUN_TEST(test_node_id_accepts_min_unicast);
    RUN_TEST(test_node_id_accepts_max_unicast);
    RUN_TEST(test_node_id_init_is_idempotent);
    RUN_TEST(test_node_id_reinit_picks_up_nvm_changes);
    RUN_TEST(test_node_id_reinit_after_tombstone_restores_default);

    /* test_bl_fdcan.c */
    RUN_TEST(test_fdcan_bus_count_is_three);
    RUN_TEST(test_fdcan_bus_maps_index_to_handle);
    RUN_TEST(test_fdcan_bus_out_of_range_clamps_to_bus0);
    RUN_TEST(test_fdcan_set_active_routes_get_handle);
    RUN_TEST(test_fdcan_set_active_null_is_ignored);
    RUN_TEST(test_fdcan_configure_filters_returns_ok);
    RUN_TEST(test_fdcan_configure_filters_covers_all_buses);
    RUN_TEST(test_fdcan_configure_filters_propagates_failure);
    RUN_TEST(test_fdcan_filter_is_exact_match_not_aliasing);
    RUN_TEST(test_fdcan_start_all_returns_ok);
    RUN_TEST(test_fdcan_start_all_starts_every_bus);
    RUN_TEST(test_fdcan_start_all_propagates_failure);

    /* test_bl_app_validate.c */
    RUN_TEST(test_app_stack_dtcm_base_accepted);
    RUN_TEST(test_app_stack_dtcm_interior_accepted);
    RUN_TEST(test_app_stack_dtcm_initial_msp_accepted);
    RUN_TEST(test_app_stack_just_past_dtcm_rejected);
    RUN_TEST(test_app_stack_d1_base_accepted);
    RUN_TEST(test_app_stack_d1_interior_accepted);
    RUN_TEST(test_app_stack_d1_initial_msp_accepted);
    RUN_TEST(test_app_stack_just_past_d1_rejected_regression);
    RUN_TEST(test_app_stack_far_past_d1_rejected);
    RUN_TEST(test_app_stack_gap_between_dtcm_and_d1_rejected);
    RUN_TEST(test_app_stack_flash_address_rejected);
    RUN_TEST(test_app_stack_zero_rejected);
    RUN_TEST(test_app_stack_top_of_address_space_rejected);

    return UNITY_END();
}
