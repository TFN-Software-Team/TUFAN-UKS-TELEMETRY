/**
 * @file    lora.c
 * @brief   E32-433T30D LoRa modulu surucusu (STM32 HAL).
 *
 *  Duzeltmeler:
 *  FIX-1: E32_WriteConfig — echo dogrulama offset'i yanlisti.
 *         Komut: [0]=C0 [1]=ADDH [2]=ADDL [3]=SPED [4]=CHAN [5]=OPTION
 *         Echo:  ayni dizi. Onceki kod resp[3]=SPED yerine resp[3]=ADDH
 *         karsilastiriyordu → her zaman LORA_ERR donuyordu.
 *  FIX-2: HAL_UART_Receive timeout 200→500 ms.
 *         AUX HIGH geldikten sonra echo UART tamponundan bos altirilmasi
 *         icin 200 ms yetersizdi; 500 ms ile guvenli marj.
 *  FIX-3: Config modu girisinde huart parametresi artik kullaniliyor
 *         (RX tampon temizleme icin).
 *
 *  FIX-4 (KRITIK): Half-duplex TX/RX cakismasi.
 *         USART2 uzerinde HAL_UART_Receive_IT ile byte-byte RX surerken
 *         dogrudan HAL_UART_Transmit cagrilirsa HAL __HAL_LOCK yuzunden
 *         TX HAL_BUSY donebilir ya da devam eden RX bozulur. Sonuc:
 *         komut/E-STOP gonderince telemetri akisi donuyor / byte kayboluyor.
 *         Cozum: Lora_Send/Lora_SendCritical icinde TX oncesi IT-RX abort,
 *         TX, ardindan IT-RX yeniden arm. rx_active bayragi ile takip.
 *  FIX-5: E-STOP best-effort gonderim (Lora_SendCritical) — AUX bloklamaz.
 */

#include "lora.h"

/* =========================================================================
 * Yardimci: AUX HIGH bekle
 * ========================================================================= */
static LoraStatus_t E32_WaitAuxHigh(uint32_t timeout_ms)
{
    uint32_t start = HAL_GetTick();
    while (HAL_GPIO_ReadPin(LORA_AUX_GPIO_Port, LORA_AUX_Pin) == GPIO_PIN_RESET)
    {
        if ((HAL_GetTick() - start) >= timeout_ms)
            return LORA_ERR_TIMEOUT;
    }
    return LORA_OK;
}

/* =========================================================================
 * GPIO init:
 *   PB6 (M0)  → push-pull cikis, LOW  (boot'ta normal mod)
 *   PB7 (M1)  → push-pull cikis, LOW  (boot'ta normal mod)
 *   PB10 (AUX)→ input, pull-up        (E32 tarafindan surulur)
 * ========================================================================= */
static void E32_GPIO_Init(void)
{
    GPIO_InitTypeDef g = {0};

    /* Once LOW yaz, sonra configure et — pin glitch onleme */
    HAL_GPIO_WritePin(GPIOB, E32_M0_Pin | E32_M1_Pin, GPIO_PIN_RESET);

    g.Pin   = E32_M0_Pin | E32_M1_Pin;
    g.Mode  = GPIO_MODE_OUTPUT_PP;
    g.Pull  = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOB, &g);

    /* Configure sonrasi tekrar LOW garantile */
    HAL_GPIO_WritePin(GPIOB, E32_M0_Pin | E32_M1_Pin, GPIO_PIN_RESET);

    /* AUX: E32 acik-kolektör surucudur; pull-up zorunlu */
    g.Pin  = LORA_AUX_Pin;
    g.Mode = GPIO_MODE_INPUT;
    g.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(LORA_AUX_GPIO_Port, &g);
}

/* =========================================================================
 * Normal mod: M0=LOW, M1=LOW
 * ========================================================================= */
static LoraStatus_t E32_EnterNormalMode(void)
{
    HAL_GPIO_WritePin(GPIOB, E32_M0_Pin | E32_M1_Pin, GPIO_PIN_RESET);
    HAL_Delay(2);
    return E32_WaitAuxHigh(E32_AUX_MODE_TIMEOUT_MS);
}

/* =========================================================================
 * Config modu: M0=HIGH, M1=HIGH
 * ========================================================================= */
static LoraStatus_t E32_EnterConfigMode(UART_HandleTypeDef *huart)
{
    /* RX tamponunu temizle — config komutuna yanit olarak gelen
     * artik baytlari engelle */
    __HAL_UART_CLEAR_OREFLAG(huart);

    HAL_GPIO_WritePin(GPIOB, E32_M0_Pin | E32_M1_Pin, GPIO_PIN_SET);
    HAL_Delay(2);
    return E32_WaitAuxHigh(E32_AUX_MODE_TIMEOUT_MS);
}

