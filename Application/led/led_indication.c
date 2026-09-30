/**
 * @file  led_indication.c
 * @brief STAT_LED patterns for bootloader states.
 *
 *  The LED is driven through a software PWM: TIM7 fires at 64 x 200 Hz and
 *  the ISR compares a free-running counter with the requested level. This
 *  gives the IDLE state a 1 Hz fade in / fade out (same look as the module
 *  firmwares' fault indication) while the other patterns simply use level 0
 *  or LED_LEVEL_MAX. The pin differs per variant (PE9 / PC6) so a hardware
 *  timer channel would not be uniform; TIM7 has no outputs and is free here.
 *  led_indication_deinit() stops the timer before the jump to the application.
 */

#include "led_indication.h"
#include "stm32f4xx_hal.h"
#include "main.h"

/* Per-variant STAT_LED pin/port (see main.h). On 12DO & 4RTD this is PE9; on
 * 12DI it is PC6. Must never be a DQ output pin. */
#define LED_PORT    STAT_LED_GPIO_Port
#define LED_PIN     STAT_LED_Pin

/* Software PWM: 84 MHz timer clock / 84 / 78 = 12.82 kHz -> 64 levels @ 200 Hz. */
#define LED_LEVELS          64u
#define LED_LEVEL_MAX       (LED_LEVELS - 1u)
#define LED_TIM_PRESCALER   (84u - 1u)
#define LED_TIM_PERIOD      (78u - 1u)

/* IDLE: fade in over half the period, fade out over the other half. */
#define LED_IDLE_FADE_MS    1000u

static led_pattern_t   s_pattern = LED_PATTERN_OFF;
static uint32_t        s_last_toggle;
static uint8_t         s_burst_count;
static uint8_t         s_on;            /* logical on/off for the blink patterns */
static volatile uint8_t s_level;        /* PWM level 0..LED_LEVEL_MAX            */
static uint8_t         s_pwm_counter;

/* Discovery "flash LED": rapid blink until this tick, overriding the pattern.
 * 0 = inactive. */
#define LED_IDENTIFY_HALF_MS   100u
static uint32_t      s_identify_until;

static inline void led_write(uint8_t on)
{
    s_on    = on;
    s_level = on ? (uint8_t)LED_LEVEL_MAX : 0u;
}

static inline void led_toggle(void)
{
    led_write((uint8_t)!s_on);
}

static void pwm_start(void)
{
    __HAL_RCC_TIM7_CLK_ENABLE();
    TIM7->CR1  = 0u;
    TIM7->PSC  = LED_TIM_PRESCALER;
    TIM7->ARR  = LED_TIM_PERIOD;
    TIM7->EGR  = TIM_EGR_UG;
    TIM7->SR   = 0u;
    TIM7->DIER = TIM_DIER_UIE;
    HAL_NVIC_SetPriority(TIM7_IRQn, 6u, 0u);
    HAL_NVIC_EnableIRQ(TIM7_IRQn);
    TIM7->CR1 = TIM_CR1_CEN;
}

void TIM7_IRQHandler(void)
{
    TIM7->SR = 0u;
    const uint8_t c = s_pwm_counter;
    s_pwm_counter = (uint8_t)((c + 1u) % LED_LEVELS);
    if (s_level > c) { LED_PORT->BSRR = LED_PIN; }
    else             { LED_PORT->BSRR = (uint32_t)LED_PIN << 16; }
}

void led_indication_init(void)
{
    s_pattern     = LED_PATTERN_OFF;
    s_last_toggle = 0;
    s_burst_count = 0;
    led_write(0u);
    HAL_GPIO_WritePin(LED_PORT, LED_PIN, GPIO_PIN_RESET);
    pwm_start();
}

void led_indication_deinit(void)
{
    HAL_NVIC_DisableIRQ(TIM7_IRQn);
    TIM7->CR1  = 0u;
    TIM7->DIER = 0u;
    TIM7->SR   = 0u;
    __HAL_RCC_TIM7_CLK_DISABLE();
    HAL_GPIO_WritePin(LED_PORT, LED_PIN, GPIO_PIN_RESET);
}

void led_indication_set(led_pattern_t pattern)
{
    if (pattern != s_pattern) {
        s_pattern     = pattern;
        s_burst_count = 0;
        s_last_toggle = HAL_GetTick();

        if (pattern == LED_PATTERN_ERROR) {
            led_write(1u);
        } else if (pattern == LED_PATTERN_OFF) {
            led_write(0u);
        }
    }
}

void led_indication_signal_identify(uint32_t ms)
{
    s_identify_until = HAL_GetTick() + ms;
    if (s_identify_until == 0u) { s_identify_until = 1u; } /* 0 = inactive */
    s_last_toggle = HAL_GetTick();
    led_write(1u);
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
                led_write(1u);
            } else if (s_pattern == LED_PATTERN_OFF) {
                led_write(0u);
            }
            /* IDLE / RECEIVING / INSTALLING resume on subsequent polls. */
        } else if ((now_ms - s_last_toggle) >= LED_IDENTIFY_HALF_MS) {
            led_toggle();
            s_last_toggle = now_ms;
        }
        return;
    }

    uint32_t elapsed = now_ms - s_last_toggle;

    switch (s_pattern) {
    case LED_PATTERN_IDLE: {
        /* Triangle 0 -> 1 -> 0 over LED_IDLE_FADE_MS, squared for a
         * perceptually even ramp. Phase is derived from the tick directly, so
         * no per-poll state is needed. */
        const uint32_t ph   = now_ms % LED_IDLE_FADE_MS;
        const uint32_t half = LED_IDLE_FADE_MS / 2u;
        const uint32_t tri  = (ph < half) ? ph : (LED_IDLE_FADE_MS - ph);   /* 0..half */
        s_level = (uint8_t)((tri * tri * LED_LEVEL_MAX) / (half * half));
        break;
    }

    case LED_PATTERN_RECEIVING:
        if (elapsed >= 100u) {
            led_toggle();
            s_last_toggle = now_ms;
        }
        break;

    case LED_PATTERN_INSTALLING:
        if (s_burst_count < 6u) {
            if (elapsed >= 80u) {
                led_toggle();
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
