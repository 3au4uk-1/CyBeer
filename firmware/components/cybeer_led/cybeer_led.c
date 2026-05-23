#include "cybeer_led.h"
#include "cybeer_config.h"
#include "cybeer_storage.h"
#include "driver/rmt_types.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "led_strip.h"
#include "led_strip_rmt.h"
#include "led_strip_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "cybeer_led";
/** Delay FINISHED flash so Wi-Fi/httpd can finish work after switch transition. */
#define FINISHED_FLASH_DELAY_US 150000
#define CYBEER_LED_FINISHED_FLASH_US    180000
#define CYBEER_LED_FINISHED_DIM_HOLD_US 700000
#define CYBEER_LED_OTA_OK_HOLD_US       3000000
#define CYBEER_LED_OTA_FAIL_PHASE_US    150000
#define CYBEER_LED_OTA_FAIL_TOTAL_US    900000
static led_strip_handle_t s_strip;
static uint8_t s_led_count = CYBEER_LED_COUNT_DEFAULT;
/** Global brightness scale [1,255]; uniform multiplier for every LED effect. */
static uint8_t s_led_brightness_base = CYBEER_LED_BRIGHTNESS_DEFAULT;
static cybeer_led_fx_t s_fx_requested = CYBEER_LED_FX_AMBIENT;
static int64_t s_finished_enter_us;
static int64_t s_ota_transient_enter_us;
static int64_t s_podium_until_us;
static bool s_has_unclaimed_run;
static cybeer_led_fx_t s_delayed_fx;
static int64_t s_delayed_fx_at_us;
static void load_led_settings(void)
{
    uint8_t c = CYBEER_LED_COUNT_DEFAULT;
    uint8_t b = CYBEER_LED_BRIGHTNESS_DEFAULT;

    esp_err_t err = cybeer_nvs_get_led_settings(&c, &b);
    if (err != ESP_OK) {
        s_led_count = CYBEER_LED_COUNT_DEFAULT;
        s_led_brightness_base = CYBEER_LED_BRIGHTNESS_DEFAULT;
        return;
    }

    if (c == 0 || c > CYBEER_LED_COUNT_MAX) {
        s_led_count = CYBEER_LED_COUNT_DEFAULT;
    } else {
        s_led_count = c;
    }
    if (b == 0) {
        s_led_brightness_base = CYBEER_LED_BRIGHTNESS_DEFAULT;
    } else {
        s_led_brightness_base = b;
    }
}
static uint32_t tri_wave(int64_t now_us, int half_period_ms)
{
    if (half_period_ms <= 0) {
        return 255;
    }
    const int64_t period_us = (int64_t)half_period_ms * 1000 * 2;
    int64_t t = now_us % period_us;
    if (t < 0) {
        t += period_us;
    }
    const int64_t half = period_us / 2;
    uint32_t u;
    if (t < half) {
        u = (uint32_t)((t * 255) / half);
    } else {
        t -= half;
        u = (uint32_t)(((half - t) * 255) / half);
    }
    return u > 255 ? 255 : u;
}
static void rgb_scale(uint32_t br, uint8_t r, uint8_t g, uint8_t b, uint32_t *out_r,
                      uint32_t *out_g, uint32_t *out_b)
{
    *out_r = (uint32_t)r * br / 255;
    *out_g = (uint32_t)g * br / 255;
    *out_b = (uint32_t)b * br / 255;
}

/** Per-effect level [0..255] scaled by global brightness from admin (1..255). */
static uint32_t led_combined_br(uint32_t effect_br)
{
    return (effect_br * (uint32_t)s_led_brightness_base) / 255;
}

