#include "cybeer_power.h"

#include "cybeer_config.h"
#include "cybeer_display.h"
#include "cybeer_led.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "cybeer_power";

#define IDLE_SLEEP_US       ((int64_t)CYBEER_IDLE_SLEEP_MS * 1000LL)
#define WAKE_WINDOW_US      ((int64_t)CYBEER_WAKE_CLICK_WINDOW_MS * 1000LL)
#define CLICK_MIN_US        ((int64_t)CYBEER_WAKE_CLICK_MIN_MS * 1000LL)
#define CLICK_MAX_US        ((int64_t)CYBEER_WAKE_CLICK_MAX_MS * 1000LL)
#define CLICK_GAP_MAX_US    ((int64_t)CYBEER_WAKE_CLICK_GAP_MS * 1000LL)

static int64_t s_last_activity_us;

typedef enum {
    TAP_IDLE = 0,
    TAP_DOWN,
} tap_state_t;

static bool switch_raw_pressed(void)
{
    return gpio_get_level(CYBEER_GPIO_SWITCH) == 0;
}

static void disable_gpio_wake_sources(void)
{
    gpio_wakeup_disable((gpio_num_t)CYBEER_GPIO_SWITCH);
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
}

static bool count_triple_tap(void)
{
    const int64_t deadline_us = esp_timer_get_time() + WAKE_WINDOW_US;
    int clicks = 0;
    tap_state_t st = TAP_IDLE;
    int64_t down_since_us = 0;
    int64_t last_click_us = 0;

    if (switch_raw_pressed()) {
        st = TAP_DOWN;
        down_since_us = esp_timer_get_time();
    }

    while (esp_timer_get_time() < deadline_us) {
        const bool pressed = switch_raw_pressed();
        const int64_t now = esp_timer_get_time();

        switch (st) {
        case TAP_IDLE:
            if (pressed) {
                if (clicks > 0 && (now - last_click_us) > CLICK_GAP_MAX_US) {
                    clicks = 0;
                }
                st = TAP_DOWN;
                down_since_us = now;
            }
            break;
        case TAP_DOWN:
            if (!pressed) {
                const int64_t held = now - down_since_us;
                if (held >= CLICK_MIN_US && held <= CLICK_MAX_US) {
                    clicks++;
                    last_click_us = now;
                    ESP_LOGI(TAG, "wake tap %d/%d", clicks, CYBEER_WAKE_CLICK_COUNT);
                    if (clicks >= CYBEER_WAKE_CLICK_COUNT) {
                        return true;
                    }
                }
                st = TAP_IDLE;
            } else if ((now - down_since_us) > CLICK_MAX_US) {
                st = TAP_IDLE;
            }
            break;
        default:
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }

    ESP_LOGW(TAG, "wake unlock failed (%d/%d taps)", clicks, CYBEER_WAKE_CLICK_COUNT);
    return false;
}

static void configure_light_sleep_gpio_wake(void)
{
    gpio_wakeup_enable((gpio_num_t)CYBEER_GPIO_SWITCH, GPIO_INTR_NEGEDGE);
    ESP_ERROR_CHECK(esp_sleep_enable_gpio_wakeup());
}

/**
 * GPIO9 is not an RTC pin on ESP32-C3 — deep-sleep GPIO wake only works on GPIO0–5.
 * Light sleep supports GPIO9; we use that for idle power-down instead of deep sleep.
 */
static void enter_idle_sleep_until_unlocked(void)
{
    if (!esp_sleep_is_valid_wakeup_gpio((gpio_num_t)CYBEER_GPIO_SWITCH)) {
        ESP_LOGI(TAG,
                 "GPIO%d: light-sleep wake (deep-sleep GPIO wake is GPIO0–5 only on ESP32-C3)",
                 CYBEER_GPIO_SWITCH);
    }

    ESP_LOGI(TAG, "entering light sleep (wake: %d taps on GPIO%d)",
             CYBEER_WAKE_CLICK_COUNT, CYBEER_GPIO_SWITCH);

    cybeer_display_blank();
    cybeer_led_prepare_sleep();

    esp_wifi_set_ps(WIFI_PS_MAX_MODEM);

    for (;;) {
        configure_light_sleep_gpio_wake();
        esp_light_sleep_start();
        disable_gpio_wake_sources();

        if (count_triple_tap()) {
            esp_wifi_set_ps(WIFI_PS_NONE);
            cybeer_power_note_activity();
            cybeer_display_show_zeros();
            ESP_LOGI(TAG, "wake unlock OK");
            return;
        }
    }
}

void cybeer_power_note_activity(void)
{
    s_last_activity_us = esp_timer_get_time();
}

bool cybeer_power_confirm_wake_or_sleep(void)
{
    s_last_activity_us = esp_timer_get_time();
    disable_gpio_wake_sources();
    return true;
}

void cybeer_power_maybe_sleep(bool ota_active, bool timer_running)
{
    if (ota_active || timer_running) {
        return;
    }

    const int64_t now = esp_timer_get_time();
    if (s_last_activity_us == 0) {
        s_last_activity_us = now;
        return;
    }
    if ((now - s_last_activity_us) < IDLE_SLEEP_US) {
        return;
    }

    enter_idle_sleep_until_unlocked();
}
