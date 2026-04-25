#ifndef __LORA_H
#define __LORA_H

#include "stm32f1xx_hal.h"

// Başarı ve hata durumları
typedef enum {
    LORA_OK = 0,
    LORA_ERR_TIMEOUT,
    LORA_ERR_UART
} LoraStatus_t;

// LoRa Kontekst Yapısı (Hangi UART'a bağlı olduğunu tutar)
typedef struct {
    UART_HandleTypeDef *huart;
} LoraCtx_t;

// Fonksiyon Prototipleri
LoraStatus_t Lora_Init(LoraCtx_t *ctx, UART_HandleTypeDef *huart);
void Lora_StartReceive(LoraCtx_t *ctx);

#endif /* __LORA_H */