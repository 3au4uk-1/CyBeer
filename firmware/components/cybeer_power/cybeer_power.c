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
static bool s_idle_latched;
static bool s_eco_mode;

typedef enum {
    TAP_IDLE = 0,
    TAP_DOWN,
} tap_state_t;

typedef struct {
    tap_state_t st;
    int clicks;
    int64_t window_deadline_us;
    int64_t down_since_us;
    int64_t last_click_us;
} tap_ctx_t;

static tap_ctx_t s_tap;

static bool switch_raw_pressed(void)
{
    return gpio_get_level(CYBEER_GPIO_SWITCH) == 0;
}

static void disable_gpio_wake_sources(void)
{
    gpio_wakeup_disable((gpio_num_t)CYBEER_GPIO_SWITCH);
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
}

static void tap_reset(void)
{
    s_tap.st = TAP_IDLE;
    s_tap.clicks = 0;
    s_tap.window_deadline_us = 0;
    s_tap.down_since_us = 0;
    s_tap.last_click_us = 0;
}

/** Advance tap detector by one sample; true when CYBEER_WAKE_CLICK_COUNT taps in window. */
static bool tap_poll_step(void)
{
    const bool pressed = switch_raw_pressed();
    const int64_t now = esp_timer_get_time();

    if (s_tap.clicks > 0 && s_tap.window_deadline_us > 0 && now > s_tap.window_deadline_us) {
        ESP_LOGD(TAG, "wake tap window expired (%d/%d)", s_tap.clicks, CYBEER_WAKE_CLICK_COUNT);
        tap_reset();
    }

    switch (s_tap.st) {
    case TAP_IDLE:
        if (pressed) {
            if (s_tap.clicks > 0 && (now - s_tap.last_click_us) > CLICK_GAP_MAX_US) {
                tap_reset();
            }
            s_tap.st = TAP_DOWN;
            s_tap.down_since_us = now;
        }
        break;
    case TAP_DOWN:
        if (!pressed) {
            const int64_t held = now - s_tap.down_since_us;
            if (held >= CLICK_MIN_US && held <= CLICK_MAX_US) {
                if (s_tap.clicks == 0) {
                    s_tap.window_deadline_us = now + WAKE_WINDOW_US;
                }
                s_tap.clicks++;
                s_tap.last_click_us = now;
                ESP_LOGI(TAG, "wake tap %d/%d", s_tap.clicks, CYBEER_WAKE_CLICK_COUNT);
                if (s_tap.clicks >= CYBEER_WAKE_CLICK_COUNT) {
                    tap_reset();
                    return true;
                }
            }
            s_tap.st = TAP_IDLE;
        } else if ((now - s_tap.down_since_us) > CLICK_MAX_US) {
            s_tap.st = TAP_IDLE;
        }
        break;
    default:
        break;
    }

    return false;
}

static void enter_idle_latched(void)
{
    if (s_idle_latched) {
        return;
    }

    s_idle_latched = true;
    tap_reset();
    cybeer_display_blank();
    cybeer_led_prepare_sleep();
    esp_wifi_set_ps(WIFI_PS_MAX_MODEM);

    ESP_LOGI(TAG, "idle: display/LED off — %d short taps on GPIO%d to wake",
             CYBEER_WAKE_CLICK_COUNT, CYBEER_GPIO_SWITCH);
}

static void leave_idle_latched(void)
{
    s_idle_latched = false;
    tap_reset();
    esp_wifi_set_ps(WIFI_PS_NONE);
    cybeer_display_show_zeros();
    if (cybeer_led_strip_active()) {
        cybeer_led_set_fx(CYBEER_LED_FX_ARMED);
    }
    cybeer_power_note_activity();
    ESP_LOGI(TAG, "idle wake OK");
}

void cybeer_power_note_activity(void)
{
    s_last_activity_us = esp_timer_get_time();
    if (s_idle_latched) {
        leave_idle_latched();
    }
}

bool cybeer_power_is_idle(void)
{
    return s_idle_latched;
}

bool cybeer_power_confirm_wake_or_sleep(void)
{
    s_last_activity_us = esp_timer_get_time();
    s_idle_latched = false;
    tap_reset();
    disable_gpio_wake_sources();
    return true;
}

void cybeer_power_poll(void)
{
    if (!s_idle_latched) {
        return;
    }
    if (tap_poll_step()) {
        leave_idle_latched();
    }
}

void cybeer_power_maybe_sleep(bool ota_active, bool timer_running)
{
    cybeer_power_poll();

    if (s_idle_latched || s_eco_mode || ota_active || timer_running) {
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

    enter_idle_latched();
}

bool cybeer_power_is_eco(void)
{
    return s_eco_mode;
}

bool cybeer_power_toggle_eco(void)
{
    s_eco_mode = !s_eco_mode;
    if (s_eco_mode) {
        ESP_LOGI(TAG, "eco mode ON");
        cybeer_display_blank();
        cybeer_led_prepare_sleep();
    } else {
        ESP_LOGI(TAG, "eco mode OFF");
        cybeer_display_show_zeros();
        if (cybeer_led_strip_active()) {
            cybeer_led_set_fx(CYBEER_LED_FX_ARMED);
        }
        cybeer_power_note_activity();
    }
    return s_eco_mode;
}

static void configure_light_sleep_gpio_wake(void)
{
    gpio_wakeup_enable((gpio_num_t)CYBEER_GPIO_SWITCH, GPIO_INTR_NEGEDGE);
    ESP_ERROR_CHECK(esp_sleep_enable_gpio_wakeup());
}

static bool count_triple_tap_after_wake(void)
{
    tap_reset();
    const int64_t deadline_us = esp_timer_get_time() + WAKE_WINDOW_US;
    while (esp_timer_get_time() < deadline_us) {
        if (tap_poll_step()) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    ESP_LOGW(TAG, "light sleep wake: triple-tap not completed");
    return false;
}

static void enter_light_sleep_until_unlocked(void)
{
    ESP_LOGI(TAG, "entering light sleep (wake: %d taps on GPIO%d)",
             CYBEER_WAKE_CLICK_COUNT, CYBEER_GPIO_SWITCH);

    cybeer_display_blank();
    cybeer_led_prepare_sleep();
    esp_wifi_set_ps(WIFI_PS_MAX_MODEM);

    for (;;) {
        configure_light_sleep_gpio_wake();
        esp_light_sleep_start();
        disable_gpio_wake_sources();

        if (count_triple_tap_after_wake()) {
            esp_wifi_set_ps(WIFI_PS_NONE);
            cybeer_power_note_activity();
            cybeer_display_show_zeros();
            if (cybeer_led_strip_active()) {
                cybeer_led_set_fx(CYBEER_LED_FX_ARMED);
            }
            ESP_LOGI(TAG, "light sleep wake OK");
            return;
        }
    }
}

static void sleep_task_fn(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(500));
    enter_light_sleep_until_unlocked();
    vTaskDelete(NULL);
}

void cybeer_power_trigger_sleep(void)
{
    xTaskCreate(sleep_task_fn, "cybeer_sleep", 3072, NULL, 5, NULL);
}
