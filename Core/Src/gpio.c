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

    /* ETHRST (PD11) — output. The KSZ8863 is a pass-through switch forwarding
     * traffic between its external ports (1<->2) autonomously, so it MUST keep
     * running across a warm MCU reboot (soft reset / IWDG / NRST): the
     * bootloader runs first on every reset, so it — not the application — owns
     * the switch-reset policy.
     *
     * Cold boot (power-on / brown-out): pulse RESET# low then high for a clean
     * switch init. Warm reboot: leave ETHRST high (never assert reset) so the
     * pass-through link on ports 1/2 survives the MCU restart.
     * (Consumes the RCC reset-cause flags; the boot-entry decision uses the
     * no-init RAM magic, not RCC, so clearing them here is safe.) */
    gi.Pin   = ETHRST_Pin;
    gi.Mode  = GPIO_MODE_OUTPUT_PP;
    gi.Pull  = GPIO_NOPULL;
    gi.Speed = GPIO_SPEED_FREQ_LOW;

    const uint32_t csr = RCC->CSR;
    const uint32_t cold_boot = csr & (RCC_CSR_PORRSTF | RCC_CSR_BORRSTF);
    __HAL_RCC_CLEAR_RESET_FLAGS();

    if (cold_boot != 0u) {
        HAL_GPIO_WritePin(ETHRST_GPIO_Port, ETHRST_Pin, GPIO_PIN_RESET);
        HAL_GPIO_Init(ETHRST_GPIO_Port, &gi);
        HAL_Delay(10);                                     /* RESET# low >= 10 ms  */
        HAL_GPIO_WritePin(ETHRST_GPIO_Port, ETHRST_Pin, GPIO_PIN_SET);
        HAL_Delay(100);                                    /* internal init >= 100 ms */
    } else {
        /* Warm reboot: keep the switch released and running. */
        HAL_GPIO_WritePin(ETHRST_GPIO_Port, ETHRST_Pin, GPIO_PIN_SET);
        HAL_GPIO_Init(ETHRST_GPIO_Port, &gi);
    }

    /* STAT_LED (per-variant: PC6 on 12DI, PE9 on 12DO/4RTD/8AIC, PC8 default) — output */
    HAL_GPIO_WritePin(STAT_LED_GPIO_Port, STAT_LED_Pin, GPIO_PIN_RESET);
    gi.Pin   = STAT_LED_Pin;
    gi.Mode  = GPIO_MODE_OUTPUT_PP;
    gi.Pull  = GPIO_NOPULL;
    gi.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(STAT_LED_GPIO_Port, &gi);

    /* FACT_RES / service button (per-variant: PC8 on 12DI, PE10 on 12DO/4RTD/8AIC, PC6 default)
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
