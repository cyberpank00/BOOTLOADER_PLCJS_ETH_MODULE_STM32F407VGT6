/**
 * @file  boot_jump.c
 * @brief Deinitialize peripherals and transfer control to the application.
 */

#include "boot_jump.h"
#include "stm32f4xx_hal.h"

extern ETH_HandleTypeDef heth;

__attribute__((naked, noreturn))
static void app_branch(uint32_t msp __attribute__((unused)),
                       uint32_t entry __attribute__((unused)))
{
    __asm volatile(
        "movs r2, #0\n"
        "msr control, r2\n"
        "msr basepri, r2\n"
        "msr faultmask, r2\n"
        "msr msp, r0\n"
        "dsb\n"
        "isb\n"
        "cpsie i\n"
        "bx r1\n");
}

void boot_jump_to_app(uint32_t app_base)
{
    const uint32_t *vectors = (const uint32_t *)app_base;
    uint32_t msp            = vectors[0];
    uint32_t reset_handler  = vectors[1];

    /* Finish HAL operations while their SysTick timeouts are still usable. */
    if (heth.Instance == ETH) {
        HAL_NVIC_DisableIRQ(ETH_IRQn);
        if (HAL_ETH_GetState(&heth) == HAL_ETH_STATE_STARTED)
            HAL_ETH_Stop_IT(&heth);
        __HAL_RCC_ETHMAC_FORCE_RESET();
        __DSB();
        __HAL_RCC_ETHMAC_RELEASE_RESET();
        HAL_ETH_DeInit(&heth);
    }
    if (HAL_RCC_DeInit() != HAL_OK) HAL_NVIC_SystemReset();
    HAL_DeInit();
    __disable_irq();

    /* 1. Stop SysTick after HAL_RCC_DeInit, which restarts it. */
    SysTick->CTRL = 0u;
    SysTick->LOAD = 0u;
    SysTick->VAL  = 0u;
    SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk | SCB_ICSR_PENDSVCLR_Msk;

    /* 2. Disable all interrupts and clear pending bits */
    for (uint32_t i = 0; i < 8u; i++) {
        NVIC->ICER[i] = 0xFFFFFFFFu;
        NVIC->ICPR[i] = 0xFFFFFFFFu;
    }

    /* 3. HAL and DMA have already been deinitialized. */

    /* 4. Relocate the vector table */
    SCB->VTOR = app_base;

    /* 5-6. Finish memory accesses before changing the stack in the trampoline. */
    __DSB();
    __ISB();

    /* 7. Jump to application Reset_Handler */
    app_branch(msp, reset_handler);

    /* Should never reach here */
    for (;;) {}
}
