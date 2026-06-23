/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : UKS Yer Istasyonu - AKS protokolu entegre.
  *
  *  Duzeltmeler:
  *  BUG #4: _write/__io_putchar printf yonlendirmesi.
  *  BUG #5: E-STOP TX timeout 50 ms.
  *  BUG #6: Klon-guvenli saat HSI 8 MHz, PLL yok.
  *  BUG #7: USART1 115200 baud — 9600'de 100 ms timeout dashboard'u
  *          kesiyor (700 byte @ 9600 = ~730 ms). 115200'de ~60 ms,
  *          timeout 200 ms ile guvenli margin.
  *  BUG #8: Debounce boot edge case — last_button_press = UINT32_MAX
  *          ile baslatildi. Onceki 0 degeri sistemin ilk 200 ms'inde
  *          E-STOP butonunu yok sayiyordu.
  ******************************************************************************
  */
/* USER CODE END Header */

#include "main.h"
#include "telemetry.h"
#include "lora.h"
#include <stdio.h>
#include <string.h>

/* ========== Donanim Handle'lari ========== */
UART_HandleTypeDef huart1;
UART_HandleTypeDef huart2;

/* ========== Sistem Durumu ========== */
TelCtx_t           tel_ctx;
static LoraCtx_t   lora_ctx;

static uint32_t          last_heartbeat_ms = 0;

/* BUG #8 DUZELTME: Boot'ta E-STOP kilitlenmesi.
 * 0 ile baslatilinca ilk 200 ms'de buton yok sayiliyordu.
 * UINT32_MAX da unsigned wraparound ile ayni sorunu yaratiyordu.
 * (uint32_t)(-2000) = 4294965296: boot'ta now~0,
 * (0 - 4294965296) wraparound ile ~2000 > 200 -> koşul geçilir. */
static uint32_t          last_button_press = (uint32_t)(-2000);

static volatile uint8_t  estop_tx_pending  = 0;

/* ========== Prototipler ========== */
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_USART1_UART_Init(void);
static void MX_USART2_UART_Init(void);

/* ====================================================================
 * printf -> USART1 yonlendirmesi.
 * BUG #7: timeout 200 ms — 115200 baud'da maks dashboard ~60 ms,
 * yeterli margin var.
 * ==================================================================== */
int __io_putchar(int ch)
{
    uint8_t c = (uint8_t)ch;
    HAL_UART_Transmit(&huart1, &c, 1, 200);
    return ch;
}

int _write(int file, char *ptr, int len)
{
    (void)file;
    HAL_UART_Transmit(&huart1, (uint8_t *)ptr, (uint16_t)len, 200);
    return len;
}

/* ====================================================================
 * LoRa RX -> Telemetry parser kopru fonksiyonu (ISR context)
 * ==================================================================== */
static void on_lora_rx_byte(uint8_t b, uint32_t now_ms, void *user)
{
    TelCtx_t *t = (TelCtx_t *)user;
    Telemetry_RxBytePush(t, b, now_ms);
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    Lora_OnUartRxCplt(&lora_ctx, huart);
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    Lora_OnUartError(&lora_ctx, huart);
}

/* ====================================================================
 * E-STOP butonu (PA0, falling edge)
 * ==================================================================== */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    if (GPIO_Pin != AC_L_STOP_Pin) return;

    uint32_t now = HAL_GetTick();
    if ((now - last_button_press) <= 200U) return;
    last_button_press = now;

    HAL_GPIO_WritePin(MOTOR_EN_GPIO_Port, MOTOR_EN_Pin, GPIO_PIN_RESET);
    Telemetry_SetEStopActive(&tel_ctx);
    estop_tx_pending = 1U;
}

/* ====================================================================
 * E-STOP TX — 50 ms timeout
 * ==================================================================== */
static void process_estop_tx(void)
{
    if (!estop_tx_pending) return;
    estop_tx_pending = 0U;

    uint8_t buf[TEL_ESTOP_BURST_COUNT];
    uint8_t n = Telemetry_EncodeEStopBurst(buf, sizeof(buf));

    HAL_StatusTypeDef hs = HAL_UART_Transmit(lora_ctx.huart, buf, n, 50U);
    if (hs == HAL_OK)
    {
        tel_ctx.stats.estop_tx_count++;
        printf("\r\n!!! E-STOP -> AKS (0xA1 x%u) gonderildi !!!\r\n\r\n",
               (unsigned)n);
    }
    else
    {
        printf("\r\n!! E-STOP gonderilemedi (HAL=%d) !!\r\n\r\n", (int)hs);
    }
}

