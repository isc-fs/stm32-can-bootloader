/*
 * bl_nvm.c — log-structured NVM key-value store in flash sector 7.
 *
 * See Core/Inc/bl_nvm.h for the storage layout, wire semantics and
 * compaction contract. All flash access in this module uses
 * HAL_FLASH_Program / HAL_FLASHEx_Erase directly rather than going
 * through bl_flash, because bl_flash is strictly the app-region
 * gatekeeper — NVM lives in a different sector with different rules.
 */

#include "bl_nvm.h"

#include "bl_iwdg.h"
#include "bl_memmap.h"
#include "stm32h7xx_hal.h"

#include <string.h>

#define BL_NVM_TOTAL_SLOTS       (BL_NVM_SIZE / BL_NVM_ENTRY_SIZE)


/* ---- Internal state ---- */

/* Byte offset within sector 7 of the next empty slot. Updated on
 * every successful append or compaction. */
static uint32_t g_write_pos = 0U;

/* Highest `seq` value we've observed so far. Next write uses
 * ++g_max_seq. Compaction resets this to the new live-count. */
static uint32_t g_max_seq = 0U;


/* ---- Helpers ---- */

static const bl_nvm_entry_t *slot_at(uint32_t slot)
{
    return (const bl_nvm_entry_t *)(BL_NVM_BASE + slot * BL_NVM_ENTRY_SIZE);
}

/* Write one 32-byte FLASHWORD at `addr`. Caller owns the unlock /
 * lock; this wrapper lets the rest of the file stay readable. */
/* `uintptr_t addr` rather than `uint32_t addr` so the host-test build
 * (where pointers are 64-bit) preserves the full BL_NVM_BASE-relative
 * address through the call. On STM32 sizeof(uintptr_t) == sizeof(uint32_t)
 * so there's no behavioural drift. */
static HAL_StatusTypeDef program_flashword(uintptr_t addr, const void *data)
{
    return HAL_FLASH_Program(FLASH_TYPEPROGRAM_FLASHWORD,
                             addr,
                             (uintptr_t)data);
}

/* Erase sector 7. The app metadata word at BL_APP_METADATA_ADDR is in
 * this sector, so the caller must have saved it in RAM and be prepared
 * to re-program it afterwards. */
static HAL_StatusTypeDef erase_nvm_sector(void)
{
    FLASH_EraseInitTypeDef init = { 0 };
    init.TypeErase = FLASH_TYPEERASE_SECTORS;
    init.Banks     = FLASH_BANK_1;
    init.Sector    = BL_NVM_SECTOR;
    init.NbSectors = 1U;

    /* #125 H6: kick the IWDG before this single-sector stall. Erasing
     * sector 7 stalls the single-bank CPU for ~1.8 s (same mechanism as
     * the app erase in bl_flash.c); one sector fits inside the ~8 s IWDG
     * period, but refreshing first guarantees a full window and keeps the
     * invariant clean — no CPU stall in the BL runs un-kicked. */
    bl_iwdg_refresh();

    uint32_t page_error = 0U;
    return HAL_FLASHEx_Erase(&init, &page_error);
}


/* ---- Init ---- */

void bl_nvm_init(void)
{
    uint32_t last_live_slot = (uint32_t)-1;
    uint32_t max_seq = 0U;

    for (uint32_t i = 0U; i < BL_NVM_TOTAL_SLOTS; i++) {
        const bl_nvm_entry_t *e = slot_at(i);
        if (e->magic == BL_NVM_ENTRY_MAGIC) {
            last_live_slot = i;
            if (e->seq > max_seq) {
                max_seq = e->seq;
            }
        }
        /* Any non-magic slot (erased 0xFFs or stale patterns) is
         * tolerated — we want the append point to be one past the
         * last MAGIC we see, not the first 0xFF. That way stray
         * bits in the middle of the sector don't make us silently
         * overwrite later live entries. */
    }

    g_write_pos = (last_live_slot == (uint32_t)-1)
                      ? 0U
                      : (last_live_slot + 1U) * BL_NVM_ENTRY_SIZE;
    g_max_seq = max_seq;
}


/* ---- Read ---- */

/* Finds the highest-seq MAGIC entry for `key` in [0, slot_limit).
 * Returns NULL if none. */
static const bl_nvm_entry_t *find_latest(uint16_t key, uint32_t slot_limit)
{
    const bl_nvm_entry_t *best = (const bl_nvm_entry_t *)0;
    uint32_t best_seq = 0U;

    for (uint32_t i = 0U; i < slot_limit; i++) {
        const bl_nvm_entry_t *e = slot_at(i);
        if (e->magic != BL_NVM_ENTRY_MAGIC) {
            continue;
        }
        if (e->key != key) {
            continue;
        }
        if (best == (const bl_nvm_entry_t *)0 || e->seq > best_seq) {
            best = e;
            best_seq = e->seq;
        }
    }
    return best;
}

