#ifndef BL_STUBS_H
#define BL_STUBS_H

/*
 * Test-only mock surface paired with mocks/bl_stubs.c. Tests that need
 * to drive deterministic uptime values include this header explicitly;
 * it isn't force-included so production sources never see it.
 *
 * FDCAN TX capture (formerly here as bl_proto_send_notify) now lives
 * at the HAL layer — see `mock_fdcan_*` in mocks/stm32h7xx_hal.h. The
 * real bl_proto.c is linked into the test build, so its send_notify
 * routes through HAL_FDCAN_AddMessageToTxFifoQ → the FDCAN capture
 * ring, and tests inspect frames there.
 */

#include <stdint.h>

/* ---- bl_health uptime control ----
 * Sets the value returned by bl_health_uptime_seconds() until reset. */
void mock_set_uptime_seconds(uint32_t s);

#endif /* BL_STUBS_H */
