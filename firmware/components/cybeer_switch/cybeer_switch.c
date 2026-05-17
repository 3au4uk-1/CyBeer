#include "cybeer_switch.h"

#include <stdbool.h>
#include <stdint.h>

#include "cybeer_config.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "sdkconfig.h"

#define DEBOUNCE_US   ((int64_t)CYBEER_DEBOUNCE_MS * 1000LL)
#define STABLE_ON_US  ((int64_t)CYBEER_STABLE_ON_MS * 1000LL)
#define STABLE_OFF_US ((int64_t)CYBEER_STABLE_OFF_MS * 1000LL)

#if CONFIG_CYBEER_SWITCH_TEST
static bool s_test_injected;
static bool s_test_raw_pressed;
#endif

static bool s_fsm_started;
static bool s_last_raw_sample;
static int64_t s_raw_stable_since_us;
static bool s_debounced_pressed;
static int64_t s_debounced_since_us;
static bool s_debounced_time_valid;

static bool read_raw_pressed(void)
{
#if CONFIG_CYBEER_SWITCH_TEST
    if (s_test_injected) {
        return s_test_raw_pressed;
    }
#endif
    /* Pull-up, active LOW when bottle on platform */
    return gpio_get_level(CYBEER_GPIO_SWITCH) == 0;
}

static void fsm_reset(void)
{
    s_fsm_started = false;
    s_last_raw_sample = false;
    s_raw_stable_since_us = 0;
    s_debounced_pressed = false;
    s_debounced_since_us = 0;
    s_debounced_time_valid = false;
}

void cybeer_switch_init(void)
{
#if !CONFIG_CYBEER_SWITCH_TEST
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << CYBEER_GPIO_SWITCH,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
#else
    s_test_injected = false;
    s_test_raw_pressed = false;
#endif
    fsm_reset();
}

#if CONFIG_CYBEER_SWITCH_TEST
void cybeer_switch_test_set_raw(bool bottle_on_platform)
{
    s_test_injected = true;
    s_test_raw_pressed = bottle_on_platform;
}

void cybeer_switch_test_reset(void)
{
    s_test_injected = false;
    s_test_raw_pressed = false;
    fsm_reset();
}
#endif

void cybeer_switch_poll(int64_t now_us, cybeer_switch_state_t *out)
{
    const bool raw = read_raw_pressed();

    if (!s_fsm_started) {
        s_last_raw_sample = raw;
        s_raw_stable_since_us = now_us;
        s_fsm_started = true;
    } else if (raw != s_last_raw_sample) {
        s_last_raw_sample = raw;
        s_raw_stable_since_us = now_us;
    }

    if ((now_us - s_raw_stable_since_us) >= DEBOUNCE_US) {
        if (s_debounced_pressed != raw) {
            s_debounced_pressed = raw;
            s_debounced_since_us = now_us;
            s_debounced_time_valid = true;
        } else if (!s_debounced_time_valid) {
            s_debounced_since_us = now_us;
            s_debounced_time_valid = true;
        }
    }

    bool pressed_stable = false;
    if (s_debounced_time_valid) {
        const int64_t need_us = s_debounced_pressed ? STABLE_ON_US : STABLE_OFF_US;
        pressed_stable = (now_us - s_debounced_since_us) >= need_us;
    }

    out->pressed = s_debounced_pressed;
    out->pressed_stable = pressed_stable;
}