static void led_color(uint32_t effect_br, uint8_t r, uint8_t g, uint8_t b, uint32_t *out_r,
                      uint32_t *out_g, uint32_t *out_b)
{
    rgb_scale(led_combined_br(effect_br), r, g, b, out_r, out_g, out_b);
}
static esp_err_t draw_all(uint32_t r, uint32_t g, uint32_t b)
{
    if (!s_strip || s_led_count == 0) {
        return ESP_OK;
    }
    esp_err_t e = ESP_OK;
    for (uint32_t i = 0; i < (uint32_t)s_led_count; i++) {
        e = led_strip_set_pixel(s_strip, i, r, g, b);
        if (e != ESP_OK) {
            return e;
        }
    }
    e = led_strip_refresh(s_strip);
    taskYIELD();
    return e;
}
static esp_err_t render_ambient(int64_t now_us)
{
    const uint32_t wave = tri_wave(now_us, 1800);
    uint32_t r;
    uint32_t g;
    uint32_t b;
    led_color(wave, 10, 200, 20, &r, &g, &b);
    return draw_all(r, g, b);
}
static esp_err_t render_claim_pending(int64_t now_us)
{
    const uint32_t wave = (((now_us / 1000) % 900) < 450) ? 255U : 0U;
    uint32_t r;
    uint32_t g;
    uint32_t b;
    led_color(wave, 255, 220, 0, &r, &g, &b);
    return draw_all(r, g, b);
}
static esp_err_t render_frame(int64_t now_us)
{
    if (!s_strip || s_led_count == 0) {
        return ESP_OK;
    }
    cybeer_led_fx_t rq = s_fx_requested;
    if (s_fx_requested == CYBEER_LED_FX_PODIUM && now_us >= s_podium_until_us) {
        s_fx_requested = CYBEER_LED_FX_AMBIENT;
        rq = CYBEER_LED_FX_AMBIENT;
    }
    if (s_fx_requested == CYBEER_LED_FX_OTA_OK &&
        (now_us - s_ota_transient_enter_us) >= (int64_t)CYBEER_LED_OTA_OK_HOLD_US) {
        s_fx_requested = CYBEER_LED_FX_AMBIENT;
        rq = CYBEER_LED_FX_AMBIENT;
    }
    if (s_fx_requested == CYBEER_LED_FX_OTA_FAIL &&
        (now_us - s_ota_transient_enter_us) >= (int64_t)CYBEER_LED_OTA_FAIL_TOTAL_US) {
        s_fx_requested = CYBEER_LED_FX_AMBIENT;
        rq = CYBEER_LED_FX_AMBIENT;
    }
    if (rq == CYBEER_LED_FX_FINISHED) {
        const int64_t dt = now_us - s_finished_enter_us;
        if (dt < (int64_t)CYBEER_LED_FINISHED_FLASH_US) {
            uint32_t r;
            uint32_t g;
            uint32_t b;
            led_color(255, 255, 255, 240, &r, &g, &b);
            return draw_all(r, g, b);
        }
        const int64_t dim_until =
            (int64_t)CYBEER_LED_FINISHED_FLASH_US + (int64_t)CYBEER_LED_FINISHED_DIM_HOLD_US;
        if (dt < dim_until) {
            uint32_t r;
            uint32_t g;
            uint32_t b;
            led_color(255, 38, 40, 42, &r, &g, &b);
            return draw_all(r, g, b);
        }
        if (s_has_unclaimed_run) {
            return render_claim_pending(now_us);
        }
        return render_ambient(now_us);
    }
    switch (rq) {
    case CYBEER_LED_FX_AMBIENT:
        return render_ambient(now_us);
    case CYBEER_LED_FX_ARMED: {
        uint32_t r;
        uint32_t g;
        uint32_t b;
        led_color(255, 20, 255, 20, &r, &g, &b);
        return draw_all(r, g, b);
    }
    case CYBEER_LED_FX_RUNNING: {
        const uint32_t n = (uint32_t)(s_led_count ? s_led_count : 1);
        const uint32_t head = ((uint32_t)(now_us / 45000)) % n;
        int tail_span = s_led_count > 14 ? 8 : ((int)s_led_count / 3);
        if (tail_span < 3) {
            tail_span = 3;
        }
        for (uint32_t i = 0; i < (uint32_t)s_led_count; i++) {
            int dist = (int)head - (int)i;
            if (dist < 0) {
                dist += s_led_count;
            }
            uint32_t rr;
            uint32_t gg;
            uint32_t bb;
            if (dist == 0) {
                led_color(255, 255, 100, 0, &rr, &gg, &bb);
            } else if (dist < tail_span) {
                const uint32_t falloff =
                    ((uint32_t)(tail_span - dist) * 255 + (uint32_t)tail_span - 1u) / (uint32_t)tail_span;
                led_color(falloff, 255, 110, 0, &rr, &gg, &bb);
            } else {
                led_color(255, 0, 14, 8, &rr, &gg, &bb);
            }
            esp_err_t e = led_strip_set_pixel(s_strip, i, rr, gg, bb);
            if (e != ESP_OK) {
                return e;
            }
        }
        return led_strip_refresh(s_strip);
    }
    case CYBEER_LED_FX_CLAIM_PENDING:
        return render_claim_pending(now_us);
    case CYBEER_LED_FX_PODIUM: {
        const uint32_t wave = tri_wave(now_us, 200);
        uint32_t r;
        uint32_t g;
        uint32_t b;
        led_color(wave, 255, 210, 32, &r, &g, &b);
        return draw_all(r, g, b);
    }
    case CYBEER_LED_FX_WIFI_SETUP: {
        const uint32_t wave = tri_wave(now_us, 380);
        const uint32_t effect_br = 96 + (wave * 159 / 255);
        uint32_t r;
        uint32_t g;
        uint32_t b;
        led_color(effect_br, 24, 64, 255, &r, &g, &b);
        return draw_all(r, g, b);
    }
    case CYBEER_LED_FX_OTA_DOWNLOAD: {
        const uint32_t wave = tri_wave(now_us, 1500);
        uint32_t r;
        uint32_t g;
        uint32_t b;
        led_color(wave, 0, 0, 255, &r, &g, &b);
        return draw_all(r, g, b);
    }
    case CYBEER_LED_FX_OTA_WRITE: {
        const uint32_t n = (uint32_t)(s_led_count ? s_led_count : 1);
        const uint32_t head = ((uint32_t)(now_us / 100000)) % n;
        int tail_span = s_led_count > 14 ? 8 : ((int)s_led_count / 3);
        if (tail_span < 3) {
            tail_span = 3;
        }
        for (uint32_t i = 0; i < (uint32_t)s_led_count; i++) {
            int dist = (int)head - (int)i;
            if (dist < 0) {
                dist += s_led_count;
            }
            uint32_t rr;
            uint32_t gg;
            uint32_t bb;
            if (dist == 0) {
                led_color(255, 0, 40, 255, &rr, &gg, &bb);
            } else if (dist < tail_span) {
                const uint32_t falloff =
                    ((uint32_t)(tail_span - dist) * 255 + (uint32_t)tail_span - 1u) / (uint32_t)tail_span;
                led_color(falloff, 0, 40, 255, &rr, &gg, &bb);
            } else {
                led_color(255, 0, 4, 20, &rr, &gg, &bb);
            }
            esp_err_t e = led_strip_set_pixel(s_strip, i, rr, gg, bb);
            if (e != ESP_OK) {
                return e;
            }
        }
        return led_strip_refresh(s_strip);
    }
    case CYBEER_LED_FX_OTA_OK: {
        uint32_t r;
        uint32_t g;
        uint32_t b;
        led_color(255, 0, 255, 0, &r, &g, &b);
        return draw_all(r, g, b);
    }
    case CYBEER_LED_FX_OTA_FAIL: {
        const int64_t dt = now_us - s_ota_transient_enter_us;
        const int phase = (int)(dt / (int64_t)CYBEER_LED_OTA_FAIL_PHASE_US);
        if (phase % 2 == 0) {
            uint32_t r;
            uint32_t g;
            uint32_t b;
            led_color(255, 255, 0, 0, &r, &g, &b);
            return draw_all(r, g, b);
        }
        return draw_all(0, 0, 0);
    }
    default:
        return draw_all(0, 0, 0);
    }
}
void cybeer_led_init(void)
{
    load_led_settings();
    ESP_LOGI(TAG, "LED strip count=%u brightness=%u (max %u)", (unsigned)s_led_count,
             (unsigned)s_led_brightness_base, (unsigned)CYBEER_LED_COUNT_MAX);
    if (s_led_count == 0) {
        s_strip = NULL;
        ESP_LOGI(TAG, "LED strip disabled (count=0), no RMT on GPIO%u", (unsigned)CYBEER_GPIO_LED_DATA);
        return;
    }
    led_strip_config_t strip_config = {
        .strip_gpio_num = CYBEER_GPIO_LED_DATA,
        .max_leds = CYBEER_LED_COUNT_MAX,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = { .invert_out = 0 },
    };
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 0,
        .flags = { .with_dma = 0 },
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip));
    ESP_LOGI(TAG, "WS2812 on GPIO%u (RMT)", (unsigned)CYBEER_GPIO_LED_DATA);
}

