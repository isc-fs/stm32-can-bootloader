#ifndef BL_DTC_H
#define BL_DTC_H

/*
 * bl_dtc — diagnostic trouble code (DTC) table.
 *
 * The table lives in Backup SRAM at D3_BKPSRAM_BASE (0x38800000). It
 * survives soft resets and watchdog fires — the exact situations we
 * most want DTCs to survive to diagnose — but is lost on power loss
 * (unless the board has a coin cell on V_BAT).
 *
 * Capacity: 32 entries of 20 bytes each, plus a 16-byte header, for
 * 656 bytes total. Plenty of BKPSRAM headroom (4 KB) left for later
 * phases to claim.
 *
 * Dedup: logging the same `code` bumps `occurrence_count` (saturates
 * at 255) and updates `last_seen_uptime_seconds`; a NOTIFY_DTC is NOT
 * emitted on dedupes, only on genuinely new codes.
 *
 * Eviction: when the 32nd new code arrives, the oldest entry (slot 0)
 * is evicted and the rest shift down. Simple FIFO; good enough for
 * a bootloader.
 *
 * The table persists across the bootloader-to-application jump (BKPSRAM
 * doesn't go anywhere) but only the bootloader writes to it today —
 * apps have their own fault-logging story, outside this module's scope.
 */

#include <stdbool.h>
#include <stdint.h>

/* ---- Capacity + magic ---- */
#define BL_DTC_MAX_ENTRIES      32U
#define BL_DTC_MAGIC            0xD1CAD1CAU

/* Backup-SRAM address carved out for the DTC table. */
#define BL_DTC_TABLE_ADDR       0x38800000U

/* ---- Severity codes (low 8 bits of the entry's severity byte) ---- */
#define BL_DTC_SEV_INFO         0U
#define BL_DTC_SEV_WARN         1U
#define BL_DTC_SEV_ERROR        2U
#define BL_DTC_SEV_FATAL        3U

/* ---- Well-known DTC codes the bootloader itself emits ---- */
#define BL_DTC_FLASH_HW         0x0010U  /* flash erase / program HAL failure */
#define BL_DTC_SESSION_TIMEOUT  0x0020U  /* session watchdog expired           */
#define BL_DTC_ISOTP_ERROR      0x0030U  /* bl_isotp_rx_feed returned an error
                                          * code the dispatcher converted to
                                          * NACK(TRANSPORT_ERROR or TIMEOUT).
                                          * context_data v2 encoding (LE-packed):
                                          *   byte 3 (high) = bl_isotp_rx_status_t
                                          *                   (2=BAD_PCI, 3=BAD_SEQ,
                                          *                   4=OVERFLOW, 5=NO_FF,
                                          *                   6=TIMEOUT)
                                          *   byte 2        = last_err_raw — full
                                          *                   data[0] byte of the
                                          *                   frame that triggered
                                          *                   the reject. For a
                                          *                   CF this is the PCI
                                          *                   byte (high nibble
                                          *                   0x2, low nibble =
                                          *                   sequence). Bench-
                                          *                   visibility into
                                          *                   what the BL actually
                                          *                   saw vs what the
                                          *                   candump shows the
                                          *                   host sent.
                                          *   byte 1        = last_err_expected
                                          *                   (BAD_SEQ: expected
                                          *                   next_seq; else 0)
                                          *   byte 0 (low)  = last_err_observed
                                          *                   (BAD_SEQ: got_seq
                                          *                   nibble; else 0)
                                          * FDCAN RX FIFO fill is dropped from
                                          * the DTC encoding in v2 (already ruled
                                          * out as the root cause on first
                                          * bench-replay; left in the log-ring
                                          * message for completeness). #94. */
