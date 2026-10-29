/**
 * @file  boot_wdg.c
 * @brief Independent watchdog for the bootloader (see boot_wdg.h).
 *
 * Register-level on purpose: the HAL IWDG module is not enabled in this build.
 */

#include "boot_wdg.h"

#include "stm32f4xx.h"

#define IWDG_KEY_RELOAD   0xAAAAu
#define IWDG_KEY_ACCESS   0x5555u
#define IWDG_KEY_START    0xCCCCu
#define IWDG_PR_DIV128    5u       /* LSI/128 */
#define IWDG_RELOAD_MAX   0x0FFFu

static volatile unsigned s_started;

void boot_wdg_start(void)
{
    /* Freeze the counter while a debugger halts the core. */
    DBGMCU->APB1FZ |= DBGMCU_APB1_FZ_DBG_IWDG_STOP;

    IWDG->KR = IWDG_KEY_START;            /* also starts LSI */
    IWDG->KR = IWDG_KEY_ACCESS;
    IWDG->PR = IWDG_PR_DIV128;
    IWDG->RLR = IWDG_RELOAD_MAX;
    /* PVU/RVU clear once the new values reach the LSI domain (a few LSI cycles). */
    for (volatile unsigned guard = 0; IWDG->SR != 0u && guard < 2000000u; guard++) { }
    IWDG->KR = IWDG_KEY_RELOAD;
    s_started = 1u;
}

void boot_wdg_refresh(void)
{
    if (s_started != 0u) { IWDG->KR = IWDG_KEY_RELOAD; }
}
