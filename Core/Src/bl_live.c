/*
 * bl_live.c — periodic bootloader snapshot streamer.
 *
 * Holds a small set of counters + cached state, composes a 32-byte
 * snapshot on each emission by querying the rest of the bootloader
 * modules, and ships it as NOTIFY_LIVE_DATA at the host-requested
 * rate.
 *
 * No locking: counters are incremented from the same main-loop
 * thread that emits. The worst the host can see is a snapshot with
 * slightly-stale cross-field values, which self-corrects on the
 * next sample.
 */

#include "bl_live.h"

#include "bl_dtc.h"
#include "bl_health.h"
#include "bl_log.h"
#include "bl_obyte.h"
#include "bl_proto.h"
#include "main.h"
#include "stm32h7xx_hal.h"

#include <string.h>

/* ---- Internal state ---- */

/* Traffic counters. Saturating at uint16_t max so a fast bus doesn't
 * wrap them in one sampling interval. */
static uint16_t g_frames_rx     = 0U;
static uint16_t g_frames_tx     = 0U;
static uint16_t g_nacks_sent    = 0U;

/* Last-op tracking. */
static uint8_t  g_last_opcode     = 0U;
static uint32_t g_last_flash_addr = 0U;

/* Streaming state. */
static bool     g_streaming      = false;
static uint32_t g_period_ms      = 0U;
static uint32_t g_last_emit_ms   = 0U;


/* ---- Saturating increment helper ---- */

static inline void sat_inc_u16(uint16_t *counter)
{
    if (*counter < 0xFFFFU) {
        (*counter)++;
    }
}


/* ---- Lifecycle ---- */

void bl_live_init(void)
{
    g_frames_rx       = 0U;
    g_frames_tx       = 0U;
    g_nacks_sent      = 0U;
    g_last_opcode     = 0U;
    g_last_flash_addr = 0U;
    g_streaming       = false;
    g_period_ms       = 0U;
    g_last_emit_ms    = 0U;
}


/* ---- Hooks called from bl_proto ---- */

void bl_live_frame_rx(void)       { sat_inc_u16(&g_frames_rx); }
void bl_live_frame_tx(void)       { sat_inc_u16(&g_frames_tx); }
void bl_live_nack_sent(void)      { sat_inc_u16(&g_nacks_sent); }
void bl_live_note_opcode(uint8_t opcode) { g_last_opcode = opcode; }
void bl_live_note_flash_addr(uint32_t addr) { g_last_flash_addr = addr; }


/* ---- Streaming control ---- */

bool bl_live_stream_start(uint8_t rate_hz)
{
    if (rate_hz < BL_LIVE_MIN_RATE_HZ || rate_hz > BL_LIVE_MAX_RATE_HZ) {
        return false;
    }
    g_streaming    = true;
    g_period_ms    = 1000U / rate_hz;
    /* Kick the emitter so the first frame lands on the next tick
     * instead of waiting one period. */
    g_last_emit_ms = HAL_GetTick() - g_period_ms;
    return true;
}

void bl_live_stream_stop(void)
{
    g_streaming = false;
}

bool bl_live_stream_active(void)
{
    return g_streaming;
}


/* ---- Snapshot ---- */

void bl_live_fill_snapshot(bl_live_t *out)
{
    out->uptime_ms     = HAL_GetTick();
    out->frames_rx     = g_frames_rx;
    out->frames_tx     = g_frames_tx;
    out->nacks_sent    = g_nacks_sent;
    out->dtc_count     = bl_dtc_count();
    out->last_dtc_code = bl_dtc_last_code();

    uint8_t flags = 0U;
    if (bl_proto_session_active()) {
        flags |= BL_LIVE_FLAG_SESSION_ACTIVE;
    }
    if (Bootloader_CheckApplication() == 0U) {
        flags |= BL_LIVE_FLAG_VALID_APP_PRESENT;
    }
    if (bl_log_stream_active()) {
        flags |= BL_LIVE_FLAG_LOG_STREAMING;
    }
    if (g_streaming) {
        flags |= BL_LIVE_FLAG_LIVEDATA_STREAMING;
    }
    if (bl_obyte_is_sector_wrp_protected(0U)) {
        flags |= BL_LIVE_FLAG_WRP_PROTECTED;
    }
    out->flags             = flags;
    out->last_opcode       = g_last_opcode;
    out->last_flash_addr   = g_last_flash_addr;
    out->isotp_rx_progress = (uint32_t)bl_proto_isotp_rx_progress();
    out->session_age_ms    = bl_proto_session_age_ms(out->uptime_ms);
    out->reserved          = 0U;
}


/* ---- Tick ---- */

void bl_live_tick(uint32_t now_ms)
{
    if (!g_streaming) {
        return;
    }
    if ((uint32_t)(now_ms - g_last_emit_ms) < g_period_ms) {
        return;
    }
    g_last_emit_ms = now_ms;

    bl_live_t snapshot;
    bl_live_fill_snapshot(&snapshot);

    uint8_t buf[1U + BL_LIVE_SNAPSHOT_SIZE];
    buf[0] = BL_NOTIFY_LIVE_DATA;
    memcpy(&buf[1], &snapshot, BL_LIVE_SNAPSHOT_SIZE);

    bl_proto_send_notify(buf, (uint16_t)sizeof(buf));
}
