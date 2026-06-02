/* USER CODE BEGIN Header */
/**
 ******************************************************************************
 * @file           : main.c
 * @brief          : Main program body
 ******************************************************************************
 * @attention
 *
 * Copyright (c) 2025 STMicroelectronics.
 * All rights reserved.
 *
 * This software is licensed under terms that can be found in the LICENSE file
 * in the root directory of this software component.
 * If no LICENSE file comes with this software, it is provided AS-IS.
 *
 ******************************************************************************
 */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

#include "stm32h7xx_hal.h"
#include "stm32h7xx_hal_rcc_ex.h"
#include "stm32h7xx_hal_pwr_ex.h"

#include "bl_app_validate.h"
#include "bl_memmap.h"
#include "bl_config.h"
#include "bl_dtc.h"
#include "bl_fault.h"
#include "bl_fdcan.h"
#include "bl_health.h"
#include "bl_iwdg.h"
#include "bl_live.h"
#include "bl_log.h"
#include "bl_node_id.h"
#include "bl_nvm.h"
#include "bl_obyte.h"
#include "bl_proto.h"

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
typedef void (*pFunction)(void);
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
/* Flash layout / magic values come from bl_memmap.h.
 * Protocol constants and opcodes come from bl_proto.h. */

#define LED_OK_ON()         HAL_GPIO_WritePin(OK_STATUS_GPIO_Port, OK_STATUS_Pin, GPIO_PIN_SET)
#define LED_OK_OFF()        HAL_GPIO_WritePin(OK_STATUS_GPIO_Port, OK_STATUS_Pin, GPIO_PIN_RESET)
#define LED_OK_TOGGLE()		HAL_GPIO_TogglePin(OK_STATUS_GPIO_Port, OK_STATUS_Pin)

#define LED_ERR_ON()         HAL_GPIO_WritePin(ERR_STATUS_GPIO_Port, ERR_STATUS_Pin, GPIO_PIN_SET)
#define LED_ERR_OFF()        HAL_GPIO_WritePin(ERR_STATUS_GPIO_Port, ERR_STATUS_Pin, GPIO_PIN_RESET)
#define LED_ERR_TOGGLE()	 HAL_GPIO_TogglePin(ERR_STATUS_GPIO_Port, ERR_STATUS_Pin)

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

FDCAN_HandleTypeDef hfdcan1;
FDCAN_HandleTypeDef hfdcan2;
FDCAN_HandleTypeDef hfdcan3;

/* USER CODE BEGIN PV */
static uint8_t  g_AutoJumpEnabled  = 0U;
static uint32_t g_AutoJumpDeadline = 0U;

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_FDCAN1_Init(void);
static void MX_FDCAN2_Init(void);
static void MX_FDCAN3_Init(void);
/* USER CODE BEGIN PFP */
static void Bootloader_Init(void);
static void Bootloader_MainLoop(void);
static void Bootloader_FdcanBusOffRecover(uint32_t now_ms);
/* Bootloader_ConfigFdcanFilters moved to bl_fdcan_configure_filters in
 * bl_fdcan.c (#120 Phase A) — see bl_fdcan.h. */
/* Bootloader_JumpToApplication / Bootloader_CheckApplication are declared
 * with external linkage in main.h so bl_proto.c can call them for
 * CMD_RESET (mode=to-app) and CMD_JUMP. */
