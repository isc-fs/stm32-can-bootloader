#ifndef STM32H7XX_HAL_H
#define STM32H7XX_HAL_H

/*
 * Host-test stub for the STM32H7 HAL. Provides just enough types and
 * function declarations for the production sources we exercise (bl_nvm
 * principally) to compile on a developer machine.
 *
 * The actual HAL_FLASH_* implementations live in mocks/hal_stubs.c
 * and target the g_fake_flash[] buffer redirected by mocks/bl_memmap.h.
 * They enforce the real STM32 1→0-only programming rule and sector-
 * erase semantics, so tests see the same failure modes the chip does.
 */

#include <stdint.h>

/* ---- Status type ---- */
typedef enum {
    HAL_OK      = 0x00,
    HAL_ERROR   = 0x01,
    HAL_BUSY    = 0x02,
    HAL_TIMEOUT = 0x03,
} HAL_StatusTypeDef;

/* ---- Flash programming + erase types (subset) ---- */
#define FLASH_TYPEPROGRAM_FLASHWORD     0x03U   /* 32-byte word write */

#define FLASH_TYPEERASE_SECTORS         0x00U
#define FLASH_BANK_1                    0x01U

typedef struct {
    uint32_t TypeErase;
    uint32_t Banks;
    uint32_t Sector;
    uint32_t NbSectors;
    uint32_t VoltageRange;
} FLASH_EraseInitTypeDef;

/* ---- Public HAL flash surface used by the bootloader ----
 *
 * Note: the real ST HAL declares FlashAddress and DataAddress as
 * `uint32_t`. We widen them to `uintptr_t` here so the host build
 * (where pointers are 64-bit) can pass real pointers in without the
 * compiler complaining about narrowing-cast. On the STM32 the two
 * types are the same width so no production code needs to change
 * behaviourally — the production call sites use `(uintptr_t)ptr`
 * which is a no-op cast on Cortex-M. */
HAL_StatusTypeDef HAL_FLASH_Unlock(void);
HAL_StatusTypeDef HAL_FLASH_Lock(void);
HAL_StatusTypeDef HAL_FLASH_Program(uint32_t TypeProgram, uintptr_t FlashAddress, uintptr_t DataAddress);
HAL_StatusTypeDef HAL_FLASHEx_Erase(FLASH_EraseInitTypeDef *pEraseInit, uint32_t *PageError);

uint32_t HAL_GetTick(void);
void     HAL_Delay(uint32_t ms);

/* ---- Backup-domain stubs ----
 *
 * bl_log + bl_dtc on the chip unlock BKPSRAM access and enable its
 * clock as part of their init. On host they're no-ops. The macro form
 * mirrors the CMSIS naming (`__HAL_RCC_*_CLK_ENABLE`) so the production
 * code compiles unchanged. */
void HAL_PWR_EnableBkUpAccess(void);
#define __HAL_RCC_BKPRAM_CLK_ENABLE()    ((void)0)
#define __HAL_RCC_RTC_ENABLE()           ((void)0)

/* ---- NVIC + RCC + RTC stubs ----
 *
 * bl_proto's handle_reset and handle_jump touch a handful of CMSIS
 * /core_cm7 inlines and ST register-block typedefs. None of these
 * paths get exercised by the current dispatcher tests (they end in
 * NVIC_SystemReset which would actually reset the chip), but the
 * linker still needs the symbols. Provide enough surface to compile,
 * with no-op semantics — a test that DOES exercise these paths will
 * upgrade these to proper fakes when it lands. */
void NVIC_SystemReset(void);

typedef struct {
    volatile uint32_t BDCR;
} RCC_TypeDef;
extern RCC_TypeDef *const RCC;
#define RCC_BDCR_RTCEN              0x00008000U

typedef struct {
    volatile uint32_t BKP0R;
} RTC_TypeDef;
extern RTC_TypeDef *const RTC;

/* ---- Option-byte launcher stub ----
 * bl_obyte's apply-WRP path on the chip calls HAL_FLASH_OB_Launch
 * which never returns (it triggers a system reset). On host it just
 * returns OK; the dispatcher's post-launch dead-code is unreachable
 * in real life. */
HAL_StatusTypeDef HAL_FLASH_OB_Launch(void);