bl_nvm_status_t bl_nvm_read(uint16_t key,
                            void *out,
                            uint8_t max_len,
                            uint8_t *actual_len)
{
    uint32_t end_slot = g_write_pos / BL_NVM_ENTRY_SIZE;
    const bl_nvm_entry_t *best = find_latest(key, end_slot);

    if (best == (const bl_nvm_entry_t *)0 || best->len == 0U) {
        return BL_NVM_NOT_FOUND;  /* never written, or latest is a tombstone */
    }
    if (best->len > max_len) {
        return BL_NVM_NOT_FOUND;  /* caller's buffer too small */
    }

    memcpy(out, best->value, best->len);
    if (actual_len != (uint8_t *)0) {
        *actual_len = best->len;
    }
    return BL_NVM_OK;
}


/* ---- Live-count (read-only scan, matches read()'s dedup rules) ---- */

uint32_t bl_nvm_live_count(void)
{
    /* Hot-loop-avoiding approach: walk sector once, track per-key
     * latest entry in a small inline array, count entries whose
     * latest is non-tombstone. */
    uint16_t keys[BL_NVM_MAX_LIVE_ENTRIES];
    uint32_t seqs[BL_NVM_MAX_LIVE_ENTRIES];
    uint8_t  lens[BL_NVM_MAX_LIVE_ENTRIES];
    uint32_t unique_count = 0U;

    uint32_t end_slot = g_write_pos / BL_NVM_ENTRY_SIZE;
    for (uint32_t i = 0U; i < end_slot; i++) {
        const bl_nvm_entry_t *e = slot_at(i);
        if (e->magic != BL_NVM_ENTRY_MAGIC) {
            continue;
        }

        /* Upsert into our local dedup table. */
        uint32_t j;
        for (j = 0U; j < unique_count; j++) {
            if (keys[j] == e->key) {
                if (e->seq > seqs[j]) {
                    seqs[j] = e->seq;
                    lens[j] = e->len;
                }
                break;
            }
        }
        if (j == unique_count) {
            if (unique_count < BL_NVM_MAX_LIVE_ENTRIES) {
                keys[unique_count] = e->key;
                seqs[unique_count] = e->seq;
                lens[unique_count] = e->len;
                unique_count++;
            }
            /* If we've seen more unique keys than the buffer tracks,
             * the overflow ones simply aren't counted. Counting is a
             * diagnostic getter; it doesn't drive behaviour. */
        }
    }

    uint32_t live = 0U;
    for (uint32_t i = 0U; i < unique_count; i++) {
        if (lens[i] > 0U) {
            live++;
        }
    }
    return live;
}


/* ---- Compaction + append ----
 *
 * Builds a RAM buffer of the latest live entries, erases sector 7,
 * rewrites the buffer + the requested new write + restores the app
 * metadata FLASHWORD. Called from bl_nvm_write when the sector fills.
 *
 * If there are more than BL_NVM_MAX_LIVE_ENTRIES unique live keys,
 * compaction refuses with BL_NVM_FULL and the caller sees the same.
 *
 * The local buffer lives on the stack — 4 KB in DTCM — which is
 * well within the bootloader's stack budget for a rarely-taken path.
 */
