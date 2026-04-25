/**
 * @file    LoRa.h
 * @brief   LoRa E32 RX test scripti — baska bir LoRa modulunden gelen
 *          mesaji yakalar ve ekrana (USART1) yazdirir.
 *
 *  Donanim varsayimlari (mevcut UKS donanimina gore):
 *    - USART2 (PA2 TX / PA3 RX, 9600 baud)  -> LoRa E32-433T30D
 *    - USART1 (PA9 TX / PA10 RX, 9600 baud) -> PC seri monitor (ekran)
 *    - E32 M0/M1 GND'ye baglidir (Normal mod), AUX pini PB10
 *
 *  Kullanim (main.c icinden):
 *
 *      #include "LoRa.h"
 *
 *      extern UART_HandleTypeDef huart1;   // ekran
 *      extern UART_HandleTypeDef huart2;   // LoRa
 *
 *      LoRa_Test_t lora_test;
 *
 *      int main(void) {
 *          // ... HAL_Init, clock, GPIO, USART1, USART2 hazir olduktan sonra:
 *          LoRa_Test_Init(&lora_test, &huart2, &huart1);
 *          LoRa_Test_Start(&lora_test);
 *
 *          while (1) {
 *              LoRa_Test_Loop(&lora_test);   // satir hazirsa ekrana basar
 *          }
 *      }
 *
 *      // HAL callback'lerini delege et:
 *      void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart) {
 *          LoRa_Test_OnRxCplt(&lora_test, huart);
 *      }
 *      void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart) {
 *          LoRa_Test_OnError(&lora_test, huart);
 *      }
 */

#ifndef LORA_TEST_H
#define LORA_TEST_H

#include "main.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bir mesaj/satir icin maksimum tampon boyutu */
#define LORA_TEST_RX_BUF_LEN   128U

/* Yarim satir timeout'u — bu sure icinde yeni byte gelmezse tampon basilir */
#define LORA_TEST_LINE_TIMEOUT_MS  500U

typedef enum {
    LORA_TEST_OK = 0,
    LORA_TEST_ERR,
    LORA_TEST_ERR_NULL,
    LORA_TEST_ERR_HAL
} LoRa_Test_Status_t;

typedef struct {
    UART_HandleTypeDef *lora_uart;       /* USART2 — LoRa */
    UART_HandleTypeDef *screen_uart;     /* USART1 — ekran/printf */

    uint8_t   rx_byte;                    /* HAL_UART_Receive_IT 1-byte tampon */

    /* Satir tamponu — ISR yazar, ana dongu okur */
    volatile uint8_t  line_buf[LORA_TEST_RX_BUF_LEN];
    volatile uint16_t line_len;
    volatile uint8_t  line_ready;         /* 1 = ana donguye basilmaya hazir  */
    volatile uint32_t last_byte_ms;       /* Son byte zamani (timeout icin)   */

    /* Istatistik */
    uint32_t total_bytes;
    uint32_t total_lines;
    uint32_t rx_errors;
} LoRa_Test_t;

/* ======== API ======== */

/** Yapiyi sifirlar ve UART handle'larini baglar. */
LoRa_Test_Status_t LoRa_Test_Init(LoRa_Test_t *t,
                                  UART_HandleTypeDef *lora_uart,
                                  UART_HandleTypeDef *screen_uart);

/** UART RX kesmesini baslatir — bundan sonra gelen byte'lar ISR'da toplanir. */
LoRa_Test_Status_t LoRa_Test_Start(LoRa_Test_t *t);

/** Ana donguden cagrilir; satir hazirsa ekrana basar, timeout'u kontrol eder. */
void LoRa_Test_Loop(LoRa_Test_t *t);

/** Kucuk yardimcilar: opsiyonel olarak LoRa'dan mesaj gondermek icin. */
LoRa_Test_Status_t LoRa_Test_Send(LoRa_Test_t *t,
                                  const uint8_t *data, uint16_t len);

/* HAL kesme delegasyonlari — main.c icindeki callback'lerden cagrilmali */
void LoRa_Test_OnRxCplt(LoRa_Test_t *t, UART_HandleTypeDef *huart);
void LoRa_Test_OnError(LoRa_Test_t *t, UART_HandleTypeDef *huart);

#ifdef __cplusplus
}
#endif

#endif /* LORA_TEST_H */
