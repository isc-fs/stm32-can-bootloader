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
 * Each stub exposes a `mock_*_*` capture or knob so tests can either
 * drive deterministic values into the stub (uptime) or inspect what
 * the production code attempted to do (last NOTIFY payload).
 */

#include <stdint.h>
#include <string.h>

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


/* ---- bl_proto_send_notify capture ----
 *
 * bl_log_tick() routes drained log payloads through this. The mock
 * stores the last call's payload + length so tests asserting on
 * NOTIFY_LOG emission have somewhere to look. Capacity matches the
 * largest realistic single-call size (NOTIFY opcode byte +
 * BL_LOG_DRAIN_BUDGET = 1 + 256 = 257; round up to 512 for safety
 * across future budget bumps). */
#define MOCK_NOTIFY_CAPTURE_CAP 512U

static uint8_t  g_last_notify_buf[MOCK_NOTIFY_CAPTURE_CAP];
static uint16_t g_last_notify_len = 0U;
static int      g_notify_call_count = 0;

void bl_proto_send_notify(const uint8_t *payload, uint16_t length)
{
    g_notify_call_count++;
    uint16_t take = (length <= MOCK_NOTIFY_CAPTURE_CAP)
                      ? length
                      : (uint16_t)MOCK_NOTIFY_CAPTURE_CAP;
    memcpy(g_last_notify_buf, payload, take);
    g_last_notify_len = take;
}

void           mock_notify_reset(void)             { g_last_notify_len = 0U; g_notify_call_count = 0; }
int            mock_notify_call_count(void)        { return g_notify_call_count; }
const uint8_t *mock_last_notify_payload(void)      { return g_last_notify_buf; }
uint16_t       mock_last_notify_length(void)       { return g_last_notify_len; }
