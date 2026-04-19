#ifndef BL_LIVE_H
#define BL_LIVE_H

/*
 * bl_live — periodic bootloader snapshot streamer.
 *
 * A 32-byte struct the host can subscribe to at 1..50 Hz. Unlike
 * CMD_GET_HEALTH (on-demand, 32-byte health record) this is a push
 * stream intended for graphing / live-table view: traffic counters,
 * current flash operation, ISO-TP reassembly progress, session age,
 * the last opcode received, etc.
 *
 * The snapshot lives in plain .bss (no reason to preserve across
 * reset — this is a running-state view).
 *
 * Host-side signal-definition files (JSON / TOML in the can-flasher
 * tool) map offsets in this struct to named fields + scaling; the
 * layout here is the single source of truth. Bumping the layout is
 * a breaking change and should come with a minor-version bump on
 * the protocol.
 *
 * App-side live-data publishing is deliberately out of scope for this
 * branch; a second stream of app-authored data is future work.
 */

#include <stdbool.h>
#include <stdint.h>

#define BL_LIVE_SNAPSHOT_SIZE       32U
#define BL_LIVE_MIN_RATE_HZ         1U
#define BL_LIVE_MAX_RATE_HZ         50U

/* Flags bitmask (byte 14 of the snapshot). */
#define BL_LIVE_FLAG_SESSION_ACTIVE       (1U << 0)
#define BL_LIVE_FLAG_VALID_APP_PRESENT    (1U << 1)
#define BL_LIVE_FLAG_LOG_STREAMING        (1U << 2)
#define BL_LIVE_FLAG_LIVEDATA_STREAMING   (1U << 3)

/* ---- The snapshot (32 bytes, natural alignment) ---- */
typedef struct {
    uint32_t uptime_ms;             /* offset 0  — HAL_GetTick(); wraps at ~49 days */
    uint16_t frames_rx;             /* offset 4  — received CAN frames (saturates)  */
    uint16_t frames_tx;             /* offset 6  — transmitted CAN frames           */
    uint16_t nacks_sent;            /* offset 8  — subset of frames_tx              */
    uint16_t dtc_count;             /* offset 10                                    */
    uint16_t last_dtc_code;         /* offset 12                                    */
    uint8_t  flags;                 /* offset 14 — BL_LIVE_FLAG_* bitmask           */
    uint8_t  last_opcode;           /* offset 15 — most recent CMD opcode received  */
    uint32_t last_flash_addr;       /* offset 16 — most recent flash op target       */
    uint32_t isotp_rx_progress;     /* offset 20 — bytes reassembled in current RX   *
                                     *             (0 when reassembler is idle)      */
    uint32_t session_age_ms;        /* offset 24 — ms since last RX activity or 0   */
    uint32_t reserved;              /* offset 28                                    */
} bl_live_t;

_Static_assert(sizeof(bl_live_t) == BL_LIVE_SNAPSHOT_SIZE,
               "bl_live_t must be exactly 32 bytes");


/* ---- Lifecycle ---- */

void bl_live_init(void);

/* Fill `out` with the current snapshot. Composed from bl_live's own
 * counters plus queries into bl_health / bl_dtc / bl_log / bl_proto. */
void bl_live_fill_snapshot(bl_live_t *out);


/* ---- Counter / state hooks, called from bl_proto.c ---- */

void bl_live_frame_rx(void);
void bl_live_frame_tx(void);
void bl_live_nack_sent(void);
void bl_live_note_opcode(uint8_t opcode);
void bl_live_note_flash_addr(uint32_t addr);


/* ---- Streaming control ---- */

/* Returns false when `rate_hz` is outside the valid range, in which
 * case no streaming is started and handle_live_data_start emits
 * NACK(UNSUPPORTED). */
bool bl_live_stream_start(uint8_t rate_hz);
void bl_live_stream_stop(void);
bool bl_live_stream_active(void);


/* ---- Main-loop tick ---- */

/* Emits NOTIFY_LIVE_DATA when streaming is active and the configured
 * period has elapsed since the last emission. */
void bl_live_tick(uint32_t now_ms);

#endif /* BL_LIVE_H */
