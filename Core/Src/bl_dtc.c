/*
 * bl_dtc.c — DTC table implementation over Backup SRAM.
 *
 * The table lives at a fixed BKPSRAM address (BL_DTC_TABLE_ADDR). Access
 * requires the backup domain to be enabled + the BKPSRAM clock gated on,
 * which bl_dtc_init handles. After that the struct is just dereferenced
 * through a typed pointer — byte, halfword and word accesses are all
 * fine on H7 BKPSRAM.
 *
 * Dependencies on other bootloader modules are internal only (bl_health
 * for uptime, bl_proto for NOTIFY_DTC emission); the public API in
 * bl_dtc.h doesn't leak anything.
 */

#include "bl_dtc.h"

#include "bl_health.h"
#include "bl_proto.h"
#include "stm32h7xx_hal.h"

#include <string.h>

/* ---- BKPSRAM-resident table pointer ---- */

static bl_dtc_table_t *const g_table = (bl_dtc_table_t *)BL_DTC_TABLE_ADDR;

/* Zero-init the table in place and stamp the magic so subsequent
 * bl_dtc_init calls recognise it as valid. */
static void reset_table(void)
{
    memset(g_table, 0, sizeof(*g_table));
    g_table->magic = BL_DTC_MAGIC;
}

/* ---- API ---- */

void bl_dtc_init(void)
{
    /* The backup domain must be unlocked before any BKPSRAM access.
     * Safe to call repeatedly — HAL tracks the write-protection bit. */
    HAL_PWR_EnableBkUpAccess();

    /* Gate the BKPSRAM clock on so reads/writes reach the SRAM block.
     * Without this, pointer access faults with a bus error. */
    __HAL_RCC_BKPRAM_CLK_ENABLE();

    /* Validate or (re-)initialise. A mismatched magic or an out-of-
     * range count means either a fresh cold boot (BKPSRAM contents
     * are undefined) or a software corruption; either way we start
     * clean. */
    if (g_table->magic != BL_DTC_MAGIC
        || g_table->count > BL_DTC_MAX_ENTRIES) {
        reset_table();
    }
}

uint16_t bl_dtc_count(void)
{
    return g_table->count;
}

uint16_t bl_dtc_last_code(void)
{
    if (g_table->count == 0U) {
        return 0U;
    }
    return g_table->entries[g_table->count - 1U].code;
}

void bl_dtc_clear(void)
{
    reset_table();
}

/* Insert a new entry at the end of the table, evicting slot 0 if full.
 * Returns a pointer to the freshly-initialised entry. */
static bl_dtc_entry_t *insert_new(uint16_t code,
                                  uint8_t  severity,
                                  uint32_t context,
                                  uint32_t now_s)
{
    uint16_t slot;
    if (g_table->count < BL_DTC_MAX_ENTRIES) {
        slot = g_table->count;
        g_table->count++;
    } else {
        /* Evict oldest (slot 0), shift the rest down by one. */
        for (uint16_t i = 0U; i + 1U < BL_DTC_MAX_ENTRIES; i++) {
            g_table->entries[i] = g_table->entries[i + 1U];
        }
        slot = BL_DTC_MAX_ENTRIES - 1U;
    }

    bl_dtc_entry_t *e = &g_table->entries[slot];
    e->code                      = code;
    e->severity                  = severity;
    e->occurrence_count          = 1U;
    e->first_seen_uptime_seconds = now_s;
    e->last_seen_uptime_seconds  = now_s;
    e->context_data              = context;
    e->reserved                  = 0U;
    return e;
}

void bl_dtc_log(uint16_t code, uint8_t severity, uint32_t context)
{
    uint32_t now_s = bl_health_uptime_seconds();

    /* Dedup on `code`: bump the existing entry if we already know it. */
    for (uint16_t i = 0U; i < g_table->count; i++) {
        if (g_table->entries[i].code == code) {
            if (g_table->entries[i].occurrence_count < 0xFFU) {
                g_table->entries[i].occurrence_count++;
            }
            g_table->entries[i].last_seen_uptime_seconds = now_s;
            /* No NOTIFY on dedupe — would be noisy for a chatty
             * persistent fault. */
            return;
        }
    }

    /* Brand-new code. */
    (void)insert_new(code, severity, context, now_s);

    /* Emit NOTIFY_DTC so any listening host picks up the new fault
     * immediately. 5-byte payload fits in a single SF. */
    uint8_t payload[5] = {
        BL_NOTIFY_DTC,
        (uint8_t)(code & 0xFFU),
        (uint8_t)((code >> 8) & 0xFFU),
        severity,
        1U,  /* occurrence_count at first sighting */
    };
    bl_proto_send_notify(payload, (uint16_t)sizeof(payload));
}

void bl_dtc_serialize(uint8_t *buf, uint16_t *out_len)
{
    uint16_t count = g_table->count;

    buf[0] = (uint8_t)(count & 0xFFU);
    buf[1] = (uint8_t)((count >> 8) & 0xFFU);

    /* Each entry is already tightly-packed and naturally aligned —
     * no conversion needed, just copy struct-wise. */
    for (uint16_t i = 0U; i < count; i++) {
        memcpy(&buf[2U + (uint32_t)i * sizeof(bl_dtc_entry_t)],
               &g_table->entries[i],
               sizeof(bl_dtc_entry_t));
    }

    *out_len = (uint16_t)(2U + count * sizeof(bl_dtc_entry_t));
}
