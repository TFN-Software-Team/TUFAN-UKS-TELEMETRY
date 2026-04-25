/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : UKS Yer Istasyonu - AKS protokolu entegre.
  *
  *  - USART1 (PA9/PA10): PC monitor (printf), 9600 baud.
  *  - USART2 (PA2/PA3) : LoRa E32-433T30D, 9600 baud (AKS uyumlu).
  *  - LoRa modulu RX kesmesi acik, byte byte telemetry parser'a gider.
  *  - PA0 EXTI: Acil durdurma butonu (operator).
  *      * MOTOR_EN pini LOW yapilir (donanimsal kesme).
  *      * UKS lokal E-STOP latchlenir.
  *      * Ana donguden 3x 0xA1 burst LoRa ile AKS'e gonderilir.
  *  - Ana donguden alinan telemetry frame'leri dashboard ile yazdirilir.
  ******************************************************************************
  */
/* USER CODE END Header */

#include "main.h"
#include "telemetry.h"
#include "lora.h"
#include <stdio.h>
#include <string.h>

/* ========== Donanim Handle'lari ========== */
UART_HandleTypeDef huart1;   /* PC monitor */
UART_HandleTypeDef huart2;   /* LoRa E32   */

/* ========== Sistem Durumu ========== */
TelCtx_t           tel_ctx;
static LoraCtx_t   lora_ctx;

static uint32_t          last_heartbeat_ms = 0;
static uint32_t          last_button_press = 0;
static volatile uint8_t  estop_tx_pending  = 0;

/* ========== Prototipler ========== */
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_USART1_UART_Init(void);
static void MX_USART2_UART_Init(void);

/* ========== printf -> USART1 yonlendirmesi ========== */
int _write(int file, char *ptr, int len)
{
    (void)file;
    if (HAL_UART_Transmit(&huart1, (uint8_t *)ptr, (uint16_t)len, 100) != HAL_OK)
        return -1;
    return len;
}

/* ====================================================================
 * LoRa RX -> Telemetry parser kopru fonksiyonu
 * Lora_OnUartRxCplt her byte icin bunu cagirir (ISR context).
 * ==================================================================== */
static void on_lora_rx_byte(uint8_t b, uint32_t now_ms, void *user)
{
    TelCtx_t *t = (TelCtx_t *)user;
    Telemetry_RxBytePush(t, b, now_ms);
}

/* ====================================================================
 * HAL UART callback'leri — LoRa modulune delege.
 * ==================================================================== */
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
 *
 * ISR icinde:
 *   - Donanim cikisi (MOTOR_EN) hemen LOW
 *   - Lokal E-STOP latch
 *   - LoRa TX flag'i set — gercek gonderim ana donguden yapilir
 *     (Lora_Send AUX bekler, ISR'da blokaj kabul edilemez).
 * ==================================================================== */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    if (GPIO_Pin != AC_L_STOP_Pin) return;

    uint32_t now = HAL_GetTick();
    if ((now - last_button_press) <= 200U) return;   /* debounce */
    last_button_press = now;

    /* 1) Donanim — anlik */
    HAL_GPIO_WritePin(MOTOR_EN_GPIO_Port, MOTOR_EN_Pin, GPIO_PIN_RESET);

    /* 2) UKS lokal latch (idempotent) */
    Telemetry_SetEStopActive(&tel_ctx);

    /* 3) AKS'e bildirim — ana dongu gonderir */
    estop_tx_pending = 1;
}

/* ====================================================================
 * Ana dongu yardimcilari
 * ==================================================================== */
static void process_estop_tx(void)
{
    if (!estop_tx_pending) return;
    estop_tx_pending = 0;

    uint8_t buf[TEL_ESTOP_BURST_COUNT];
    uint8_t n = Telemetry_EncodeEStopBurst(buf, sizeof(buf));

    LoraStatus_t s = Lora_Send(&lora_ctx, buf, n);
    if (s == LORA_OK)
    {
        tel_ctx.stats.estop_tx_count++;
        printf("\r\n!!! E-STOP -> AKS (0xA1 x%u) gonderildi !!!\r\n\r\n",
               (unsigned)n);
    }
    else
    {
        printf("\r\n!! E-STOP gonderilemedi (LoRa status=%d) !!\r\n\r\n",
               (int)s);
    }
}

/**
  * @brief  Uygulama Giris Noktasi
  */
