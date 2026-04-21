/**
 * @file    lora.c
 * @brief   Ebyte E32-433T30D surucusu — implementasyon.
 *
 *  NOT: M0 ve M1 donanimda GND'ye bagli oldugundan modul daima Normal
 *  modda calisir; STM32 bu pinleri kontrol etmez. Burada yalnizca AUX
 *  okunur ve UART TX/RX yonetilir.
 */

#include "lora.h"
#include <string.h>

/* ========== Dahili Yardimcilar ========== */

/** AUX GPIO'sunu input pull-up olarak konfigure et. Clock enable OLMALI. */
static void E32_InitAuxGPIO(void)
{
    GPIO_InitTypeDef g = {0};

    /* AUX: input, pull-up (modul disconnect olursa pin float etmesin) */
    g.Mode  = GPIO_MODE_INPUT;
    g.Pull  = GPIO_PULLUP;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    g.Pin   = LORA_AUX_Pin;
    HAL_GPIO_Init(LORA_AUX_GPIO_Port, &g);
}

static inline uint8_t Lora_HandleMatches(const LoraCtx_t *ctx,
                                          const UART_HandleTypeDef *h)
{
    return (ctx && ctx->initialized && ctx->huart == h);
}

/* ========== API ========== */

LoraStatus_t Lora_Init(LoraCtx_t *ctx, UART_HandleTypeDef *huart)
{
    if (!ctx || !huart) return LORA_ERR_NULL;

    memset(ctx, 0, sizeof(*ctx));
    ctx->huart = huart;

    E32_InitAuxGPIO();

    /* Modul boot'u tamamlasin — AUX HIGH olmali. Boot sirasinda
     * AUX tahminen LOW'da, 170ms icinde HIGH'a cikar. */
    uint32_t start = HAL_GetTick();
    while (HAL_GPIO_ReadPin(LORA_AUX_GPIO_Port, LORA_AUX_Pin) == GPIO_PIN_RESET)
    {
        if ((HAL_GetTick() - start) > LORA_E32_BOOT_MS)
        {
            /* Modul cevap vermiyor — yine de initialized say, kullanici
             * loglayip devam edebilir. Donanim baglantisini kontrol et. */
            ctx->initialized = 1;
            return LORA_ERR_TIMEOUT;
        }
    }

    ctx->initialized = 1;
    return LORA_OK;
}

void Lora_SetRxByteHandler(LoraCtx_t *ctx, LoraRxByteFn_t cb, void *user)
{
    if (!ctx) return;
    ctx->rx_cb      = cb;
    ctx->rx_cb_user = user;
}

LoraStatus_t Lora_StartReceive(LoraCtx_t *ctx)
{
    if (!ctx || !ctx->initialized) return LORA_ERR_NOT_INIT;

    HAL_StatusTypeDef hs = HAL_UART_Receive_IT(ctx->huart, &ctx->rx_byte, 1);
    if (hs != HAL_OK)
    {
        return (hs == HAL_BUSY) ? LORA_ERR_BUSY : LORA_ERR_TX;
    }
    return LORA_OK;
}

LoraStatus_t Lora_Send(LoraCtx_t *ctx, const uint8_t *data, uint16_t len)
{
    if (!ctx || !data)         return LORA_ERR_NULL;
    if (!ctx->initialized)     return LORA_ERR_NOT_INIT;
    if (len == 0)              return LORA_OK;

    /* Oncelikle modulun hazir olmasini bekle. E32 onceki RF TX'i
     * bitirmeden yeni paket almaz/iletmez; burst'lerde bu kritik. */
    LoraStatus_t ws = Lora_WaitReady(ctx, LORA_E32_TX_WAIT_MS);
    if (ws != LORA_OK)
    {
        ctx->stats.tx_fails++;
        ctx->stats.aux_timeouts++;
        return ws;
    }

    HAL_StatusTypeDef hs = HAL_UART_Transmit(ctx->huart,
                                             (uint8_t *)data, len,
                                             LORA_TX_TIMEOUT_MS);
    if (hs != HAL_OK)
    {
        ctx->stats.tx_fails++;
        return (hs == HAL_TIMEOUT) ? LORA_ERR_TIMEOUT : LORA_ERR_TX;
    }

    ctx->stats.tx_bytes += len;
    return LORA_OK;
}

void Lora_OnUartRxCplt(LoraCtx_t *ctx, UART_HandleTypeDef *huart)
{
    if (!Lora_HandleMatches(ctx, huart)) return;

    ctx->stats.rx_bytes++;

    if (ctx->rx_cb)
    {
        ctx->rx_cb(ctx->rx_byte, HAL_GetTick(), ctx->rx_cb_user);
    }

    (void)HAL_UART_Receive_IT(ctx->huart, &ctx->rx_byte, 1);
}

void Lora_OnUartError(LoraCtx_t *ctx, UART_HandleTypeDef *huart)
{
    if (!Lora_HandleMatches(ctx, huart)) return;

    ctx->stats.rx_errors++;
    (void)HAL_UART_Receive_IT(ctx->huart, &ctx->rx_byte, 1);
}

uint8_t Lora_IsBusy(const LoraCtx_t *ctx)
{
    (void)ctx;
    return (HAL_GPIO_ReadPin(LORA_AUX_GPIO_Port, LORA_AUX_Pin) == GPIO_PIN_RESET);
}

LoraStatus_t Lora_WaitReady(const LoraCtx_t *ctx, uint32_t timeout_ms)
{
    uint32_t start = HAL_GetTick();
    while (Lora_IsBusy(ctx))
    {
        if ((HAL_GetTick() - start) > timeout_ms) return LORA_ERR_TIMEOUT;
    }
    return LORA_OK;
}

const LoraStats_t *Lora_GetStats(const LoraCtx_t *ctx)
{
    return ctx ? &ctx->stats : NULL;
}

void Lora_ResetStats(LoraCtx_t *ctx)
{
    if (ctx) memset(&ctx->stats, 0, sizeof(ctx->stats));
}