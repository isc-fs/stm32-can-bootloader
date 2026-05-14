/*
 * bl_health.c — health and heartbeat logic.
 *
 * Reset cause is latched once from RCC->RSR in bl_health_init and the
 * register is cleared so the next reset starts with a clean slate.
 * The heartbeat tick emits NOTIFY_HEARTBEAT at 1 Hz while a session is
 * active — idle bootloaders stay silent.
 */

#include "bl_health.h"

#include "bl_config.h"
#include "bl_dtc.h"
#include "bl_memmap.h"
#include "bl_nvm.h"
#include "bl_obyte.h"
#include "bl_proto.h"
#include "main.h"
#include "stm32h7xx_hal.h"

#include <string.h>

#define BL_HEALTH_HEARTBEAT_INTERVAL_MS   1000U

/* ---- Latched reset cause + heartbeat scheduler ---- */

static uint8_t  g_reset_cause         = BL_RESET_UNKNOWN;
static uint32_t g_last_heartbeat_ms   = 0U;

/* Lifetime counter of bl_flash_{write,erase} ops. Loaded from NVM at
 * boot (defaults to 0 if no persisted value yet), bumped + persisted
 * by bl_health_record_flash_write(). Lives in RAM as well so the
 * snapshot path doesn't have to round-trip through NVM on every call
 * to bl_health_fill_record. */
static uint32_t g_flash_write_count   = 0U;

/* Map an RCC->RSR snapshot to a single BL_RESET_* value.
 *
 * In normal operation only one flag is set per reset cycle because
 * __HAL_RCC_CLEAR_RESET_FLAGS() runs at the start of bl_health_init.
 * The order below fires fault causes first (WWDG, IWDG) so a reboot
 * caused by a rare dual-condition scenario still reports the more
 * actionable cause. */
static uint8_t rsr_to_reset_cause(uint32_t rsr)
{
    if ((rsr & RCC_RSR_WWDG1RSTF) != 0U) return BL_RESET_WWDG;
    if ((rsr & RCC_RSR_IWDG1RSTF) != 0U) return BL_RESET_IWDG;
    if ((rsr & RCC_RSR_LPWRRSTF)  != 0U) return BL_RESET_LOW_POWER;
    if ((rsr & RCC_RSR_SFTRSTF)   != 0U) return BL_RESET_SOFTWARE;
    if ((rsr & RCC_RSR_PINRSTF)   != 0U) return BL_RESET_PIN;
    if ((rsr & RCC_RSR_BORRSTF)   != 0U) return BL_RESET_BROWNOUT;
    if ((rsr & RCC_RSR_PORRSTF)   != 0U) return BL_RESET_POWER_ON;
    return BL_RESET_UNKNOWN;
}


/* ---- API ---- */

void bl_health_init(void)
{
    uint32_t rsr = RCC->RSR;
    g_reset_cause = rsr_to_reset_cause(rsr);
    __HAL_RCC_CLEAR_RESET_FLAGS();

    g_last_heartbeat_ms = HAL_GetTick();

    /* Restore the persistent flash-op counter. NOT_FOUND just means
     * we've never persisted one yet — start at 0. Anything else
     * unexpected (truncated value, hardware error) also defaults to
     * 0 so the field stays well-defined. */
    uint8_t actual = 0U;
    uint32_t saved = 0U;
    bl_nvm_status_t r = bl_nvm_read(BL_NVM_KEY_FLASH_WRITE_COUNT,
                                    &saved, sizeof(saved), &actual);
    if (r == BL_NVM_OK && actual == sizeof(saved)) {
        g_flash_write_count = saved;
    } else {
        g_flash_write_count = 0U;
    }
}

void bl_health_record_flash_write(void)
{
    g_flash_write_count++;
    /* Best-effort persist. A failed NVM write (sector full + compaction
     * also full, hardware error) leaves the in-RAM count ahead of the
     * persisted one — the next successful persist or the next boot's
     * read closes the gap. We don't surface a failure here because
     * the caller is in the middle of a real flash op whose own status
     * is what matters. */
    (void)bl_nvm_write(BL_NVM_KEY_FLASH_WRITE_COUNT,
                       &g_flash_write_count, sizeof(g_flash_write_count));
}

uint8_t bl_health_reset_cause(void)
{
    return g_reset_cause;
}

uint32_t bl_health_uptime_seconds(void)
{
    return HAL_GetTick() / 1000U;
}

uint32_t bl_health_flags(void)
{
    uint32_t flags = 0U;
    if (bl_proto_session_active()) {
        flags |= BL_HEALTH_FLAG_SESSION_ACTIVE;
    }
    if (Bootloader_CheckApplication() == 0U) {
        flags |= BL_HEALTH_FLAG_VALID_APP_PRESENT;
    }
    /* Report whether the bootloader's own sector is WRP'd. The host
     * uses this to decide whether to issue OB_APPLY_WRP during
     * provisioning. */
    if (bl_obyte_is_sector_wrp_protected(0U)) {
        flags |= BL_HEALTH_FLAG_WRP_PROTECTED;
    }
    return flags;
}

void bl_health_fill_record(bl_health_record_t *out)
{
    out->uptime_seconds    = bl_health_uptime_seconds();
    out->reset_cause       = (uint32_t)g_reset_cause;
    out->flags             = bl_health_flags();
    out->flash_write_count = g_flash_write_count;
    out->dtc_count         = (uint32_t)bl_dtc_count();
    out->last_dtc_code     = (uint32_t)bl_dtc_last_code();
    out->reserved[0]       = 0U;
    out->reserved[1]       = 0U;
}

void bl_health_tick(uint32_t now_ms)
{
    /* Heartbeat only ticks while a session is active. When the
     * session is idle we keep advancing the scheduler anchor so the
     * first heartbeat after a fresh CONNECT lands a full interval
     * later instead of immediately. */
    if (!bl_proto_session_active()) {
        g_last_heartbeat_ms = now_ms;
        return;
    }

    uint32_t elapsed = now_ms - g_last_heartbeat_ms;
    if (elapsed < BL_HEALTH_HEARTBEAT_INTERVAL_MS) {
        return;
    }
    g_last_heartbeat_ms = now_ms;

    /* 7-byte SF payload fits in a single CAN frame:
     *   [0] opcode = NOTIFY_HEARTBEAT
     *   [1] node_id
     *   [2] reset_cause
     *   [3] flags low byte
     *   [4..6] uptime_le24 (wraps at ~194 days) */
    uint32_t uptime = bl_health_uptime_seconds();
    uint32_t flags  = bl_health_flags();

    uint8_t payload[7] = {
        BL_NOTIFY_HEARTBEAT,
        (uint8_t)BL_NODE_ID,
        g_reset_cause,
        (uint8_t)(flags & 0xFFU),
        (uint8_t)(uptime         & 0xFFU),
        (uint8_t)((uptime >>  8) & 0xFFU),
        (uint8_t)((uptime >> 16) & 0xFFU),
    };
    bl_proto_send_notify(payload, (uint16_t)sizeof(payload));
}