#define BL_DTC_FDCAN_BUSOFF     0x0040U  /* FDCAN entered Bus_Off and the main
                                          * loop performed a Stop/Start recovery
                                          * (issue #125 C1). context_data = the
                                          * recovery-attempt count at the time of
                                          * this event. WARN severity — recovery
                                          * is automatic; the DTC is a breadcrumb
                                          * that the bus had a hard fault. */
#define BL_DTC_CPU_FAULT        0x0050U  /* the previous boot ended in a
                                          * controlled reboot from a terminal
                                          * spin — a CPU fault handler,
                                          * Error_Handler, or a failed FDCAN
                                          * init (issue #125 H6 reset-on-spin).
                                          * Logged once, after recovery, by
                                          * Bootloader_Init. context_data =
                                          * BL_FAULT_* reason (see bl_fault.h).
                                          * FATAL severity. */

/* ---- Entry layout (20 bytes, no padding) ---- */
typedef struct {
    uint16_t code;                          /* offset 0  — vendor-defined fault code        */
    uint8_t  severity;                      /* offset 2  — BL_DTC_SEV_*                     */
    uint8_t  occurrence_count;              /* offset 3  — saturates at 255                 */
    uint32_t first_seen_uptime_seconds;     /* offset 4                                     */
    uint32_t last_seen_uptime_seconds;      /* offset 8                                     */
    uint32_t context_data;                  /* offset 12 — e.g. failing flash address       */
    uint32_t reserved;                      /* offset 16 — zero until claimed               */
} bl_dtc_entry_t;

_Static_assert(sizeof(bl_dtc_entry_t) == 20,
               "bl_dtc_entry_t must be exactly 20 bytes");

/* ---- Table layout (656 bytes) ---- */
typedef struct {
    uint32_t magic;                         /* offset 0  — BL_DTC_MAGIC   */
    uint16_t count;                         /* offset 4  — active entries */
    uint16_t header_reserved_0;             /* offset 6                    */
    uint32_t header_reserved_1;             /* offset 8                    */
    uint32_t header_reserved_2;             /* offset 12                   */
    bl_dtc_entry_t entries[BL_DTC_MAX_ENTRIES]; /* offset 16..655          */
} bl_dtc_table_t;

_Static_assert(sizeof(bl_dtc_table_t) == 16U + 20U * BL_DTC_MAX_ENTRIES,
               "bl_dtc_table_t size drifted");

/* ---- API ---- */

/*
 * Validate and prepare the DTC table. Enables backup-domain access +
 * the BKPSRAM clock, checks the magic and bounds; if either is bad the
 * table is zeroed and re-magic'd. Must run before any bl_dtc_log /
 * bl_dtc_read call — typically in Bootloader_Init right after
 * bl_health_init.
 */
void bl_dtc_init(void);

/* Number of active entries currently in the table. */
uint16_t bl_dtc_count(void);

/* `code` of the most recently logged entry, or 0 if the table is empty. */
uint16_t bl_dtc_last_code(void);

/*
 * Log a fault. If `code` already exists, bumps occurrence_count and
 * updates last_seen; otherwise inserts a fresh entry (evicting the
 * oldest if the table is full). A NEW entry triggers a
 * NOTIFY_DTC broadcast via bl_proto_send_notify; dedupes do not.
 *
 * Safe to call from any context that uses bl_health_uptime_seconds()
 * (i.e. after HAL_Init + bl_health_init). Does not block.
 */
void bl_dtc_log(uint16_t code, uint8_t severity, uint32_t context);

/* Erase every entry and reset count to zero. Preserves the magic
 * so the table is still considered valid. */
void bl_dtc_clear(void);

/*
 * Serialise the table into `buf` as a byte stream:
 *   [count_le16, entry_0, entry_1, …, entry_N-1]
 *
 * The caller is responsible for sizing `buf` to at least
 * 2 + count * sizeof(bl_dtc_entry_t) bytes. `*out_len` receives the
 * number of bytes written.
 */
void bl_dtc_serialize(uint8_t *buf, uint16_t *out_len);

#endif /* BL_DTC_H */
