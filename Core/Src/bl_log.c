/*
 * bl_log.c — log ring + NOTIFY_LOG streamer.
 *
 * The ring lives in Backup SRAM (same region family as bl_dtc). Access
 * sequence requires `HAL_PWR_EnableBkUpAccess` and the BKPSRAM clock —
 * both are already turned on by bl_dtc_init; redoing them here is a
 * no-op, and keeps bl_log stand-alone safe if the module boot order
 * ever changes.
 *
 * Entries are variable-length, stored back-to-back. Wrap-around is
 * handled via a split-memcpy pair in ring_write / ring_read — so a
 * large entry can straddle the end-of-buffer boundary without needing
 * to pad out unused bytes.
 */

#include "bl_log.h"

#include "bl_health.h"
#include "bl_proto.h"
#include "stm32h7xx_hal.h"

#include <stdio.h>
#include <string.h>

/* ---- BKPSRAM-resident state ---- */

static bl_log_ring_t *const g_ring = (bl_log_ring_t *)BL_LOG_RING_ADDR;

/* ---- Streaming state (not persistent — reset on every boot) ---- */

static bool     g_streaming    = false;
static uint8_t  g_min_severity = BL_LOG_SEV_INFO;
static uint32_t g_last_emit_ms = 0U;


/* ---- Ring primitives (handle wrap-around) ---- */

static void ring_write(const uint8_t *src, size_t len)
{
    size_t end  = g_ring->write_pos;
    size_t tail = BL_LOG_RING_BYTES - end;
    if (len <= tail) {
        memcpy(&g_ring->buffer[end], src, len);
    } else {
        memcpy(&g_ring->buffer[end], src, tail);
        memcpy(&g_ring->buffer[0], src + tail, len - tail);
    }
    g_ring->write_pos = (uint32_t)((end + len) % BL_LOG_RING_BYTES);
    g_ring->unread_bytes += (uint32_t)len;
}

static void ring_read(uint8_t *dst, size_t len)
{
    size_t start = g_ring->read_pos;
    size_t tail  = BL_LOG_RING_BYTES - start;
    if (len <= tail) {
        memcpy(dst, &g_ring->buffer[start], len);
    } else {
        memcpy(dst,                &g_ring->buffer[start], tail);
        memcpy(dst + tail,         &g_ring->buffer[0],     len - tail);
    }
    g_ring->read_pos = (uint32_t)((start + len) % BL_LOG_RING_BYTES);
    g_ring->unread_bytes -= (uint32_t)len;
}

/* Peek `offset` bytes past the current read position without consuming. */
static uint8_t ring_peek(size_t offset)
{
    size_t pos = (g_ring->read_pos + offset) % BL_LOG_RING_BYTES;
    return g_ring->buffer[pos];
}

/* Advance read_pos by `len` bytes, discarding them (used by the severity
 * filter when an entry is suppressed). */
static void ring_skip(size_t len)
{
    g_ring->read_pos = (uint32_t)((g_ring->read_pos + len) % BL_LOG_RING_BYTES);
    g_ring->unread_bytes -= (uint32_t)len;
}


/* ---- Corruption handling ----
 *
 * BKPSRAM is reachable by application firmware running before us and
 * survives soft resets, so anything we read out of it must be treated
 * as untrusted in the same way we treat host CAN traffic. The two
 * places that lift a length byte off the ring — bl_log_drain's main
 * loop and evict_oldest — have to bound-check before doing any
 * memcpy / ring_read / pointer-advance, otherwise a corrupted
 * `ent_len = 0xFF` overflows the `scratch[]` in bl_log_drain (sized
 * for HEADER + BL_LOG_MAX_ENTRY_TEXT = 6 + 120, an attacker-readable
 * 0xFF byte would ask for HEADER + 255). Issue #65.
 *
 * `entry_consistent` returns true iff the entry currently at the
 * read pointer has a sensible header and fits inside what the ring
 * claims is buffered. `reset_ring_corrupt` puts the ring back into a
 * known-clean state — losing the existing contents on purpose: if we
 * can't trust the length byte we can't trust the rest of the buffer
 * either. */
