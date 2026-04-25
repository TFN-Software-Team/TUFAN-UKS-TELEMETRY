/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Yer Istasyonu (UKS) — Teşhis ve UART Test Versiyonu
  * [CLEANUP] LoRa/Telemetry dependencies removed from E-Stop for stable local button testing.
  ******************************************************************************
  */
/* USER CODE END Header */

#include "main.h"
#include "telemetry.h"
// #include "lora.h" // [CLEANUP] Lora header included only when needed
#include <stdio.h>
#include <string.h>

/* Donanım Handle'ları */
UART_HandleTypeDef huart1;
UART_HandleTypeDef huart2; // Keep huart2 definition for completeness, but minimize use

/* Sistem Değişkenleri */
TelCtx_t    tel_ctx;     // Sadece bu dosyada değil, lora.c'den de erişilebilir olmalı
// static LoraCtx_t   lora_ctx; // [CLEANUP] If no LoRa module is connected, remove this definition.
static uint32_t    last_heartbeat_ms = 0;
static uint32_t    last_button_press = 0; // Debounce için

/* Fonksiyon Prototipleri */
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_USART1_UART_Init(void);
static void MX_USART2_UART_Init(void);

/* ====================================================================
 * GCC (VS Code vb.) için printf yönlendirmesi.
 * printf kullanıldığında karakterler USART1 (Ekran) üzerinden yollanır.
 * ==================================================================== */
int _write(int file, char *ptr, int len)
{
    if (HAL_UART_Transmit(&huart1, (uint8_t *)ptr, (uint16_t)len, 100) != HAL_OK) {
        return -1;
    }
    return len;
}

/* ====================================================================
 * KESME (INTERRUPT) YÖNETİMİ: ACİL DURDURMA (E-STOP)
 * [FIXED] LoRa bağımlılığı kaldırıldı. Sadece donanım ve monitor çıktısı kalacak.
 * ==================================================================== */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    // E-STOP Butonuna basıldı mı? (PA0)
    if (GPIO_Pin == AC_L_STOP_Pin)
    {
        uint32_t now = HAL_GetTick();

        // Debounce Kontrolü
        if (now - last_button_press > 200) // 200ms Debounce
        {
            last_button_press = now;
            
            // 1. Motor durum pinini LOW yap (Donanımsal kesme)
            HAL_GPIO_WritePin(MOTOR_EN_GPIO_Port, MOTOR_EN_Pin, GPIO_PIN_RESET);
            
            // ****************************************************
            // [FIXED] LoRa üzerinden E-STOP komutu gönderme kısmı kaldırıldı/yorumlandı. 
            // Çünkü huart2 bağlı değilken bu işlem başarısızlığa yol açıyordu.
            // uint8_t estop_buf[TEL_ESTOP_BURST_MAX_LEN];
            // uint8_t len = Telemetry_EncodeEStopBurst(estop_buf, sizeof(estop_buf));
            // HAL_UART_Transmit(&huart2, estop_buf, len, 100);
            // ****************************************************

            // 3. Bilgisayar ekranına acil durumu bas (Monitor)
            printf("\r\n========================================\r\n");
            printf("!!! YER ISTASYONUNDAN E-STOP BASILDI !!!\r\n");
            printf("========================================\r\n\n");
        }
    }
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
  MX_USART1_UART_Init();   /* Monitor: 115200 Baud (PC Ekranı) */
  MX_USART2_UART_Init();   /* LoRa: 9600 Baud (Physical port setup kept for completeness) */

  /* USER CODE BEGIN 2 */
  
  /* printf tamponlamasını kapatıyoruz. (Anında ekrana basılması için) */
  setvbuf(stdout, NULL, _IONBF, 0);

  /* DONANIM TESTİ: Cihaz açılış mesajı */
  printf("\r\n>>> UKS YER ISTASYONU BASLATILIYOR (Guvende / LoRa Devre Disi) <<<\r\n");

  /* Modül Başlatmaları */
  Telemetry_Init(&tel_ctx);
  
  /* [CLEANUP] LoRa modülü test aşamasında kilitlenmeleri önlemek için tamamen yoruma alındı.
  LoraStatus_t ls = Lora_Init(&lora_ctx, &huart2);
  if (ls == LORA_OK) {
      printf("[OK] LoRa Hazir ve Dinlemede.\r\n");
      Lora_StartReceive(&lora_ctx); 
  }
  */

  /* USER CODE END 2 */

  /* Ana Döngü */
  while (1)
  {
    uint32_t now = HAL_GetTick();

    /* 1. KALP ATIŞI (Heartbeat) - Sistemin donup donmadığını kontrol eder */
    if (now - last_heartbeat_ms >= 3000) { 
        last_heartbeat_ms = now;
        printf("[SISTEM] Heartbeat - %lu ms\r\n", now);
    }

    /* 2. TELEMETRİ ZAMANLAYICISI (Timeout Kontrolleri İçin) */
    Telemetry_Tick(&tel_ctx, now);

    /* 3. LORA'DAN VERİ GELDİ Mİ? 
       (LoRa kesmesi kapalı olduğu için buraya şimdilik hiç girmeyecek, ama kod yapısını koruyoruz) */
    if (Telemetry_IsFrameReady(&tel_ctx)) 
    {
        TelData_t incoming_data;
        TelStatus_t status = Telemetry_Parse(&tel_ctx, &incoming_data);
        Telemetry_PrintDashboard(&incoming_data, status, Telemetry_IsEStopActive(&tel_ctx));
    }
  }
}

/**
  * @brief USART1 Initialization Function (Monitor / PC Ekranı)
  */
static void MX_USART1_UART_Init(void)
{
  huart1.Instance = USART1;
  huart1.Init.BaudRate = 9600; // Ekran hızı 9600
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
  * @brief USART2 Initialization Function (LoRa Modülü)
  */
static void MX_USART2_UART_Init(void)
{
  huart2.Instance = USART2;
  huart2.Init.BaudRate = 115200; // LoRa E32 modülünün varsayılan hızı
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

  /* Motor Durum Çıkışı (PB11) (Başlangıçta Yüksek/Aktif) */
  HAL_GPIO_WritePin(MOTOR_EN_GPIO_Port, MOTOR_EN_Pin, GPIO_PIN_SET);

  /* PA0 Acil Stop Butonu (EXTI) Olarak Tekrar Ayarlandı */
  GPIO_InitStruct.Pin = AC_L_STOP_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_FALLING;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(AC_L_STOP_GPIO_Port, &GPIO_InitStruct);

  /* Motor Durum Çıkışı Pin Ayarları */
  GPIO_InitStruct.Pin = MOTOR_EN_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(MOTOR_EN_GPIO_Port, &GPIO_InitStruct);

  /* EXTI kesmesini aktif et (Acil Stop için) */
  HAL_NVIC_SetPriority(EXTI0_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(EXTI0_IRQn);
}

/* ====================================================================
 * DÜZELTİLMİŞ SAAT AYARLARI: HSI (Dahili Osilatör - 64 MHz)
 * Dış kristale (HSE) bağımlılığı tamamen kaldırır ve sistemi kilitlenmekten kurtarır.
 * ==================================================================== */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI_DIV2;
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL16; // 4MHz * 16 = 64 MHz
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

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

void Error_Handler(void)
{
  __disable_irq();
  while (1) {}
}