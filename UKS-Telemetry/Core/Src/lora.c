#include "lora.h"
#include "telemetry.h"

#define LORA_RX_BUF_SIZE 64

// DMA'nın verileri doğrudan yazacağı tampon
uint8_t lora_dma_rx_buf[LORA_RX_BUF_SIZE];

// main.c dosyasında oluşturduğumuz telemetri değişkenine (tel_ctx) dışarıdan erişim
extern TelCtx_t tel_ctx; 

LoraStatus_t Lora_Init(LoraCtx_t *ctx, UART_HandleTypeDef *huart) 
{
    if (!ctx || !huart) return LORA_ERR_UART;
    
    ctx->huart = huart;
    return LORA_OK;
}

void Lora_StartReceive(LoraCtx_t *ctx) 
{
    if (!ctx || !ctx->huart) return;
    
    // DMA'yı Idle Line (Paket sonu) modunda dinlemeye başlat
    HAL_UARTEx_ReceiveToIdle_DMA(ctx->huart, lora_dma_rx_buf, LORA_RX_BUF_SIZE);
    
    // Gereksiz yere işlemciyi yormaması için "Half-Transfer" kesmesini kapatıyoruz
    __HAL_DMA_DISABLE_IT(ctx->huart->hdmarx, DMA_IT_HT);
}

// STM32'nin donanımsal DMA paket alma kesmesi (Paket bittiğinde otomatik çalışır)
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size) 
{
    if (huart->Instance == USART2) // Veri LoRa'dan mı geldi?
    {
        uint32_t now = HAL_GetTick();
        
        // 1. DMA tamponundaki verileri byte-byte telemetri state-machine'ine gönder
        for (uint16_t i = 0; i < Size; i++) 
        {
            Telemetry_RxBytePush(&tel_ctx, lora_dma_rx_buf[i], now);
        }
        
        // 2. İşimiz bitti, bir sonraki veri paketi için DMA'yı tekrar kur ve dinlemeye geç
        HAL_UARTEx_ReceiveToIdle_DMA(huart, lora_dma_rx_buf, LORA_RX_BUF_SIZE);
        __HAL_DMA_DISABLE_IT(huart->hdmarx, DMA_IT_HT);
    }
}