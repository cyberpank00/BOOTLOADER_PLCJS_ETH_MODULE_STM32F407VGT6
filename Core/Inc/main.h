/**
 * @file  main.h
 * @brief Bootloader main header — GPIO defines and prototypes.
 */
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32f4xx_hal.h"

void Error_Handler(void);
void SystemClock_Config(void);

/* GPIO pin aliases (matching main application). ETHINT/ETHRST are identical on
 * every D4MG variant; STAT_LED and FACT_RES are routed to different pins on
 * some variants and MUST be selected per variant — on 12DO, PC8/PC6 are DQ
 * outputs, so driving them as the status LED would toggle a real relay. */
#define ETHINT_Pin          GPIO_PIN_1
#define ETHINT_GPIO_Port    GPIOB

#define ETHRST_Pin          GPIO_PIN_11
#define ETHRST_GPIO_Port    GPIOD

/* PRODUCT_ID_DEFAULT is provided by CMake (-DPRODUCT_ID -> PRODUCT_ID_DEFAULT).
 * Fallback keeps a stand-alone compile sane; matches flash_map.h default. */
#ifndef PRODUCT_ID_DEFAULT
#define PRODUCT_ID_DEFAULT  0x504C1201u
#endif

#if (PRODUCT_ID_DEFAULT == 0x504C1202u)   /* 12DO — STAT_LED=PE9, FACT_RES=PE10 */
#define STAT_LED_Pin        GPIO_PIN_9
#define STAT_LED_GPIO_Port  GPIOE
#define FACT_RES_Pin        GPIO_PIN_10
#define FACT_RES_GPIO_Port  GPIOE
#elif (PRODUCT_ID_DEFAULT == 0x504C1201u) /* 12DI — STAT_LED=PC6, FACT_RES=PC8 (swapped board rev) */
#define STAT_LED_Pin        GPIO_PIN_6
#define STAT_LED_GPIO_Port  GPIOC
#define FACT_RES_Pin        GPIO_PIN_8
#define FACT_RES_GPIO_Port  GPIOC
#elif (PRODUCT_ID_DEFAULT == 0x504C0403u) /* 4RTD — STAT_LED=PE9, FACT_RES=PE10 (board rev) */
#define STAT_LED_Pin        GPIO_PIN_9
#define STAT_LED_GPIO_Port  GPIOE
#define FACT_RES_Pin        GPIO_PIN_10
#define FACT_RES_GPIO_Port  GPIOE
#else                                      /* other variants — STAT_LED=PC8, FACT_RES=PC6 */
#define STAT_LED_Pin        GPIO_PIN_8
#define STAT_LED_GPIO_Port  GPIOC
#define FACT_RES_Pin        GPIO_PIN_6
#define FACT_RES_GPIO_Port  GPIOC
#endif

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
