/**
 * @file    LoRa.c
 * @brief   LoRa E32 RX test scripti — gelen mesajlari ekrana basar.
 *
 *  Calisma akisi:
 *    1) LoRa_Test_Init   : yapi sifirlanir, UART handle'lari kaydedilir.
 *    2) LoRa_Test_Start  : USART2 uzerinde HAL_UART_Receive_IT(...,1) baslar.
 *    3) ISR (HAL_UART_RxCpltCallback) her byte icin LoRa_Test_OnRxCplt'i
 *       cagirir; byte tampona eklenir, '\n' goruldugunde "satir hazir" flag'i
 *       set edilir, RX kesmesi yeniden silahlanir.
 *    4) Ana donguden LoRa_Test_Loop cagrilir; satir hazirsa printf ile USART1
 *       (ekran) uzerine yazdirir. Yarim satir LORA_TEST_LINE_TIMEOUT_MS
 *       icinde tamamlanmazsa "[LoRa partial]" basligi ile basilir ve atilir.
 *
 *  Bu dosyanin printf'in USART1'e yonlendirildigini varsaydigini unutma
 *  (mevcut main.c'de _write override'i bunu yapiyor).
 */

#include "LoRa.h"
#include <stdio.h>
#include <string.h>

/* ======== Yardimci ======== */

static void buf_reset(LoRa_Test_t *t)
{
    t->line_len    = 0;
    t->line_ready  = 0;
}

static void emit_line(LoRa_Test_t *t, const char *prefix)
{
    /* line_buf NUL-terminated degil — kopyalayip sonuna 0 koyuyoruz. */
    char tmp[LORA_TEST_RX_BUF_LEN + 1];
    uint16_t n = t->line_len;
    if (n > LORA_TEST_RX_BUF_LEN) n = LORA_TEST_RX_BUF_LEN;

    for (uint16_t i = 0; i < n; ++i) {
        uint8_t c = t->line_buf[i];
        /* Kontrol karakterlerini gorulebilir hale getir (CR/LF zaten silindi). */
        tmp[i] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
    }
    tmp[n] = '\0';

    printf("%s [%lu B] \"%s\"\r\n",
           prefix,
           (unsigned long)n,
           tmp);
}

/* ======== API ======== */

LoRa_Test_Status_t LoRa_Test_Init(LoRa_Test_t *t,
                                  UART_HandleTypeDef *lora_uart,
                                  UART_HandleTypeDef *screen_uart)
{
    if (!t || !lora_uart || !screen_uart) return LORA_TEST_ERR_NULL;

    memset(t, 0, sizeof(*t));
    t->lora_uart   = lora_uart;
    t->screen_uart = screen_uart;

    return LORA_TEST_OK;
}

LoRa_Test_Status_t LoRa_Test_Start(LoRa_Test_t *t)
{
    if (!t || !t->lora_uart) return LORA_TEST_ERR_NULL;

    buf_reset(t);
    t->last_byte_ms = HAL_GetTick();

    if (HAL_UART_Receive_IT(t->lora_uart, &t->rx_byte, 1) != HAL_OK) {
        return LORA_TEST_ERR_HAL;
    }

    printf("[LoRa-Test] RX dinleme aktif (USART2 @ 9600).\r\n");
    return LORA_TEST_OK;
}

LoRa_Test_Status_t LoRa_Test_Send(LoRa_Test_t *t,
                                  const uint8_t *data, uint16_t len)
{
    if (!t || !t->lora_uart || !data || len == 0) return LORA_TEST_ERR_NULL;

    if (HAL_UART_Transmit(t->lora_uart, (uint8_t *)data, len, 1000) != HAL_OK) {
        return LORA_TEST_ERR_HAL;
    }
    return LORA_TEST_OK;
}

void LoRa_Test_Loop(LoRa_Test_t *t)
{
    if (!t) return;

    /* 1) Hazir satir varsa ekrana bas. */
    if (t->line_ready) {
        emit_line(t, "[LoRa RX]");
        t->total_lines++;

        /* ISR ile yaris koşulundan kacinmak icin once flag'i dusur, sonra sifirla. */
        __disable_irq();
        t->line_len   = 0;
        t->line_ready = 0;
        __enable_irq();
    }

    /* 2) Yarim satir timeout — gonderici '\n' koymasa bile veriyi ekrana al. */
    if (t->line_len > 0 && !t->line_ready) {
        uint32_t now = HAL_GetTick();
        if ((now - t->last_byte_ms) >= LORA_TEST_LINE_TIMEOUT_MS) {
            __disable_irq();
            emit_line(t, "[LoRa partial]");
            t->total_lines++;
            t->line_len   = 0;
            t->line_ready = 0;
            __enable_irq();
        }
    }
}

/* ======== HAL kesme delegasyonlari ======== */

void LoRa_Test_OnRxCplt(LoRa_Test_t *t, UART_HandleTypeDef *huart)
{
    if (!t || !huart || huart != t->lora_uart) return;

    uint8_t b = t->rx_byte;
    t->total_bytes++;
    t->last_byte_ms = HAL_GetTick();

    /* Satir sonu: \n gordugunde tamponu kapatip ana donguye teslim et. */
    if (b == '\n') {
        t->line_ready = 1;
    }
    else if (b == '\r') {
        /* CR'yi yutuyoruz — bir sonraki LF satir sonu olarak isleyecek. */
    }
    else if (t->line_len < (LORA_TEST_RX_BUF_LEN - 1) && !t->line_ready) {
        /* Onceki satir okunmadan yenisi gelirse, eskisini ezmemek icin
         * line_ready flag'i dustugunde tamponu doldur. */
        t->line_buf[t->line_len++] = b;
    }
    else if (!t->line_ready) {
        /* Tampon doldu, '\n' bekleniyor; mevcut satiri zorla bitiriyoruz. */
        t->line_ready = 1;
    }

    /* RX kesmesini yeniden silahla — bir sonraki byte icin. */
    HAL_UART_Receive_IT(t->lora_uart, &t->rx_byte, 1);
}

void LoRa_Test_OnError(LoRa_Test_t *t, UART_HandleTypeDef *huart)
{
    if (!t || !huart || huart != t->lora_uart) return;

    t->rx_errors++;

    /* F1 serisi UART hata bayraklarini temizle, aksi halde IT taklilir. */
    __HAL_UART_CLEAR_OREFLAG(huart);
    __HAL_UART_CLEAR_NEFLAG(huart);
    __HAL_UART_CLEAR_FEFLAG(huart);
    __HAL_UART_CLEAR_PEFLAG(huart);

    /* RX'i yeniden silahla. */
    HAL_UART_Receive_IT(t->lora_uart, &t->rx_byte, 1);
}
