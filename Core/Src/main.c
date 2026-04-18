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

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
typedef void (*pFunction)(void);
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define BOOT_REQ_MAGIC   0xB00710ADU  /* Magic value to request bootloader on next reset */

#define APP_START_ADDRESS   0x08020000U

#define APP_FLASH_SIZE        0x000DFFE0U
#define APP_METADATA_ADDRESS  (APP_START_ADDRESS + APP_FLASH_SIZE)  /* 0x080FFFE0 */

#define APP_META_MAGIC        0xB007C0DEU  //valid firmware
#define BOOT_REQ_MAGIC   0xB00710ADU  /* Magic value to request bootloader on next reset */

#define CAN_BOOT_RX_ID      0x21U        /* Host -> Bootloader */
#define CAN_BOOT_TX_ID      0x22U        /* Bootloader -> Host */

/* Protocol commands (data[0]) */
#define BL_CMD_PING         0x01U
#define BL_CMD_JUMP_TO_APP  0x02U
#define BL_CMD_ERASE_APP    0x10U
#define BL_CMD_WRITE_DATA   0x20U
#define BL_CMD_WRITE_FINISH  0x21U
#define BL_CMD_SET_SIZE      0x30U
#define BL_CMD_SET_CRC       0x31U
#define BL_CMD_SET_VERSION   0x32U
#define BL_CMD_GET_INFO      0x40U

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
static uint32_t g_CurrentWriteAddress = APP_START_ADDRESS;

static uint8_t g_AutoJumpEnabled = 0U;
static uint32_t g_AutoJumpDeadline = 0U;

/*CRC checks for firmware integrity*/
static uint32_t g_ExpectedImageSize = 0U;
static uint32_t g_ExpectedImageCrc = 0U;
static uint32_t g_ExpectedImageVersion = 0U;

/* 1 FLASHWORD (32 bytes) buffer for writing to H7 */
static uint8_t s_FlashBuf[32];
static uint32_t s_FlashBufIndex = 0;
static uint32_t s_FlashBufAddress = APP_START_ADDRESS;

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_FDCAN2_Init(void);
/* USER CODE BEGIN PFP */
static void Bootloader_Init(void);
static void Bootloader_MainLoop(void);
static void Bootloader_HandleFrame(FDCAN_RxHeaderTypeDef *rxHeader,
		uint8_t *rxData);
static void Bootloader_SendAck(uint8_t cmd, uint8_t status);
static void Bootloader_JumpToApplication(void);
static void Bootloader_EraseApplicationArea(void);
static HAL_StatusTypeDef Bootloader_WriteDataChunk(const uint8_t *data,
		uint32_t length);
static HAL_StatusTypeDef Bootloader_FlushFlashBuffer(void);
static uint32_t Bootloader_CalcCrc32(uint32_t address, uint32_t lengthBytes);
static uint8_t Bootloader_CheckApplication(void);
static void Bootloader_SendInfoResponse(uint8_t status, uint32_t appSize,
		uint32_t appVersion);
static uint8_t Bootloader_IsBootRequestActive(void);

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/**
 * @brief  The application entry point.
 * @retval int
 */
int main(void) {

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

	/* USER CODE END 2 */

	/* Infinite loop */

	Bootloader_MainLoop();
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
void SystemClock_Config(void) {
	RCC_OscInitTypeDef RCC_OscInitStruct = { 0 };
	RCC_ClkInitTypeDef RCC_ClkInitStruct = { 0 };

	/** Supply configuration update enable
	 */
	HAL_PWREx_ConfigSupply(PWR_LDO_SUPPLY);

	/** Configure the main internal regulator output voltage
	 */
	__HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE0);

	while (!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)) {
	}

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
	if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK) {
		Error_Handler();
	}

	/** Initializes the CPU, AHB and APB buses clocks
	 */
	RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK
			| RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2 | RCC_CLOCKTYPE_D3PCLK1
			| RCC_CLOCKTYPE_D1PCLK1;
	RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
	RCC_ClkInitStruct.SYSCLKDivider = RCC_SYSCLK_DIV1;
	RCC_ClkInitStruct.AHBCLKDivider = RCC_HCLK_DIV2;
	RCC_ClkInitStruct.APB3CLKDivider = RCC_APB3_DIV2;
	RCC_ClkInitStruct.APB1CLKDivider = RCC_APB1_DIV2;
	RCC_ClkInitStruct.APB2CLKDivider = RCC_APB2_DIV2;
	RCC_ClkInitStruct.APB4CLKDivider = RCC_APB4_DIV2;

	if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_3) != HAL_OK) {
		Error_Handler();
	}
}

