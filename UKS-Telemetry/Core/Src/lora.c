#include "lora.h"

LoraStatus_t Lora_Init(LoraCtx_t *ctx, UART_HandleTypeDef *huart)
{
    if (!ctx || !huart) return LORA_ERR;
    
    ctx->huart   = huart;
    ctx->rx_cb   = NULL;
    ctx->rx_user = NULL;
    
    /* If you have an AUX pin wired to the STM32 to check E32 status, 
       you can add a short delay or check here. For now, assume OK. */
    
    return LORA_OK;
}

void Lora_SetRxByteHandler(LoraCtx_t *ctx, LoraRxCb_t cb, void *user)
{
    if (!ctx) return;
    ctx->rx_cb   = cb;
    ctx->rx_user = user;
}

LoraStatus_t Lora_StartReceive(LoraCtx_t *ctx)
{
    if (!ctx || !ctx->huart) return LORA_ERR;
    
    /* Start listening for the very first byte via UART Interrupt */
    if (HAL_UART_Receive_IT(ctx->huart, &ctx->rx_byte_buf, 1) != HAL_OK)
    {
        return LORA_ERR;
    }
    
    return LORA_OK;
}

LoraStatus_t Lora_Send(LoraCtx_t *ctx, const uint8_t *data, uint16_t len)
{
    if (!ctx || !ctx->huart || !data) return LORA_ERR;
    
    /* Optional: If you mapped the E32 AUX pin, read it here 
       and wait for it to go HIGH before transmitting. */

    if (HAL_UART_Transmit(ctx->huart, (uint8_t*)data, len, 1000) != HAL_OK)
    {
        return LORA_ERR;
    }
    
    return LORA_OK;
}

void Lora_OnUartRxCplt(LoraCtx_t *ctx, UART_HandleTypeDef *huart)
{
    if (!ctx) return;
    
    /* Ensure the interrupt belongs to the LoRa UART */
    if (ctx->huart == huart)
    {
        /* Pass the received byte to the main.c / telemetry parser */
        if (ctx->rx_cb)
        {
            ctx->rx_cb(ctx->rx_byte_buf, HAL_GetTick(), ctx->rx_user);
        }
        
        /* Re-arm the UART interrupt to listen for the next byte */
        HAL_UART_Receive_IT(ctx->huart, &ctx->rx_byte_buf, 1);
    }
}

void Lora_OnUartError(LoraCtx_t *ctx, UART_HandleTypeDef *huart)
{
    if (!ctx) return;
    
    if (ctx->huart == huart)
    {
        /* Clear Overrun/Noise errors to prevent the interrupt from getting stuck */
        __HAL_UART_CLEAR_OREFLAG(huart);
        __HAL_UART_CLEAR_NEFLAG(huart);
        __HAL_UART_CLEAR_FEFLAG(huart);
        
        /* Re-arm the UART interrupt */
        HAL_UART_Receive_IT(ctx->huart, &ctx->rx_byte_buf, 1);
    }
}