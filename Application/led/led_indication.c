/**
 * @file  led_indication.c
 * @brief STAT_LED (PC8) patterns for bootloader states.
 */

#include "led_indication.h"
#include "stm32f4xx_hal.h"
#include "main.h"

/* Per-variant STAT_LED pin/port (see main.h). On 12DO this is PE9; on 12DI/4RTD
 * it is PC8. Must never be a DQ output pin. */
#define LED_PORT    STAT_LED_GPIO_Port
#define LED_PIN     STAT_LED_Pin

static led_pattern_t s_pattern = LED_PATTERN_OFF;
static uint32_t      s_last_toggle;
static uint8_t       s_burst_count;

/* Discovery "flash LED": rapid blink until this tick, overriding the pattern.
 * 0 = inactive. */
#define LED_IDENTIFY_HALF_MS   100u
static uint32_t      s_identify_until;

void led_indication_init(void)
{
    s_pattern     = LED_PATTERN_OFF;
    s_last_toggle = 0;
    s_burst_count = 0;
    HAL_GPIO_WritePin(LED_PORT, LED_PIN, GPIO_PIN_RESET);
}

void led_indication_set(led_pattern_t pattern)
{
    if (pattern != s_pattern) {
        s_pattern     = pattern;
        s_burst_count = 0;
        s_last_toggle = HAL_GetTick();

        if (pattern == LED_PATTERN_ERROR) {
            HAL_GPIO_WritePin(LED_PORT, LED_PIN, GPIO_PIN_SET);
        } else if (pattern == LED_PATTERN_OFF) {
            HAL_GPIO_WritePin(LED_PORT, LED_PIN, GPIO_PIN_RESET);
        }
    }
}

void led_indication_signal_identify(uint32_t ms)
{
    s_identify_until = HAL_GetTick() + ms;
    if (s_identify_until == 0u) { s_identify_until = 1u; } /* 0 = inactive */
    s_last_toggle = HAL_GetTick();
    HAL_GPIO_WritePin(LED_PORT, LED_PIN, GPIO_PIN_SET);
}

void led_indication_poll(uint32_t now_ms)
{
    /* Identify override: rapid blink for its bounded duration, then let the
     * base pattern resume. */
    if (s_identify_until != 0u) {
        if ((int32_t)(now_ms - s_identify_until) >= 0) {
            s_identify_until = 0u;
            s_last_toggle    = now_ms;
            s_burst_count    = 0u;
            if (s_pattern == LED_PATTERN_ERROR) {
                HAL_GPIO_WritePin(LED_PORT, LED_PIN, GPIO_PIN_SET);
            } else if (s_pattern == LED_PATTERN_OFF) {
                HAL_GPIO_WritePin(LED_PORT, LED_PIN, GPIO_PIN_RESET);
            }
            /* IDLE / RECEIVING / INSTALLING resume on subsequent polls. */
        } else if ((now_ms - s_last_toggle) >= LED_IDENTIFY_HALF_MS) {
            HAL_GPIO_TogglePin(LED_PORT, LED_PIN);
            s_last_toggle = now_ms;
        }
        return;
    }

    uint32_t elapsed = now_ms - s_last_toggle;

    switch (s_pattern) {
    case LED_PATTERN_IDLE:
        if (elapsed >= 500u) {
            HAL_GPIO_TogglePin(LED_PORT, LED_PIN);
            s_last_toggle = now_ms;
        }
        break;

    case LED_PATTERN_RECEIVING:
        if (elapsed >= 100u) {
            HAL_GPIO_TogglePin(LED_PORT, LED_PIN);
            s_last_toggle = now_ms;
        }
        break;

    case LED_PATTERN_INSTALLING:
        if (s_burst_count < 6u) {
            if (elapsed >= 80u) {
                HAL_GPIO_TogglePin(LED_PORT, LED_PIN);
                s_last_toggle = now_ms;
                s_burst_count++;
            }
        } else {
            if (elapsed >= 600u) {
                s_burst_count = 0;
                s_last_toggle = now_ms;
            }
        }
        break;

    case LED_PATTERN_ERROR:
    case LED_PATTERN_OFF:
    default:
        break;
    }
}