/* ---- FDCAN surface ----
 *
 * The real ST HAL's FDCAN headers are large and full of register-level
 * detail bl_proto doesn't touch. We mirror only the types and constants
 * bl_proto.c actually uses, with field names matching the production
 * header so production code compiles unchanged. */
typedef struct {
    int _opaque;
} FDCAN_HandleTypeDef;

typedef struct {
    uint32_t Identifier;
    uint32_t IdType;
    uint32_t TxFrameType;
    uint32_t DataLength;
    uint32_t ErrorStateIndicator;
    uint32_t BitRateSwitch;
    uint32_t FDFormat;
    uint32_t TxEventFifoControl;
    uint32_t MessageMarker;
} FDCAN_TxHeaderTypeDef;

/* Flags / mode constants used by bl_proto. Values are arbitrary —
 * the production HAL gives them specific bit patterns but bl_proto
 * just assigns them as opaque tokens, so any unique uint32_t works. */
#define FDCAN_STANDARD_ID           0x00000000U
#define FDCAN_DATA_FRAME            0x00000000U
#define FDCAN_ESI_ACTIVE            0x00000000U
#define FDCAN_BRS_OFF               0x00000000U
#define FDCAN_CLASSIC_CAN           0x00000000U
#define FDCAN_NO_TX_EVENTS          0x00000000U

/* The real HAL encodes 0..8 byte counts in a non-linear table; bl_proto
 * computes the right token from dlc_bytes_to_fdcan() and we just need
 * to round-trip distinct values. Match the production HAL's actual
 * encoding so the dlc table assertion lines up if any test wants to
 * verify it. */
#define FDCAN_DLC_BYTES_0           (0U << 16)
#define FDCAN_DLC_BYTES_1           (1U << 16)
#define FDCAN_DLC_BYTES_2           (2U << 16)
#define FDCAN_DLC_BYTES_3           (3U << 16)
#define FDCAN_DLC_BYTES_4           (4U << 16)
#define FDCAN_DLC_BYTES_5           (5U << 16)
#define FDCAN_DLC_BYTES_6           (6U << 16)
#define FDCAN_DLC_BYTES_7           (7U << 16)
#define FDCAN_DLC_BYTES_8           (8U << 16)

/* #120: the production FDCAN handles, hfdcan1/2/3, are DEFINED in
 * hal_stubs.c (on-chip: CubeMX main.c). They're deliberately NOT
 * extern-declared here — bl_fdcan.c carries its own extern (it must, since
 * on-chip there's no header for them), and declaring them here too would be
 * a redundant declaration in that TU (clang-tidy readability-redundant-
 * declaration). test_bl_fdcan.c externs them locally to assert handle
 * identity. */

/* RX-filter surface used by bl_fdcan_configure_filters. The host stubs
 * (hal_stubs.c) just return HAL_OK — tests exercise the instance
 * resolution, not the filter geometry; this only has to compile + link. */
typedef struct {
    uint32_t IdType;
    uint32_t FilterIndex;
    uint32_t FilterType;
    uint32_t FilterConfig;
    uint32_t FilterID1;
    uint32_t FilterID2;
} FDCAN_FilterTypeDef;

#define FDCAN_FILTER_MASK           0x00000001U
#define FDCAN_FILTER_TO_RXFIFO0     0x00000001U
#define FDCAN_REJECT                0x00000040U
#define FDCAN_REJECT_REMOTE         0x00000002U

HAL_StatusTypeDef HAL_FDCAN_ConfigFilter(FDCAN_HandleTypeDef *h,
                                         FDCAN_FilterTypeDef *f);
HAL_StatusTypeDef HAL_FDCAN_ConfigGlobalFilter(FDCAN_HandleTypeDef *h,
                                               uint32_t nonmatch_std,
                                               uint32_t nonmatch_ext,
                                               uint32_t reject_remote_std,
                                               uint32_t reject_remote_ext);

/* #120: the BL starts all three buses via bl_fdcan_start_all(). */
HAL_StatusTypeDef HAL_FDCAN_Start(FDCAN_HandleTypeDef *h);

HAL_StatusTypeDef HAL_FDCAN_AddMessageToTxFifoQ(FDCAN_HandleTypeDef *h,
                                                FDCAN_TxHeaderTypeDef *hdr,
                                                uint8_t *data);

