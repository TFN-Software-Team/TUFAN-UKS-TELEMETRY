/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : UKS Yer Istasyonu — AKS protokolu entegre.
  *
  *  Duzeltmeler:
  *  BUG #4 : printf → USART1 yonlendirmesi (__io_putchar)
  *  BUG #5 : E-STOP TX timeout 50 ms
  *  BUG #6 : Klon-guvenli saat — HSI 8 MHz, PLL yok
  *  BUG #7 : USART1 115200 baud (9600'de dashboard 730 ms sürüyordu)
  *  BUG #8 : Debounce boot edge case — (uint32_t)(-2000) ile baslatma
  *  FIX-E22: M0/M1 artik PB6/PB7 — Lora_Init() icinde config moduna
  *           alinip register blogu (e22_regs.h hedefleri) flash'a kalici
  *           olarak yazilir (read-before-write: zaten hedefle ayniysa
  *           yazmaz). Eski kodda bu pinler floating kaliyor, modul mod
  *           belirsiz hale geliyordu → rx_byte = 0.
  *  E32->E22: LoRa donanimi E32-433T30D'den E22-400T30D-V2'ye (SX1268,
  *           30 dBm) geciyor; config protokolu register-tabanli C0/C1
  *           komutlarina degisti (bkz. Core/Inc/e22_regs.h, Core/Src/lora.c).
  *
  *  YENI DUZELTMELER:
  *  9.2.a  : UKS -> AKS komut kanali (eski 0xA1-0xA4) ve arac ustu acil
  *           durdurma girisinin donanim zinciri sistemden tamamen
  *           kaldirildi — RF hatti tek yonlu telemetri + 0xB0
  *           heartbeat'tir. Acil durdurma arac ustundeki fiziksel
  *           kontaktorle saglanir, RF'ten bagimsizdir.
  *  FIX-B : printf PicoLibc uzerinden calisir (starm_putc -> __io_putchar).
  *           _write main.c'de strong tanimli; syscalls.c'deki
  *           __strong_reference(_write, write) icin gerekli, KALDIRILAMAZ.
  *  FIX-C : Dashboard her frame yerine her DASH_EVERY_N frame'de bir
  *           basiliyor. Tum frame'ler yine parse edilir; sadece blocking
  *           ekran ciktisi seyreltilir, RX'e nefes alani birakilir.
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

/* Heartbeat TX (0xB0) — UYUM_NOTU.md bolum 2: madde 9.2.a'nin izin verdigi
 * stabilizasyon-teyidi geri bildirimi. RF hattindaki TEK TX kaynagidir. */
static uint32_t         last_heartbeat_tx_ms = 0;

/* Link-down tespiti: son gecerli TEL frame'inin alindigi tick.
 * 0 = henuz hic gecerli frame alinmadi (boot durumu, timeout tetiklemez). */
static uint32_t         last_valid_tel_tick = 0;
static uint8_t          link_down = 0;

/* FIX-C: dashboard throttle — her DASH_EVERY_N frame'de bir bas */
#define DASH_EVERY_N   3U
static uint32_t         dash_frame_counter = 0;

/* ========== Prototip ========== */
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_USART1_UART_Init(void);
static void MX_USART2_UART_Init(void);

/* ====================================================================
 * BUG #4 / FIX-B: printf → USART1 yonlendirmesi.
 *
 * Bu proje PicoLibc kullaniyor (syscalls.c icinde __PICOLIBC__ + FDEV
 * stream). PicoLibc'te printf zinciri:
 *     printf -> stdout (FDEV_SETUP_STREAM) -> starm_putc -> __io_putchar
 * Yani karakter cikisi __io_putchar uzerindendir; _write DOGRUDAN bu zincire
 * girmez. _write yine de gereklidir cunku syscalls.c'deki
 *     __strong_reference(_write, write);
 * satiri _write sembolunun TANIMLI olmasini sart kosar. _write yalnizca
 * burada (main.c, strong) tanimli; syscalls.c onu bilerek tanimlamaz.
 * Dolayisiyla _write'i buradan KALDIRMA — link hatasi olur (undefined _write).
 * ==================================================================== */
int __io_putchar(int ch)
{
    uint8_t c = (uint8_t)ch;
    HAL_UART_Transmit(&huart1, &c, 1, 200);
    return ch;
}

/* _write — PicoLibc'in __strong_reference(_write, write) baglamasi icin
 * gerekli. POSIX write() cagrisi gelirse de byte'lari __io_putchar'a yansitir. */