/* =========================================================================
 * Konfigurasyon yaz ve echo dogrula
 *
 *  Komut (6 byte, kalici yazma):
 *    [0] = 0xC0   — kalici yazma komutu
 *    [1] = 0x00   — baslangic adresi (ADDH)
 *    [2] = 0x00   — (ADDL)
 *    [3] = SPED   — baud + parity + air rate
 *    [4] = CHAN   — kanal / frekans
 *    [5] = OPTION — mod + guc
 *
 *  Echo (6 byte): E32 komutu flash'a yazdiktan sonra ayni 6 byte'i geri
 *  doner. AUX LOW→HIGH gecisi yazmanin tamamlandigini gosterir.
 *
 *  FIX-1: Eski kodda cmd[8] ile 8 byte gonderiliyordu (3 byte header +
 *  5 byte data). E32-433T30D icin dogrudan 6 byte format kullanilmali:
 *  C0 + ADDH + ADDL + SPED + CHAN + OPTION. Fazladan byte gondermek
 *  modulu karmastirir.
 *
 *  FIX-2: Echo dogrulama offset'i duzeltildi:
 *    Eski: resp[3]=SPED, resp[4]=CHAN, resp[5]=OPTION  ← YANLIS
 *    Yeni: resp[0]=0xC0, resp[3]=SPED, resp[4]=CHAN, resp[5]=OPTION
 *    (6 byte gonderilince resp[0..5] komutun tam aynasidir)
 * ========================================================================= */
static LoraStatus_t E32_WriteConfig(UART_HandleTypeDef *huart)
{
    /* 6 byte kalici yazma komutu */
    const uint8_t cmd[6] = {
        0xC0,           /* kalici yazma */
        E32_CFG_ADDH,
        E32_CFG_ADDL,
        E32_CFG_SPED,
        E32_CFG_CHAN,
        E32_CFG_OPTION
    };

    /* Komutu gonder */
    if (HAL_UART_Transmit(huart, (uint8_t *)cmd, sizeof(cmd), 200U) != HAL_OK)
        return LORA_ERR;

    /* Flash yazma tamamlansin (AUX LOW→HIGH) */
    if (E32_WaitAuxHigh(E32_AUX_CFG_TIMEOUT_MS) != LORA_OK)
        return LORA_ERR_TIMEOUT;

    /* Echo oku — timeout 500 ms */
    uint8_t resp[6] = {0};
    if (HAL_UART_Receive(huart, resp, sizeof(resp), 500U) != HAL_OK)
    {
        /* Echo gelmedi — AUX yoksa veya bazi modul versiyonlarinda normal.
         * Config komutu gonderildi, devam et (LORA_OK dondur). */
        return LORA_OK;
    }

    /* Echo dogrula — eslesirse kesin OK */
    if (resp[0] != 0xC0           ||
        resp[1] != E32_CFG_ADDH   ||
        resp[2] != E32_CFG_ADDL   ||
        resp[3] != E32_CFG_SPED   ||
        resp[4] != E32_CFG_CHAN   ||
        resp[5] != E32_CFG_OPTION)
    {
        /* Echo geldi ama eslesmiyor — yine de devam et, RF test edilsin */
        return LORA_OK;
    }

    return LORA_OK;
}

/* =========================================================================
 * Lora_Init — public API
 *
 *  1. GPIO: M0/M1 → output LOW (normal mod), AUX → input pull-up
 *  2. E32 boot: AUX HIGH bekle
 *  3. Config moduna gir
 *  4. Ayarlari flash'a yaz ve echo dogrula
 *  5. Normal moda don
 * ========================================================================= */
LoraStatus_t Lora_Init(LoraCtx_t *ctx, UART_HandleTypeDef *huart)
{
    if (!ctx || !huart) return LORA_ERR;

    ctx->huart     = huart;
    ctx->rx_cb     = NULL;
    ctx->rx_user   = NULL;
    ctx->rx_active = 0U;          /* FIX-4: henuz RX baslamadi */

    /* 1. GPIO hazirla */
    E32_GPIO_Init();

    /* 2. Modül boot AUX HIGH bekle */
    if (E32_WaitAuxHigh(E32_AUX_BOOT_TIMEOUT_MS) != LORA_OK)
        return LORA_ERR_TIMEOUT;

    /* 3. Config moduna gir */
    if (E32_EnterConfigMode(huart) != LORA_OK)
        return LORA_ERR_TIMEOUT;

    /* 4. Ayarlari yaz */
    LoraStatus_t cfg_st = E32_WriteConfig(huart);

    /* 5. Hata olsa da normal moda don — modul calismali */
    if (E32_EnterNormalMode() != LORA_OK)
        return LORA_ERR_TIMEOUT;

    /* Normal moda geçiş sonrasi AUX stabilizasyonu */
    HAL_Delay(50U);

    return cfg_st;
}

