/*
 * bl_flashcount.c — lifetime flash-op counter, RAM-first with
 * session-boundary persistence (#187). See bl_flashcount.h.
 *
 * Depends only on bl_nvm so the host unit tests can link it directly.
 */

#include "bl_flashcount.h"

#include "bl_nvm.h"

/* Live value (reported in the health record) and the value last
 * successfully written to NVM. flush() is a no-op while they match. */
static uint32_t g_count     = 0U;
static uint32_t g_persisted = 0U;

void bl_flashcount_restore(void)
{
    uint8_t  actual = 0U;
    uint32_t saved  = 0U;
    bl_nvm_status_t r = bl_nvm_read(BL_NVM_KEY_FLASH_WRITE_COUNT,
                                    &saved, sizeof(saved), &actual);
    if (r != BL_NVM_OK || actual != sizeof(saved)) {
        /* Never persisted, truncated value or read error — start at 0
         * so the field stays well-defined. */
        saved = 0U;
    }
    g_count     = saved;
    g_persisted = saved;
}

void bl_flashcount_bump(void)
{
    g_count++;
}

void bl_flashcount_flush(void)
{
    if (g_count == g_persisted) {
        return;
    }
    /* Best-effort. On failure (NVM degraded / full / hardware error)
     * leave g_persisted behind so the next boundary retries. */
    if (bl_nvm_write(BL_NVM_KEY_FLASH_WRITE_COUNT,
                     &g_count, sizeof(g_count)) == BL_NVM_OK) {
        g_persisted = g_count;
    }
}

uint32_t bl_flashcount_get(void)
{
    return g_count;
}
