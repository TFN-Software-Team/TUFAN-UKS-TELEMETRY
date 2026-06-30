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
  *  FIX-E32: M0/M1 artik PB6/PB7 — Lora_Init() icinde config moduna
  *           alinip E32_CFG_* degerleri flash'a kalici olarak yazilir.
  *           Eski kodda bu pinler floating kaliyor, modul mod belirsiz
  *           hale geliyordu → rx_byte = 0.
  *
  *  YENI DUZELTMELER:
  *  FIX-A (KRITIK): E-STOP artik Lora_SendCritical ile gonderiliyor.
  *           Eski kod dogrudan HAL_UART_Transmit cagiriyordu; bu, devam
  *           eden IT-tabanli RX ile cakisip telemetri akisini donduruyor
  *           ve AUX-busy durumunda E-STOP dusebiliyordu. Yeni yol IT-RX'i
  *           guvenli durdurup TX yapar, sonra RX'i yeniden baslatir;
  *           AUX bloklamaz. Gonderim basarisizsa tekrar denenir.
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
 * stabilizasyon-teyidi geri bildirimi, komut kanalindan (0xA1-0xA4) bagimsiz. */
static uint32_t         last_heartbeat_tx_ms = 0;

/* Link-down tespiti: son gecerli TEL frame'inin alindigi tick.
 * 0 = henuz hic gecerli frame alinmadi (boot durumu, timeout tetiklemez). */
static uint32_t         last_valid_tel_tick = 0;
static uint8_t          link_down = 0;

/*
 * BUG #8 DUZELTME: Boot'ta E-STOP kilitlenmesi.
 * 0 ile baslatilinca ilk 200 ms'de buton yok sayiliyordu.
 * (uint32_t)(-2000) → wraparound ile now=0'da fark=2000 > 200 → gecilir.
 */
static volatile uint32_t last_button_press = (uint32_t)(-2000);

static volatile uint8_t estop_tx_pending  = 0;

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
 * FIX-A DUZELTME: E-STOP TX — Lora_SendCritical ile.
 * IT-RX guvenli durdurulur/yeniden baslatilir, AUX bloklamaz.
 *
 * FIX-D2 (retry throttle): Gonderim basarisizsa pending tekrar set edilir
 * AMA en az ESTOP_RETRY_MS gecmeden tekrar DENENMEZ. Aksi halde modul AUX'u
 * surekli LOW'da takiliysa (elektriksel ariza/RF parazit) ana dongu her
 * turda Lora_SendCritical'in 50 ms AUX beklemesine girer; while(1) saniyede
 * 6-7 kez doner, Telemetry_Process ac kalir ve printf hatti spam'lenir.
 * Throttle ile retry'lar seyreltilir, telemetri akisi korunur. E-STOP'un
 * ILK denemesi gecikmesizdir; yalnizca BASARISIZ tekrarlar throttle'lanir.
 * ==================================================================== */
#define ESTOP_RETRY_MS  100U

