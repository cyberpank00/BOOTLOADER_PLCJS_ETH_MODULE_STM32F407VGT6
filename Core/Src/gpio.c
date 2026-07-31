/**
 * @file  gpio.c
 * @brief Bootloader GPIO init — only LED, button, and Ethernet-reset pins.
 */

#include "gpio.h"
#include "main.h"

void MX_GPIO_Init(void)
{
    GPIO_InitTypeDef gi = {0};

    __HAL_RCC_GPIOH_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();
    __HAL_RCC_GPIOE_CLK_ENABLE();  /* STAT_LED/FACT_RES on PE for some variants (12DO) */

    /* ETHRST (PD11) — output, assert reset low first */
    HAL_GPIO_WritePin(ETHRST_GPIO_Port, ETHRST_Pin, GPIO_PIN_RESET);
    gi.Pin   = ETHRST_Pin;
    gi.Mode  = GPIO_MODE_OUTPUT_PP;
    gi.Pull  = GPIO_NOPULL;
    gi.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(ETHRST_GPIO_Port, &gi);

    /* KSZ8863 hardware reset sequence:
     *   RESET# held LOW >= 10 ms  (already asserted above)
     *   then HIGH to release, wait >= 100 ms for internal init before MDIO. */
    HAL_Delay(10);
    HAL_GPIO_WritePin(ETHRST_GPIO_Port, ETHRST_Pin, GPIO_PIN_SET);
    HAL_Delay(100);

    /* STAT_LED (per-variant: PC8 on 12DI/4RTD, PE9 on 12DO) — output */
    HAL_GPIO_WritePin(STAT_LED_GPIO_Port, STAT_LED_Pin, GPIO_PIN_RESET);
    gi.Pin   = STAT_LED_Pin;
    gi.Mode  = GPIO_MODE_OUTPUT_PP;
    gi.Pull  = GPIO_NOPULL;
    gi.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(STAT_LED_GPIO_Port, &gi);

    /* FACT_RES / service button (per-variant: PC6 on 12DI/4RTD, PE10 on 12DO)
     * — input with pull-up. The bootloader does not read it; configured only
     * to a defined state (and to avoid leaving the correct pin floating). */
    gi.Pin  = FACT_RES_Pin;
    gi.Mode = GPIO_MODE_INPUT;
    gi.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(FACT_RES_GPIO_Port, &gi);

    /* ETHINT (PB1) — input, active interrupt from PHY */
    gi.Pin  = ETHINT_Pin;
    gi.Mode = GPIO_MODE_INPUT;
    gi.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(ETHINT_GPIO_Port, &gi);
}