int _write(int file, char *ptr, int len)
{
    (void)file;
    for (int i = 0; i < len; i++)
        __io_putchar((unsigned char)ptr[i]);
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
    printf("    Protokol  : ASCII CSV v%u, %u alan, AKS uyumlu\r\n",
           (unsigned)TEL_PROTOCOL_VERSION, (unsigned)TEL_FIELD_COUNT);
    printf("    RF hatti tek yonlu (9.2.a): TX yalnizca heartbeat\r\n");
    printf("    Saat      : HSI 8 MHz (PLL yok)\r\n");
    printf("    Monitor   : USART1 115200 baud\r\n");
    printf("    LoRa UART : USART2 9600 baud\r\n");
    /* Boot mesaji e22_regs.h HEDEF degerlerinden turetilir — hicbir sayi
     * burada ayrica hardcode edilmez (bkz. e22_regs.h decode yardimcilari). */
    {
        uint32_t freq_x1000 = E22_DecodeFreqMhzX1000(E22_VAL_REG2);
        printf("    E22 hedef : UART=%lu baud | hava hizi=%lu bps | "
               "guc kademesi=%u (0=en yuksek) | %lu.%03lu MHz\r\n",
               (unsigned long)E22_DecodeUartBaud(E22_VAL_REG0),
               (unsigned long)E22_DecodeAirRateBps(E22_VAL_REG0),
               (unsigned)E22_DecodeTxPowerStep(E22_VAL_REG1),
               (unsigned long)(freq_x1000 / 1000U),
               (unsigned long)(freq_x1000 % 1000U));
        printf("      (REG0=0x%02X REG1=0x%02X REG2=0x%02X — bkz. Core/Inc/e22_regs.h)\r\n",
               (unsigned)E22_VAL_REG0, (unsigned)E22_VAL_REG1, (unsigned)E22_VAL_REG2);
    }

    Telemetry_Init(&tel_ctx);

    /*
     * Lora_Init() siralari:
     *   1. PB6/PB7 → output LOW  (normal mod, floating degil)
     *   2. AUX HIGH bekle         (E22 boot tamamlansin)
     *   3. Config moduna gec      (M0=0, M1=1 — E32'den FARKLI)
     *   4. Register blogunu (ADDH..CRYPT_L) oku + hex dump'la; hedeften
     *      (e22_regs.h) farkliysa C0 ile flash'a yaz ve dogrula
     *   5. Normal moda don
     */
    LoraStatus_t ls = Lora_Init(&lora_ctx, &huart2);
    if (ls == LORA_OK)
    {
        uint32_t freq_x1000 = E22_DecodeFreqMhzX1000(E22_VAL_REG2);
        printf("[OK] LoRa hazir: guc kademesi %u (0=en yuksek, T30D max 30 dBm) | "
               "%lu bps hava hizi | %lu.%03lu MHz\r\n",
               (unsigned)E22_DecodeTxPowerStep(E22_VAL_REG1),
               (unsigned long)E22_DecodeAirRateBps(E22_VAL_REG0),
               (unsigned long)(freq_x1000 / 1000U),
               (unsigned long)(freq_x1000 % 1000U));
    }
    else if (ls == LORA_ERR_TIMEOUT)
        printf("[WARN] LoRa AUX zaman asimi — donanim / besleme kontrol edin.\r\n");
    else
        printf("[ERR] LoRa config okuma/yazma hatasi (ls=%d) — FF FF FF ya da "
               "beklenmeyen yanit basligi; modul calisiyor olabilir ama "
               "ayarlar teyit edilemedi.\r\n", (int)ls);

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

        /* Heartbeat TX (0xB0): AKS'e periyodik "canliyim" sinyali.
         * Best-effort — basarisizsa sessizce atlanir ve bir sonraki
         * periyotta tekrar denenir. */
        if ((now - last_heartbeat_tx_ms) >= LORA_HEARTBEAT_PERIOD_MS)
        {
            last_heartbeat_tx_ms = now;

            uint8_t hb = LORA_HEARTBEAT_BYTE;
            (void)Lora_Send(&lora_ctx, &hb, 1U);
        }

        /* Link-down tespiti: TEL_LINK_TIMEOUT_MS suredir gecerli TEL
         * frame'i gelmediyse baglanti kopmus sayilir. LINK,UP gecisi
         * asagida, gecerli bir TEL_VALID frame parse edildiginde yapilir
         * (st o noktada bilinir). */
        if (!link_down &&
            last_valid_tel_tick != 0U &&
            (now - last_valid_tel_tick) > TEL_LINK_TIMEOUT_MS)
        {
            link_down = 1U;
            printf("LINK,DOWN,%lu\r\n", (unsigned long)now);
        }

        /* Heartbeat: her 3 saniyede istatistik bas */
        if ((now - last_heartbeat_ms) >= 3000U)
        {
            last_heartbeat_ms = now;

            /* WATCHDOG: rx_active=0 ise RX bir noktada donanim kilitleniyor
             * demektir (ORE/BUSY sonrasi StartReceive basarisiz olmus).
             * Hic kurtarma yoksa UKS arac kapanana kadar sonsuza kadar
             * sagir kalir. Burada sessizce yeniden arm et. */
            if (lora_ctx.rx_active == 0U)
            {
                printf("[WARN] RX donanim kilidi algilandi (rx_active=0) — "
                       "yeniden baslatiliyor...\r\n");
                if (Lora_StartReceive(&lora_ctx) == LORA_OK)
                    printf("[OK] RX kurtarildi.\r\n");
                else
                    printf("[ERR] RX kurtarma basarisiz — donanim/LoRa kontrol et.\r\n");
            }

            const TelStats_t *s = Telemetry_GetStats(&tel_ctx);
            printf("[HB] t=%lu ms | rx_byte=%lu  good=%lu  bad=%lu  gap=%lu\r\n",
                   (unsigned long)now,
                   (unsigned long)s->rx_bytes,
                   (unsigned long)s->good_packets,
                   (unsigned long)(s->parse_fail + s->bad_tag +
                                   s->bad_version + s->range_fail +
                                   s->ring_overflow + s->timeout_drop),
                   (unsigned long)s->seq_gaps);
        }

        /* FIX (v3): Ring buffer'daki ham byte'lari ISLE.
         * Parse/decode artik burada (main context) yapilir; ISR sadece
         * ring'e yaziyor. Bu cagri Tick'ten ONCE olmali ki bu turda gelen
         * byte'lar islensin, ardindan yarim-satir timeout'u dogru hesaplansin. */
        Telemetry_Process(&tel_ctx, now);

        /* Yarim satir timeout kontrolu */
        Telemetry_Tick(&tel_ctx, now);

        /* Hazir frame varsa: her zaman PARSE et, ekrani throttle'la bas */
        if (Telemetry_IsFrameReady(&tel_ctx))
        {
            TelData_t   d;
            TelStatus_t st = Telemetry_Parse(&tel_ctx, &d);

            if (st == TEL_VALID)
            {
                last_valid_tel_tick = now;

                if (link_down)
                {
                    link_down = 0U;
                    printf("LINK,UP,%lu\r\n", (unsigned long)now);
                }

                /* PC izleme merkezi icin makine-okunur CSV forward satiri.
                 * Dashboard throttle'indan (DASH_EVERY_N) BAGIMSIZ — her
                 * gecerli pakette basilir. T_bat_C=tempH, V_bat=packV/10,
                 * hiz=spd_x10/10; kalan enerji PC tarafinda soc'ten turetilir. */
                printf("CSV,%lu,%u,%d,%u,%u,%lu\r\n",
                       (unsigned long)d.timestamp_ms,
                       (unsigned)d.speed_kmh_x10,
                       (int)d.bms_temp_highest_c,
                       (unsigned)d.bms_pack_voltage_deciv,
                       (unsigned)d.bms_soc_hundredths,
                       (unsigned long)d.sequence);
            }

            /* FIX-C: dashboard ciktisini seyrelt — RX'e nefes alani */
            if ((++dash_frame_counter % DASH_EVERY_N) == 0U)
            {
                Telemetry_PrintDashboard(&d, st, link_down);
            }
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
 * USART2 — LoRa E22-400T30D-V2 (PA2=TX, PA3=RX), 9600 baud
 * E22 fabrika UART hizi 9600 baud. REG0 de 9600 yazilir (e22_regs.h).
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
 * NOT: PB6 (E22_M0) ve PB7 (E22_M1) BURADA degil, Lora_Init() →
 *      E22_GPIO_Init() icinde ayarlaniyor. MX_GPIO_Init() cagrildiginda
 *      bu pinler henuz output degil; LORA_UART init'ten once modulu
 *      config moduna almamak icin bu siralama kasitlidir.
 * ==================================================================== */
static void MX_GPIO_Init(void)
{
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();

    /* Kesme oncelikleri: SysTick en yuksekte (0) kalir ki HAL_GetTick()
     * tabanli timeout'lar (ornegin UART TX/RX) her zaman ilerlesin.
     * USART2 NVIC onceligi/enable'i HAL_UART_MspInit icinde ayarlanir. */
    HAL_NVIC_SetPriority(SysTick_IRQn, 0, 0);
}

/* ====================================================================
 * BUG #6 DUZELTME: Klon-guvenli saat — HSI 8 MHz, PLL yok.
 *
 * PLL kullanilinca klonlanmis F103'lerde (HSE yok veya kristal
 * frekans farki var) saat hatali calisor. HSI ile bu risk ortadan
 * kalkar.
 *
 * USART1 @ 115200 baud, HSI 8 MHz:
 *   BRR = 8_000_000 / 115200 ≈ 69.4 → ~0.64% hata — 115200 icin kabul
 *   edilebilir tolerans icinde (sinir: <%2)
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