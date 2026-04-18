#ifndef BL_CONFIG_H
#define BL_CONFIG_H

/*
 * Per-board bootloader configuration.
 *
 * These knobs are set at build time (typically via `-DBL_NODE_ID=0x3`
 * from the toolchain) and describe a specific board's identity on the
 * CAN bus. Phase 4 will add an NVM-backed override for BL_NODE_ID so
 * the same firmware image can be provisioned to different nodes, but
 * for now the ID is baked in at compile time.
 */

#include <stdint.h>

/* 4-bit destination address carried in bits 3:0 of every CAN frame ID.
 *
 * Values 0x0..0xE are valid node IDs. 0xF is reserved for broadcast and
 * must not be used as a per-board ID. 0x0 is reserved for the host side
 * so bootloaders on the bus should pick 0x1..0xE.
 *
 * Default is 0x1 purely so a stock build is addressable out of the box;
 * real deployments override this at compile time. */
#ifndef BL_NODE_ID
#define BL_NODE_ID  0x1U
#endif

#if (BL_NODE_ID > 0xEU)
#error "BL_NODE_ID must be in 0x0..0xE. 0xF is reserved for broadcast."
#endif

#endif /* BL_CONFIG_H */