static bl_nvm_status_t compact_and_append(uint16_t new_key,
                                          const void *new_value,
                                          uint8_t new_len)
{
    /* Defence in depth on caller-supplied length. bl_nvm_write does
     * the same check before calling us, but a future caller (test
     * harness, future opcode handler) could forget — so we re-check
     * here. memcpy(entry.value, new_value, new_len) below copies into
     * a fixed-size `value[BL_NVM_MAX_VALUE_LEN]` field, and STM32
     * flash's program-by-FLASHWORD semantics mean any byte past the
     * struct end gets quietly clobbered with the next sector's data.
     * Audit follow-up (#68 bl_nvm bullet) — same encapsulation
     * pattern issue #54 cured for bl_isotp. */
    if (new_len > BL_NVM_MAX_VALUE_LEN) {
        return BL_NVM_BAD_ARG;
    }

    bl_nvm_entry_t buf[BL_NVM_MAX_LIVE_ENTRIES];
    uint32_t count = 0U;

    /* Pass 1: dedup all MAGIC entries into `buf`, keeping the latest
     * seq per key. This also naturally preserves tombstones — they're
     * stripped in pass 2. */
    uint32_t end_slot = g_write_pos / BL_NVM_ENTRY_SIZE;
    for (uint32_t i = 0U; i < end_slot; i++) {
        const bl_nvm_entry_t *e = slot_at(i);
        if (e->magic != BL_NVM_ENTRY_MAGIC) {
            continue;
        }
        uint32_t j;
        for (j = 0U; j < count; j++) {
            if (buf[j].key == e->key) {
                if (e->seq > buf[j].seq) {
                    buf[j] = *e;
                }
                break;
            }
        }
        if (j == count) {
            if (count >= BL_NVM_MAX_LIVE_ENTRIES) {
                return BL_NVM_FULL;
            }
            buf[count++] = *e;
        }
    }

    /* Pass 2: drop tombstones (len == 0). The new write may itself
     * be a tombstone for a key we just dropped — that's fine; we
     * still append it below for record-keeping, but it could be
     * elided. Keeping the record is simpler. */
    {
        uint32_t live = 0U;
        for (uint32_t i = 0U; i < count; i++) {
            if (buf[i].len > 0U) {
                if (i != live) {
                    buf[live] = buf[i];
                }
                live++;
            }
        }
        count = live;
    }

    /* Room check: after adding the new entry, `count + 1` slots must
     * fit inside BL_NVM_TOTAL_SLOTS. In practice this is only tight
     * when count ≈ BL_NVM_MAX_LIVE_ENTRIES. */
    if (count >= BL_NVM_MAX_LIVE_ENTRIES || (count + 1U) > BL_NVM_TOTAL_SLOTS) {
        return BL_NVM_FULL;
    }

    /* Snapshot the metadata FLASHWORD before we erase the sector. */
    uint32_t meta[BL_APP_METADATA_SIZE / 4U];
    memcpy(meta,
           (const void *)BL_APP_METADATA_ADDR,
           BL_APP_METADATA_SIZE);

    if (HAL_FLASH_Unlock() != HAL_OK) {
        return BL_NVM_HARDWARE;
    }

    HAL_StatusTypeDef st = erase_nvm_sector();
    if (st != HAL_OK) {
        HAL_FLASH_Lock();
        return BL_NVM_HARDWARE;
    }

    /* Rewrite live entries with freshly-numbered seq values. */
    uint32_t new_seq = 1U;
    for (uint32_t i = 0U; i < count; i++) {
        buf[i].seq = new_seq++;
        st = program_flashword(BL_NVM_BASE + i * BL_NVM_ENTRY_SIZE, &buf[i]);
        if (st != HAL_OK) {
            HAL_FLASH_Lock();
            return BL_NVM_HARDWARE;
        }
    }

    /* Append the new entry. */
    bl_nvm_entry_t entry = { 0 };
    entry.magic = BL_NVM_ENTRY_MAGIC;
    entry.key   = new_key;
    entry.len   = new_len;
    entry.seq   = new_seq++;
    if (new_len > 0U) {
        memcpy(entry.value, new_value, new_len);
    }
    st = program_flashword(BL_NVM_BASE + count * BL_NVM_ENTRY_SIZE, &entry);
    if (st != HAL_OK) {
        HAL_FLASH_Lock();
        return BL_NVM_HARDWARE;
    }
    count++;

    /* Restore the app metadata FLASHWORD. This is the very last flash
     * write of the compaction; if power dies earlier the metadata
     * stays erased and Bootloader_CheckApplication will reject the
     * app on next boot — recoverable via a host re-flash. */
    st = program_flashword(BL_APP_METADATA_ADDR, meta);
    HAL_FLASH_Lock();
    if (st != HAL_OK) {
        return BL_NVM_HARDWARE;
    }

    g_write_pos = count * BL_NVM_ENTRY_SIZE;
    g_max_seq   = new_seq - 1U;
    return BL_NVM_OK;
}


/* ---- Compact + replace metadata (used by bl_flash_write_metadata) ----
 *
 * STM32 flash can only flip 1→0; the app metadata FLASHWORD at
 * BL_APP_METADATA_ADDR therefore can't be rewritten in place once a
 * previous flash has set any bit. This helper does the same
 * snapshot / erase / rewrite dance as compact_and_append, but writes a
 * caller-supplied metadata value at the end instead of restoring the
 * old one — preserving live NVM entries while replacing the metadata
 * with fresh content.
 *
 * Returns BL_NVM_FULL if there are more than BL_NVM_MAX_LIVE_ENTRIES
 * live keys (same cap as compact_and_append). The caller should treat
 * that as a fatal condition, recoverable only via bl_nvm_format().
 */
