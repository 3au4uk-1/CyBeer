#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    CYBEER_LED_FX_AMBIENT,
    CYBEER_LED_FX_ARMED,
    CYBEER_LED_FX_RUNNING,
    CYBEER_LED_FX_FINISHED,
    CYBEER_LED_FX_CLAIM_PENDING,
    CYBEER_LED_FX_PODIUM,
    CYBEER_LED_FX_WIFI_SETUP,
} cybeer_led_fx_t;

void cybeer_led_init(void);
void cybeer_led_set_fx(cybeer_led_fx_t fx);
void cybeer_led_set_unclaimed_flag(bool has_unclaimed);
void cybeer_led_task_tick(int64_t now_us);
