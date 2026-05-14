#ifndef BL_STUBS_H
#define BL_STUBS_H

/*
 * Test-only mock surface paired with mocks/bl_stubs.c. Tests that need
 * to drive deterministic uptime values or inspect captured NOTIFY
 * payloads include this header explicitly; it isn't force-included so
 * production sources never see it.
 */

#include <stdint.h>

/* ---- bl_health uptime control ----
 * Sets the value returned by bl_health_uptime_seconds() until reset. */
void mock_set_uptime_seconds(uint32_t s);

/* ---- bl_proto_send_notify capture ----
 *
 * Every call to bl_proto_send_notify() copies its payload + length
 * into the capture buffers below. Tests use these to assert what the
 * production code emitted. `mock_notify_reset()` clears the capture
 * so each test starts from a known state.
 */
void           mock_notify_reset(void);
int            mock_notify_call_count(void);
const uint8_t *mock_last_notify_payload(void);
uint16_t       mock_last_notify_length(void);

#endif /* BL_STUBS_H */