int main(void)
{
    HAL_Init();
    SystemClock_Config();

    MX_GPIO_Init();
    MX_USART1_UART_Init();   /* PC monitor 9600 */
    MX_USART2_UART_Init();   /* LoRa 9600       */

    setvbuf(stdout, NULL, _IONBF, 0);

    printf("\r\n>>> UKS YER ISTASYONU BASLATILIYOR <<<\r\n");
    printf("    Telemetry  : ASCII CSV (15 alan, AKS uyumlu)\r\n");
    printf("    LoRa baud  : 9600\r\n");

    /* Telemetri modulu */
    Telemetry_Init(&tel_ctx);

    /* LoRa modulu */
    LoraStatus_t ls = Lora_Init(&lora_ctx, &huart2);
    if (ls == LORA_OK)
    {
        printf("[OK] LoRa hazir (AUX HIGH).\r\n");
    }
    else if (ls == LORA_ERR_TIMEOUT)
    {
        /* Boot AUX timeout — modul takili degil ya da yavas. Yine de
         * RX'i deneriz; kullanici loga gore sorun teshis eder. */
        printf("[WARN] LoRa AUX timeout - donanim kontrol edin.\r\n");
    }
    else
    {
        printf("[ERR] LoRa init hata: %d\r\n", (int)ls);
    }

    /* RX'i baslat — gelen byte'lar telemetry parser'a yonlenecek */
    Lora_SetRxByteHandler(&lora_ctx, on_lora_rx_byte, &tel_ctx);
    if (Lora_StartReceive(&lora_ctx) == LORA_OK)
    {
        printf("[OK] LoRa RX dinleme aktif.\r\n");
    }
    else
    {
        printf("[ERR] LoRa RX baslatilamadi.\r\n");
    }

    printf("\r\n--- AKS telemetry bekleniyor ---\r\n\r\n");

    /* Ana Dongu */
    while (1)
    {
        uint32_t now = HAL_GetTick();

        /* 1) E-STOP butonuna basildiysa AKS'e bildir */
        process_estop_tx();

        /* 2) Heartbeat (3s) */
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

        /* 3) Yarim satir timeout kontrolu */
        Telemetry_Tick(&tel_ctx, now);

        /* 4) Yeni telemetry frame geldi mi? */
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
 * USART1 — PC monitor (PA9 TX / PA10 RX), 9600 baud
 * ==================================================================== */
static void MX_USART1_UART_Init(void)
{
    huart1.Instance          = USART1;
    huart1.Init.BaudRate     = 9600;
    huart1.Init.WordLength   = UART_WORDLENGTH_8B;
    huart1.Init.StopBits     = UART_STOPBITS_1;
    huart1.Init.Parity       = UART_PARITY_NONE;
    huart1.Init.Mode         = UART_MODE_TX_RX;
    huart1.Init.HwFlowCtl    = UART_HWCONTROL_NONE;
    huart1.Init.OverSampling = UART_OVERSAMPLING_16;
    if (HAL_UART_Init(&huart1) != HAL_OK) Error_Handler();
}

/* ====================================================================
 * USART2 — LoRa E32 (PA2 TX / PA3 RX), 9600 baud
 *  AKS UART_LoRa_Protocol.md doc'una gore 9600 baud zorunlu.
 * ==================================================================== */
static void MX_USART2_UART_Init(void)
{
    huart2.Instance          = USART2;
    huart2.Init.BaudRate     = 9600;          /* AKS ile birebir */
    huart2.Init.WordLength   = UART_WORDLENGTH_8B;
    huart2.Init.StopBits     = UART_STOPBITS_1;
    huart2.Init.Parity       = UART_PARITY_NONE;
    huart2.Init.Mode         = UART_MODE_TX_RX;
    huart2.Init.HwFlowCtl    = UART_HWCONTROL_NONE;
    huart2.Init.OverSampling = UART_OVERSAMPLING_16;
    if (HAL_UART_Init(&huart2) != HAL_OK) Error_Handler();
}

/* ====================================================================
 * GPIO baslatma
 * ==================================================================== */
static void MX_GPIO_Init(void)
{
    GPIO_InitTypeDef g = {0};

    __HAL_RCC_GPIOD_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();

    /* MOTOR_EN baslangicta HIGH (nominal calisma) */
    HAL_GPIO_WritePin(MOTOR_EN_GPIO_Port, MOTOR_EN_Pin, GPIO_PIN_SET);

    /* PA0: E-STOP butonu, EXTI falling, pull-up */
    g.Pin  = AC_L_STOP_Pin;
    g.Mode = GPIO_MODE_IT_FALLING;
    g.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(AC_L_STOP_GPIO_Port, &g);

    /* PB11: MOTOR_EN cikisi */
    g.Pin   = MOTOR_EN_Pin;
    g.Mode  = GPIO_MODE_OUTPUT_PP;
    g.Pull  = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(MOTOR_EN_GPIO_Port, &g);

    /* EXTI0 (PA0 E-STOP) — yuksek oncelik */
    HAL_NVIC_SetPriority(EXTI0_IRQn, 0, 0);
    HAL_NVIC_EnableIRQ(EXTI0_IRQn);

    /* USART2 RX kesmesi onceligi (LoRa byte stream) — E-STOP'tan dusuk */
    HAL_NVIC_SetPriority(USART2_IRQn, 1, 0);
    HAL_NVIC_EnableIRQ(USART2_IRQn);
}

/* ====================================================================
 * Saat: HSI -> PLL x16 -> 64 MHz (HSE bagimliligi yok)
 * ==================================================================== */
void SystemClock_Config(void)
{
    RCC_OscInitTypeDef o = {0};
    RCC_ClkInitTypeDef c = {0};

    o.OscillatorType      = RCC_OSCILLATORTYPE_HSI;
    o.HSIState            = RCC_HSI_ON;
    o.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
    o.PLL.PLLState        = RCC_PLL_ON;
    o.PLL.PLLSource       = RCC_PLLSOURCE_HSI_DIV2;
    o.PLL.PLLMUL          = RCC_PLL_MUL16;        /* 4MHz * 16 = 64 MHz */
    if (HAL_RCC_OscConfig(&o) != HAL_OK) Error_Handler();

    c.ClockType      = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
                       RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    c.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
    c.AHBCLKDivider  = RCC_SYSCLK_DIV1;
    c.APB1CLKDivider = RCC_HCLK_DIV2;
    c.APB2CLKDivider = RCC_HCLK_DIV1;
    if (HAL_RCC_ClockConfig(&c, FLASH_LATENCY_2) != HAL_OK) Error_Handler();
}

void Error_Handler(void)
{
    __disable_irq();
    while (1) { }
}