static bool entry_consistent(uint8_t ent_len)
{
    if (ent_len > BL_LOG_MAX_ENTRY_TEXT) {
        return false;
    }
    size_t ent_total = (size_t)BL_LOG_ENTRY_HEADER_SIZE + ent_len;
    if (ent_total > (size_t)g_ring->unread_bytes) {
        return false;
    }
    return true;
}

static void reset_ring_corrupt(void)
{
    memset(g_ring, 0, sizeof(*g_ring));
    g_ring->magic = BL_LOG_MAGIC;
}


/* ---- Eviction ---- */

/* Evict the oldest entry. Returns its total size (header + text) on
 * success, or the full ring size when the entry was corrupt and the
 * ring was hard-reset — that gives bl_log()'s eviction loop a clean
 * exit (any new entry now fits because unread_bytes is 0). */
static size_t evict_oldest(void)
{
    uint8_t len = ring_peek(0);
    if (!entry_consistent(len)) {
        reset_ring_corrupt();
        return (size_t)BL_LOG_RING_BYTES;
    }
    size_t total = (size_t)BL_LOG_ENTRY_HEADER_SIZE + len;
    g_ring->read_pos = (uint32_t)((g_ring->read_pos + total) % BL_LOG_RING_BYTES);
    g_ring->unread_bytes -= (uint32_t)total;
    g_ring->dropped_bytes += (uint32_t)total;
    return total;
}


/* ---- Lifecycle ---- */

void bl_log_init(void)
{
    HAL_PWR_EnableBkUpAccess();
    __HAL_RCC_BKPRAM_CLK_ENABLE();

    bool valid = (g_ring->magic == BL_LOG_MAGIC)
              && (g_ring->write_pos    < BL_LOG_RING_BYTES)
              && (g_ring->read_pos     < BL_LOG_RING_BYTES)
              && (g_ring->unread_bytes <= BL_LOG_RING_BYTES);

    if (!valid) {
        memset(g_ring, 0, sizeof(*g_ring));
        g_ring->magic = BL_LOG_MAGIC;
    }

    /* Streaming is always off at boot; host reopens it with
     * LOG_STREAM_START once it's ready to consume. */
    g_streaming    = false;
    g_min_severity = BL_LOG_SEV_INFO;
    g_last_emit_ms = 0U;
}


/* ---- Logging ---- */

void bl_log(uint8_t severity, const char *fmt, ...)
{
    /* Format into a stack buffer. 120 bytes + NUL is plenty for
     * bootloader-scale messages; anything longer gets truncated. */
    char tmp[BL_LOG_MAX_ENTRY_TEXT + 1U];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }

    size_t text_len = (n < (int)BL_LOG_MAX_ENTRY_TEXT)
                          ? (size_t)n
                          : BL_LOG_MAX_ENTRY_TEXT;
    size_t entry_size = (size_t)BL_LOG_ENTRY_HEADER_SIZE + text_len;

    /* Drop-oldest eviction until the new entry fits. */
    while (g_ring->unread_bytes + entry_size > BL_LOG_RING_BYTES) {
        (void)evict_oldest();
    }

    uint32_t uptime = bl_health_uptime_seconds();
    uint8_t header[BL_LOG_ENTRY_HEADER_SIZE];
    header[0] = (uint8_t)text_len;
    header[1] = severity;
    header[2] = (uint8_t)(uptime        & 0xFFU);
    header[3] = (uint8_t)((uptime >> 8) & 0xFFU);
    header[4] = (uint8_t)((uptime >> 16) & 0xFFU);
    header[5] = (uint8_t)((uptime >> 24) & 0xFFU);

    ring_write(header, sizeof(header));
    ring_write((const uint8_t *)tmp, text_len);
}


/* ---- Streaming control ---- */

void bl_log_stream_start(uint8_t min_severity)
{
    g_streaming    = true;
    g_min_severity = min_severity;
    /* Seed last_emit so the first tick after START can immediately
     * emit if the ring has pre-existing content. */
    g_last_emit_ms = HAL_GetTick() - BL_LOG_MIN_EMIT_INTERVAL_MS;
}

