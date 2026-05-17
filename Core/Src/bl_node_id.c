/*
 * Resolved bootloader node ID.
 *
 * See `bl_node_id.h` for the why-and-when. This TU is intentionally
 * tiny — the only state is a single byte and the only logic is the
 * NVM read + validation.
 */

#include "bl_node_id.h"

#include "bl_config.h"
#include "bl_nvm.h"
#include "bl_proto.h"  /* BL_PROTO_NODE_BROADCAST */

#include <stdint.h>

/* Cached resolved ID. Initialised to the compile-time default so that
 * a caller reaching `bl_node_id_get()` *before* init still gets a
 * sane value (the FDCAN filter would then accept frames addressed to
 * the compile-time default, which matches pre-override behaviour). */
static uint8_t s_node_id = BL_NODE_ID;

void bl_node_id_init_from_nvm(void)
{
    /* Always reset to the compile-time default first so the call is
     * idempotent — back-to-back inits with different NVM states yield
     * consistent results, and a previously-cached NVM value can't
     * leak forward into a now-empty NVM. */
    s_node_id = BL_NODE_ID;

    uint8_t value = 0U;
    uint8_t value_len = 0U;
    bl_nvm_status_t st = bl_nvm_read(BL_NVM_KEY_NODE_ID,
                                     &value, sizeof(value), &value_len);
    if (st != BL_NVM_OK) {
        /* NOT_FOUND is the normal "no override stored" case; other
         * statuses mean the NVM read itself failed — in both cases
         * the compile-time default is the right answer. */
        return;
    }

    /* Validate the override byte. Anything malformed → silently keep
     * the compile-time default rather than refusing to boot. */
    if (value_len != 1U) {
        return;
    }
    if ((value == 0U) || (value >= BL_PROTO_NODE_BROADCAST)) {
        /* 0x0 is the host's reserved ID, 0xF is broadcast; either as
         * a per-board ID would corrupt the entire bus. */
        return;
    }

    s_node_id = value;
}

uint8_t bl_node_id_get(void)
{
    return s_node_id;
}
