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

static const char *TAG = "cybeer_led";
#define CYBEER_LED_FINISHED_FLASH_US    180000
#define CYBEER_LED_FINISHED_DIM_HOLD_US 700000
#ifndef CYBEER_LED_POST_FINISH_CLAIM_PENDING
#define CYBEER_LED_POST_FINISH_CLAIM_PENDING 0
#endif
static led_strip_handle_t s_strip;
static uint8_t s_led_count = CYBEER_LED_COUNT_DEFAULT;
/** Global brightness scale [1,255]; applied alongside per-effect offsets. */
static uint8_t s_led_brightness_base = CYBEER_LED_BRIGHTNESS_DEFAULT;
static cybeer_led_fx_t s_fx_requested = CYBEER_LED_FX_AMBIENT;
static int64_t s_finished_enter_us;
static int64_t s_podium_until_us;
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
static esp_err_t draw_all(uint32_t r, uint32_t g, uint32_t b)
{
    esp_err_t e = ESP_OK;
    for (uint32_t i = 0; i < (uint32_t)s_led_count; i++) {
        e = led_strip_set_pixel(s_strip, i, r, g, b);
        if (e != ESP_OK) {
            return e;
        }
    }
    return led_strip_refresh(s_strip);
}
static esp_err_t render_ambient(int64_t now_us)
{
    uint32_t br = tri_wave(now_us, 1800);
    const uint32_t base = s_led_brightness_base;
    uint32_t r;
    uint32_t g;
    uint32_t b;
    rgb_scale(base * br / 255, 10, 200, 20, &r, &g, &b);
    return draw_all(r, g, b);
}
static esp_err_t render_claim_pending(int64_t now_us)
{
    uint32_t br = ((((now_us / 1000) % 900) < 450) ? (uint32_t)s_led_brightness_base + 60 : 0);
    uint32_t r;
    uint32_t g;
    uint32_t b;
    rgb_scale(br, 255, 220, 0, &r, &g, &b);
    return draw_all(r, g, b);
}
static esp_err_t render_frame(int64_t now_us)
{
    cybeer_led_fx_t rq = s_fx_requested;
    if (s_fx_requested == CYBEER_LED_FX_PODIUM && now_us >= s_podium_until_us) {
        s_fx_requested = CYBEER_LED_FX_AMBIENT;
        rq = CYBEER_LED_FX_AMBIENT;
    }
    if (rq == CYBEER_LED_FX_FINISHED) {
        const int64_t dt = now_us - s_finished_enter_us;
        if (dt < (int64_t)CYBEER_LED_FINISHED_FLASH_US) {
            return draw_all(255, 255, 240);
        }
        const int64_t dim_until =
            (int64_t)CYBEER_LED_FINISHED_FLASH_US + (int64_t)CYBEER_LED_FINISHED_DIM_HOLD_US;
        if (dt < dim_until) {
            return draw_all(38, 40, 42);
        }
#if CYBEER_LED_POST_FINISH_CLAIM_PENDING
        return render_claim_pending(now_us);
#else
        return render_ambient(now_us);
#endif
    }
    switch (rq) {
    case CYBEER_LED_FX_AMBIENT:
        return render_ambient(now_us);
    case CYBEER_LED_FX_ARMED: {
        uint32_t r;
        uint32_t g;
        uint32_t b;
        const uint32_t bright = (uint32_t)s_led_brightness_base + 80;
        rgb_scale(bright > 255 ? 255 : bright, 20, 255, 20, &r, &g, &b);
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
                rr = 255;
                gg = 100;
                bb = 0;
            } else if (dist < tail_span) {
                uint32_t falloff =
                    ((uint32_t)(tail_span - dist) * 255 + (uint32_t)tail_span - 1u) / (uint32_t)tail_span;
                rgb_scale(((uint32_t)s_led_brightness_base + 140) * falloff / 255, 255, 110, 0, &rr, &gg, &bb);
            } else {
                rr = 0;
                gg = 14;
                bb = 8;
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
        uint32_t br = (uint32_t)s_led_brightness_base + tri_wave(now_us, 200);
        if (br > 255) {
            br = 255;
        }
        uint32_t r;
        uint32_t g;
        uint32_t b;
        rgb_scale(br, 255, 210, 32, &r, &g, &b);
        return draw_all(r, g, b);
    }
    case CYBEER_LED_FX_WIFI_SETUP: {
        uint32_t br = (uint32_t)s_led_brightness_base / 3 + tri_wave(now_us, 380) * 170 / 255;
        uint32_t r;
        uint32_t g;
        uint32_t b;
        rgb_scale(br, 24, 64, 255, &r, &g, &b);
        return draw_all(r, g, b);
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
void cybeer_led_set_fx(cybeer_led_fx_t fx)
{
    if (fx == s_fx_requested) {
        return;
    }
    const int64_t now = esp_timer_get_time();
    if (fx == CYBEER_LED_FX_FINISHED) {
        s_finished_enter_us = now;
    }
    if (fx == CYBEER_LED_FX_PODIUM) {
        s_podium_until_us = now + (int64_t)3000000;
    }
    s_fx_requested = fx;
}
void cybeer_led_task_tick(int64_t now_us)
{
    esp_err_t e = render_frame(now_us);
    if (e != ESP_OK) {
        ESP_LOGD(TAG, "strip err %s", esp_err_to_name(e));
    }
}
