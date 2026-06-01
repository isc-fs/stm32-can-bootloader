/*
 * bl_fdcan.c — FDCAN-instance abstraction.
 *
 * See `bl_fdcan.h` for the why-and-when. Phase A (compile-time only):
 * everything keyed on `BL_FDCAN_INSTANCE`, the compiler folds the
 * `#if` chains to a single instance's path at -O2 / -Os.
 *
 * Phase B will replace the compile-time switches with a cached
 * runtime value driven by `BL_NVM_KEY_FDCAN_INSTANCE`; the public API
 * (`bl_fdcan_get_handle` etc.) stays unchanged so its callers don't
 * have to track the iteration.
 *
 * Pin-map note: Phase A picks one default pin pair per instance.
 * Multiple H7 alt-mappings exist (e.g. FDCAN1 also offers PA11/PA12
 * and PB8/PB9); future carriers that wire CAN to different pins will
 * either need build-flag-gated alt entries or a runtime override
 * driven by NVM. See issue #120 Open Question #2.
 */

#include "bl_fdcan.h"

#include "bl_config.h"
#include "bl_proto.h"          /* BL_PROTO_NODE_MASK / NODE_BROADCAST / ID_VALID_MASK */
#include "main.h"              /* Error_Handler */
#include "stm32h7xx_hal.h"

#include <stdint.h>

/* The single FDCAN handle. Generic name (no `2` suffix) so the
 * compile-time selector can target any instance without renaming the
 * symbol every external caller knows about. The `.Instance` field is
 * the part that actually picks which peripheral. Defined here (not
 * declared in the header) so other TUs can't sneak a direct
 * reference past the `bl_fdcan_get_handle()` accessor. */
FDCAN_HandleTypeDef bl_fdcan_handle;

FDCAN_HandleTypeDef *bl_fdcan_get_handle(void)
{
    return &bl_fdcan_handle;
}

uint8_t bl_fdcan_get_instance_number(void)
{
    return (uint8_t)BL_FDCAN_INSTANCE;
}

/* ---- Peripheral pointer for the resolved instance ---------------- */

#if BL_FDCAN_INSTANCE == 1
#define BL_FDCAN_PERIPH        FDCAN1
#elif BL_FDCAN_INSTANCE == 2
#define BL_FDCAN_PERIPH        FDCAN2
#else
/* The build-time #error in bl_config.h already rejects this case, so
 * we should never reach it; keep a redundant guard for defence. */
#error "BL_FDCAN_INSTANCE must be 1 or 2 — see bl_config.h"
#endif

/* ---- bl_fdcan_mx_init ------------------------------------------- *
 *
 * Body matches the CubeMX-generated `MX_FDCAN2_Init` that used to
 * live in `main.c` — copied verbatim except for the `.Instance`
 * assignment and the global variable name (`bl_fdcan_handle`
 * instead of `hfdcan2`). The audit log for these specific Init
 * fields lives in the original main.c comments and in the v1.2.0
 * CHANGELOG entry — `AutoRetransmission = ENABLE` is the post-#94
 * Bug B setting, do not flip without re-reading that thread. */
