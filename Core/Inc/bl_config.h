#ifndef BL_CONFIG_H
#define BL_CONFIG_H

/*
 * Per-board bootloader configuration.
 *
 * These knobs are set at build time (typically via `-DBL_NODE_ID=0x3`
 * from the toolchain) and describe a specific board's identity on the
 * CAN bus. The compile-time `BL_NODE_ID` is the *fallback*; the
 * effective ID at runtime is resolved by `bl_node_id_get()`
 * (see `bl_node_id.h`), which prefers a 1-byte override stored under
 * `BL_NVM_KEY_NODE_ID` in sector 7 when one is present and valid.
 * That lets one firmware image be provisioned to many boards from
 * the host side without rebuild + reflash. Callers that need the
 * effective ID must use `bl_node_id_get()`, NOT this macro.
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

/* ---- Independent watchdog (IWDG1) — issue #125 H6 ---------------------
 *
 * BL_IWDG_ENABLE: master build switch for the hardware watchdog. Default
 * on. Build with -DBL_IWDG_ENABLE=0 to compile it out (bl_iwdg_start /
 * bl_iwdg_refresh become no-ops) — an escape hatch for bring-up or for
 * bisecting a suspected period regression. Production builds keep it on.
 */
#ifndef BL_IWDG_ENABLE
#define BL_IWDG_ENABLE  1
#endif

/* BL_IWDG_RELOAD: IWDG down-counter reload value, paired with the fixed
 * /256 prescaler in bl_iwdg.c.
 *
 *   period = BL_IWDG_RELOAD * 256 / f_LSI
 *
 * At the nominal LSI (32 kHz) the tick is 8 ms, so 1000 -> ~8.0 s.
 *
 * Sizing rationale (the period must always exceed ONE 128 KB sector erase,
 * which stalls the CPU un-refreshably on the single-bank H733):
 *   - measured single-sector erase ............ 1768 ms (bench)
 *   - H743-family datasheet sector-erase max .. ~2.2 s
 *   - worst-case design target ................ ~3 s (max + aging + slow silicon)
 *
 *   nominal LSI (32 kHz):  8.0 s  -> 4.5x the bench erase, 2.7x the 3 s target
 *   fast LSI corner (~46 kHz, deliberately pessimistic): ~5.5 s
 *                          -> 3.1x bench, 1.8x the 3 s target
 *   slow LSI corner (~17 kHz): ~15 s -> worst-case self-recovery on a genuine hang
 *
 * Tunable from the bench: the BL reports the per-boot max erase duration
 * (bl_health_max_flash_op_ms); keep this reload comfortably above the
 * fleet-observed worst case. RLR is 12-bit, so the max is 4095 (~32 s).
 */
#ifndef BL_IWDG_RELOAD
#define BL_IWDG_RELOAD  1000U
#endif

#if (BL_IWDG_RELOAD > 0xFFFU)
#error "BL_IWDG_RELOAD must fit the 12-bit IWDG reload register (<= 4095)."
#endif

/* #120: the BL serves all three FDCAN peripherals (FDCAN1/2/3) at once, so
 * a host on whichever bus the board wires CAN to is heard — there is no
 * instance to select or configure here. The pin map (set in
 * stm32h7xx_hal_msp.c) is FDCAN1 = PD0/PD1, FDCAN2 = PB12/PB13,
 * FDCAN3 = PG10/PG9; the .ioc keeps all three configured identically
 * (notably AutoRetransmission = ENABLE, the issue #94 fix). The bus front-
 * end lives in bl_fdcan.c. */

#endif /* BL_CONFIG_H */