static void process_estop_tx(void)
{
    /* Wraparound init: boot'ta (uint32_t)(-ESTOP_RETRY_MS) ile basla ki
     * now=0..100 araliginda gelen ILK E-STOP bile throttle'a takilmadan
     * aninda gonderilsin. (last_button_press ile ayni desen.) */
    static uint32_t last_retry_ms = (uint32_t)(0U - ESTOP_RETRY_MS);

    __disable_irq();
    uint8_t pending = estop_tx_pending;
    estop_tx_pending = 0U;
    __enable_irq();
    if (!pending) return;

    /* Onceki deneme basarisiz olduysa, art arda spam'i onlemek icin bekle.
     * (last_retry_ms yalnizca basarisizlikta guncellenir; ilk denemede
     *  fark zaten buyuk oldugu icin gecikme yasanmaz.) */
    uint32_t now = HAL_GetTick();
    if ((now - last_retry_ms) < ESTOP_RETRY_MS)
    {
        estop_tx_pending = 1U;
        return;
    }

    uint8_t buf[TEL_ESTOP_BURST_COUNT];
    uint8_t n = Telemetry_EncodeEStopBurst(buf, sizeof(buf));

    LoraStatus_t ls = Lora_SendCritical(&lora_ctx, buf, n);
    if (ls == LORA_OK)
    {
        tel_ctx.stats.estop_tx_count++;
        printf("\r\n!!! E-STOP -> AKS (0xA1 x%u) gonderildi !!!\r\n\r\n",
               (unsigned)n);
    }
    else
    {
        estop_tx_pending = 1U;   /* kritik: tekrar dene (throttle'li) */
        last_retry_ms    = now;  /* basarisizlik anini kaydet */
        printf("\r\n!! E-STOP TX hatasi (ls=%d) — %lu ms sonra tekrar !!\r\n\r\n",
               (int)ls, (unsigned long)ESTOP_RETRY_MS);
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
    printf("    Protokol  : ASCII CSV v%u, %u alan, AKS uyumlu\r\n",
           (unsigned)TEL_PROTOCOL_VERSION, (unsigned)TEL_FIELD_COUNT);
    printf("    Saat      : HSI 8 MHz (PLL yok)\r\n");
    printf("    Monitor   : USART1 115200 baud\r\n");
    printf("    LoRa UART : USART2 9600 baud\r\n");
    printf("    E32 ayar  : SPED=0x%02X (9600 UART | 2.4kbps air)  "
           "CHAN=0x%02X  OPTION=0x%02X\r\n",
           E32_CFG_SPED, E32_CFG_CHAN, E32_CFG_OPTION);
    printf("    (hedef: SPED=0x1A | CHAN=0x17 | OPTION=0x47)\r\n");

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

        /* Heartbeat TX (0xB0): AKS'e periyodik "canliyim" sinyali.
         * Best-effort — E-STOP gibi kritik degil, basarisizsa sessizce
         * atlanir ve bir sonraki periyotta tekrar denenir. Lora_Send
         * (Lora_SendCritical degil) kullanilir: E-STOP'un AUX-busy
         * onceligini heartbeat calmaz. */
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
                Telemetry_PrintDashboard(&d, st,
                                         Telemetry_IsEStopActive(&tel_ctx),
                                         link_down);
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

    /* PA0 — E-STOP butonu: falling edge + pull-up */
    g.Pin  = AC_L_STOP_Pin;
    g.Mode = GPIO_MODE_IT_FALLING;
    g.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(AC_L_STOP_GPIO_Port, &g);

    /* PB11 — MOTOR_EN cikisi.
     * STM32 glitch-free baslatma standardi: ONCE WritePin (ODR latch'le),
     * SONRA HAL_GPIO_Init (output moda gec).
     *
     * Nedeni: Boot'ta pin floating input, ODR=0. Eger once Init yapilirsa
     * pin aninda 0V surer (~200ns LOW glitch), SONRA WritePin HIGH yapar.
     * Motor surucunun E-STOP devresi bu kisa LOW'u okursa arac boot'ta
     * yanlis E-STOP'a girer. Dogru sira: once WritePin ile ODR'ye HIGH
     * yaz (pin hala floating, fiziksel hatta etki yok), sonra Init ile
     * output moduna gec — pin o an hazirda bekleyen HIGH degerini surer,
     * glitch olmaz. CubeMX'in urettigi tum GPIO kodlari bu siraya uyar. */
    HAL_GPIO_WritePin(MOTOR_EN_GPIO_Port, MOTOR_EN_Pin, GPIO_PIN_SET);
    g.Pin   = MOTOR_EN_Pin;
    g.Mode  = GPIO_MODE_OUTPUT_PP;
    g.Pull  = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(MOTOR_EN_GPIO_Port, &g);

    /* ====================================================================
     * FIX-D (DEADLOCK ONLEME): Kesme oncelikleri.
     *
     * HAL_UART_Transmit/Receive timeout olcumu icin HAL_GetTick() kullanir;
     * bu sayaci SysTick kesmesi artirir. Eger bir UART islemi, SysTick'ten
     * YUKSEK oncelikli bir kesme (orn. EXTI0) icinde kilitlenirse SysTick
     * araya giremez, tick artmaz, timeout asla dolmaz → deadlock.
     *
     * Bu kodda E-STOP ISR'i Transmit cagirmiyor (sadece bayrak set ediyor),
     * yani deadlock zaten olusmaz. Yine de kusak+aski: SysTick en yuksekte
     * (0) kalir, EXTI0=1, USART2=2. Boylece SysTick her zaman ilerler.
     * ==================================================================== */
    HAL_NVIC_SetPriority(SysTick_IRQn, 0, 0);   /* tick her zaman ilerlesin */

    /* EXTI0 (E-STOP) — SysTick'ten dusuk ama USART'tan yuksek */
    HAL_NVIC_SetPriority(EXTI0_IRQn, 1, 0);
    HAL_NVIC_EnableIRQ(EXTI0_IRQn);

    /* USART2 NVIC onceligi/enable'i HAL_UART_MspInit icinde ayarlanir.
     * MspInit, MX_GPIO_Init'ten SONRA (UART init sirasinda) calistigi icin
     * burada set edilen deger eziliyordu; tek-kaynak orasi olsun diye bu
     * blok kaldirildi. Hedef hiyerarsi: SysTick(0) > EXTI0(1) > USART2(2). */
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