bool cybeer_led_strip_active(void)
{
    return s_strip != NULL && s_led_count > 0;
}

static void apply_fx_immediate(cybeer_led_fx_t fx)
{
    if (fx == s_fx_requested) {
        return;
    }
    const int64_t now = esp_timer_get_time();
    if (fx == CYBEER_LED_FX_FINISHED) {
        s_finished_enter_us = now;
    }
    if (fx == CYBEER_LED_FX_OTA_OK || fx == CYBEER_LED_FX_OTA_FAIL) {
        s_ota_transient_enter_us = now;
    }
    if (fx == CYBEER_LED_FX_PODIUM) {
        s_podium_until_us = now + (int64_t)6000000;
    }
    s_fx_requested = fx;
}

void cybeer_led_set_fx(cybeer_led_fx_t fx)
{
    if (fx == CYBEER_LED_FX_FINISHED) {
        s_delayed_fx = fx;
        s_delayed_fx_at_us = esp_timer_get_time() + (int64_t)FINISHED_FLASH_DELAY_US;
        return;
    }
    s_delayed_fx_at_us = 0;
    apply_fx_immediate(fx);
}
void cybeer_led_set_unclaimed_flag(bool has_unclaimed)
{
    s_has_unclaimed_run = has_unclaimed;
}
void cybeer_led_task_tick(int64_t now_us)
{
    if (s_delayed_fx_at_us != 0 && now_us >= s_delayed_fx_at_us) {
        s_delayed_fx_at_us = 0;
        apply_fx_immediate(s_delayed_fx);
    }
    esp_err_t e = render_frame(now_us);
    if (e != ESP_OK) {
        ESP_LOGD(TAG, "strip err %s", esp_err_to_name(e));
    }
}
