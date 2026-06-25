/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : UKS Yer Istasyonu — AKS protokolu entegre.
  *
  *  Duzeltmeler:
  *  BUG #4 : printf → USART1 yonlendirmesi (_write / __io_putchar)
  *  BUG #5 : E-STOP TX timeout 50 ms
  *  BUG #6 : Klon-guvenli saat — HSI 8 MHz, PLL yok
  *  BUG #7 : USART1 115200 baud (9600'de dashboard 730 ms sürüyordu)
  *  BUG #8 : Debounce boot edge case — (uint32_t)(-2000) ile baslatma
  *  FIX-E32: M0/M1 artik PB6/PB7 — Lora_Init() icinde config moduna
  *           alinip E32_CFG_* degerleri flash'a kalici olarak yazilir.
  *           Eski kodda bu pinler floating kaliyor, modul mod belirsiz
  *           hale geliyordu → rx_byte = 0.
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

static uint32_t         last_heartbeat_ms = 0;

/*
 * BUG #8 DUZELTME: Boot'ta E-STOP kilitlenmesi.
 * 0 ile baslatilinca ilk 200 ms'de buton yok sayiliyordu.
 * (uint32_t)(-2000) → wraparound ile now=0'da fark=2000 > 200 → gecilir.
 */
static uint32_t         last_button_press = (uint32_t)(-2000);

static volatile uint8_t estop_tx_pending  = 0;

/* ========== Prototip ========== */
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_USART1_UART_Init(void);
static void MX_USART2_UART_Init(void);

/* ====================================================================
 * BUG #4 DUZELTME: printf → USART1 yonlendirmesi.
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
 * LoRa RX → Telemetry parser koprusu (ISR context)
 * ==================================================================== */
static void on_lora_rx_byte(uint8_t b, uint32_t now_ms, void *user)
{
    Telemetry_RxBytePush((TelCtx_t *)user, b, now_ms);
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
 * E-STOP butonu (PA0, falling edge, pull-up)
 * ==================================================================== */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    if (GPIO_Pin != AC_L_STOP_Pin) return;

    uint32_t now = HAL_GetTick();
    if ((now - last_button_press) <= 200U) return;   /* debounce */
    last_button_press = now;

    HAL_GPIO_WritePin(MOTOR_EN_GPIO_Port, MOTOR_EN_Pin, GPIO_PIN_RESET);
    Telemetry_SetEStopActive(&tel_ctx);
    estop_tx_pending = 1U;
}

/* ====================================================================
 * BUG #5 DUZELTME: E-STOP TX — 50 ms timeout.
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

/* ====================================================================
 * main
 * ==================================================================== */
int main(void)
{
    HAL_Init();
    SystemClock_Config();

    MX_GPIO_Init();
    MX_USART1_UART_Init();
    MX_USART2_UART_Init();

    setvbuf(stdout, NULL, _IONBF, 0);

    printf("\r\n>>> UKS YER ISTASYONU BASLATILIYOR <<<\r\n");
    printf("    Protokol  : ASCII CSV, 15 alan, AKS uyumlu\r\n");
    printf("    Saat      : HSI 8 MHz (PLL yok)\r\n");
    printf("    Monitor   : USART1 115200 baud\r\n");
    printf("    LoRa UART : USART2 9600 baud\r\n");
    printf("    E32 ayar  : SPED=0xC2 (9600|8N1|2.4kbps) "
           "CHAN=0x17 (433 MHz) OPTION=0x47 (30dBm|FEC)\r\n");

    Telemetry_Init(&tel_ctx);

    /*
     * Lora_Init() siralari:
     *   1. PB6/PB7 → output LOW  (normal mod, floating degil)
     *   2. AUX HIGH bekle         (E32 boot tamamlansin)
     *   3. Config moduna gec
     *   4. E32_CFG_* degerlerini flash'a yaz + echo dogrula
     *   5. Normal moda don
     */
    LoraStatus_t ls = Lora_Init(&lora_ctx, &huart2);
    if (ls == LORA_OK)
        printf("[OK] LoRa hazir: 30 dBm | 2.4 kbps hava hizi | 433 MHz\r\n");
    else if (ls == LORA_ERR_TIMEOUT)
        printf("[WARN] LoRa AUX zaman asimi — donanim / besleme kontrol edin.\r\n");
    else
        printf("[ERR] LoRa config echo hatasi (ls=%d) — modul calisiyor "
               "olabilir ama ayarlar yazilmamis.\r\n", (int)ls);

    Lora_SetRxByteHandler(&lora_ctx, on_lora_rx_byte, &tel_ctx);

    if (Lora_StartReceive(&lora_ctx) == LORA_OK)
        printf("[OK] LoRa RX interrupt aktif.\r\n");
    else
        printf("[ERR] LoRa RX baslatilamadi.\r\n");

    printf("\r\n--- AKS telemetri bekleniyor ---\r\n\r\n");

    /* ---- Ana dongu ---- */
    while (1)
    {
        uint32_t now = HAL_GetTick();

        /* E-STOP gonderimi (ISR'dan set edilen flag) */
        process_estop_tx();

        /* Heartbeat: her 3 saniyede istatistik bas */
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

        /* Yarim satir timeout kontrolu */
        Telemetry_Tick(&tel_ctx, now);

        /* Hazir frame varsa dashboard'u guncelle */
        if (Telemetry_IsFrameReady(&tel_ctx))
        {
            TelData_t   d;
            TelStatus_t st = Telemetry_Parse(&tel_ctx, &d);
            Telemetry_PrintDashboard(&d, st, Telemetry_IsEStopActive(&tel_ctx));
        }
    }
}

