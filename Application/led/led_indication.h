/**
 * @file  led_indication.h
 * @brief Status LED indication for bootloader states.
 *
 *  Pattern              Meaning
 *  -------------------  ----------------------------
 *  Fade in/out (1 Hz)   Waiting for command
 *  Fast blink (5 Hz)    Receiving firmware
 *  Burst (3 blinks)     Installing firmware
 *  Solid ON             Error
 *
 *  Driven through a TIM7 software PWM (see led_indication.c); call
 *  led_indication_deinit() before jumping to the application.
 */
#ifndef LED_INDICATION_H
#define LED_INDICATION_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LED_PATTERN_IDLE,       /* slow blink  */
    LED_PATTERN_RECEIVING,  /* fast blink  */
    LED_PATTERN_INSTALLING, /* burst       */
    LED_PATTERN_ERROR,      /* solid on    */
    LED_PATTERN_OFF,
} led_pattern_t;

void led_indication_init(void);
/** Stop the PWM timer and its interrupt, LED off. Required before the jump. */
void led_indication_deinit(void);
void led_indication_set(led_pattern_t pattern);
void led_indication_poll(uint32_t now_ms);

/**
 * Blink the STAT_LED rapidly for @p ms milliseconds to physically locate the
 * device (discovery "flash LED"). Overrides the current pattern for the
 * duration, then the base pattern resumes on the next poll.
 */
void led_indication_signal_identify(uint32_t ms);

#ifdef __cplusplus
}
#endif

#endif /* LED_INDICATION_H */