bl_nvm_status_t bl_nvm_compact_replace_meta(const uint32_t new_meta[])
{
    bl_nvm_entry_t buf[BL_NVM_MAX_LIVE_ENTRIES];
    uint32_t count = 0U;

    /* Pass 1: dedup MAGIC entries, keep latest seq per key (mirrors
     * compact_and_append's pass 1). */
    uint32_t end_slot = g_write_pos / BL_NVM_ENTRY_SIZE;
    for (uint32_t i = 0U; i < end_slot; i++) {
        const bl_nvm_entry_t *e = slot_at(i);
        if (e->magic != BL_NVM_ENTRY_MAGIC) {
            continue;
        }
        uint32_t j;
        for (j = 0U; j < count; j++) {
            if (buf[j].key == e->key) {
                if (e->seq > buf[j].seq) {
                    buf[j] = *e;
                }
                break;
            }
        }
        if (j == count) {
            if (count >= BL_NVM_MAX_LIVE_ENTRIES) {
                return BL_NVM_FULL;
            }
            buf[count++] = *e;
        }
    }

    /* Pass 2: drop tombstones. */
    {
        uint32_t live = 0U;
        for (uint32_t i = 0U; i < count; i++) {
            if (buf[i].len > 0U) {
                if (i != live) {
                    buf[live] = buf[i];
                }
                live++;
            }
        }
        count = live;
    }

    if (HAL_FLASH_Unlock() != HAL_OK) {
        return BL_NVM_HARDWARE;
    }

    HAL_StatusTypeDef st = erase_nvm_sector();
    if (st != HAL_OK) {
        HAL_FLASH_Lock();
        return BL_NVM_HARDWARE;
    }

    /* Rewrite live entries with freshly-numbered seq values. */
    uint32_t new_seq = 1U;
    for (uint32_t i = 0U; i < count; i++) {
        buf[i].seq = new_seq++;
        st = program_flashword(BL_NVM_BASE + i * BL_NVM_ENTRY_SIZE, &buf[i]);
        if (st != HAL_OK) {
            HAL_FLASH_Lock();
            return BL_NVM_HARDWARE;
        }
    }

    /* Write the new metadata FLASHWORD. */
    st = program_flashword(BL_APP_METADATA_ADDR, new_meta);
    HAL_FLASH_Lock();
    if (st != HAL_OK) {
        return BL_NVM_HARDWARE;
    }

    g_write_pos = count * BL_NVM_ENTRY_SIZE;
    g_max_seq   = new_seq - 1U;
    return BL_NVM_OK;
}


/* ---- Format (unconditional sector erase + reset) ---- */
/*
 * Erases sector 7 in its entirety — NVM key-value entries AND the app
 * metadata FLASHWORD — and resets in-RAM pointers. Intended for
 * explicit operator-driven recovery when the sector has wedged in a
 * way the automatic compaction paths can't escape (e.g. partial writes
 * from prior power loss).
 *
 * After this call the BL boot path will see no_valid_app and a fresh
 * flash session is required to install a runnable image.
 */
bl_nvm_status_t bl_nvm_format(void)
{
    if (HAL_FLASH_Unlock() != HAL_OK) {
        return BL_NVM_HARDWARE;
    }
    HAL_StatusTypeDef st = erase_nvm_sector();
    HAL_FLASH_Lock();
    if (st != HAL_OK) {
        return BL_NVM_HARDWARE;
    }
    g_write_pos = 0U;
    g_max_seq   = 0U;
    return BL_NVM_OK;
}


/* ---- Write (fast append, with compaction fallback) ---- */

bl_nvm_status_t bl_nvm_write(uint16_t key, const void *value, uint8_t len)
{
    if (len > BL_NVM_MAX_VALUE_LEN) {
        return BL_NVM_BAD_ARG;
    }

    if (g_write_pos + BL_NVM_ENTRY_SIZE > BL_NVM_SIZE) {
        return compact_and_append(key, value, len);
    }

    bl_nvm_entry_t entry = { 0 };
    entry.magic = BL_NVM_ENTRY_MAGIC;
    entry.key   = key;
    entry.len   = len;
    entry.seq   = ++g_max_seq;
    if (len > 0U) {
        memcpy(entry.value, value, len);
    }

    if (HAL_FLASH_Unlock() != HAL_OK) {
        return BL_NVM_HARDWARE;
    }
    HAL_StatusTypeDef st = program_flashword(BL_NVM_BASE + g_write_pos, &entry);
    HAL_FLASH_Lock();

    if (st != HAL_OK) {
        /* Append failed — most common cause: the destination slot has
         * stray non-0xFF bits from a previous partial / power-lost write.
         * STM32 flash refuses 0→1 transitions, so that slot is
         * permanently un-programmable until the whole sector is erased.
         * Fall through to compaction (sector erase + rewrite of live
         * entries) and retry the append from a clean base. If compaction
         * itself fails, the original HARDWARE error is surfaced. */
        g_max_seq--;
        return compact_and_append(key, value, len);
    }

    g_write_pos += BL_NVM_ENTRY_SIZE;
    return BL_NVM_OK;
}
