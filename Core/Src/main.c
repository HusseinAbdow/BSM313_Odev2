/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
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

#include <stdint.h>
#include "stm32f1xx_hal_flash_ex.h"

/*
 * BSM316 / ÖDEV 3 (Summary)
 * - TIM2 @ 1 Hz interrupt drives the whole timing (no HAL_Delay).
 * - PC13 LED blinks blink_count times (1s ON, 1s OFF) then stays OFF for 5s; repeats forever.
 * - Button on PA0 updates blink_count in range [4..7] with wrap (7 -> 4).
 * - blink_count is loaded from internal flash at boot, and written back on every change.
 * - If stored value is erased/invalid (>7), default to 4 and write 4 to flash.
 * - Factory reset: if button is held continuously from startup for >= 3 seconds,
 *   blink_count becomes 4 (factory default).
 * - PA1 is configured as GPIO output and remains LOW for the entire runtime.
 *
 * NOTE about wiring:
 * The assignment's "A0-A1 pads touched together" trick requires:
 *   PA1 = output LOW, PA0 = input pull-up, EXTI on FALLING edge.
 */

/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

#define BLINK_COUNT_MIN                (4u)
#define BLINK_COUNT_MAX                (7u)

/* Ödev 3: blink_count is persisted in internal flash.
 * We reserve the last 1KB flash page in the linker script and store 1 halfword there.
 */
#define BLINK_COUNT_FLASH_ADDR         ((uint32_t)0x0800FC00u)

/* Bluepill PC13 LED is typically active-low. */
#define LED_ON()   HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, GPIO_PIN_RESET)
#define LED_OFF()  HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, GPIO_PIN_SET)

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
TIM_HandleTypeDef htim2;

/* USER CODE BEGIN PV */

/* Ödev 3: blink_count runtime value (must stay in [4..7]). */
static volatile uint16_t g_blink_count = BLINK_COUNT_MIN;

static volatile uint8_t g_button_press_pending = 0;
static volatile uint8_t g_factory_reset_pending = 0;

static volatile uint32_t g_seconds_since_boot = 0;
static volatile uint8_t g_boot_hold_active = 0;
static volatile uint8_t g_boot_hold_seconds = 0;

typedef enum
{
  BLINK_STATE_ON = 0,
  BLINK_STATE_OFF,
  BLINK_STATE_PAUSE
} BlinkState;

static volatile BlinkState g_blink_state = BLINK_STATE_ON;
static volatile uint8_t g_blinks_done = 0;
static volatile uint8_t g_pause_remaining = 0;

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_TIM2_Init(void);
/* USER CODE BEGIN PFP */

static uint16_t Flash_ReadBlinkCount(void);
static HAL_StatusTypeDef Flash_WriteBlinkCount(uint16_t value);
static void Blink_ResetCycle(void);
static uint8_t Button_IsPressed(void);

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

static uint8_t Button_IsPressed(void)
{
  /* PA0 is configured with pull-up; pressed means logic 0 (active-low). */
  return (HAL_GPIO_ReadPin(BUTTON_GPIO_Port, BUTTON_Pin) == GPIO_PIN_RESET) ? 1u : 0u;
}

static uint16_t Flash_ReadBlinkCount(void)
{
  /* Ödev 3: read blink_count from flash (erased reads as 0xFFFF). */
  return *((__IO uint16_t*)BLINK_COUNT_FLASH_ADDR);
}

static HAL_StatusTypeDef Flash_WriteBlinkCount(uint16_t value)
{
  /* Ödev 3: write blink_count to flash.
   * We erase 1 page then program a single halfword.
   */
  FLASH_EraseInitTypeDef erase_init = {0};
  uint32_t page_error = 0;

  __disable_irq();
  HAL_FLASH_Unlock();

  erase_init.TypeErase = FLASH_TYPEERASE_PAGES;
  erase_init.PageAddress = BLINK_COUNT_FLASH_ADDR;
  erase_init.NbPages = 1;

  if (HAL_FLASHEx_Erase(&erase_init, &page_error) != HAL_OK)
  {
    HAL_FLASH_Lock();
    __enable_irq();
    return HAL_ERROR;
  }

  if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, BLINK_COUNT_FLASH_ADDR, (uint64_t)value) != HAL_OK)
  {
    HAL_FLASH_Lock();
    __enable_irq();
    return HAL_ERROR;
  }

  HAL_FLASH_Lock();
  __enable_irq();
  return HAL_OK;
}