/* RX FIFO selectors. The chip-side HAL exposes FDCAN_RX_FIFO0 /
 * FDCAN_RX_FIFO1 as opaque tokens — values don't matter for the host
 * tests, just that they exist as distinct identifiers. */
#define FDCAN_RX_FIFO0                  0x00000000U
#define FDCAN_RX_FIFO1                  0x00000080U

/* Returns the number of messages currently waiting in the given RX
 * FIFO. The host mock returns 0 by default — the dispatcher
 * instrumentation in bl_proto's log_isotp_error reads this to encode
 * "FIFO fill at error" into the DTC context. Tests that want to
 * simulate a near-full FIFO at error time can install a smarter
 * fake. */
uint32_t HAL_FDCAN_GetRxFifoFillLevel(FDCAN_HandleTypeDef *h, uint32_t fifo);

/* Returns the number of empty slots in the TX FIFO/queue. The mock
 * always reports "fully drained" (= queue depth from main.c) so
 * bl_proto's wait_tx_drain loop exits in zero iterations during host
 * tests. Dispatcher tests that want to exercise the drain-timeout
 * behaviour can install a smarter fake on top of this. */
uint32_t HAL_FDCAN_GetTxFifoFreeLevel(FDCAN_HandleTypeDef *h);

/* ---- FDCAN TX capture (test-only) ----
 *
 * Every successful HAL_FDCAN_AddMessageToTxFifoQ call appends a record
 * to a ring sized for typical test cases. Tests inspect the captured
 * frames to assert "what did the BL emit in response to X?". */
#define MOCK_FDCAN_CAPTURE_DEPTH    16U

typedef struct {
    uint32_t identifier;
    uint8_t  dlc_bytes;
    uint8_t  data[8];
} mock_fdcan_frame_t;

void  mock_fdcan_reset(void);
int   mock_fdcan_tx_count(void);
const mock_fdcan_frame_t *mock_fdcan_get(int index);  /* 0..count-1, or NULL */

/* #120 per-bus call counters (bus 0/1/2 = hfdcan1/2/3) — tests assert
 * configure_filters + start_all cover EVERY bus; the fail knobs drive the
 * error-propagation paths. Counters reset in mock_fdcan_reset(). */
int  mock_fdcan_cfgfilter_count(int bus);
int  mock_fdcan_globalfilter_count(int bus);
int  mock_fdcan_start_count(int bus);
void mock_fdcan_set_configfilter_fail(int n);
void mock_fdcan_set_start_fail(int n);

/* #147: override the TX-FIFO free level (slots). Default 16 (idle); set 0
 * to simulate a full queue so bl_log_tick's emission gate is exercised. */
void mock_fdcan_set_tx_free(uint32_t free_slots);

/* #154: acceptance mask of the last ConfigFilter call (exact-match check). */
uint32_t mock_fdcan_last_filter_mask(void);

/* ---- Test-only helpers (defined alongside the stubs) ---- */
void mock_flash_reset(void);            /* fill the 1-MB buffer with 0xFF */
void mock_flash_set_program_fail(int n);/* next N program calls return HAL_ERROR */
void mock_flash_set_erase_fail(int n);  /* next N erase calls return HAL_ERROR */
int  mock_flash_program_call_count(void);
int  mock_flash_erase_call_count(void);
void mock_set_tick(uint32_t t);
void mock_advance_tick(uint32_t dt);

/* Bootloader jump observability (bl_peer_stubs.c) — for #125 C2 tests. */
int  mock_bootloader_jump_count(void);
void mock_bootloader_reset(void);
void mock_set_check_application(uint8_t rv);
int  mock_appcheck_invalidate_count(void);   /* #146 H2 */

/* OB_APPLY_WRP observability (bl_peer_stubs.c) — for #125 C4 tests. */
uint32_t mock_ob_apply_wrp_last_mask(void);
int      mock_ob_apply_wrp_calls(void);
void     mock_ob_apply_wrp_reset(void);

/* Flash-op-duration probe observability (bl_peer_stubs.c) — #125 H6. */
int  mock_flash_op_ms_calls(void);
void mock_flash_op_ms_reset(void);

/* IWDG kick observability (bl_peer_stubs.c) — #125 H6. */
int  mock_iwdg_refresh_calls(void);
void mock_iwdg_refresh_reset(void);

#endif /* STM32H7XX_HAL_H */
