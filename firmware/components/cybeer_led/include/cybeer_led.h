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
    CYBEER_LED_FX_OTA_DOWNLOAD,
    CYBEER_LED_FX_OTA_WRITE,
    CYBEER_LED_FX_OTA_OK,
    CYBEER_LED_FX_OTA_FAIL,
} cybeer_led_fx_t;

void cybeer_led_init(void);
/** False when LED count is 0 (no RMT traffic; saves CPU for Wi-Fi). */
bool cybeer_led_strip_active(void);
void cybeer_led_set_fx(cybeer_led_fx_t fx);
void cybeer_led_set_unclaimed_flag(bool has_unclaimed);
void cybeer_led_task_tick(int64_t now_us);
/** Black out strip before deep sleep. */
void cybeer_led_prepare_sleep(void);
