/*
 * bl_peer_stubs.c — host-side linker stubs for bl_proto's peer modules.
 *
 * bl_proto.c calls into bl_dtc / bl_flash / bl_health / bl_live /
 * bl_obyte and the two Bootloader_* entry points in main.h. None of
 * those modules are part of the host test build yet, so this file
 * provides no-op stubs purely to satisfy the linker.
 *
 * These are NOT functional mocks — there are no capture buffers or
 * tunable knobs. Dispatcher tests that need observable behaviour will
 * replace the relevant stub with a proper mock when they land. The
 * neutral return values here ("succeeded", "writable", zero bytes
 * serialised) are picked so the happy-path dispatch flow runs without
 * tripping a NACK in the production code.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "bl_dtc.h"
#include "bl_flash.h"
#include "bl_health.h"
#include "bl_live.h"
#include "bl_obyte.h"
#include "main.h"


/* ---- bl_dtc ---- */
void bl_dtc_clear(void) { }

void bl_dtc_log(uint16_t code, uint8_t severity, uint32_t context)
{
    (void)code; (void)severity; (void)context;
}

void bl_dtc_serialize(uint8_t *buf, uint16_t *out_len)
{
    (void)buf;
    if (out_len != NULL) {
        *out_len = 0U;
    }
}


/* ---- bl_flash ---- */
bool bl_flash_range_is_writable(uint32_t start, uint32_t length)
{
    (void)start; (void)length;
    return true;
}

bl_flash_status_t bl_flash_erase(uint32_t start,
                                 uint32_t length,
                                 uint32_t *sectors_erased)
{
    (void)start; (void)length;
    if (sectors_erased != NULL) {
        *sectors_erased = 0U;
    }
    return BL_FLASH_OK;
}

bl_flash_status_t bl_flash_write(uint32_t addr,
                                 const uint8_t *data,
                                 uint32_t length)
{
    (void)addr; (void)data; (void)length;
    return BL_FLASH_OK;
}

bl_flash_status_t bl_flash_write_metadata(uint32_t size,
                                          uint32_t crc,
                                          uint32_t version)
{
    (void)size; (void)crc; (void)version;
    return BL_FLASH_OK;
}

uint32_t bl_flash_crc32(uint32_t addr, uint32_t length)
{
    (void)addr; (void)length;
    /* Return a fixed sentinel. Dispatcher tests that exercise CRC
     * paths will install a proper fake; everywhere else this just
     * keeps the linker happy. */
    return 0xDEADBEEFU;
}


/* ---- bl_health ---- */
void bl_health_fill_record(bl_health_record_t *out)
{
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
}


/* ---- bl_live ---- */
void bl_live_frame_rx(void) { }
void bl_live_frame_tx(void) { }
void bl_live_nack_sent(void) { }
void bl_live_note_opcode(uint8_t opcode)        { (void)opcode; }
void bl_live_note_flash_addr(uint32_t addr)     { (void)addr; }

bool bl_live_stream_start(uint8_t rate_hz)
{
    (void)rate_hz;
    return true;
}

void bl_live_stream_stop(void) { }


/* ---- bl_obyte ---- */
/* Observable for #125 C4 tests: record the last mask actually passed
 * to apply (i.e. that survived the handler's validation) + a call
 * count, so a test can assert a rejected mask never reached the
 * option-byte layer. */
static uint32_t g_apply_wrp_last_mask = 0xFFFFFFFFU;
static int      g_apply_wrp_calls     = 0;

bl_ob_rc_t bl_obyte_apply_wrp(uint32_t sector_bitmap)
{
    g_apply_wrp_last_mask = sector_bitmap;
    g_apply_wrp_calls++;
    return BL_OB_OK;
}

uint32_t mock_ob_apply_wrp_last_mask(void) { return g_apply_wrp_last_mask; }
int      mock_ob_apply_wrp_calls(void)     { return g_apply_wrp_calls; }
void     mock_ob_apply_wrp_reset(void)     { g_apply_wrp_last_mask = 0xFFFFFFFFU;
                                             g_apply_wrp_calls = 0; }

bl_ob_rc_t bl_obyte_read(bl_ob_status_t *out)
{
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    return BL_OB_OK;
}


/* ---- Bootloader_* entry points (declared in main.h) ----
 *
 * Jump is observable so tests can assert whether the BL handed off to
 * the app (e.g. the #125 C2 session-timeout-no-jump guard). On real
 * hardware Bootloader_JumpToApplication never returns; in the host
 * harness it just records the call and returns so the test can
 * continue. CheckApplication returns "valid app present" (0) by
 * default; mock_set_check_application() overrides it for tests that
 * need the no-valid-app branch. */
static int     g_jump_count   = 0;
static uint8_t g_check_app_rv = 0U;

void Bootloader_JumpToApplication(void) { g_jump_count++; }
uint8_t Bootloader_CheckApplication(void)  { return g_check_app_rv; }

int  mock_bootloader_jump_count(void)        { return g_jump_count; }
void mock_bootloader_reset(void)             { g_jump_count = 0; g_check_app_rv = 0U; }
void mock_set_check_application(uint8_t rv)  { g_check_app_rv = rv; }