void bl_fdcan_mx_init(void)
{
    FDCAN_HandleTypeDef *h = &bl_fdcan_handle;

    h->Instance                  = BL_FDCAN_PERIPH;
    h->Init.FrameFormat          = FDCAN_FRAME_CLASSIC;
    h->Init.Mode                 = FDCAN_MODE_NORMAL;
    /* See main.c history / issue #94 for why AutoRetransmission MUST
     * be ENABLE on this BL. Lost-arbitration silent drops + tx->seq
     * skip downstream = wrecks ISO-TP framing. */
    h->Init.AutoRetransmission   = ENABLE;
    h->Init.TransmitPause        = DISABLE;
    h->Init.ProtocolException    = DISABLE;
    h->Init.NominalPrescaler     = 6;
    h->Init.NominalSyncJumpWidth = 1;
    h->Init.NominalTimeSeg1      = 2;
    h->Init.NominalTimeSeg2      = 5;
    h->Init.DataPrescaler        = 1;
    h->Init.DataSyncJumpWidth    = 1;
    h->Init.DataTimeSeg1         = 1;
    h->Init.DataTimeSeg2         = 1;
    h->Init.MessageRAMOffset     = 0;
    h->Init.StdFiltersNbr        = 2;
    h->Init.ExtFiltersNbr        = 1;
    h->Init.RxFifo0ElmtsNbr      = 16;
    h->Init.RxFifo0ElmtSize      = FDCAN_DATA_BYTES_8;
    h->Init.RxFifo1ElmtsNbr      = 16;
    h->Init.RxFifo1ElmtSize      = FDCAN_DATA_BYTES_8;
    h->Init.RxBuffersNbr         = 0;
    h->Init.RxBufferSize         = FDCAN_DATA_BYTES_8;
    h->Init.TxEventsNbr          = 0;
    h->Init.TxBuffersNbr         = 0;
    h->Init.TxFifoQueueElmtsNbr  = 16;
    h->Init.TxFifoQueueMode      = FDCAN_TX_FIFO_OPERATION;
    h->Init.TxElmtSize           = FDCAN_DATA_BYTES_8;

    if (HAL_FDCAN_Init(h) != HAL_OK) {
        Error_Handler();
    }
}

/* ---- bl_fdcan_msp_init ------------------------------------------ *
 *
 * Body matches what used to be inside `HAL_FDCAN_MspInit` —
 * peripheral clock select, instance-specific clock + GPIO + NVIC
 * setup. The HAL invokes this through its MspInit callback during
 * `HAL_FDCAN_Init`; the dispatch on `hfdcan->Instance` keeps the
 * function shape identical to CubeMX's, just routed through a
 * compile-time selector for the body.
 *
 * Kernel-clock source is shared across all FDCAN instances on H7
 * (the FDCANSEL bits in RCC->D2CCIP1R), so the
 * `HAL_RCCEx_PeriphCLKConfig` call sits outside the per-instance
 * blocks. */
void bl_fdcan_msp_init(FDCAN_HandleTypeDef *hfdcan)
{
    if (hfdcan->Instance != BL_FDCAN_PERIPH) {
        return;  /* HAL only ever calls this with our handle */
    }

    /* Shared FDCAN kernel clock — HSE-derived per the v1.0.0 baseline. */
    RCC_PeriphCLKInitTypeDef pclk = { 0 };
    pclk.PeriphClockSelection = RCC_PERIPHCLK_FDCAN;
    pclk.FdcanClockSelection  = RCC_FDCANCLKSOURCE_HSE;
    if (HAL_RCCEx_PeriphCLKConfig(&pclk) != HAL_OK) {
        Error_Handler();
    }

    /* The FDCAN bus clock enable is shared too — one bit covers all
     * instances. Subsequent inits (if any) become no-ops. */
    __HAL_RCC_FDCAN_CLK_ENABLE();

    GPIO_InitTypeDef io = { 0 };
    io.Mode      = GPIO_MODE_AF_PP;
    io.Pull      = GPIO_NOPULL;
    io.Speed     = GPIO_SPEED_FREQ_LOW;

#if BL_FDCAN_INSTANCE == 1
    /* FDCAN1: PD0 (RX), PD1 (TX), AF9.
     *
     * TODO(#120 Open Question #2): confirm against carrier
     * schematic. H733 also offers FDCAN1 on PA11/PA12 (often used
     * for USB) and PB8/PB9. Picking PD0/PD1 here as the default
     * because it doesn't clash with USB on the common ST eval-board
     * pinouts; carrier-specific overrides land in Phase B if real
     * boards wire differently. */
    __HAL_RCC_GPIOD_CLK_ENABLE();
    io.Pin       = GPIO_PIN_0 | GPIO_PIN_1;
    io.Alternate = GPIO_AF9_FDCAN1;
    HAL_GPIO_Init(GPIOD, &io);

    HAL_NVIC_SetPriority(FDCAN1_IT0_IRQn, 2, 0);
    HAL_NVIC_EnableIRQ(FDCAN1_IT0_IRQn);
#elif BL_FDCAN_INSTANCE == 2
    /* FDCAN2: PB12 (RX), PB13 (TX), AF9 — unchanged from v1.3.x. */
    __HAL_RCC_GPIOB_CLK_ENABLE();
    io.Pin       = GPIO_PIN_12 | GPIO_PIN_13;
    io.Alternate = GPIO_AF9_FDCAN2;
    HAL_GPIO_Init(GPIOB, &io);

    HAL_NVIC_SetPriority(FDCAN2_IT0_IRQn, 2, 0);
    HAL_NVIC_EnableIRQ(FDCAN2_IT0_IRQn);
#endif
}

