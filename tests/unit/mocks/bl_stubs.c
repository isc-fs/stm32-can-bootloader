/*
 * bl_stubs.c — host-side stubs for bl_* modules whose real
 * implementation isn't linked in yet.
 *
 * Some production sources we exercise (today: bl_log) call into peer
 * modules (bl_health, bl_proto) that aren't part of the host test
 * build yet. The full mock surface for those modules will land
 * incrementally — until then, no-op stubs here keep the linker happy
 * and make it obvious to anyone reading the test build what's faked.
 *
 * Linker-only stubs for the bigger peer modules (bl_dtc, bl_flash,
 * bl_health-fill, bl_live, bl_obyte, and the Bootloader_* entry
 * points) live in bl_peer_stubs.c next door — kept separate so the
 * settable / inspectable knobs in this file don't get lost in a
 * sea of empty-body stubs.
 */

#include <stdint.h>

/* ---- BKPSRAM-resident state used by bl_log ----
 *
 * Lives here so it has process-lifetime storage referenced by the
 * BL_LOG_RING_ADDR override in mocks/bl_memmap.h. We don't include
 * bl_log.h to avoid a dependency cycle on the mock include chain;
 * the size literal matches sizeof(bl_log_ring_t) on both targets
 * (the chip-side _Static_assert in bl_log.h still applies). */
uint8_t g_fake_log_ring[1056] = {0};


/* ---- bl_health uptime ----
 *
 * Production: `uint32_t bl_health_uptime_seconds(void)` returns
 * `HAL_GetTick() / 1000U`. We provide a settable mock so tests that
 * exercise bl_log()'s write path (which stamps the entry header with
 * the current uptime) can assert on a known value. */
static uint32_t g_mock_uptime_seconds = 0U;

void mock_set_uptime_seconds(uint32_t s) { g_mock_uptime_seconds = s; }

uint32_t bl_health_uptime_seconds(void) { return g_mock_uptime_seconds; }