/* ====================================================================
 * BUG #7 DUZELTME: USART1 115200 baud.
 * 9600 baud + 200 ms timeout → dashboard (~700 byte) 730 ms suruyor,
 * timeout asimina giriyor. 115200'de ~60 ms, 200 ms timeout guvenli.
 * ==================================================================== */
static void MX_USART1_UART_Init(void)
{
    huart1.Instance          = USART1;
    huart1.Init.BaudRate     = 115200;
    huart1.Init.WordLength   = UART_WORDLENGTH_8B;
    huart1.Init.StopBits     = UART_STOPBITS_1;
    huart1.Init.Parity       = UART_PARITY_NONE;
    huart1.Init.Mode         = UART_MODE_TX_RX;
    huart1.Init.HwFlowCtl    = UART_HWCONTROL_NONE;
    huart1.Init.OverSampling = UART_OVERSAMPLING_16;
    if (HAL_UART_Init(&huart1) != HAL_OK) Error_Handler();
}

/* ====================================================================
 * USART2 — LoRa E32 (PA2=TX, PA3=RX), 9600 baud
 * E32 fabrika UART hizi 9600 baud. E32_CFG_SPED de 9600 yazilir.
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

/* ====================================================================
 * GPIO Init.
 * NOT: PB6 (E32_M0) ve PB7 (E32_M1) BURADA degil, Lora_Init() →
 *      E32_GPIO_Init() icinde ayarlaniyor. MX_GPIO_Init() cagrildiginda
 *      bu pinler henuz output degil; LORA_UART init'ten once modulu
 *      config moduna almamak icin bu siralama kasitlidir.
 * ==================================================================== */
static void MX_GPIO_Init(void)
{
    GPIO_InitTypeDef g = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();

    /* PB11 (MOTOR_EN) once HIGH: acil durumda fail-safe */
    HAL_GPIO_WritePin(MOTOR_EN_GPIO_Port, MOTOR_EN_Pin, GPIO_PIN_SET);

    /* PA0 — E-STOP butonu: falling edge + pull-up */
    g.Pin  = AC_L_STOP_Pin;
    g.Mode = GPIO_MODE_IT_FALLING;
    g.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(AC_L_STOP_GPIO_Port, &g);

    /* PB11 — MOTOR_EN cikisi */
    g.Pin   = MOTOR_EN_Pin;
    g.Mode  = GPIO_MODE_OUTPUT_PP;
    g.Pull  = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(MOTOR_EN_GPIO_Port, &g);

    /* EXTI0 (E-STOP) — en yuksek oncelik */
    HAL_NVIC_SetPriority(EXTI0_IRQn, 0, 0);
    HAL_NVIC_EnableIRQ(EXTI0_IRQn);

    /* USART2 interrupt — EXTI'den dusuk oncelik */
    HAL_NVIC_SetPriority(USART2_IRQn, 1, 0);
    HAL_NVIC_EnableIRQ(USART2_IRQn);
}

/* ====================================================================
 * BUG #6 DUZELTME: Klon-guvenli saat — HSI 8 MHz, PLL yok.
 *
 * PLL kullanilinca klonlanmis F103'lerde (HSE yok veya kristal
 * frekans farki var) saat hatali calisor. HSI ile bu risk ortadan
 * kalkar.
 *
 * USART1 @ 115200 baud, HSI 8 MHz:
 *   BRR = 8_000_000 / 115200 ≈ 69.4 → ~0.16% hata (sinir: <2%) ✓
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

    c.ClockType      = RCC_CLOCKTYPE_HCLK   | RCC_CLOCKTYPE_SYSCLK |
                       RCC_CLOCKTYPE_PCLK1  | RCC_CLOCKTYPE_PCLK2;
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