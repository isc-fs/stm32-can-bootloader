#ifndef BL_APP_VALIDATE_H
#define BL_APP_VALIDATE_H

/*
 * bl_app_validate — predicates used to gate the BL→APP jump.
 *
 * Single source of truth for "is this MSP value legal on the
 * STM32H733 / H7-family chip we ship to". Pulled out of main.c
 * because the BL had two slightly different copies of the same
 * check, one in Bootloader_CheckApplication and another in
 * Bootloader_JumpToApplication — and they disagreed on the legal
 * RAM_D1 range (the bitmask form silently accepted any address up
 * to 0x240FFFFF, while the range form correctly stopped at the
 * 320 KB end of the real region). An app whose vector table put
 * MSP at e.g. 0x24080000 was therefore "approved" by Check and
 * silently rejected by Jump, with no diagnostic surface. Issue
 * #66.
 *
 * Keeping the predicate inline + header-only:
 *   - production main.c uses it directly, no extra link surface
 *   - host unit tests include this header to exercise boundaries
 *     and lock the chip-side rules in place
 *
 * The DTCM and RAM_D1 base/size constants below are STM32H733
 * memory-map facts; they should not be overridden by the host
 * test mock (the predicate is a chip-side rule, the test verifies
 * the rule as the chip sees it).
 */

#include <stdbool.h>
#include <stdint.h>

/* ---- STM32H733ZG SRAM regions used as legal app stacks ---- */
#define BL_RAM_DTCM_BASE    0x20000000U
#define BL_RAM_DTCM_SIZE    (128U * 1024U)

#define BL_RAM_D1_BASE      0x24000000U
#define BL_RAM_D1_SIZE      (320U * 1024U)

/*
 * Returns true iff `sp` (the application's initial MSP, read from
 * the first word of its vector table) falls inside one of the two
 * SRAM regions the BL allows as a stack.
 *
 * ARM convention: the initial MSP points one byte past the top of
 * stack — so the upper bound is inclusive at BASE + SIZE. A region
 * of size 128 KB starting at 0x20000000 therefore accepts MSP values
 * 0x20000000 .. 0x20020000 inclusive.
 */
static inline bool bl_app_stack_in_legal_range(uint32_t sp)
{
    if (sp >= BL_RAM_DTCM_BASE
        && sp <= (BL_RAM_DTCM_BASE + BL_RAM_DTCM_SIZE)) {
        return true;
    }
    if (sp >= BL_RAM_D1_BASE
        && sp <= (BL_RAM_D1_BASE + BL_RAM_D1_SIZE)) {
        return true;
    }
    return false;
}

#endif /* BL_APP_VALIDATE_H */
