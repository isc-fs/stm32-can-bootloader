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

#include "bl_memmap.h"
#include "bl_config.h"
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

FDCAN_HandleTypeDef hfdcan2;

/* USER CODE BEGIN PV */
static uint8_t  g_AutoJumpEnabled  = 0U;
static uint32_t g_AutoJumpDeadline = 0U;

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_FDCAN2_Init(void);
/* USER CODE BEGIN PFP */
static void Bootloader_Init(void);
static void Bootloader_MainLoop(void);
static HAL_StatusTypeDef Bootloader_ConfigFdcanFilters(void);
static void Bootloader_JumpToApplication(void);
static uint32_t Bootloader_CalcCrc32(uint32_t address, uint32_t lengthBytes);
static uint8_t  Bootloader_CheckApplication(void);
static uint8_t  Bootloader_IsBootRequestActive(void);

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
  MX_FDCAN2_Init();
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
  hfdcan2.Init.AutoRetransmission = DISABLE;
  hfdcan2.Init.TransmitPause = DISABLE;
  hfdcan2.Init.ProtocolException = DISABLE;
  hfdcan2.Init.NominalPrescaler = 6;
  hfdcan2.Init.NominalSyncJumpWidth = 1;
  hfdcan2.Init.NominalTimeSeg1 = 2;
  hfdcan2.Init.NominalTimeSeg2 = 5;
  hfdcan2.Init.DataPrescaler = 1;
  hfdcan2.Init.DataSyncJumpWidth = 1;
  hfdcan2.Init.DataTimeSeg1 = 1;
  hfdcan2.Init.DataTimeSeg2 = 1;
  hfdcan2.Init.MessageRAMOffset = 0;
  hfdcan2.Init.StdFiltersNbr = 1;
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
	LED_OK_ON();
	LED_ERR_OFF();

	/* Filters must be configured while the FDCAN is still in Init mode —
	 * i.e. before HAL_FDCAN_Start. */
	if (Bootloader_ConfigFdcanFilters() != HAL_OK) {
		LED_ERR_ON();
		while (1) {
		}
	}

	if (HAL_FDCAN_Start(&hfdcan2) != HAL_OK) {
		LED_ERR_ON();
		while (1) {
		}
	}

	if (HAL_FDCAN_ActivateNotification(&hfdcan2,
	FDCAN_IT_RX_FIFO0_NEW_MESSAGE, 0) != HAL_OK) {
		LED_ERR_ON();
		while (1) {
		}
	}

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

static void Bootloader_MainLoop(void) {
	FDCAN_RxHeaderTypeDef rxHeader;
	uint8_t rxData[8];

	while (1) {
		/* Drain any pending frames — each one cancels the auto-jump and
		 * is handed off to the protocol dispatcher. */
		if (HAL_FDCAN_GetRxFifoFillLevel(&hfdcan2, FDCAN_RX_FIFO0) > 0) {
			if (HAL_FDCAN_GetRxMessage(&hfdcan2,
			FDCAN_RX_FIFO0, &rxHeader, rxData) == HAL_OK) {

				/* The bootloader only speaks classic 11-bit IDs; any
				 * extended-ID frame is ignored entirely. */
				if (rxHeader.IdType == FDCAN_STANDARD_ID) {
					g_AutoJumpEnabled = 0U;

					bl_proto_id_t id;
					bl_proto_parse_id(rxHeader.Identifier, &id);

					/* FDCAN_DLC_BYTES_N is encoded in bits 19:16 of the
					 * DataLength word. For classic CAN (0..8) the field
					 * value equals the byte count. */
					uint8_t length = (uint8_t)((rxHeader.DataLength >> 16) & 0x0FU);
					if (length > 8U) {
						length = 8U;
					}

					bl_proto_dispatch(&id, rxData, length);
				}
			}
		}

		/* Protocol tick — drives ISO-TP reassembly timeout. NACKs any
		 * peer whose multi-frame transfer stalled past BL_ISOTP_TIMEOUT_MS. */
		bl_proto_tick(HAL_GetTick());

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

		HAL_Delay(1);
	}
}

