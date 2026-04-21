/**
 * @file    lora.h
 * @brief   Ebyte E32-433T30D surucusu — sadece normal mod TX/RX.
 *
 *  Hedef modul: E32-433T30D (SX1278, 433MHz, 1W / 30dBm).
 *  Bu surucu config yazmaz — modul onceden ayarlanmis varsayilir.
 *
 *  Donanim baglantisi (STM32 tarafi):
 *    TXD <--> LoRa RX        (UART2 RX bacagina)
 *    RXD <--> LoRa TX        (UART2 TX bacagina)
 *    M0  -->  GND            (kalici Normal mod — STM32 baglantisi YOK)
 *    M1  -->  GND            (kalici Normal mod — STM32 baglantisi YOK)
 *    AUX <--> GPIO in        (LORA_AUX_Pin, pull-up)
 *    VCC <--> 3.3V/5V (besleme akimi ~650mA zirve, 1W TX icin)
 *    GND <--> GND
 *
 *  main.h icinde asagidaki define'lar ZORUNLU:
 *      LORA_AUX_Pin, LORA_AUX_GPIO_Port
 *  Bunlari CubeMX'te AUX pinine User Label vererek otomatik olusturabilirsiniz.
 *
 *  Mod tablosu (Ebyte datasheet — M1:M0 sirasiyla):
 *      M1=0 M0=0 -> Normal       (UART ve RF aktif)
 *      M1=0 M0=1 -> Wake-Up      (TX'e wake preamble ekler)
 *      M1=1 M0=0 -> Power-Save   (dusuk guc RX)
 *      M1=1 M0=1 -> Sleep/Config (config frame'leri buraya yazilir)
 *
 *  M0/M1 donanimda GND'ye bagli oldugundan modul DAIMA Normal modda calisir.
 *  Config degistirmek icin modul USB-UART ile Ebyte tool'una baglanmalidir.
 */

#ifndef LORA_H
#define LORA_H

#include "main.h"
#include <stdint.h>
#include <stddef.h>

/* ========== Derleme-zamani kontrol ========== */

#if !defined(LORA_AUX_Pin) || !defined(LORA_AUX_GPIO_Port)
#  error "LORA_AUX_Pin / LORA_AUX_GPIO_Port main.h'de tanimli olmali"
#endif

/* ========== E32'ye ozel zamanlama sabitleri ========== */

/** Modul boot suresi (datasheet: ~170ms). Guvenli marj. */
#ifndef LORA_E32_BOOT_MS
#  define LORA_E32_BOOT_MS          200U
#endif

/** TX oncesi AUX HIGH beklenecek maksimum sure. */
#ifndef LORA_E32_TX_WAIT_MS
#  define LORA_E32_TX_WAIT_MS       1000U
#endif

/** HAL_UART_Transmit blocking timeout. */
#ifndef LORA_TX_TIMEOUT_MS
#  define LORA_TX_TIMEOUT_MS        500U
#endif

/* ========== Tipler ========== */

typedef enum {
    LORA_OK = 0,
    LORA_ERR_NULL,
    LORA_ERR_NOT_INIT,
    LORA_ERR_BUSY,
    LORA_ERR_TX,
    LORA_ERR_TIMEOUT
} LoraStatus_t;

/**
 * @brief RX byte callback — UART ISR context'inde cagrilir. KISA TUTUN.
 *        now_ms: byte'in alindigi ISR anindaki HAL_GetTick() degeri.
 */
typedef void (*LoraRxByteFn_t)(uint8_t byte, uint32_t now_ms, void *user);

typedef struct {
    uint32_t tx_bytes;
    uint32_t tx_fails;
    uint32_t rx_bytes;
    uint32_t rx_errors;       /* UART overrun/frame/parity */
    uint32_t aux_timeouts;    /* AUX HIGH beklenirken timeout */
} LoraStats_t;

typedef struct {
    UART_HandleTypeDef *huart;
    uint8_t             rx_byte;        /* Tek byte RX tampon (IT) */
    uint8_t             initialized;
    LoraRxByteFn_t      rx_cb;
    void               *rx_cb_user;
    LoraStats_t         stats;
} LoraCtx_t;

/* ========== Public API ========== */

/**
 * @brief E32'yi baslat. UART daha once HAL ile init edilmis olmali.
 *        Bu fonksiyon AUX GPIO'sunu konfigure eder ve modul boot'u
 *        (AUX HIGH) bekler. M0/M1 donanimda GND'ye baglidir; STM32 bu
 *        pinleri kontrol etmez. GPIO port clock'unun daha once enable
 *        edilmis oldugu varsayilir.
 */
LoraStatus_t Lora_Init(LoraCtx_t *ctx, UART_HandleTypeDef *huart);

/** Her alinan byte icin cagrilacak handler'i ayarla. NULL -> kaldirir. */
void         Lora_SetRxByteHandler(LoraCtx_t *ctx, LoraRxByteFn_t cb, void *user);

/** Interrupt modunda RX'i baslat (tek-byte donguye girer). */
LoraStatus_t Lora_StartReceive(LoraCtx_t *ctx);

/**
 * @brief Blocking TX. AUX HIGH olmadan gondermez — onceki RF TX'in
 *        bitmesini bekler. Burst icinde ardisik cagrilar guvenlidir.
 */
LoraStatus_t Lora_Send(LoraCtx_t *ctx, const uint8_t *data, uint16_t len);

/** HAL delegasyon — main.c HAL_UART_RxCpltCallback icinden cagir. */
void         Lora_OnUartRxCplt(LoraCtx_t *ctx, UART_HandleTypeDef *huart);

/** HAL delegasyon — main.c HAL_UART_ErrorCallback icinden cagir. */
void         Lora_OnUartError (LoraCtx_t *ctx, UART_HandleTypeDef *huart);

/** AUX LOW iken modul busy (TX/boot surdurur). */
uint8_t      Lora_IsBusy   (const LoraCtx_t *ctx);

/** AUX HIGH olana kadar polling bekle. Timeout asilirsa LORA_ERR_TIMEOUT. */
LoraStatus_t Lora_WaitReady(const LoraCtx_t *ctx, uint32_t timeout_ms);

const LoraStats_t *Lora_GetStats  (const LoraCtx_t *ctx);
void               Lora_ResetStats(LoraCtx_t *ctx);

#endif /* LORA_H */
