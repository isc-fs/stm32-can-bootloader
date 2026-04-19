/*
 * bl_fwinfo.c — lookup and validation of the application firmware-info
 * record. See Core/Inc/bl_fwinfo.h for the on-flash layout and the
 * app-side placement convention.
 */

#include "bl_fwinfo.h"

bool bl_fwinfo_is_present(void)
{
    const bl_fwinfo_t *info = (const bl_fwinfo_t *)BL_FWINFO_ADDR;

    if (info->magic != BL_FWINFO_MAGIC) {
        return false;
    }
    /* Accept any record whose major version is >= 1 — future minor
     * revisions keep the first-64-bytes layout compatible. */
    uint32_t major = (info->record_version >> 16) & 0xFFFFU;
    if (major < 1U) {
        return false;
    }
    return true;
}

const bl_fwinfo_t *bl_fwinfo_get(void)
{
    if (!bl_fwinfo_is_present()) {
        return (const bl_fwinfo_t *)0;
    }
    return (const bl_fwinfo_t *)BL_FWINFO_ADDR;
}