static void Blink_ResetCycle(void)
{
  /* Ödev 3: restart the repeating pattern using the current g_blink_count. */
  __disable_irq();
  g_blinks_done = 0;
  g_pause_remaining = 0;

  /* Start with LED on immediately for the first 1-second window. */
  LED_ON();
  g_blink_state = BLINK_STATE_OFF;
  __enable_irq();
}

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
  MX_TIM2_Init();
  /* USER CODE BEGIN 2 */

  /* Ödev 3 (Requirement): At boot, load blink_count from flash.
   * If value is invalid/erased (>7 or <4), default to 4 and write 4 back.
   */
  {
    uint16_t stored = Flash_ReadBlinkCount();
    if ((stored >= BLINK_COUNT_MIN) && (stored <= BLINK_COUNT_MAX))
    {
      g_blink_count = stored;
    }
    else
    {
      g_blink_count = BLINK_COUNT_MIN;
      (void)Flash_WriteBlinkCount(g_blink_count);
    }
  }

  /* Ödev 3 (Requirement): Factory reset to 4 if button is held continuously
   * from startup for >= 3 seconds.
   */
  g_boot_hold_active = Button_IsPressed();
  g_boot_hold_seconds = 0;

  LED_OFF();
  Blink_ResetCycle();

  /* Ödev 3 (Requirement): TIM2 configured to generate 1 Hz interrupts.
   * (See MX_TIM2_Init: Prescaler=7999, Period=8999 for 72MHz timer clock.)
   */
  HAL_TIM_Base_Start_IT(&htim2);

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* Ödev 3: apply pending events outside IRQ context (safer flash writes). */
    if (g_factory_reset_pending)
    {
      __disable_irq();
      g_factory_reset_pending = 0;
      g_blink_count = BLINK_COUNT_MIN;
      __enable_irq();

      (void)Flash_WriteBlinkCount(g_blink_count);
      Blink_ResetCycle();
    }

    if (g_button_press_pending)
    {
      __disable_irq();
      g_button_press_pending = 0;
      __enable_irq();

      /* Ödev 3 (Requirement): blink_count increments 4->5->6->7->4 on each press. */
      uint16_t next = g_blink_count;
      if (next < BLINK_COUNT_MAX)
      {
        next++;
      }
      else
      {
        next = BLINK_COUNT_MIN;
      }

      g_blink_count = next;
      /* Ödev 3 (Requirement): write updated blink_count to flash whenever it changes. */
      (void)Flash_WriteBlinkCount(g_blink_count);
      Blink_ResetCycle();
    }
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

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL9;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief TIM2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM2_Init(void)
{

  /* USER CODE BEGIN TIM2_Init 0 */

  /* USER CODE END TIM2_Init 0 */

  TIM_ClockConfigTypeDef sClockSourceConfig = {0};
  TIM_MasterConfigTypeDef sMasterConfig = {0};

  /* USER CODE BEGIN TIM2_Init 1 */

  /* USER CODE END TIM2_Init 1 */
  htim2.Instance = TIM2;
  htim2.Init.Prescaler = 7999;
  htim2.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim2.Init.Period = 8999;
  htim2.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim2.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&htim2) != HAL_OK)
  {
    Error_Handler();
  }
  sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
  if (HAL_TIM_ConfigClockSource(&htim2, &sClockSourceConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim2, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM2_Init 2 */

  /* USER CODE END TIM2_Init 2 */

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
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOD_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, GPIO_PIN_SET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(OUTPUT_PIN_GPIO_Port, OUTPUT_PIN_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin : LED_Pin */
  GPIO_InitStruct.Pin = LED_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(LED_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : BUTTON_Pin */
  GPIO_InitStruct.Pin = BUTTON_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_FALLING;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(BUTTON_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : OUTPUT_PIN_Pin */
  GPIO_InitStruct.Pin = OUTPUT_PIN_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(OUTPUT_PIN_GPIO_Port, &GPIO_InitStruct);

  /* EXTI interrupt init*/
  HAL_NVIC_SetPriority(EXTI0_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(EXTI0_IRQn);

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
  if (GPIO_Pin == BUTTON_Pin)
  {
    /* Ödev 3: button press detected via EXTI.
     * We debounce and then notify main loop to update blink_count and flash.
     */
    static uint32_t last_ms = 0;
    uint32_t now = HAL_GetTick();
    if ((now - last_ms) < 50u)
    {
      return;
    }
    last_ms = now;

    /* If button was held since boot, factory reset is handled by TIM2 (first 3 seconds). */
    if ((g_boot_hold_active != 0u) && (g_seconds_since_boot < 3u))
    {
      return;
    }

    g_button_press_pending = 1;
  }
}

void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  if (htim->Instance != TIM2)
  {
    return;
  }

  /* Ödev 3 (Requirement): TIM2 must generate 1 interrupt per second.
   * This callback is executed once per second.
   */
  g_seconds_since_boot++;

  /* Ödev 3 (Requirement): Factory reset to 4 if button held for >=3 seconds
   * continuously from startup.
   */
  if ((g_seconds_since_boot <= 3u) && (g_boot_hold_active != 0u))
  {
    if (Button_IsPressed())
    {
      if (g_boot_hold_seconds < 3u)
      {
        g_boot_hold_seconds++;
      }
      if (g_boot_hold_seconds >= 3u)
      {
        g_factory_reset_pending = 1;
        g_boot_hold_active = 0;
      }
    }
    else
    {
      g_boot_hold_active = 0;
      g_boot_hold_seconds = 0;
    }
  }

  /* Ödev 3 (Requirement):
   * - LED blinks blink_count times (1-second ON / 1-second OFF)
   * - then stays OFF for 5 seconds
   * - then repeats forever
   */
  switch (g_blink_state)
  {
    case BLINK_STATE_ON:
      LED_ON();
      g_blink_state = BLINK_STATE_OFF;
      break;

    case BLINK_STATE_OFF:
      LED_OFF();
      if (g_blinks_done < 0xFFu)
      {
        g_blinks_done++;
      }
      if (g_blinks_done >= (uint8_t)g_blink_count)
      {
        g_blink_state = BLINK_STATE_PAUSE;
        g_pause_remaining = 5u;
      }
      else
      {
        g_blink_state = BLINK_STATE_ON;
      }
      break;

    case BLINK_STATE_PAUSE:
    default:
      LED_OFF();
      if (g_pause_remaining > 0u)
      {
        g_pause_remaining--;
      }
      if (g_pause_remaining == 0u)
      {
        g_blinks_done = 0;
        g_blink_state = BLINK_STATE_ON;
      }
      break;
  }
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
  while (1)
  {
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
