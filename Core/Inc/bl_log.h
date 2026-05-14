#ifndef BL_LOG_H
#define BL_LOG_H

/*
 * bl_log — printf-style log ring + NOTIFY_LOG streamer.
 *
 * The ring lives in Backup SRAM at BL_LOG_RING_ADDR (right after the
 * DTC table). That means log entries survive soft resets and watchdog
 * fires, so a host that reconnects after a crash can stream out the
 * last ~1 KB of bootloader log leading up to the reboot. Lost on power
 * loss unless the board has a V_BAT coin cell (same caveat as DTC).
 *
 * BKPSRAM map (so far):
 *
 *   0x38800000 .. 0x38800290   bl_dtc_table_t    (656 B)
 *   0x38800290 .. 0x388003FF   reserved gap     (368 B)
 *   0x38800400 .. 0x3880081F   bl_log_ring_t   (≈1056 B)
 *   0x38800820 .. 0x38800FFF   unclaimed       (2016 B)
 *
 * Entry format on the wire (inside NOTIFY_LOG payloads) and in the
 * ring:
 *
 *   [0]     len_u8         — length of `text`, 0..BL_LOG_MAX_ENTRY_TEXT
 *   [1]     severity_u8    — BL_LOG_SEV_*
 *   [2..5]  uptime_le32    — seconds since boot when the entry was made
 *   [6..]   text[len]      — utf-8, not NUL-terminated on the wire
 *
 * 6-byte header + up to 120 bytes of text per entry. Entries are
 * packed back-to-back in the ring; wrap-around is handled transparently.
 *
 * Drain policy: on NOTIFY_LOG emission, up to BL_LOG_DRAIN_BUDGET bytes
 * are pulled from the ring and shipped as one ISO-TP message. At most
 * one emission per BL_LOG_MIN_EMIT_INTERVAL_MS so a busy ring doesn't
 * starve the rest of the bus.
 *
 * Overflow policy: when a new entry won't fit, the oldest entries are
 * evicted until there's room. The total number of evicted bytes is
 * tracked; on the next drain a synthetic "<LOG_OVERFLOW N bytes dropped>"
 * marker entry is prepended so the host always sees a breadcrumb for
 * lost data.
 *
 * Severity filter: `LOG_STREAM_START` carries a minimum severity. Entries
 * below that threshold are consumed from the ring but never emitted to
 * NOTIFY_LOG — once you start streaming, the ring is "played back" at
 * the host's preferred verbosity.
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- Tunables ---- */
#define BL_LOG_RING_BYTES           1024U
#define BL_LOG_MAX_ENTRY_TEXT       120U
#define BL_LOG_ENTRY_HEADER_SIZE    6U

#define BL_LOG_DRAIN_BUDGET         256U    /* bytes per NOTIFY_LOG emission */
#define BL_LOG_MIN_EMIT_INTERVAL_MS 50U     /* rate-limit NOTIFY_LOG traffic */

/* ---- Placement in BKPSRAM ----
 *
 * BL_LOG_RING_ADDR is overridable so host-side unit tests can point it
 * into a process-local buffer. On the chip the value is fixed to the
 * BKPSRAM slot that follows bl_dtc's table (see the map above). */
#ifndef BL_LOG_RING_ADDR
#define BL_LOG_RING_ADDR            0x38800400U
#endif
#define BL_LOG_MAGIC                0x10CCABCDU

/* ---- Severity levels (match bl_dtc for consistency) ---- */
#define BL_LOG_SEV_INFO             0U
#define BL_LOG_SEV_WARN             1U
#define BL_LOG_SEV_ERROR            2U
#define BL_LOG_SEV_FATAL            3U

/* ---- Ring layout (BKPSRAM-resident) ---- */
typedef struct {
    uint32_t magic;                         /* BL_LOG_MAGIC                */
    uint32_t write_pos;                     /* next byte to write in buffer */
    uint32_t read_pos;                      /* next unstreamed byte         */
    uint32_t unread_bytes;                  /* bytes available to drain     */
    uint32_t dropped_bytes;                 /* running counter of evicted   *
                                             * bytes since last marker emit */
    uint32_t header_reserved[3];            /* zero-pad to 32 B header      */
    uint8_t  buffer[BL_LOG_RING_BYTES];
} bl_log_ring_t;

_Static_assert(sizeof(bl_log_ring_t) == 32U + BL_LOG_RING_BYTES,
               "bl_log_ring_t size drifted");


/* ---- Lifecycle ---- */

/* Validate the ring magic, zero the ring if cold-booted or corrupted,
 * reset streaming state. Call from Bootloader_Init AFTER bl_dtc_init
 * (both rely on the same BKPSRAM clock + backup-domain setup that
 * bl_dtc_init already performs, so the order doesn't matter much —
 * but keeping them adjacent keeps intent clear). */
void bl_log_init(void);


/* ---- Logging API (printf-style) ---- */

void bl_log(uint8_t severity, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* Convenience wrappers. */
#define bl_log_info(...)   bl_log(BL_LOG_SEV_INFO,  __VA_ARGS__)
#define bl_log_warn(...)   bl_log(BL_LOG_SEV_WARN,  __VA_ARGS__)
#define bl_log_error(...)  bl_log(BL_LOG_SEV_ERROR, __VA_ARGS__)
#define bl_log_fatal(...)  bl_log(BL_LOG_SEV_FATAL, __VA_ARGS__)


/* ---- Streaming control (called from bl_proto handlers) ---- */

/* Start streaming NOTIFY_LOG frames. `min_severity` filters entries
 * below the given level (BL_LOG_SEV_INFO = stream everything). Current
 * ring contents are replayed first, then new entries as they land. */
void bl_log_stream_start(uint8_t min_severity);

/* Stop streaming. Entries already written to the ring stay put — a
 * later START replays whatever is still buffered. */
void bl_log_stream_stop(void);

bool bl_log_stream_active(void);


/* ---- Main-loop tick ---- */

/* Emits one NOTIFY_LOG frame per tick while streaming is active, the
 * ring has data, and BL_LOG_MIN_EMIT_INTERVAL_MS has elapsed since the
 * last emission. Drain is capped at BL_LOG_DRAIN_BUDGET bytes to keep
 * a single emission within one ISO-TP reassembly. */
void bl_log_tick(uint32_t now_ms);


/* ---- Low-level drain (used by bl_log_tick) ---- */

/* Pull up to `out_cap` bytes of serialised entries into `out`, filtered
 * to entries with severity >= `min_severity`. Prepends a synthetic
 * marker entry if there are pending dropped bytes. Returns the number
 * of bytes written. */
size_t bl_log_drain(uint8_t *out, size_t out_cap, uint8_t min_severity);

#endif /* BL_LOG_H */