/**
 * @brief FDCAN2 Initialization Function
 * @param None
 * @retval None
 */
static void MX_FDCAN2_Init(void) {

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
	if (HAL_FDCAN_Init(&hfdcan2) != HAL_OK) {
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
static void MX_GPIO_Init(void) {
	GPIO_InitTypeDef GPIO_InitStruct = { 0 };
	/* USER CODE BEGIN MX_GPIO_Init_1 */

	/* USER CODE END MX_GPIO_Init_1 */

	/* GPIO Ports Clock Enable */
	__HAL_RCC_GPIOH_CLK_ENABLE();
	__HAL_RCC_GPIOB_CLK_ENABLE();
	__HAL_RCC_GPIOD_CLK_ENABLE();
	__HAL_RCC_GPIOA_CLK_ENABLE();

	/*Configure GPIO pin Output Level */
	HAL_GPIO_WritePin(GPIOD, OK_STATUS_Pin | ERR_STATUS_Pin, GPIO_PIN_RESET);

	/*Configure GPIO pins : OK_STATUS_Pin ERR_STATUS_Pin */
	GPIO_InitStruct.Pin = OK_STATUS_Pin | ERR_STATUS_Pin;
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

	/* Initial write address = start of application area */
	g_CurrentWriteAddress = APP_START_ADDRESS;

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
		/* If there are CAN messages -> process and cancel autojump */
		if (HAL_FDCAN_GetRxFifoFillLevel(&hfdcan2, FDCAN_RX_FIFO0) > 0) {
			if (HAL_FDCAN_GetRxMessage(&hfdcan2,
			FDCAN_RX_FIFO0, &rxHeader, rxData) == HAL_OK) {

				g_AutoJumpEnabled = 0U;

				Bootloader_HandleFrame(&rxHeader, rxData);
			}
		}

		/* Check if auto-jump to the application is enabled */
		if (g_AutoJumpEnabled) {
			uint32_t now = HAL_GetTick();

			if ((int32_t) (now - g_AutoJumpDeadline) >= 0) {
				/* Disarm to prevent re-entry */
				g_AutoJumpEnabled = 0U;

				/* Optional double check: app is still valid */
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

static void Bootloader_HandleFrame(FDCAN_RxHeaderTypeDef *rxHeader,
		uint8_t *rxData) {

	if (rxHeader->IdType != FDCAN_STANDARD_ID
			|| rxHeader->Identifier != CAN_BOOT_RX_ID) {
		return;
	}

	uint8_t cmd = rxData[0];

	switch (cmd) {
	case BL_CMD_PING:
		LED_OK_TOGGLE();
		Bootloader_SendAck(BL_CMD_PING, 0x00);
		break;

	case BL_CMD_JUMP_TO_APP: {
		uint8_t status = Bootloader_CheckApplication();

		Bootloader_SendAck(BL_CMD_JUMP_TO_APP, status);

		if (status == 0x00) {
			HAL_Delay(10);
			Bootloader_JumpToApplication();
		} else {
			LED_ERR_ON();
		}
		break;
	}

	case BL_CMD_ERASE_APP:
		Bootloader_EraseApplicationArea();
		g_CurrentWriteAddress = APP_START_ADDRESS;
		Bootloader_SendAck(BL_CMD_ERASE_APP, 0x00);
		break;

	case BL_CMD_WRITE_DATA: {
		/* data[0] = BL_CMD_WRITE_DATA
		 *  data[1..7] = 7 bytes payload*/
		if (rxHeader->DataLength >= FDCAN_DLC_BYTES_8) {
			const uint8_t payloadLen = 7U; // bytes in data[1..7]

			if (Bootloader_WriteDataChunk(&rxData[1], payloadLen) == HAL_OK) {
				Bootloader_SendAck(BL_CMD_WRITE_DATA, 0x00);
			} else {
				Bootloader_SendAck(BL_CMD_WRITE_DATA, 0xFF);
				LED_ERR_ON();
			}
		} else {
			Bootloader_SendAck(BL_CMD_WRITE_DATA, 0xFE);
			LED_ERR_ON();
		}
		break;
	}

	case BL_CMD_WRITE_FINISH: {
		HAL_StatusTypeDef st = Bootloader_FlushFlashBuffer();
		uint8_t status = 0x00;

		if (st != HAL_OK) {
			status = 0xFE; /* flash programming error on last FLASHWORD */
			LED_ERR_ON();
		} else {
			/* Check that size, CRC and version were configured */
			if ((g_ExpectedImageSize != 0U) && (g_ExpectedImageCrc != 0U)
					&& (g_ExpectedImageVersion != 0U)) {
				uint32_t crcCalc = Bootloader_CalcCrc32(APP_START_ADDRESS,
						g_ExpectedImageSize);
				if (crcCalc != g_ExpectedImageCrc) {
					status = 0x01; /* CRC mismatch */
					LED_ERR_ON();
				} else {
					/* CRC is OK: write metadata into reserved flash area */
					uint32_t meta[8];

					meta[0] = APP_META_MAGIC; /* magic */
					meta[1] = g_ExpectedImageSize; /* image size in bytes */
					meta[2] = g_ExpectedImageCrc; /* image CRC32 */
					meta[3] = APP_START_ADDRESS; /* application base address */
					meta[4] = g_ExpectedImageVersion; /* firmware version */
					meta[5] = 0U;
					meta[6] = 0U;
					meta[7] = 0U;

					HAL_FLASH_Unlock();
					st = HAL_FLASH_Program(FLASH_TYPEPROGRAM_FLASHWORD,
					APP_METADATA_ADDRESS, (uint32_t) meta);
					HAL_FLASH_Lock();

					if (st != HAL_OK) {
						status = 0x02; /* error writing metadata */
						LED_ERR_ON();
					}
				}
			} else {
				status = 0x03; /* missing size/CRC/version configuration */
				LED_ERR_ON();
			}
		}

		Bootloader_SendAck(BL_CMD_WRITE_FINISH, status);
		break;
	}

	case BL_CMD_SET_SIZE: {
		uint32_t size = (uint32_t) rxData[1] | ((uint32_t) rxData[2] << 8)
				| ((uint32_t) rxData[3] << 16) | ((uint32_t) rxData[4] << 24);

		g_ExpectedImageSize = size;
		Bootloader_SendAck(BL_CMD_SET_SIZE, 0x00);
		break;
	}

	case BL_CMD_SET_CRC: {
		uint32_t crc = (uint32_t) rxData[1] | ((uint32_t) rxData[2] << 8)
				| ((uint32_t) rxData[3] << 16) | ((uint32_t) rxData[4] << 24);

		g_ExpectedImageCrc = crc;
		Bootloader_SendAck(BL_CMD_SET_CRC, 0x00);
		break;
	}

	case BL_CMD_GET_INFO: {
		uint8_t status = Bootloader_CheckApplication();
		uint32_t appSize = 0U;
		uint32_t appVersion = 0U;

		if (status == 0x00U) {
			/* Read size and version from metadata in flash */
			uint32_t const *meta = (uint32_t const*) APP_METADATA_ADDRESS;
			appSize = meta[1];
			appVersion = meta[4];
		}

		Bootloader_SendInfoResponse(status, appSize, appVersion);
		break;
	}

	case BL_CMD_SET_VERSION: {
		/* data[1..4] = firmware version (user-defined encoding) */
		uint32_t ver = (uint32_t) rxData[1] | ((uint32_t) rxData[2] << 8)
				| ((uint32_t) rxData[3] << 16) | ((uint32_t) rxData[4] << 24);

		g_ExpectedImageVersion = ver;
		Bootloader_SendAck(BL_CMD_SET_VERSION, 0x00);
		break;
	}

	default:
		LED_ERR_ON();
		Bootloader_SendAck(cmd, 0xFF);
		break;
	}
}

static void Bootloader_SendAck(uint8_t cmd, uint8_t status) {
	FDCAN_TxHeaderTypeDef txHeader;
	uint8_t txData[8] = { 0 };

	txHeader.Identifier = CAN_BOOT_TX_ID;
	txHeader.IdType = FDCAN_STANDARD_ID;
	txHeader.TxFrameType = FDCAN_DATA_FRAME;
	txHeader.DataLength = FDCAN_DLC_BYTES_2;
	txHeader.ErrorStateIndicator = FDCAN_ESI_ACTIVE;
	txHeader.BitRateSwitch = FDCAN_BRS_OFF;
	txHeader.FDFormat = FDCAN_CLASSIC_CAN;
	txHeader.TxEventFifoControl = FDCAN_NO_TX_EVENTS;
	txHeader.MessageMarker = 0;

	txData[0] = cmd;
	txData[1] = status;

	if (HAL_FDCAN_AddMessageToTxFifoQ(&hfdcan2, &txHeader, txData) != HAL_OK) {
		LED_ERR_ON();
	}
}

static void Bootloader_SendInfoResponse(uint8_t status, uint32_t appSize,
		uint32_t appVersion) {
	FDCAN_TxHeaderTypeDef txHeader;
	uint8_t txData[8] = { 0 };

	txHeader.Identifier = CAN_BOOT_TX_ID;
	txHeader.IdType = FDCAN_STANDARD_ID;
	txHeader.TxFrameType = FDCAN_DATA_FRAME;
	txHeader.DataLength = FDCAN_DLC_BYTES_8;
	txHeader.ErrorStateIndicator = FDCAN_ESI_ACTIVE;
	txHeader.BitRateSwitch = FDCAN_BRS_OFF;
	txHeader.FDFormat = FDCAN_CLASSIC_CAN;
	txHeader.TxEventFifoControl = FDCAN_NO_TX_EVENTS;
	txHeader.MessageMarker = 0;

	txData[0] = BL_CMD_GET_INFO;
	txData[1] = status;

	if (status == 0x00U) {
		/* Bytes 2..5: image size in bytes (little endian) */
		txData[2] = (uint8_t) (appSize & 0xFFU);
		txData[3] = (uint8_t) ((appSize >> 8) & 0xFFU);
		txData[4] = (uint8_t) ((appSize >> 16) & 0xFFU);
		txData[5] = (uint8_t) ((appSize >> 24) & 0xFFU);

		/* Bytes 6..7: lower 16 bits of firmware version (little endian) */
		txData[6] = (uint8_t) (appVersion & 0xFFU);
		txData[7] = (uint8_t) ((appVersion >> 8) & 0xFFU);
	}

	if (HAL_FDCAN_AddMessageToTxFifoQ(&hfdcan2, &txHeader, txData) != HAL_OK) {
		LED_ERR_ON();
	}
}

/* ===================== JUMP TO APPLICATION ====================== */

static void Bootloader_JumpToApplication(void) {
	uint32_t appStack = *(__IO uint32_t*) APP_START_ADDRESS;       // 0x08020000
	uint32_t appEntry = *(__IO uint32_t*) (APP_START_ADDRESS + 4U); // 0x08020004
	pFunction JumpToApp = (pFunction) appEntry;

	/* --- Basic healthchecks --- */

	/* 1) Stack pointer in valid RAM (DTCM o RAM_D1) */
	if (!((appStack >= 0x20000000U && appStack <= 0x20020000U) ||  // DTCM 128K
			(appStack >= 0x24000000U && appStack <= 0x24050000U))) { // RAM_D1 320K
		LED_ERR_ON();
		return;
	}

	/* 2) Entry point inside app in FLASH */
	if (appEntry < APP_START_ADDRESS || appEntry > 0x080FFFFFU) {
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
	SCB->VTOR = APP_START_ADDRESS;   // 0x08020000

	/* Application stack */
	__set_MSP(appStack);

	__enable_irq();

	/* Jump to app */
	JumpToApp();

	/* Just in case */
	while (1) {
	}
}

/* ===================== FLASH ERASE & WRITE ====================== */

static void Bootloader_EraseApplicationArea(void) {
	FLASH_EraseInitTypeDef eraseInit = { 0 };
	uint32_t pageError = 0;

	const uint32_t FLASH_BASE_ADDR = 0x08000000U;
	const uint32_t APP_START = 0x08020000U;
	const uint32_t APP_END = 0x080FFFFFU;

	uint32_t firstSector = (APP_START - FLASH_BASE_ADDR) / FLASH_SECTOR_SIZE; /* 1 */
	uint32_t lastSector = (APP_END - FLASH_BASE_ADDR) / FLASH_SECTOR_SIZE; /* 7 */

	eraseInit.TypeErase = FLASH_TYPEERASE_SECTORS;
	eraseInit.Banks = FLASH_BANK_1;
	eraseInit.Sector = firstSector;
	eraseInit.NbSectors = (lastSector - firstSector) + 1U;

	HAL_FLASH_Unlock();
	if (HAL_FLASHEx_Erase(&eraseInit, &pageError) != HAL_OK) {
		LED_ERR_ON();

	}
	HAL_FLASH_Lock();

	/*Reset write pointers*/
	g_CurrentWriteAddress = APP_START_ADDRESS;
	s_FlashBufIndex = 0;
	s_FlashBufAddress = APP_START_ADDRESS;

	/*Reset flash image metadata*/

	g_ExpectedImageSize = 0U;
	g_ExpectedImageCrc = 0U;
	g_ExpectedImageVersion = 0U;
}

static HAL_StatusTypeDef Bootloader_WriteDataChunk(const uint8_t *data,
		uint32_t length) {
	HAL_StatusTypeDef status = HAL_OK;

	for (uint32_t i = 0; i < length; i++) {
		/* Si el buffer está lleno, programamos un FLASHWORD */
		if (s_FlashBufIndex >= 32U) {
			HAL_FLASH_Unlock();
			status = HAL_FLASH_Program(FLASH_TYPEPROGRAM_FLASHWORD,
					s_FlashBufAddress, (uint32_t) s_FlashBuf);
			HAL_FLASH_Lock();

			if (status != HAL_OK) {
				return status;
			}

			s_FlashBufAddress += 32U;
			s_FlashBufIndex = 0U;
		}

		s_FlashBuf[s_FlashBufIndex++] = data[i];
		g_CurrentWriteAddress++;
	}

	return HAL_OK;
}

static HAL_StatusTypeDef Bootloader_FlushFlashBuffer(void) {
	HAL_StatusTypeDef status = HAL_OK;

	if (s_FlashBufIndex > 0U) {
		/* Rellenar el resto con 0xFF */
		for (uint32_t i = s_FlashBufIndex; i < 32U; i++) {
			s_FlashBuf[i] = 0xFF;
		}

		HAL_FLASH_Unlock();
		status = HAL_FLASH_Program(FLASH_TYPEPROGRAM_FLASHWORD,
				s_FlashBufAddress, (uint32_t) s_FlashBuf);
		HAL_FLASH_Lock();

		s_FlashBufIndex = 0U;
		s_FlashBufAddress += 32U;
	}

	return status;
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
	uint32_t const *meta = (uint32_t const*) APP_METADATA_ADDRESS;

	uint32_t magic = meta[0];
	uint32_t size = meta[1];
	uint32_t crc = meta[2];
	uint32_t appBase = meta[3];

	if (magic != APP_META_MAGIC) {
		return 0x10;  // incorrect magic -> no valid firmware
	}

	if ((size == 0U) || (size > APP_FLASH_SIZE)) {
		return 0x11;  // invalid size
	}

	if ((crc == 0U) || (crc == 0xFFFFFFFFU)) {
		return 0x12;  // Invalid CRC
	}

	if (appBase != APP_START_ADDRESS) {
		return 0x13;  // metadata does not correspond to this configuration
	}
	/* Recalculate CRC on the Flash app to be sure */
	uint32_t crcCalc = Bootloader_CalcCrc32(APP_START_ADDRESS, size);
	if (crcCalc != crc) {
		return 0x14;  // Calculated CRC does not match stored CRC
	}

	/* Check vector table (stack pointer and entry) */
	uint32_t appStack = *(__IO uint32_t*) APP_START_ADDRESS;
	uint32_t appEntry = *(__IO uint32_t*) (APP_START_ADDRESS + 4U);

	/* stack in DTCM o D1 RAM */
	if (((appStack & 0x2FFE0000U) != 0x20000000U) && /* DTCM */
	((appStack & 0x2FF00000U) != 0x24000000U)) /* D1 SRAM */
	{
		return 0x15;  // stack pointer fuera de rango
	}

	/* entry must be within the app's range */
	if ((appEntry < APP_START_ADDRESS)
			|| (appEntry >= (APP_START_ADDRESS + APP_FLASH_SIZE))) {
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

	if (val == BOOT_REQ_MAGIC) {
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
void Error_Handler(void) {
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