void bl_fdcan_msp_deinit(FDCAN_HandleTypeDef *hfdcan)
{
    if (hfdcan->Instance != BL_FDCAN_PERIPH) {
        return;
    }

    /* Bus clock disable is shared; only the last user should call it.
     * Phase A only inits one instance so this is unambiguous. */
    __HAL_RCC_FDCAN_CLK_DISABLE();

#if BL_FDCAN_INSTANCE == 1
    HAL_GPIO_DeInit(GPIOD, GPIO_PIN_0 | GPIO_PIN_1);
    HAL_NVIC_DisableIRQ(FDCAN1_IT0_IRQn);
#elif BL_FDCAN_INSTANCE == 2
    HAL_GPIO_DeInit(GPIOB, GPIO_PIN_12 | GPIO_PIN_13);
    HAL_NVIC_DisableIRQ(FDCAN2_IT0_IRQn);
#endif
}

/* ---- bl_fdcan_configure_filters --------------------------------- *
 *
 * Body matches the old `main.c::Bootloader_ConfigFdcanFilters`. The
 * filter geometry doesn't depend on the FDCAN instance — same
 * 5-low-bit mask, same two filter entries (unicast + broadcast),
 * same reject-everything-else global filter. The only piece that
 * varies is the handle it operates on, and that's the whole point of
 * routing through `bl_fdcan_get_handle()`. */
HAL_StatusTypeDef bl_fdcan_configure_filters(uint8_t node_id)
{
    FDCAN_HandleTypeDef *h = bl_fdcan_get_handle();

    FDCAN_FilterTypeDef filter = { 0 };
    filter.IdType       = FDCAN_STANDARD_ID;
    filter.FilterType   = FDCAN_FILTER_MASK;
    filter.FilterConfig = FDCAN_FILTER_TO_RXFIFO0;
    filter.FilterID2    = BL_PROTO_ID_VALID_MASK;

    /* Unicast: host → this node. */
    filter.FilterIndex = 0U;
    filter.FilterID1   = (uint32_t)node_id & BL_PROTO_NODE_MASK;
    HAL_StatusTypeDef st = HAL_FDCAN_ConfigFilter(h, &filter);
    if (st != HAL_OK) {
        return st;
    }

    /* Broadcast: host → 0xF. */
    filter.FilterIndex = 1U;
    filter.FilterID1   = BL_PROTO_NODE_BROADCAST;
    st = HAL_FDCAN_ConfigFilter(h, &filter);
    if (st != HAL_OK) {
        return st;
    }

    /* Reject everything that doesn't match one of the two filters; reject
     * remote frames outright — the protocol has no use for them. */
    return HAL_FDCAN_ConfigGlobalFilter(h,
                                        FDCAN_REJECT,
                                        FDCAN_REJECT,
                                        FDCAN_REJECT_REMOTE,
                                        FDCAN_REJECT_REMOTE);
}
