#ifndef BL_FLASHCOUNT_H
#define BL_FLASHCOUNT_H

/*
 * bl_flashcount — lifetime counter of bootloader flash ops (#187).
 *
 * Counts every successful bl_flash_{write,erase} call and reports it as
 * `flash_write_count` in the GET_HEALTH record. Persisted in NVM under
 * BL_NVM_KEY_FLASH_WRITE_COUNT, but NOT on every op:
 *
 *   - bl_flashcount_bump()  — RAM only. Called from bl_flash on each
 *     successful program / erase. No flash traffic, so it's safe in the
 *     WRITE_CHUNK hot path.
 *   - bl_flashcount_flush() — persists the RAM value once, and only if
 *     it changed since the last persist. Called at session boundaries
 *     (FLASH_VERIFY commit, DISCONNECT, session timeout, and before any
 *     reset / jump), so a full app flash costs O(1) NVM records instead
 *     of one per chunk. Previously every op appended a record: one
 *     87 KB AMS flash wrote 513 records (~12.7 % of sector 7), forcing a
 *     full-sector compaction erase every ~8 flashes.
 *
 * The counter is approximately durable by design: ops since the last
 * flush are lost if power is cut mid-session. That's fine for a
 * wear/health statistic and far cheaper than the churn it replaces.
 *
 * bl_flashcount_restore() reads the persisted value back. It must run
 * AFTER bl_nvm_init() (which finds the append point — before it, every
 * read scans zero slots and returns NOT_FOUND, the pre-#187 "counter
 * restarts at 0 every boot" bug) and inside Bootloader_Init's ECC guard,
 * like every other sector-7 read. It is skipped on the degraded-NVM
 * (G-A2) recovery path; the counter then starts at 0 for that boot.
 */

#include <stdint.h>

/* Load the persisted counter from NVM into RAM. NOT_FOUND (never
 * persisted) or any malformed value yields 0. Idempotent. */
void bl_flashcount_restore(void);

/* Record one successful bootloader flash op. RAM only. */
void bl_flashcount_bump(void);

/* Persist the RAM counter to NVM if it changed since the last persist.
 * Best-effort: a failed NVM write keeps the value dirty so the next
 * flush retries. Never call from a flash-op hot path. */
void bl_flashcount_flush(void);

/* Current (RAM) counter value — what the health record reports. */
uint32_t bl_flashcount_get(void);

#endif /* BL_FLASHCOUNT_H */
