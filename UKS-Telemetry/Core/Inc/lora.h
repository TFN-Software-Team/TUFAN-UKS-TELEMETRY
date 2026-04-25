#ifndef LORA_H
#define LORA_H

#include "main.h"
#include <stdint.h>

/* Status codes */
typedef enum {
    LORA_OK = 0,
    LORA_ERR,
    LORA_ERR_TIMEOUT,
    LORA_ERR_BUSY
} LoraStatus_t;

/* Callback type for when a single byte is received over UART */
typedef void (*LoraRxCb_t)(uint8_t rx_byte, uint32_t now_ms, void *user);

/* LoRa Context Structure */
typedef struct {
    UART_HandleTypeDef *huart;
    LoraRxCb_t          rx_cb;
    void               *rx_user;
    uint8_t             rx_byte_buf; /* Buffer for 1-byte IT reception */
} LoraCtx_t;

/* Public API */
LoraStatus_t Lora_Init(LoraCtx_t *ctx, UART_HandleTypeDef *huart);

void         Lora_SetRxByteHandler(LoraCtx_t *ctx, LoraRxCb_t cb, void *user);

LoraStatus_t Lora_StartReceive(LoraCtx_t *ctx);

LoraStatus_t Lora_Send(LoraCtx_t *ctx, const uint8_t *data, uint16_t len);

/* HAL Interrupt hooks (Must be called from HAL_UART_RxCpltCallback and HAL_UART_ErrorCallback) */
void         Lora_OnUartRxCplt(LoraCtx_t *ctx, UART_HandleTypeDef *huart);
void         Lora_OnUartError(LoraCtx_t *ctx, UART_HandleTypeDef *huart);

#endif /* LORA_H */