/* Configure FDCAN2 filters so FIFO0 only receives frames whose 4-bit
 * destination field (bits 3:0 of the 11-bit ID) matches either this
 * board's BL_NODE_ID or the broadcast address 0xF. Everything else is
 * rejected at the peripheral and never hits software. */
static HAL_StatusTypeDef Bootloader_ConfigFdcanFilters(void) {
	FDCAN_FilterTypeDef filter = { 0 };
	HAL_StatusTypeDef st;

	filter.IdType       = FDCAN_STANDARD_ID;
	filter.FilterType   = FDCAN_FILTER_MASK;
	filter.FilterConfig = FDCAN_FILTER_TO_RXFIFO0;
	filter.FilterID2    = BL_PROTO_DST_MASK;  /* match dst bits only */

	/* Unicast to this node. */
	filter.FilterIndex = 0U;
	filter.FilterID1   = BL_NODE_ID & BL_PROTO_DST_MASK;
	st = HAL_FDCAN_ConfigFilter(&hfdcan2, &filter);
	if (st != HAL_OK) {
		return st;
	}

	/* Broadcast to every bootloader on the bus. */
	filter.FilterIndex = 1U;
	filter.FilterID1   = BL_PROTO_NODE_BROADCAST;
	st = HAL_FDCAN_ConfigFilter(&hfdcan2, &filter);
	if (st != HAL_OK) {
		return st;
	}

	/* Reject everything that doesn't match one of the two filters, and
	 * reject remote frames outright — the protocol has no use for them. */
	return HAL_FDCAN_ConfigGlobalFilter(&hfdcan2,
	                                    FDCAN_REJECT,
	                                    FDCAN_REJECT,
	                                    FDCAN_REJECT_REMOTE,
	                                    FDCAN_REJECT_REMOTE);
}

/* ===================== JUMP TO APPLICATION ====================== */

static void Bootloader_JumpToApplication(void) {
	uint32_t appStack = *(__IO uint32_t*) BL_APP_BASE;       // 0x08020000
	uint32_t appEntry = *(__IO uint32_t*) (BL_APP_BASE + 4U); // 0x08020004
	pFunction JumpToApp = (pFunction) appEntry;

	/* --- Basic healthchecks --- */

	/* 1) Stack pointer in valid RAM (DTCM o RAM_D1) */
	if (!((appStack >= 0x20000000U && appStack <= 0x20020000U) ||  // DTCM 128K
			(appStack >= 0x24000000U && appStack <= 0x24050000U))) { // RAM_D1 320K
		LED_ERR_ON();
		return;
	}

	/* 2) Entry point inside app in FLASH */
	if (appEntry < BL_APP_BASE || appEntry > BL_APP_END) {
		LED_ERR_ON();
		return;
	}

	/* --- Disable peripherals and jump --- */

	HAL_FDCAN_DeInit(&hfdcan2);
	HAL_DeInit();

	/* Stop SysTick */
	SysTick->CTRL = 0;
	SysTick->LOAD = 0;
	SysTick->VAL = 0;

	__disable_irq();

	/* App vector table */
	SCB->VTOR = BL_APP_BASE;   // 0x08020000

	/* Application stack */
	__set_MSP(appStack);

	__enable_irq();

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

static uint8_t Bootloader_CheckApplication(void) {
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

	/* stack in DTCM o D1 RAM */
	if (((appStack & 0x2FFE0000U) != 0x20000000U) && /* DTCM */
	((appStack & 0x2FF00000U) != 0x24000000U)) /* D1 SRAM */
	{
		return 0x15;  // stack pointer fuera de rango
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

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
	/* User can add his own implementation to report the HAL error return state */
	__disable_irq();
	while (1) {
	}
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
