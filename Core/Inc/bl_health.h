#ifndef BL_HEALTH_H
#define BL_HEALTH_H

/*
 * bl_health — health and heartbeat reporting.
 *
 * Three responsibilities:
 *
 *   1. Latch the reset cause from RCC->RSR once at boot and clear the
 *      flags. After this the reset cause is stable for the lifetime of
 *      the bootloader session; reading RCC->RSR later would just show
 *      an empty register.
 *
 *   2. Emit an unsolicited NOTIFY_HEARTBEAT once a second **while a
 *      session is active**. Idle bootloaders stay silent so a bus with
 *      many nodes is not flooded with heartbeats going nowhere.
 *
 *   3. Provide a fixed-layout 32-byte health record for the
 *      CMD_GET_HEALTH opcode. Fields belonging to future phases
 *      (flash-write count, DTC counters) are wired in as zeros today
 *      and flipped to real values as feat/12+ land.
 *
 * The reset-cause latch has to run early — ideally as the first thing
 * in Bootloader_Init — so nothing accidentally clears RCC->RSR before
 * we read it.
 */

#include <stdbool.h>
#include <stdint.h>

/* ---- Reset cause (one byte, sent in heartbeat + health record) ---- */
#define BL_RESET_UNKNOWN      0x00U
#define BL_RESET_POWER_ON     0x01U  /* RCC_RSR_PORRSTF   */
#define BL_RESET_PIN          0x02U  /* RCC_RSR_PINRSTF (NRST) */
#define BL_RESET_SOFTWARE     0x03U  /* RCC_RSR_SFTRSTF (NVIC_SystemReset) */
#define BL_RESET_IWDG         0x04U  /* RCC_RSR_IWDG1RSTF */
#define BL_RESET_WWDG         0x05U  /* RCC_RSR_WWDG1RSTF */
#define BL_RESET_LOW_POWER    0x06U  /* RCC_RSR_LPWRRSTF  */
#define BL_RESET_BROWNOUT     0x07U  /* RCC_RSR_BORRSTF   */

/* ---- Flags bitmask, carried in heartbeat (low 8 bits) and the full
 * 32-bit health record. Higher bits reserved for later phases.  ---- */
#define BL_HEALTH_FLAG_SESSION_ACTIVE       (1U << 0)
#define BL_HEALTH_FLAG_VALID_APP_PRESENT    (1U << 1)
#define BL_HEALTH_FLAG_WRP_PROTECTED        (1U << 4)  /* sector 0 WRP'd in option bytes */
/* bits 2..3, 5..31: reserved (pending DTC, encrypted session, …) */

/* ---- Health record returned by CMD_GET_HEALTH (32 bytes) ---- */
#define BL_HEALTH_RECORD_SIZE   32U

typedef struct {
    uint32_t uptime_seconds;    /* offset 0  — seconds since boot     */
    uint32_t reset_cause;       /* offset 4  — BL_RESET_* value        */
    uint32_t flags;             /* offset 8  — BL_HEALTH_FLAG_* bitmask */
    uint32_t flash_write_count; /* offset 12 — 0 until Phase 4 NVM     */
    uint32_t dtc_count;         /* offset 16 — 0 until feat/12-dtc     */
    uint32_t last_dtc_code;     /* offset 20 — 0 until feat/12-dtc     */
    uint32_t reserved[2];       /* offset 24..31 — zero                */
} bl_health_record_t;

_Static_assert(sizeof(bl_health_record_t) == BL_HEALTH_RECORD_SIZE,
               "bl_health_record_t must be exactly 32 bytes");

/* ---- API ---- */

/* Capture and clear RCC->RSR. Call once from Bootloader_Init before
 * anything else has a chance to clear the register. Also seeds the
 * heartbeat-cadence timer so the first heartbeat fires one second
 * after session start, not immediately. */
void bl_health_init(void);

/* Latched reset cause from the boot that started this session. */
uint8_t bl_health_reset_cause(void);

/* Uptime since boot, in whole seconds. */
uint32_t bl_health_uptime_seconds(void);

/* Composed BL_HEALTH_FLAG_* bitmask reflecting current session and
 * application state. */
uint32_t bl_health_flags(void);

/* Fill the 32-byte health record with current state. */
void bl_health_fill_record(bl_health_record_t *out);

/* Main-loop tick. Called every iteration with HAL_GetTick(). Emits
 * NOTIFY_HEARTBEAT once per second while a session is active; no-op
 * otherwise. */
void bl_health_tick(uint32_t now_ms);

/* Record one bootloader-initiated flash operation (program or erase
 * via bl_flash_*). Bumps the in-RAM counter immediately and persists
 * it to NVM under BL_NVM_KEY_FLASH_WRITE_COUNT so the next boot's
 * counter starts where this one left off. The persisted value is
 * what bl_health_fill_record reports as `flash_write_count`. NVM
 * writes are themselves coalesced via the log-structured store's
 * own compaction, so calling this per-op is not flash-pathological
 * — every ~4000 ops triggers a single compaction-erase. */
void bl_health_record_flash_write(void);

#endif /* BL_HEALTH_H */