void bl_log_stream_stop(void)
{
    g_streaming = false;
}

bool bl_log_stream_active(void)
{
    return g_streaming;
}


/* ---- Drain ---- */

size_t bl_log_drain(uint8_t *out, size_t out_cap, uint8_t min_severity)
{
    size_t written = 0U;

    /* If anything has been evicted since the last drain, prepend a
     * synthetic marker so the host always sees a breadcrumb. The
     * marker is a log entry with our own "dropped N bytes" text —
     * it never lives in the ring itself. */
    if (g_ring->dropped_bytes > 0U) {
        char marker_text[48];
        int n = snprintf(marker_text, sizeof(marker_text),
                         "<LOG_OVERFLOW %u bytes dropped>",
                         (unsigned int)g_ring->dropped_bytes);
        if (n > 0) {
            size_t text_len = (n < (int)sizeof(marker_text))
                                  ? (size_t)n
                                  : sizeof(marker_text) - 1U;
            size_t marker_total = (size_t)BL_LOG_ENTRY_HEADER_SIZE + text_len;
            if (marker_total <= out_cap) {
                uint32_t now = bl_health_uptime_seconds();
                out[0] = (uint8_t)text_len;
                out[1] = BL_LOG_SEV_WARN;
                out[2] = (uint8_t)(now        & 0xFFU);
                out[3] = (uint8_t)((now >> 8) & 0xFFU);
                out[4] = (uint8_t)((now >> 16) & 0xFFU);
                out[5] = (uint8_t)((now >> 24) & 0xFFU);
                memcpy(&out[6], marker_text, text_len);
                written = marker_total;
                g_ring->dropped_bytes = 0U;
            }
        }
    }

    /* Drain entries, honouring the severity filter. Entries below the
     * threshold are consumed from the ring but not emitted — once the
     * host picks a verbosity, anything below it is gone for good. */
    while (g_ring->unread_bytes > 0U) {
        uint8_t ent_len = ring_peek(0);
        uint8_t ent_sev = ring_peek(1);

        /* Bound-check before any memcpy / ring_read / pointer advance.
         * See the comment on entry_consistent() for the why. */
        if (!entry_consistent(ent_len)) {
            reset_ring_corrupt();
            break;
        }
        size_t ent_total = (size_t)BL_LOG_ENTRY_HEADER_SIZE + ent_len;

        if (ent_sev >= min_severity) {
            if (written + ent_total > out_cap) {
                break;  /* can't fit this entry in the remaining budget */
            }
            uint8_t scratch[BL_LOG_ENTRY_HEADER_SIZE + BL_LOG_MAX_ENTRY_TEXT];
            ring_read(scratch, ent_total);
            memcpy(&out[written], scratch, ent_total);
            written += ent_total;
        } else {
            ring_skip(ent_total);
        }
    }

    return written;
}


/* ---- Tick ---- */

void bl_log_tick(uint32_t now_ms)
{
    if (!g_streaming) {
        return;
    }
    if (g_ring->unread_bytes == 0U && g_ring->dropped_bytes == 0U) {
        return;
    }
    if ((uint32_t)(now_ms - g_last_emit_ms) < BL_LOG_MIN_EMIT_INTERVAL_MS) {
        return;
    }

    /* opcode + up to BL_LOG_DRAIN_BUDGET bytes of serialised entries. */
    uint8_t buf[1U + BL_LOG_DRAIN_BUDGET];
    buf[0] = BL_NOTIFY_LOG;
    size_t drained = bl_log_drain(&buf[1], BL_LOG_DRAIN_BUDGET, g_min_severity);
    if (drained == 0U) {
        /* Nothing at or above the current threshold; suppress the
         * emission but still stretch last_emit_ms so we don't spin on
         * the filter every tick. */
        g_last_emit_ms = now_ms;
        return;
    }

    bl_proto_send_notify(buf, (uint16_t)(1U + drained));
    g_last_emit_ms = now_ms;
}
