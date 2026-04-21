/* USER CODE BEGIN Header */
/**
  **************************
  * @file           : main.c
  * @brief          : Yer Istasyonu (UKS) — Teşhis ve UART Test Versiyonu
  **************************
  */
/* USER CODE END Header */

#include "main.h"
#include "telemetry.h"
#include "lora.h"
#include <stdio.h>
#include <string.h>

/* Donanım Handle'ları */
UART_HandleTypeDef huart1;
UART_HandleTypeDef huart2;

/* Sistem Değişkenleri */
static TelCtx_t    tel_ctx;
static LoraCtx_t   lora_ctx;
static uint32_t    last_heartbeat_ms = 0;

/* Buton olay bayrağı — ISR içinden set edilir */
static volatile uint8_t button_pressed_flag = 0;
static uint32_t last_button_press_ms = 0;

/* Fonksiyon Prototipleri */
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_USART1_UART_Init(void);
static void MX_USART2_UART_Init(void);
static void Task_Button(void);

/* * ÖNEMLİ: Makefile ve VS Code (GCC) için printf yönlendirmesi.
 * Bu fonksiyon printf'in karakterlerini huart1 üzerinden gönderir.
 */
int _write(int file, char *ptr, int len)
{
    // EKRAN (Monitor) USART1 kullanıyor
    if (HAL_UART_Transmit(&huart1, (uint8_t *)ptr, (uint16_t)len, 100) != HAL_OK) {
        return -1;
    }
    return len;
}

/**
  * @brief  Uygulama Giriş Noktası
  */
int main(void)
{
  HAL_Init();
  SystemClock_Config();

  /* Donanım Başlatma */
  MX_GPIO_Init();
  MX_USART1_UART_Init();   /* Monitor: 9600 Baud */
  MX_USART2_UART_Init();   /* LoRa: 115200 Baud */

  /* USER CODE BEGIN 2 */
  
  /* * 1. ADIM: GCC İÇİN KRİTİK AYAR
   * printf tamponlamasını kapatıyoruz. Bu satır olmazsa seri monitör boş kalabilir.
   */
  setvbuf(stdout, NULL, _IONBF, 0);

  /* * 2. ADIM: DONANIM TESTİ (printf'ten bağımsız)
   * Eğer bu mesaj gelmiyorsa kablo bağlantısı veya baud rate (9600) hatalıdır.
   */
  char *raw_msg = "\r\n>>> UKS SISTEM CALISIYOR (DONANIM TESTI OK) <<<\r\n";
  HAL_UART_Transmit(&huart1, (uint8_t *)raw_msg, strlen(raw_msg), 500);

  /* 3. ADIM: PRINTF TESTİ */
  printf("Sistem Baslatiliyor (printf OK)...\r\n");

  /* Modül Başlatmaları */
  Telemetry_Init(&tel_ctx);
  
  // LoRa bağlı değilse Lora_Init timeout dönecektir.
  LoraStatus_t ls = Lora_Init(&lora_ctx, &huart2);
  
  if (ls == LORA_OK) {
      printf("[OK] LoRa Hazir.\r\n");
      Lora_StartReceive(&lora_ctx);
  } else {
      printf("[UYARI] LoRa Donanimi Tespit Edilemedi (Status: %d)\r\n", ls);
  }

  /* USER CODE END 2 */

  /* Ana Döngü */
  while (1)
  {
    uint32_t now = HAL_GetTick();

    /* * KALP ATIŞI: Her 2 saniyede bir mesaj göndererek sistemin 
     * kilitlenmediğini ve UART'ın açık olduğunu teyit eder.
     */
    if (now - last_heartbeat_ms >= 2000) {
        last_heartbeat_ms = now;
        
        // Hem ham veri hem printf ile test
        HAL_UART_Transmit(&huart1, (uint8_t *)"HEARTBEAT_HAL\r\n", 15, 10);
        printf("HEARTBEAT_PRINTF - Zaman: %lu ms\r\n", now);
    }

    /* Diğer Görevler */
    Telemetry_Tick(&tel_ctx, now);
    
    Task_Button(); // Buton olaylarını işle
    //Task_Rx();     // Gelen verileri parse et
  }
}

/**
  * @brief  EXTI kesme callback'i — HAL tarafından otomatik çağrılır.
  *         Butona basıldığında sadece bayrağı set eder (ISR'de printf YOK!).
  */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    if (GPIO_Pin == AC_L_STOP_Pin) {
        button_pressed_flag = 1;
    }
}

/**
  * @brief  Buton görevi — ana döngüde çağrılır.
  *         Bayrak set olduysa terminale mesaj yazar, 200 ms debounce uygular.
  */
static void Task_Button(void)
{
    if (button_pressed_flag) {
        uint32_t now = HAL_GetTick();

        /* Debounce: mekanik sıçramayı filtrele */
        if (now - last_button_press_ms >= 200) {
            last_button_press_ms = now;
            printf(">>> BUTONA BASILDI! (Zaman: %lu ms)\r\n", now);
        }

        button_pressed_flag = 0;
    }
}

/**
  * @brief USART1 Initialization Function (Monitor)
  */
static void MX_USART1_UART_Init(void)
{
  huart1.Instance = USART1;
  huart1.Init.BaudRate = 9600;
  huart1.Init.WordLength = UART_WORDLENGTH_8B;
  huart1.Init.StopBits = UART_STOPBITS_1;
  huart1.Init.Parity = UART_PARITY_NONE;
  huart1.Init.Mode = UART_MODE_TX_RX;
  huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart1.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart1) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief USART2 Initialization Function (LoRa)
  */
static void MX_USART2_UART_Init(void)
{
  huart2.Instance = USART2;
  huart2.Init.BaudRate = 115200;
  huart2.Init.WordLength = UART_WORDLENGTH_8B;
  huart2.Init.StopBits = UART_STOPBITS_1;
  huart2.Init.Parity = UART_PARITY_NONE;
  huart2.Init.Mode = UART_MODE_TX_RX;
  huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart2.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart2) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief GPIO Initialization Function
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  __HAL_RCC_GPIOD_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /* Acil Stop Butonu */
  GPIO_InitStruct.Pin = AC_L_STOP_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_FALLING;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(AC_L_STOP_GPIO_Port, &GPIO_InitStruct);

  /* Motor Durum Çıkışı */
  HAL_GPIO_WritePin(MOTOR_EN_GPIO_Port, MOTOR_EN_Pin, GPIO_PIN_SET);
  GPIO_InitStruct.Pin = MOTOR_EN_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(MOTOR_EN_GPIO_Port, &GPIO_InitStruct);

  /* EXTI Kesme Aktifleştirme — Acil Stop Butonu
   * NOT: IRQ adını pin numarasına göre ayarla:
   *   Pin 0      -> EXTI0_IRQn
   *   Pin 1      -> EXTI1_IRQn
   *   Pin 2      -> EXTI2_IRQn
   *   Pin 3      -> EXTI3_IRQn
   *   Pin 4      -> EXTI4_IRQn
   *   Pin 5..9   -> EXTI9_5_IRQn
   *   Pin 10..15 -> EXTI15_10_IRQn
   */
  HAL_NVIC_SetPriority(EXTI15_10_IRQn, 2, 0);
  HAL_NVIC_EnableIRQ(EXTI15_10_IRQn);
}

void SystemClock_Config(void)
{
  /* Standart HSE 72MHz Ayarları */
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL9;
  HAL_RCC_OscConfig(&RCC_OscInitStruct);

  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK|RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;
  HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2);
}


void Error_Handler(void)
{
  __disable_irq();
  while (1) {}

  
}