/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  * UKS - Uzaktan Kontrol Sistemi pin tanimlari
  *
  * Pin haritasi (STM32F103):
  * PA0         -> AC_L_STOP    (Acil stop butonu, EXTI falling, pull-up)
  * PA2 / PA3   -> USART2 TX/RX -> LoRa E32 modulu           (9600 baud)
  * PA9 / PA10  -> USART1 TX/RX -> Ekran / Seri monitor      (115200 baud)
  * PB6         -> E32_M0       (Normal mod = LOW, Config mod = HIGH)
  * PB7         -> E32_M1       (Normal mod = LOW, Config mod = HIGH)
  * PB10        -> LORA_AUX     (E32 busy/ready — HIGH=hazir, input pull-up)
  * PB11        -> MOTOR_EN     (Durum cikisi: HIGH=nominal, LOW=E-STOP)
  ******************************************************************************
  */
/* USER CODE END Header */

#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32f1xx_hal.h"

void Error_Handler(void);

/* --------------------------------------------------------------------------
 * USART pin tanimlari
 * -------------------------------------------------------------------------- */
#define AC_L_STOP_Pin           GPIO_PIN_0
#define AC_L_STOP_GPIO_Port     GPIOA
#define AC_L_STOP_EXTI_IRQn     EXTI0_IRQn

#define LORA_TX_Pin             GPIO_PIN_2
#define LORA_TX_GPIO_Port       GPIOA

#define LORA_RX_Pin             GPIO_PIN_3
#define LORA_RX_GPIO_Port       GPIOA

#define EKRAN_TX_Pin            GPIO_PIN_9
#define EKRAN_TX_GPIO_Port      GPIOA

#define EKRAN_RX_Pin            GPIO_PIN_10
#define EKRAN_RX_GPIO_Port      GPIOA

/* USER CODE BEGIN Private defines */

/* --------------------------------------------------------------------------
 * E32 Mode pinleri — PB6 (M0) ve PB7 (M1)
 *
 * M0=LOW,  M1=LOW  → Normal (transparan) mod  — veri gonder/al
 * M0=HIGH, M1=HIGH → Config modu              — AT komutlari
 * M0=HIGH, M1=LOW  → WOR (Wake-on-Radio) modu — kullanilmiyor
 * M0=LOW,  M1=HIGH → Sleep modu               — kullanilmiyor
 *
 * Boot'ta Lora_Init() icinde:
 *   1. PB6/PB7 output PP olarak ayarlanir, LOW yazilir (normal mod).
 *   2. Config moduna alinip E32_CFG_* degerleri flash'a yazilir.
 *   3. Normal moda donulur.
 * -------------------------------------------------------------------------- */
#define E32_M0_Pin              GPIO_PIN_6
#define E32_M0_GPIO_Port        GPIOB

#define E32_M1_Pin              GPIO_PIN_7
#define E32_M1_GPIO_Port        GPIOB

/* E32 AUX: HIGH = hazir, LOW = mesgul/boot. Input + pull-up. */
#define LORA_AUX_Pin            GPIO_PIN_10
#define LORA_AUX_GPIO_Port      GPIOB

/* Durum cikisi: HIGH = nominal, LOW = E-STOP aktif */
#define MOTOR_EN_Pin            GPIO_PIN_11
#define MOTOR_EN_GPIO_Port      GPIOB

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */