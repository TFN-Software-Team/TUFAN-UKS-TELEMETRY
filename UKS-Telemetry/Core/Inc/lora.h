#ifndef LORA_H
#define LORA_H

#include "main.h"
#include <stdint.h>

/* =========================================================================
 * E32-433T30D Hedef Konfigürasyon
 *
 *  SPED register bit duzeni (EBYTE E32 — DIKKAT, alanlar bu sirada):
 *    bit[7:6] = UART parity   (00/11 = 8N1, 01 = 8O1, 10 = 8E1)
 *    bit[5:3] = UART baud     (000=1200 001=2400 010=4800 011=9600
 *                              100=19200 101=38400 110=57600 111=115200)
 *    bit[2:0] = air data rate (000=0.3k 001=1.2k 010=2.4k 011=4.8k
 *                              100=9.6k 101=19.2k ...)
 *
 *  SPED = 0x1A = 0001 1010
 *    bit[7:6] = 00  → 8N1, parity yok
 *    bit[5:3] = 011 → UART 9600 baud   (STM32 USART2 ile AYNI olmali)
 *    bit[2:0] = 010 → 2.4 kbps hava hizi
 *
 *    NOT — eski deger 0xC2 = parity 8N1 + UART 1200 baud + 2.4k air idi.
 *    Bit alanlari yanlis cozuldugu icin UART'in 9600 oldugu saniliyordu;
 *    aslinda 1200 yaziliyordu. STM32 9600'den, E32 normal modda 1200'den
 *    konusunca link kopuyor ve rx_byte = 0 kaliyordu.
 *    (Config/sleep modunda E32 UART'i her zaman sabit 9600 oldugu icin
 *    config komutunun kendisi yine de yaziliyordu — init "[OK]" donuyordu.)
 *
 *    NOT — daha eski deger 0x18 = 8N1 + UART 9600 + 0.3k air idi: UART
 *    baud'u dogruydu ama air rate yanlisti.
 *
 *  CHAN = 0x17  (23 decimal)
 *    Frekans = 410 + CHAN = 410 + 23 = 433 MHz
 *    (433 ISM bandi: 433.05 – 434.79 MHz)
 *
 *    NOT — eski deger 0x06 → 416 MHz, ISM bandi disindaydi.
 *
 *  OPTION = 0x47
 *    bit7    = 0   → Transparan (saydamsiz) mod — fixed-point kapali
 *    bit6    = 1   → Push-pull IO
 *    bit[5:3]= 000 → 250 ms wakeup suresi
 *    bit2    = 1   → FEC (Forward Error Correction) acik
 *    bit[1:0]= 11  → 30 dBm TX gucu (E32-433T30D maksimum)
 *
 *    NOT — eski deger 0x44 → 20 dBm (bit[1:0]=00), gereksiz guc kaybi.
 *
 *  RF UYUMU: Iki modul birbirini duyabilmek icin AYNI air rate (SPED
 *  bit[2:0]), AYNI CHAN ve AYNI OPTION (transparan/FEC) degerine sahip
 *  olmali. SPED'in UART baud alani (bit[5:3]) ise YERELdir — her modulun
 *  kendi MCU'suyla konustugu hizdir; iki modulde farkli olabilir. Yani
 *  AKS'in UART baud'unu degistirmen gerekmez, yeter ki AKS'in kendi
 *  MCU'su da kendi E32'siyle ayni baud'da olsun.
 *
 *  ADRES: 0x0000 — broadcast, her iki yone de paket gider.
 * ========================================================================= */
#define E32_CFG_ADDH    0x00U
#define E32_CFG_ADDL    0x00U
#define E32_CFG_SPED    0x1AU   /* 8N1 | 9600 UART | 2.4 kbps air rate    */
#define E32_CFG_CHAN    0x17U   /* Kanal 23  | 433 MHz (ISM bandi)        */
#define E32_CFG_OPTION  0x47U   /* Transparan | PP | 250ms | FEC | 30 dBm */

/* =========================================================================
 * AUX bekleme zaman asimi (ms)
 *
 *  BOOT   : E32 guc açilisinda AUX'u ~200-500 ms LOW tutar.
 *           3000 ms genis marj birakildi.
 *  MODE   : M0/M1 degisiminden sonra AUX HIGH'a gelmesi icin sure.
 *  CFG    : Flash yazma (C0 komutu) sonrasi AUX HIGH bekleme suresi.
 *           E32 datasheet: flash yazma ~200 ms; 2000 ms marj.
 * ========================================================================= */
#define E32_AUX_BOOT_TIMEOUT_MS   3000U
#define E32_AUX_MODE_TIMEOUT_MS    500U
#define E32_AUX_CFG_TIMEOUT_MS    2000U

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
 * @brief  E32 GPIO'larini hazirlar, config moduna girip E32_CFG_* degerlerini
 *         flash'a kalici olarak yazar, Normal moda doner.
 *
 * @retval LORA_OK          Basarili
 *         LORA_ERR_TIMEOUT AUX zaman asimi (donanim / besleme kontrol et)
 *         LORA_ERR         Config echo dogrulama hatasi (ayarlar yazilmamis olabilir)
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

/**
 * @brief  KRITIK gonderim (E-STOP). AUX'u kisa bekler ama timeout olsa
 *         bile best-effort gonderir (FIX-5). IT-RX yine guvenli yonetilir.
 *         En kritik komutun AUX-busy yuzunden dusmesini onler.
 */
LoraStatus_t Lora_SendCritical(LoraCtx_t *ctx, const uint8_t *data, uint16_t len);

/** HAL_UART_RxCpltCallback icinden cagrilmali. */
void Lora_OnUartRxCplt(LoraCtx_t *ctx, UART_HandleTypeDef *huart);

/** HAL_UART_ErrorCallback icinden cagrilmali. */
void Lora_OnUartError (LoraCtx_t *ctx, UART_HandleTypeDef *huart);

#endif /* LORA_H */