static uint32_t Bootloader_CalcCrc32(uint32_t address, uint32_t lengthBytes);
static uint8_t  Bootloader_IsBootRequestActive(void);
static uint8_t  Bootloader_IsBootAppRequestActive(void);   /* #142 */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_FDCAN1_Init();
  MX_FDCAN2_Init();
  MX_FDCAN3_Init();
  /* USER CODE BEGIN 2 */

	Bootloader_Init();
	Bootloader_MainLoop();  /* never returns — either auto-jumps to app
	                           or keeps serving CAN frames forever */

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */

	while (1) {

    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
	}
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Supply configuration update enable
  */
  HAL_PWREx_ConfigSupply(PWR_LDO_SUPPLY);

  /** Configure the main internal regulator output voltage
  */
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE0);

  while(!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)) {}

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 2;
  RCC_OscInitStruct.PLL.PLLN = 44;
  RCC_OscInitStruct.PLL.PLLP = 1;
  RCC_OscInitStruct.PLL.PLLQ = 4;
  RCC_OscInitStruct.PLL.PLLR = 2;
  RCC_OscInitStruct.PLL.PLLRGE = RCC_PLL1VCIRANGE_3;
  RCC_OscInitStruct.PLL.PLLVCOSEL = RCC_PLL1VCOWIDE;
  RCC_OscInitStruct.PLL.PLLFRACN = 0;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2
                              |RCC_CLOCKTYPE_D3PCLK1|RCC_CLOCKTYPE_D1PCLK1;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.SYSCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB3CLKDivider = RCC_APB3_DIV2;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_APB1_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_APB2_DIV2;
  RCC_ClkInitStruct.APB4CLKDivider = RCC_APB4_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_3) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief FDCAN1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_FDCAN1_Init(void)
{

  /* USER CODE BEGIN FDCAN1_Init 0 */

  /* USER CODE END FDCAN1_Init 0 */

  /* USER CODE BEGIN FDCAN1_Init 1 */

  /* USER CODE END FDCAN1_Init 1 */
  hfdcan1.Instance = FDCAN1;
  hfdcan1.Init.FrameFormat = FDCAN_FRAME_CLASSIC;
  hfdcan1.Init.Mode = FDCAN_MODE_NORMAL;
  hfdcan1.Init.AutoRetransmission = ENABLE;   /* #94: MUST be ENABLE — lost-arbitration frames retried, not silently dropped */
  hfdcan1.Init.TransmitPause = DISABLE;
  hfdcan1.Init.ProtocolException = DISABLE;
  hfdcan1.Init.NominalPrescaler = 6;
  hfdcan1.Init.NominalSyncJumpWidth = 1;
  hfdcan1.Init.NominalTimeSeg1 = 1;
  hfdcan1.Init.NominalTimeSeg2 = 2;
  hfdcan1.Init.DataPrescaler = 1;
  hfdcan1.Init.DataSyncJumpWidth = 1;
  hfdcan1.Init.DataTimeSeg1 = 1;
  hfdcan1.Init.DataTimeSeg2 = 1;
  hfdcan1.Init.MessageRAMOffset = 0;
  hfdcan1.Init.StdFiltersNbr = 2;
  hfdcan1.Init.ExtFiltersNbr = 1;
  hfdcan1.Init.RxFifo0ElmtsNbr = 16;
  hfdcan1.Init.RxFifo0ElmtSize = FDCAN_DATA_BYTES_8;
  hfdcan1.Init.RxFifo1ElmtsNbr = 16;
  hfdcan1.Init.RxFifo1ElmtSize = FDCAN_DATA_BYTES_8;
  hfdcan1.Init.RxBuffersNbr = 0;
  hfdcan1.Init.RxBufferSize = FDCAN_DATA_BYTES_8;
  hfdcan1.Init.TxEventsNbr = 0;
  hfdcan1.Init.TxBuffersNbr = 0;
  hfdcan1.Init.TxFifoQueueElmtsNbr = 16;
  hfdcan1.Init.TxFifoQueueMode = FDCAN_TX_FIFO_OPERATION;
  hfdcan1.Init.TxElmtSize = FDCAN_DATA_BYTES_8;
  if (HAL_FDCAN_Init(&hfdcan1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN FDCAN1_Init 2 */

  /* USER CODE END FDCAN1_Init 2 */

}

/**
  * @brief FDCAN2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_FDCAN2_Init(void)
{

  /* USER CODE BEGIN FDCAN2_Init 0 */

  /* USER CODE END FDCAN2_Init 0 */

  /* USER CODE BEGIN FDCAN2_Init 1 */

  /* USER CODE END FDCAN2_Init 1 */
  hfdcan2.Instance = FDCAN2;
  hfdcan2.Init.FrameFormat = FDCAN_FRAME_CLASSIC;
  hfdcan2.Init.Mode = FDCAN_MODE_NORMAL;
  hfdcan2.Init.AutoRetransmission = ENABLE;   /* #94: MUST be ENABLE — lost-arbitration frames retried, not silently dropped */
  hfdcan2.Init.TransmitPause = DISABLE;
  hfdcan2.Init.ProtocolException = DISABLE;
  hfdcan2.Init.NominalPrescaler = 6;
  hfdcan2.Init.NominalSyncJumpWidth = 1;
  hfdcan2.Init.NominalTimeSeg1 = 1;
  hfdcan2.Init.NominalTimeSeg2 = 2;
  hfdcan2.Init.DataPrescaler = 1;
  hfdcan2.Init.DataSyncJumpWidth = 1;
  hfdcan2.Init.DataTimeSeg1 = 1;
  hfdcan2.Init.DataTimeSeg2 = 1;
  hfdcan2.Init.MessageRAMOffset = 0;
  hfdcan2.Init.StdFiltersNbr = 2;
  hfdcan2.Init.ExtFiltersNbr = 1;
  hfdcan2.Init.RxFifo0ElmtsNbr = 16;
  hfdcan2.Init.RxFifo0ElmtSize = FDCAN_DATA_BYTES_8;
  hfdcan2.Init.RxFifo1ElmtsNbr = 16;
  hfdcan2.Init.RxFifo1ElmtSize = FDCAN_DATA_BYTES_8;
  hfdcan2.Init.RxBuffersNbr = 0;
  hfdcan2.Init.RxBufferSize = FDCAN_DATA_BYTES_8;
  hfdcan2.Init.TxEventsNbr = 0;
  hfdcan2.Init.TxBuffersNbr = 0;
  hfdcan2.Init.TxFifoQueueElmtsNbr = 16;
  hfdcan2.Init.TxFifoQueueMode = FDCAN_TX_FIFO_OPERATION;
  hfdcan2.Init.TxElmtSize = FDCAN_DATA_BYTES_8;
  if (HAL_FDCAN_Init(&hfdcan2) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN FDCAN2_Init 2 */

  /* USER CODE END FDCAN2_Init 2 */

}

/**
  * @brief FDCAN3 Initialization Function
  * @param None
  * @retval None
  */
static void MX_FDCAN3_Init(void)
{

  /* USER CODE BEGIN FDCAN3_Init 0 */

  /* USER CODE END FDCAN3_Init 0 */

  /* USER CODE BEGIN FDCAN3_Init 1 */

  /* USER CODE END FDCAN3_Init 1 */
  hfdcan3.Instance = FDCAN3;
  hfdcan3.Init.FrameFormat = FDCAN_FRAME_CLASSIC;
  hfdcan3.Init.Mode = FDCAN_MODE_NORMAL;
  hfdcan3.Init.AutoRetransmission = ENABLE;   /* #94: MUST be ENABLE — lost-arbitration frames retried, not silently dropped */
  hfdcan3.Init.TransmitPause = DISABLE;
  hfdcan3.Init.ProtocolException = DISABLE;
  hfdcan3.Init.NominalPrescaler = 6;
  hfdcan3.Init.NominalSyncJumpWidth = 1;
  hfdcan3.Init.NominalTimeSeg1 = 1;
  hfdcan3.Init.NominalTimeSeg2 = 2;
  hfdcan3.Init.DataPrescaler = 1;
  hfdcan3.Init.DataSyncJumpWidth = 1;
  hfdcan3.Init.DataTimeSeg1 = 1;
  hfdcan3.Init.DataTimeSeg2 = 1;
  hfdcan3.Init.MessageRAMOffset = 0;
  hfdcan3.Init.StdFiltersNbr = 2;
  hfdcan3.Init.ExtFiltersNbr = 1;
  hfdcan3.Init.RxFifo0ElmtsNbr = 16;
  hfdcan3.Init.RxFifo0ElmtSize = FDCAN_DATA_BYTES_8;
  hfdcan3.Init.RxFifo1ElmtsNbr = 16;
  hfdcan3.Init.RxFifo1ElmtSize = FDCAN_DATA_BYTES_8;
  hfdcan3.Init.RxBuffersNbr = 0;
  hfdcan3.Init.RxBufferSize = FDCAN_DATA_BYTES_8;
  hfdcan3.Init.TxEventsNbr = 0;
  hfdcan3.Init.TxBuffersNbr = 0;
  hfdcan3.Init.TxFifoQueueElmtsNbr = 16;
  hfdcan3.Init.TxFifoQueueMode = FDCAN_TX_FIFO_OPERATION;
  hfdcan3.Init.TxElmtSize = FDCAN_DATA_BYTES_8;
  if (HAL_FDCAN_Init(&hfdcan3) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN FDCAN3_Init 2 */

  /* USER CODE END FDCAN3_Init 2 */

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOH_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOD_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOG_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOD, OK_STATUS_Pin|ERR_STATUS_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pins : OK_STATUS_Pin ERR_STATUS_Pin */
  GPIO_InitStruct.Pin = OK_STATUS_Pin|ERR_STATUS_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOD, &GPIO_InitStruct);

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

/* ===================== BOOTLOADER CORE ========================= */

static void Bootloader_Init(void) {
	/* #125 H6: start (or re-period) the IWDG as the very FIRST thing we
	 * do — see bl_iwdg.h. On an app->BL reset (the `002` re-enter-BL
	 * trick) we inherit the app's tight ~100 ms watchdog, still running;
	 * this re-periods it to the BL's ~8 s and reloads the counter within a
	 * few ms of reset, well before the inherited period could fire. On a
	 * cold boot it simply arms the watchdog. Everything below (NVM scan,
	 * filter config) then runs under the BL's own generous period. */
	bl_iwdg_start();

	LED_OK_ON();
	LED_ERR_OFF();

	/* #125 H6: enable the DWT cycle counter, used by bl_flash_erase to
	 * time the erase. HAL_GetTick can't measure it — on the single-bank
	 * H733 the CPU stalls on instruction fetch for the whole erase, so
	 * SysTick interrupts starve and the tick barely advances. CYCCNT is
	 * clock-driven and counts through the stall. Set once, here. */
	CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
	DWT->CYCCNT = 0U;
	DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk;

	/* Latch the reset cause before anything else has a chance to clear
	 * RCC->RSR. Health reporting depends on this surviving the rest of
	 * the boot sequence. */
	bl_health_init();

	/* Bring up the DTC table in Backup SRAM. Runs after bl_health_init
	 * so bl_dtc_log can timestamp entries against uptime. Also validates
	 * the magic so a power-cycled BKPSRAM starts from a clean slate. */
	bl_dtc_init();

	/* Bring up the log ring in Backup SRAM (adjacent to the DTC table).
	 * Same persistence semantics — a host that reconnects after a crash
	 * can replay the last ~1 KB of bootloader log by issuing
	 * LOG_STREAM_START. */
	bl_log_init();
	bl_log_info("bootloader up (reset_cause=%u)",
	            (unsigned int)bl_health_reset_cause());

	/* #125 H6 (reset-on-spin): if the previous boot ended in a
	 * controlled reboot from a terminal fault (a CPU fault handler,
	 * Error_Handler, or a failed FDCAN init), bl_fault left a
	 * reset-surviving breadcrumb. Log it now — in this safe post-reset
	 * context, where bl_dtc_log's NOTIFY path is fine — so the cause is
	 * visible via `diagnose dtc` / health.last_dtc_code. One-shot:
	 * take_pending clears it. */
	uint8_t fault_reason = BL_FAULT_NONE;
	if (bl_fault_take_pending(&fault_reason)) {
		bl_dtc_log(BL_DTC_CPU_FAULT, BL_DTC_SEV_FATAL, (uint32_t)fault_reason);
		bl_log_error("recovered from fault reboot (reason=%u)",
		             (unsigned int)fault_reason);
	}

	/* Zero the live-data counters. The snapshot is pulled together on
	 * demand in bl_live_tick, so no persistent state needs initialising
	 * beyond this. */
	bl_live_init();

	/* Scan the NVM sector to find the append point and the current
	 * highest seq. Safe to call even when sector 7 is fully erased —
	 * in that case g_write_pos lands at 0 and future writes grow the
	 * log from scratch. */
	bl_nvm_init();

	/* Resolve the effective node ID now that NVM is queryable. Reads
	 * BL_NVM_KEY_NODE_ID if present, validates it, and caches the
	 * result for `bl_node_id_get()`. Must happen BEFORE the FDCAN
	 * filter config below — the filter is built from the resolved ID
	 * and is set up only once at boot. */
	bl_node_id_init_from_nvm();

	/* #120: the BL serves all three FDCAN peripherals at once (see
	 * bl_fdcan.c). MX_FDCAN{1,2,3}_Init in main() has already initialised
	 * the handles; the filter config + start_all below bring up every bus,
	 * so a host on whichever bus the board wires CAN to is heard. */

	/* Boot-time WRP self-check. Production-provisioned units are
	 * expected to have sector 0 (the bootloader) WRP-protected; a
	 * missing latch is not fatal but it earns a WARN log line so the
	 * host's log stream flags it on connect. The WRP_PROTECTED flag in
	 * both health and live-data reflects this same check, so a host
	 * can gate OB_APPLY_WRP on the flag. */
	if (!bl_obyte_is_sector_wrp_protected(0U)) {
		bl_log_warn("WRP: bootloader sector 0 not write-protected; run OB_APPLY_WRP to latch");
	}

	/* Filters must be configured while the FDCAN is still in Init mode —
	 * i.e. before HAL_FDCAN_Start. Filter shape is instance-agnostic;
	 * `bl_fdcan_configure_filters` lives in bl_fdcan.c (#120 Phase A)
	 * and accepts the resolved node-id directly so it doesn't have to
	 * include bl_node_id.h itself. */
	if (bl_fdcan_configure_filters(bl_node_id_get()) != HAL_OK) {
		/* #125 H6: a failed FDCAN bring-up used to spin here forever —
		 * the board went dead with no CAN, unflashable. Reboot instead;
		 * a transient init failure self-recovers, and the breadcrumb
		 * surfaces a BL_DTC_CPU_FAULT next boot. */
		LED_ERR_ON();
		bl_fault_reboot(BL_FAULT_FDCAN_INIT);
	}

	if (bl_fdcan_start_all() != HAL_OK) {
		LED_ERR_ON();
		bl_fault_reboot(BL_FAULT_FDCAN_INIT);
	}

	/* #125 M3: guard the issue #94 fix. AutoRetransmission MUST be
	 * ENABLE or lost-arbitration frames are silently dropped while
	 * tx->seq has already advanced — which corrupts multi-frame TX
	 * sequencing on a contended bus (the #94 Bug B signature). That's
	 * set in MX_FDCAN2_Init, but a CubeMX regeneration could flip it
	 * back with no test catching it. Read the LIVE peripheral register
	 * (CCCR.DAR = "Disable Automatic Retransmission") rather than the
	 * RAM Init struct, so this actually reflects hardware state. DAR
	 * set ⇒ retransmission disabled ⇒ regression. Log loudly; the BL
	 * still runs (degraded) so the bus stays reachable to reflash a
	 * corrected build. */
	for (uint8_t b = 0U; b < BL_FDCAN_BUS_COUNT; b++) {
		if ((bl_fdcan_bus(b)->Instance->CCCR & FDCAN_CCCR_DAR) != 0U) {
			bl_log_error("FDCAN%u AutoRetransmission DISABLED (CCCR.DAR set) — "
			             "issue #94 regression; multi-frame TX will desync",
			             (unsigned int)(b + 1U));
		}
	}

	/* NOTE on FDCAN interrupts: the main loop drains RX_FIFO0 by
	 * polling (HAL_FDCAN_GetRxFifoFillLevel + GetRxMessage below), so
	 * we deliberately do NOT activate FDCAN_IT_RX_FIFO0_NEW_MESSAGE.
	 *
	 * The previous code enabled the notification but never overrode
	 * HAL_FDCAN_RxFifo0Callback — so every RX frame entered the IRQ
	 * (FDCAN2_IT0_IRQHandler → HAL_FDCAN_IRQHandler) only to dispatch
	 * the default weak callback and return. Pure overhead, and a
	 * footgun for anyone who later added a real callback expecting
	 * it to actually drive RX while the main loop was still polling
	 * — concurrent drain of the same FIFO would have dropped frames.
	 * Audit follow-up (#68 last bullet). */

	/* #142: honour a one-shot "boot the app" request first. handle_jump /
	 * handle_reset set BL_BOOT_APP_MAGIC + reset when a flash write happened
	 * this session, so a write-then-jump reaches the app through a clean
	 * reset (the flash is idle again, cold-equivalent) rather than a direct
	 * warm jump that can leave the freshly-written app stuck. Jump straight
	 * to the just-written, re-validated app. */
	if (Bootloader_IsBootAppRequestActive()) {
		if (Bootloader_CheckApplication() == 0x00U) {
			Bootloader_JumpToApplication();   /* never returns on success */
		}
		/* App not valid (shouldn't happen right after a verified write) —
		 * fall through to listen mode rather than brick. */
		g_AutoJumpEnabled = 0U;
	} else {
		/* Check if application explicitly requested to stay in bootloader */
		uint8_t bootReq = Bootloader_IsBootRequestActive();

		if (bootReq) {
			/* Stay in bootloader, do not arm auto-jump */
			g_AutoJumpEnabled = 0U;
		} else {
			/* Normal behavior: arm auto-jump if a valid application is present */
			uint8_t appStatus = Bootloader_CheckApplication();
			if (appStatus == 0x00U) {
				g_AutoJumpEnabled = 1U;
				g_AutoJumpDeadline = HAL_GetTick() + 2000U; /* e.g. 2 seconds window */
			} else {
				g_AutoJumpEnabled = 0U;
			}
		}
	}
}

/* #125 C1: poll FDCAN protocol status and recover from Bus_Off.
 *
 * On Bus_Off the M_CAN sets CCCR.INIT and halts both TX and RX — the
 * BL stops ACKing and the host's session dies; the device is
 * unflashable over CAN until a power-cycle (and re-enters Bus_Off if
 * the fault persists). Recovery is a Stop→Start: HAL_FDCAN_Stop puts
 * the peripheral back to READY (INIT stays set), HAL_FDCAN_Start
 * clears INIT and arms the M_CAN's automatic recovery (it rejoins
 * after 128×11 consecutive recessive bits — ~2.8 ms of idle bus at
 * 500 kbps). The two RX filters live in message RAM, which Stop/Start
 * does NOT touch, so they survive — no need to reconfigure.
 *
 * Rate-limit: act on the transition INTO Bus_Off, then at most once
 * per BL_FDCAN_BUSOFF_RETRY_MS while it persists. Without this, calling
 * Stop/Start every loop iteration would continually restart the
 * recovery sequence and the node would never actually rejoin.
 *
 * GetProtocolStatus is a cheap PSR register read; safe to poll every
 * iteration. The Stop/Start only runs on an actual fault (rare).
 *
 * Observability (so the bench can confirm it fired): a per-boot
 * recovery counter in the health record (`fdcan_recovery_count`), a
 * BL_DTC_FDCAN_BUSOFF DTC carrying the attempt count, and a WARN log
 * line. */
#define BL_FDCAN_BUSOFF_RETRY_MS   100U

static void Bootloader_FdcanBusOffRecover(uint32_t now_ms) {
	static uint8_t  s_was_busoff[BL_FDCAN_BUS_COUNT]      = { 0U };
	static uint32_t s_last_attempt_ms[BL_FDCAN_BUS_COUNT] = { 0U };

	/* Each bus recovers independently — one bus going Bus_Off (a shorted
	 * transceiver, a disconnected stub) must not disturb the others. */
	for (uint8_t b = 0U; b < BL_FDCAN_BUS_COUNT; b++) {
		FDCAN_HandleTypeDef *h = bl_fdcan_bus(b);

		FDCAN_ProtocolStatusTypeDef ps = { 0 };
		if (HAL_FDCAN_GetProtocolStatus(h, &ps) != HAL_OK) {
			continue;
		}

		if (ps.BusOff == 0U) {
			s_was_busoff[b] = 0U;
			continue;
		}

		/* Bus_Off is set. Act on the first detection, then rate-limited. */
		if (s_was_busoff[b] != 0U &&
		    (now_ms - s_last_attempt_ms[b]) < BL_FDCAN_BUSOFF_RETRY_MS) {
			continue;
		}
		s_was_busoff[b]      = 1U;
		s_last_attempt_ms[b] = now_ms;

		/* Stop/Start to clear INIT and rejoin. Errors are non-fatal — we
		 * retry on the next poll; nothing else we can do but keep trying. */
		(void)HAL_FDCAN_Stop(h);
		(void)HAL_FDCAN_Start(h);

		bl_health_record_fdcan_recovery();
		bl_dtc_log(BL_DTC_FDCAN_BUSOFF, BL_DTC_SEV_WARN,
		           bl_health_fdcan_recovery_count());
		bl_log_warn("FDCAN%u bus-off — Stop/Start recovery #%u",
		            (unsigned int)(b + 1U),
		            (unsigned int)bl_health_fdcan_recovery_count());
	}
}

/* #125 H3: max RX frames drained per main-loop pass. Sized to the
 * RX_FIFO0 depth (MX_FDCAN2_Init's RxFifo0ElmtsNbr = 16) so a full
 * FIFO can be cleared in one pass, while bounding the work so the
 * per-pass ticks (session watchdog, bus-off recovery) always run. */
#define BL_FDCAN_RX_DRAIN_BUDGET   16U

/* Drain up to BL_FDCAN_RX_DRAIN_BUDGET frames from one bus's RX FIFO0 and
 * dispatch each to the protocol layer (#120: the BL serves all three FDCAN
 * buses concurrently). Bounding the per-bus batch keeps a continuous flood
 * on one bus from starving the others — or the per-pass ticks. Each reply
 * is routed back to this bus via bl_fdcan_set_active(). */
static void Bootloader_DrainBus(FDCAN_HandleTypeDef *h) {
	FDCAN_RxHeaderTypeDef rxHeader;
	uint8_t rxData[8];

	uint8_t rx_budget = BL_FDCAN_RX_DRAIN_BUDGET;
	while (rx_budget > 0U &&
	       HAL_FDCAN_GetRxFifoFillLevel(h, FDCAN_RX_FIFO0) > 0U) {
		rx_budget--;
		if (HAL_FDCAN_GetRxMessage(h, FDCAN_RX_FIFO0, &rxHeader, rxData) != HAL_OK) {
			continue;
		}

		/* The bootloader only speaks classic 11-bit IDs; any extended-ID
		 * frame is ignored entirely. */
		if (rxHeader.IdType != FDCAN_STANDARD_ID) {
			continue;
		}
		g_AutoJumpEnabled = 0U;

		/* `bl_proto_parse_id` returns false on malformed IDs (bits 10..5
		 * set, or reserved node→host IDs). The FDCAN filter already rejects
		 * anything we can't decode, so this is defence-in-depth against
		 * future filter drift or loopback-harness feeds. */
		bl_proto_id_t id;
		if (!bl_proto_parse_id(rxHeader.Identifier, &id)) {
			continue;
		}

		/* `HAL_FDCAN_GetRxMessage` already extracts the DLC nibble into
		 * bits 3:0 of rxHeader.DataLength (see `FDCAN_ELEMENT_MASK_DLC >>
		 * 16U` in the STM32H7 HAL). For classic CAN the DLC value equals
		 * the byte count, so mask to the low nibble — NO extra shift.
		 * Earlier versions shifted >>16 again, zeroing every length and
		 * silently dropping every frame at bl_proto_dispatch's
		 * `length == 0U` gate. */
		uint8_t length = (uint8_t)(rxHeader.DataLength & 0x0FU);
		if (length > 8U) {
			length = 8U;
		}

		/* Reply on the bus this frame arrived on: bl_proto's TX path sends
		 * via bl_fdcan_get_handle(), which returns the active bus. */
		bl_fdcan_set_active(h);
		bl_proto_dispatch(&id, rxData, length);
	}
}

static void Bootloader_MainLoop(void) {
	while (1) {
		/* #125 H6: kick the hardware watchdog every iteration. The loop
		 * body is sub-millisecond except when a dispatch triggers a flash
		 * erase — and bl_flash_erase / bl_nvm refresh between sectors
		 * themselves, so the longest gap between kicks is a single ~1.8 s
		 * sector erase, well inside the ~8 s IWDG period. */
		bl_iwdg_refresh();

		/* #120: a host may be on any of the three buses — poll every one.
		 * Each bus drains into the shared protocol dispatcher; replies
		 * route back to the originating bus (bl_fdcan_set_active in the
		 * helper). The per-bus budget bounds a flood on any one bus so the
		 * per-pass ticks below always run. */
		for (uint8_t b = 0U; b < BL_FDCAN_BUS_COUNT; b++) {
			Bootloader_DrainBus(bl_fdcan_bus(b));
		}

		/* Protocol tick — drives ISO-TP reassembly timeout. NACKs any
		 * peer whose multi-frame transfer stalled past BL_ISOTP_TIMEOUT_MS. */
		uint32_t tick_now = HAL_GetTick();
		bl_proto_tick(tick_now);
		bl_health_tick(tick_now);
		bl_log_tick(tick_now);
		bl_live_tick(tick_now);

		/* #125 C1: FDCAN bus-off auto-recovery. If a hard bus fault
		 * (sustained TX-arbitration loss, dominant-bit fault) drove
		 * the node into Bus_Off, the M_CAN sets CCCR.INIT and stops
		 * TX + RX — the BL would otherwise be permanently deaf until a
		 * power-cycle, i.e. unflashable over CAN. This rejoins the bus
		 * automatically. */
		Bootloader_FdcanBusOffRecover(tick_now);

		/* Auto-jump window: if we booted with a valid app and nobody
		 * has talked to us before the deadline, hand control over. */
		if (g_AutoJumpEnabled) {
			uint32_t now = HAL_GetTick();

			if ((int32_t)(now - g_AutoJumpDeadline) >= 0) {
				g_AutoJumpEnabled = 0U;

				if (Bootloader_CheckApplication() == 0x00) {
					Bootloader_JumpToApplication();
				} else {
					LED_ERR_ON();
				}
			}
		}

		/* NO HAL_Delay() here — the previous `HAL_Delay(1)` capped the
		 * main loop at 1 kHz, which made the BL drain FDCAN RX_FIFO0 at
		 * 1 kHz. At 500 kbps the host emits classic-CAN CFs at ~3 kHz
		 * (one frame every ~330 µs), so RX_FIFO0 (depth 16) overflowed
		 * mid-burst on every multi-frame WRITE_CHUNK. The
		 * reassembler then saw a sequence gap and NACKed with
		 * BL_NACK_TRANSPORT_ERROR — issue #94 Bug A (the bench-side
		 * symptom that the AutoRetransmission fix didn't address).
		 *
		 * Polling at MHz scale is fine here: the bootloader is wall-
		 * powered, has no power budget to defend, and the bl_*_tick
		 * functions above all enforce their own emission cadences
		 * internally so they don't get noisy under a fast loop. The
		 * audit-follow-up "drain in ISR" remains a future option but
		 * isn't needed once the artificial throttle is removed. */
	}
}

/* Bootloader_ConfigFdcanFilters body has moved to
 * `bl_fdcan_configure_filters` in bl_fdcan.c (#120 Phase A). The
 * filter geometry is instance-agnostic; the only thing that changed
 * is the handle it operates on, which is what justified the move. */

/* ===================== JUMP TO APPLICATION ====================== */

void Bootloader_JumpToApplication(void) {
	uint32_t appStack = *(__IO uint32_t*) BL_APP_BASE;       // 0x08020000
	uint32_t appEntry = *(__IO uint32_t*) (BL_APP_BASE + 4U); // 0x08020004
	pFunction JumpToApp = (pFunction) appEntry;

	/* --- Basic healthchecks --- */

	/* 1) Stack pointer in valid RAM (DTCM or RAM_D1) — same predicate
	 *    Bootloader_CheckApplication uses, so a metadata-approved app
	 *    can't be silently rejected here. See bl_app_validate.h. */
	if (!bl_app_stack_in_legal_range(appStack)) {
		LED_ERR_ON();
		return;
	}

	/* 2) Entry point inside app in FLASH */
	if (appEntry < BL_APP_BASE || appEntry > BL_APP_END) {
		LED_ERR_ON();
		return;
	}

	/* --- Disable peripherals and jump --- */

	/* #125 H6: kick the watchdog once more right before handing off, so
	 * the app inherits a full ~8 s window to reach its own IWDG re-init
	 * (the AMS app gets there in ~50-70 ms; IFS08-CE-AMS#280). Must happen
	 * BEFORE the __set_MSP below — once the stack pointer is swapped to the
	 * app's stack we can no longer make ordinary function calls. */
	bl_iwdg_refresh();

	for (uint8_t b = 0U; b < BL_FDCAN_BUS_COUNT; b++) {
		HAL_FDCAN_DeInit(bl_fdcan_bus(b));
	}
	HAL_DeInit();

	/* Stop SysTick */
	SysTick->CTRL = 0;
	SysTick->LOAD = 0;
	SysTick->VAL = 0;

	__disable_irq();

	/* App vector table */
	SCB->VTOR = BL_APP_BASE;   // 0x08020000

	/* Make the VTOR write globally visible before anything that might
	 * read or be affected by it. On Cortex-M7 (vs M4 or M0) the CPU has
	 * a separate L1 I-cache and a speculative instruction prefetcher,
	 * plus a deeper pipeline; without an explicit data-synchronisation
	 * barrier here an instruction fetch that started before the VTOR
	 * change can still complete against the OLD vector table.
	 *
	 * The BL doesn't enable I/D-cache today, so the cache-coherency
	 * piece is latent — but ARM AN-298 (and the STM32H7 HAL examples)
	 * specify the DSB/ISB pair regardless of cache state, and we want
	 * this to be a no-op the day someone flips cache on to speed up
	 * CRC / VERIFY. Issue #67. */
	__DSB();

	/* Application stack */
	__set_MSP(appStack);

	/* Pipeline barrier so the MSP change is in effect before the next
	 * instruction starts executing. Without this the CPU is allowed
	 * to speculatively decode the upcoming `JumpToApp()` indirect
	 * branch with the OLD MSP in its rename state, which on a deep
	 * M7 pipeline could affect exception-frame placement if a fault
	 * triggered during the transition window. Issue #67. */
	__ISB();

	/* IRQs stay masked across the jump. The previous code re-enabled
	 * them here, which let any IRQ that was pending while masked (e.g.
	 * a queued SysTick — CTRL=0 above stops the counter but doesn't
	 * clear the pending flag) dispatch through the new VTOR before the
	 * application's Reset_Handler has run: the app's peripherals
	 * aren't initialised, its globals aren't relocated, stack-beyond-
	 * MSP isn't set up. Result: undefined behaviour at jump time,
	 * intermittent fault under bus load.
	 *
	 * The app's HAL_Init() / Reset_Handler will re-enable IRQs after
	 * it owns the CPU state. Matches the convention used by other
	 * production bootloaders (e.g. AM32-bootloader's blutil jump:
	 * disable once, never re-enable on the BL side). Issue #59. */

	/* Jump to app */
	JumpToApp();

	/* Just in case */
	while (1) {
	}
}
static uint32_t Bootloader_CalcCrc32(uint32_t address, uint32_t lengthBytes) {
	uint32_t crc = 0xFFFFFFFFU;
	uint8_t *ptr = (uint8_t*) address;

	for (uint32_t i = 0; i < lengthBytes; i++) {
		crc ^= ptr[i];
		for (uint32_t bit = 0; bit < 8; bit++) {
			if (crc & 1U) {
				crc = (crc >> 1) ^ 0xEDB88320U;  // polinomial CRC32-IEEE
			} else {
				crc >>= 1;
			}
		}
	}

	crc ^= 0xFFFFFFFFU;
	return crc;
}

uint8_t Bootloader_CheckApplication(void) {
	/* Read metadata from flash */
	uint32_t const *meta = (uint32_t const*) BL_APP_METADATA_ADDR;

	uint32_t magic = meta[0];
	uint32_t size = meta[1];
	uint32_t crc = meta[2];
	uint32_t appBase = meta[3];

	if (magic != BL_APP_META_MAGIC) {
		return 0x10;  // incorrect magic -> no valid firmware
	}

	if ((size == 0U) || (size > BL_APP_SIZE)) {
		return 0x11;  // invalid size
	}

	if ((crc == 0U) || (crc == 0xFFFFFFFFU)) {
		return 0x12;  // Invalid CRC
	}

	if (appBase != BL_APP_BASE) {
		return 0x13;  // metadata does not correspond to this configuration
	}
	/* Recalculate CRC on the Flash app to be sure */
	uint32_t crcCalc = Bootloader_CalcCrc32(BL_APP_BASE, size);
	if (crcCalc != crc) {
		return 0x14;  // Calculated CRC does not match stored CRC
	}

	/* Check vector table (stack pointer and entry) */
	uint32_t appStack = *(__IO uint32_t*) BL_APP_BASE;
	uint32_t appEntry = *(__IO uint32_t*) (BL_APP_BASE + 4U);

	/* Stack in DTCM or D1 RAM. Previously this used bitmask checks
	 * (& 0x2FF00000 == 0x24000000) that accepted the full 1 MB above
	 * 0x24000000 — much larger than the real 320 KB RAM_D1 — while
	 * Bootloader_JumpToApplication used a stricter range check. That
	 * divergence let a malformed app pass Check and then silently
	 * fail at Jump time with only an LED_ERR_ON. Both now share the
	 * same predicate (see bl_app_validate.h). Issue #66. */
	if (!bl_app_stack_in_legal_range(appStack)) {
		return 0x15;
	}

	/* entry must be within the app's range */
	if ((appEntry < BL_APP_BASE)
			|| (appEntry >= (BL_APP_BASE + BL_APP_SIZE))) {
		return 0x16;  // entry out of range
	}

	return 0x00;  //OK
}

static uint8_t Bootloader_IsBootRequestActive(void) {
	/* Enable backup domain access.
	 PWR clock is already enabled by SystemClock_Config(), so we do not call
	 __HAL_RCC_PWR_CLK_ENABLE() here. */
	HAL_PWR_EnableBkUpAccess();

	/* Make sure RTC clock is enabled so backup registers are accessible */
	if ((RCC->BDCR & RCC_BDCR_RTCEN) == 0U) {
		__HAL_RCC_RTC_ENABLE();
	}

	uint32_t val = RTC->BKP0R;

	if (val == BL_BOOT_REQ_MAGIC) {
		/* Clear the flag so we do not stay in bootloader forever */
		RTC->BKP0R = 0U;
		return 1U;
	}

	return 0U;
}

/* #142: one-shot "boot the app via a clean reset" request. Set by
 * handle_jump / handle_reset (bl_proto) when a flash write happened in the
 * session; consumed here on the next boot. Reads + clears BKP0R only if it
 * holds BL_BOOT_APP_MAGIC, so a BL_BOOT_REQ_MAGIC (stay-in-BL) request is
 * left intact for Bootloader_IsBootRequestActive. */
static uint8_t Bootloader_IsBootAppRequestActive(void) {
	HAL_PWR_EnableBkUpAccess();
	if ((RCC->BDCR & RCC_BDCR_RTCEN) == 0U) {
		__HAL_RCC_RTC_ENABLE();
	}
	if (RTC->BKP0R == BL_BOOT_APP_MAGIC) {
		RTC->BKP0R = 0U;   /* one-shot */
		return 1U;
	}
	return 0U;
}

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
	/* #125 H6: HAL failures (mostly clock config in SystemClock_Config)
	 * used to spin here forever — a marginal HSE would leave the board
	 * dead before CAN even started. Reboot instead so a transient
	 * failure self-recovers; a hard failure loops slowly (bl_fault's
	 * dwell) and stays SWD-recoverable. */
	__disable_irq();
	bl_fault_reboot(BL_FAULT_ERROR_HANDLER);
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