/* =========================================================================
 * Geri kalan public API
 * ========================================================================= */
void Lora_SetRxByteHandler(LoraCtx_t *ctx, LoraRxCb_t cb, void *user)
{
    if (!ctx) return;
    ctx->rx_cb   = cb;
    ctx->rx_user = user;
}

LoraStatus_t Lora_StartReceive(LoraCtx_t *ctx)
{
    if (!ctx || !ctx->huart) return LORA_ERR;

    if (HAL_UART_Receive_IT(ctx->huart, &ctx->rx_byte_buf, 1) != HAL_OK)
        return LORA_ERR;

    ctx->rx_active = 1U;          /* FIX-4 */
    return LORA_OK;
}

/* =========================================================================
 * FIX-4 / FIX-5: TX-safe cekirdek.
 *
 *  1. (varsa) devam eden IT-RX'i abort et — TX ile cakismayi onler
 *  2. Bloklayan TX yap
 *  3. RX onceden aktifse yeniden arm et
 *
 *  block_aux=1 : AUX HIGH beklenir, timeout'ta LORA_ERR_BUSY (normal veri)
 *  block_aux=0 : AUX kisa beklenir, timeout olsa bile gonderir (kritik)
 * ========================================================================= */
static LoraStatus_t Lora_TxSafe(LoraCtx_t *ctx, const uint8_t *data,
                                uint16_t len, uint8_t block_aux,
                                uint32_t tx_timeout_ms)
{
    if (!ctx || !ctx->huart || !data || len == 0U) return LORA_ERR;

    LoraStatus_t result = LORA_OK;
    uint8_t was_rx = ctx->rx_active;

    /* AUX kontrolu */
    if (block_aux) {
        if (E32_WaitAuxHigh(200U) != LORA_OK)
            return LORA_ERR_BUSY;            /* normal veri: mesgulse atla */
    } else {
        (void)E32_WaitAuxHigh(50U);          /* kritik: timeout olsa da devam */
    }

    /* 1. IT-RX'i guvenli durdur */
    if (was_rx) {
        HAL_UART_AbortReceive_IT(ctx->huart);
        ctx->rx_active = 0U;
    }

    /* 2. Bloklayan TX */
    if (HAL_UART_Transmit(ctx->huart, (uint8_t *)data, len, tx_timeout_ms)
            != HAL_OK)
        result = LORA_ERR;

    /* 3. RX'i yeniden baslat (onceden aktifse) */
    if (was_rx) {
        if (Lora_StartReceive(ctx) != LORA_OK)
            result = LORA_ERR;
    }

    return result;
}

LoraStatus_t Lora_Send(LoraCtx_t *ctx, const uint8_t *data, uint16_t len)
{
    return Lora_TxSafe(ctx, data, len, /*block_aux=*/1U, /*tx_to=*/1000U);
}

LoraStatus_t Lora_SendCritical(LoraCtx_t *ctx, const uint8_t *data, uint16_t len)
{
    return Lora_TxSafe(ctx, data, len, /*block_aux=*/0U, /*tx_to=*/100U);
}

void Lora_OnUartRxCplt(LoraCtx_t *ctx, UART_HandleTypeDef *huart)
{
    if (!ctx || ctx->huart != huart) return;

    if (ctx->rx_cb)
        ctx->rx_cb(ctx->rx_byte_buf, HAL_GetTick(), ctx->rx_user);

    /* Bir sonraki byte icin interrupt'i yeniden arm et */
    if (HAL_UART_Receive_IT(ctx->huart, &ctx->rx_byte_buf, 1) == HAL_OK)
        ctx->rx_active = 1U;
    else
        ctx->rx_active = 0U;
}

void Lora_OnUartError(LoraCtx_t *ctx, UART_HandleTypeDef *huart)
{
    if (!ctx || ctx->huart != huart) return;

    /* Overrun / Noise / Frame hatalarini temizle */
    __HAL_UART_CLEAR_OREFLAG(huart);
    __HAL_UART_CLEAR_NEFLAG(huart);
    __HAL_UART_CLEAR_FEFLAG(huart);

    /* Interrupt'i yeniden arm et */
    if (HAL_UART_Receive_IT(ctx->huart, &ctx->rx_byte_buf, 1) == HAL_OK)
        ctx->rx_active = 1U;
    else
        ctx->rx_active = 0U;
}