#ifndef BL_NODE_ID_H
#define BL_NODE_ID_H

/*
 * Resolved bootloader node ID.
 *
 * The bootloader's identity on the CAN bus has two possible sources:
 *
 *   1. Compile-time default:   `BL_NODE_ID` from `bl_config.h`, baked
 *      in by the build (e.g. `-DBL_NODE_ID=0x2`).
 *
 *   2. NVM-backed override:    a 1-byte entry under
 *      `BL_NVM_KEY_NODE_ID` (key = 0x0001) in sector 7. When present
 *      and well-formed, this overrides the compile-time default so
 *      one firmware image can be provisioned to many boards from the
 *      host side (no rebuild + reflash needed).
 *
 * Resolution happens once at boot, in `bl_node_id_init_from_nvm()`,
 * after `bl_nvm_init()` and before any code reads the node ID. The
 * cached result is exposed by `bl_node_id_get()`, which is safe to
 * call from any context (including RX IRQ — the read is a
 * single-byte load).
 *
 * Validation rules for the NVM-stored byte (anything else → fall
 * back silently to the compile-time default):
 *
 *   - value length must be exactly 1 byte
 *   - byte must be in the unicast range 0x1..0xE
 *   - 0x0 is reserved for the host side; 0xF is the broadcast
 *     pseudo-node — neither is a legal per-board ID
 *
 * Notes:
 *
 *   - The NVM value takes effect on the *next* reboot after a
 *     successful `NVM_WRITE`. Hot-swapping a node's identity
 *     mid-session would silently desynchronise the host's filter
 *     state, which is far worse than the operator having to
 *     reboot once.
 *
 *   - If the NVM entry is corrupted (wrong length, out-of-range
 *     byte) we silently fall back rather than refusing to boot.
 *     The compile-time default is always a known-good value (the
 *     `bl_config.h` build asserts that), and a corrupted NVM
 *     override should never make a board unrecoverable.
 */

#include <stdint.h>

/* Re-read the NVM-backed override and cache the result.
 *
 * Must be called once during boot, after `bl_nvm_init()` and before
 * the FDCAN filter setup / first call to `bl_node_id_get()`.
 *
 * Idempotent — calling it again rescans NVM and replaces the
 * cached value. Tests rely on this for back-to-back scenarios
 * without exposing test-only helpers from this module. */
void bl_node_id_init_from_nvm(void);

/* Return the resolved node ID — NVM value if a valid override is
 * stored, compile-time `BL_NODE_ID` otherwise. Always returns a byte
 * in the unicast range 0x1..0xE (the build-time `#error` in
 * `bl_config.h` guarantees the fallback fits).
 *
 * Safe to call from IRQ context — reads a single static byte. */
uint8_t bl_node_id_get(void);

#endif /* BL_NODE_ID_H */