int main(void)
{
    HAL_Init();
    SystemClock_Config();

    MX_GPIO_Init();
    MX_USART1_UART_Init();
    MX_USART2_UART_Init();

    setvbuf(stdout, NULL, _IONBF, 0);

    printf("\r\n>>> UKS YER ISTASYONU BASLATILIYOR <<<\r\n");
    printf("    Telemetry  : ASCII CSV (15 alan, AKS uyumlu)\r\n");
    printf("    Sistem saat: HSI 8 MHz\r\n");
    printf("    Monitor    : 115200 baud\r\n");
    printf("    LoRa       : 9600 baud\r\n");

    Telemetry_Init(&tel_ctx);

    LoraStatus_t ls = Lora_Init(&lora_ctx, &huart2);
    if (ls == LORA_OK)
        printf("[OK] LoRa hazir.\r\n");
    else if (ls == LORA_ERR_TIMEOUT)
        printf("[WARN] LoRa AUX timeout - donanim kontrol edin.\r\n");
    else
        printf("[ERR] LoRa init hata: %d\r\n", (int)ls);

    Lora_SetRxByteHandler(&lora_ctx, on_lora_rx_byte, &tel_ctx);
    if (Lora_StartReceive(&lora_ctx) == LORA_OK)
        printf("[OK] LoRa RX dinleme aktif.\r\n");
    else
        printf("[ERR] LoRa RX baslatilamadi.\r\n");

    printf("\r\n--- AKS telemetry bekleniyor ---\r\n\r\n");

    while (1)
    {
        uint32_t now = HAL_GetTick();

        process_estop_tx();

        if ((now - last_heartbeat_ms) >= 3000U)
        {
            last_heartbeat_ms = now;
            const TelStats_t *s = Telemetry_GetStats(&tel_ctx);
            printf("[HB] t=%lu ms | rx_byte=%lu  good=%lu  bad=%lu  gap=%lu\r\n",
                   (unsigned long)now,
                   (unsigned long)s->rx_bytes,
                   (unsigned long)s->good_packets,
                   (unsigned long)(s->parse_fail + s->bad_tag +
                                   s->bad_version + s->range_fail),
                   (unsigned long)s->seq_gaps);
        }

        Telemetry_Tick(&tel_ctx, now);

        if (Telemetry_IsFrameReady(&tel_ctx))
        {
            TelData_t   d;
            TelStatus_t st = Telemetry_Parse(&tel_ctx, &d);
            Telemetry_PrintDashboard(&d, st,
                                     Telemetry_IsEStopActive(&tel_ctx));
        }
    }
}

/* ====================================================================
 * BUG #7 DUZELTME: USART1 115200 baud.
 * 9600 baud + 100 ms timeout kombinasyonu dashboard ciktisini (700 byte)
 * kesiyordu. 115200'de ayni veri ~60 ms, 200 ms timeout ile guvenli.
 * Seri monitor tarafinda da baud rate'i 115200'e guncelle.
 * ==================================================================== */
static void MX_USART1_UART_Init(void)
{
    huart1.Instance          = USART1;
    huart1.Init.BaudRate     = 115200;  /* 9600'den degistirildi */
    huart1.Init.WordLength   = UART_WORDLENGTH_8B;
    huart1.Init.StopBits     = UART_STOPBITS_1;
    huart1.Init.Parity       = UART_PARITY_NONE;
    huart1.Init.Mode         = UART_MODE_TX_RX;
    huart1.Init.HwFlowCtl    = UART_HWCONTROL_NONE;
    huart1.Init.OverSampling = UART_OVERSAMPLING_16;
    if (HAL_UART_Init(&huart1) != HAL_OK) Error_Handler();
}

/* ====================================================================
 * USART2 — LoRa E32 (PA2 TX / PA3 RX), 9600 baud (AKS ile sabit)
 * ==================================================================== */
static void MX_USART2_UART_Init(void)
{
    huart2.Instance          = USART2;
    huart2.Init.BaudRate     = 9600;
    huart2.Init.WordLength   = UART_WORDLENGTH_8B;
    huart2.Init.StopBits     = UART_STOPBITS_1;
    huart2.Init.Parity       = UART_PARITY_NONE;
    huart2.Init.Mode         = UART_MODE_TX_RX;
    huart2.Init.HwFlowCtl    = UART_HWCONTROL_NONE;
    huart2.Init.OverSampling = UART_OVERSAMPLING_16;
    if (HAL_UART_Init(&huart2) != HAL_OK) Error_Handler();
}

static void MX_GPIO_Init(void)
{
    GPIO_InitTypeDef g = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();

    HAL_GPIO_WritePin(MOTOR_EN_GPIO_Port, MOTOR_EN_Pin, GPIO_PIN_SET);

    g.Pin  = AC_L_STOP_Pin;
    g.Mode = GPIO_MODE_IT_FALLING;
    g.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(AC_L_STOP_GPIO_Port, &g);

    g.Pin   = MOTOR_EN_Pin;
    g.Mode  = GPIO_MODE_OUTPUT_PP;
    g.Pull  = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(MOTOR_EN_GPIO_Port, &g);

    HAL_NVIC_SetPriority(EXTI0_IRQn, 0, 0);
    HAL_NVIC_EnableIRQ(EXTI0_IRQn);

    HAL_NVIC_SetPriority(USART2_IRQn, 1, 0);
    HAL_NVIC_EnableIRQ(USART2_IRQn);
}

/* ====================================================================
 * Klon-guvenli saat: HSI 8 MHz, PLL yok, FLASH_LATENCY_0.
 * NOT: USART1 115200 baud @ 8 MHz HCLK — APB2 prescaler 1 (PCLK2=8MHz).
 * STM32F103 USART1 baud hesabi: 8000000 / 115200 = 69.4 -> BRR=0x45 (hata ~0.16%)
 * Bu hata orani UART icin kabul edilebilir sinirin (<2%) cok altinda.
 * ==================================================================== */
void SystemClock_Config(void)
{
    RCC_OscInitTypeDef o = {0};
    RCC_ClkInitTypeDef c = {0};

    o.OscillatorType      = RCC_OSCILLATORTYPE_HSI;
    o.HSIState            = RCC_HSI_ON;
    o.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
    o.PLL.PLLState        = RCC_PLL_NONE;
    if (HAL_RCC_OscConfig(&o) != HAL_OK) Error_Handler();

    c.ClockType      = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
                       RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    c.SYSCLKSource   = RCC_SYSCLKSOURCE_HSI;
    c.AHBCLKDivider  = RCC_SYSCLK_DIV1;
    c.APB1CLKDivider = RCC_HCLK_DIV1;
    c.APB2CLKDivider = RCC_HCLK_DIV1;
    if (HAL_RCC_ClockConfig(&c, FLASH_LATENCY_0) != HAL_OK) Error_Handler();
}

void Error_Handler(void)
{
    __disable_irq();
    while (1) { }
}