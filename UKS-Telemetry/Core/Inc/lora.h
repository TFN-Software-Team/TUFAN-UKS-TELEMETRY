#ifndef LORA_H
#define LORA_H

#include "main.h"
#include "e22_regs.h"
#include <stdint.h>

/* =========================================================================
 * EBYTE E22-400T30D-V2 (SX1268) surucusu
 *
 *  Register adresleri/hedef degerleri/komut sabitleri BURADA DEGIL,
 *  e22_regs.h icinde tanimlidir (TEK dogruluk kaynagi — bkz. o dosyanin
 *  basindaki DOGRULAMA NOTU). lora.h/lora.c yalnizca bu sabitleri kullanir.
 *
 *  E32'den (eski donanim) farklar: register-tabanli C0/C1 komut protokolu,
 *  farkli config-modu pin seviyeleri (M0=0,M1=1 — E32'de M0=1,M1=1 idi),
 *  farkli yanit formati (E32: gonderilenin echo'su | E22: C1 basligiyla
 *  yanit, hata icin FF FF FF). TX/RX veri duzlemi (transparan mod)
 *  degismedi — E22 de E32 gibi konfigure edildikten sonra ham UART
 *  transparan koprusu olarak calisir.
 * ========================================================================= */

/* =========================================================================
 * Heartbeat (UKS -> AKS)
 *
 *  9.2.a: RF hatti tek yonlu telemetri + heartbeat'tir, komut kanali yok.
 *  0xB0 icerik tasimayan bir "canliyim" sinyalidir. Zamanlayici
 *  tetiklemelidir; operator girdisi GEREKTIRMEZ. Bkz. UYUM_NOTU.md bolum 2.
 * ========================================================================= */
#define LORA_HEARTBEAT_BYTE       0xB0U
#define LORA_HEARTBEAT_PERIOD_MS  1000U

/* =========================================================================
 * Tip tanimlari
 * ========================================================================= */
typedef enum {
    LORA_OK = 0,
    LORA_ERR,
    LORA_ERR_TIMEOUT,
    LORA_ERR_BUSY
} LoraStatus_t;

/** Tek byte alindiginda cagrilacak callback (ISR context). */
typedef void (*LoraRxCb_t)(uint8_t rx_byte, uint32_t now_ms, void *user);

typedef struct {
    UART_HandleTypeDef *huart;
    LoraRxCb_t          rx_cb;
    void               *rx_user;
    uint8_t             rx_byte_buf;  /* 1-byte IT RX tamponu */
    volatile uint8_t    rx_active;    /* FIX-4: IT-RX su an arm'li mi (TX-safe icin) */
} LoraCtx_t;

/* =========================================================================
 * Public API
 * ========================================================================= */

/**
 * @brief  E22 GPIO'larini hazirlar, config moduna girip mevcut register
 *         blogunu (ADDH..CRYPT_L) okur; hedeften (e22_regs.h) farkli ise
 *         C0 ile flash'a kalici yazar (ayniysa YAZMAZ — flash omru).
 *         ADDH..REG3 zaten hedefle ayni oldugu (flash'a yazilmadigi)
 *         dalda CRYPT, G7-FIX-2 geregi C2/RAM (kalici olmayan) ile her
 *         boot yeniden yazilir. Ardindan Normal moda doner.
 *
 * @retval LORA_OK          Basarili (yazildi ya da zaten hedefle esiyordu)
 *         LORA_ERR_TIMEOUT AUX zaman asimi (donanim / besleme kontrol et)
 *         LORA_ERR         Okuma/yazma yaniti gecersiz (FF FF FF ya da
 *                           baslik uyumsuz) — E22'de bu net bir hata
 *                           sinyalidir, E32'deki gibi tolere EDILMEZ
 */
LoraStatus_t Lora_Init        (LoraCtx_t *ctx, UART_HandleTypeDef *huart);

void         Lora_SetRxByteHandler(LoraCtx_t *ctx, LoraRxCb_t cb, void *user);

/**
 * @brief  UART interrupt ile byte-by-byte RX dinlemeyi baslatir.
 *         Lora_Init'ten SONRA cagrilmali.
 */
LoraStatus_t Lora_StartReceive(LoraCtx_t *ctx);

/**
 * @brief  Normal veri/komut gonder. TX oncesi devam eden IT-RX guvenli
 *         durdurulur, TX sonrasi yeniden baslatilir (FIX-4). AUX HIGH
 *         beklenir; modul mesgulse LORA_ERR_BUSY doner.
 * @retval LORA_ERR_BUSY AUX 200 ms icinde HIGH gelmedi (modul mesgul)
 */
LoraStatus_t Lora_Send        (LoraCtx_t *ctx, const uint8_t *data, uint16_t len);

/** HAL_UART_RxCpltCallback icinden cagrilmali. */
void Lora_OnUartRxCplt(LoraCtx_t *ctx, UART_HandleTypeDef *huart);

/** HAL_UART_ErrorCallback icinden cagrilmali. */
void Lora_OnUartError (LoraCtx_t *ctx, UART_HandleTypeDef *huart);

#endif /* LORA_H */