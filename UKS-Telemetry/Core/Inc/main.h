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
  * PB10        -> LORA_AUX     (E32 busy/ready, input pull-up)
  * PB11        -> MOTOR_EN     (Durum cikisi: HIGH=nominal, LOW=E-STOP)
  *
  * NOT: E32 modulunun M0 ve M1 pinleri donanimsal olarak GND'ye baglidir
  * (kalici Normal mod). Bu nedenle STM32 tarafindan kontrol edilmezler.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32f1xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define AC_L_STOP_Pin GPIO_PIN_0
#define AC_L_STOP_GPIO_Port GPIOA
#define AC_L_STOP_EXTI_IRQn EXTI0_IRQn
#define LORA_TX_Pin GPIO_PIN_2
#define LORA_TX_GPIO_Port GPIOA
#define LORA_RX_Pin GPIO_PIN_3
#define LORA_RX_GPIO_Port GPIOA
#define EKRAN_TX_Pin GPIO_PIN_9
#define EKRAN_TX_GPIO_Port GPIOA
#define EKRAN_RX_Pin GPIO_PIN_10
#define EKRAN_RX_GPIO_Port GPIOA

/* USER CODE BEGIN Private defines */

/* ----------------------------------------------------------------------------
 * LoRa E32-433T30D status pini (AUX)
 * lora.h bu define'lari ZORUNLU kabul eder; yoksa compile hatasi verir.
 *
 * E32'nin M0 ve M1 pinleri donanimda GND'ye baglanmistir (kalici Normal mod)
 * — bu yuzden STM32 tarafinda tanimlanmalari gerekmiyor.
 *
 * Bu blok "USER CODE" icinde oldugu icin CubeMX regenerate sonrasinda
 * korunacaktir.
 * -------------------------------------------------------------------------- */

/* E32 AUX: modul hazir iken HIGH, TX/boot sirasinda LOW. Input + pull-up. */
#define LORA_AUX_Pin         GPIO_PIN_10
#define LORA_AUX_GPIO_Port   GPIOB

/* Durum cikisi (role / LED): HIGH=nominal, LOW=E-STOP aktif */
#define MOTOR_EN_Pin         GPIO_PIN_11
#define MOTOR_EN_GPIO_Port   GPIOB

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */