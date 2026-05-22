#include "cybeer_battery.h"

#include <stdbool.h>

#include "cybeer_config.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "cybeer_battery";

/* ESP32-C3: ADC1 channel 0 == GPIO0 (must match CYBEER_GPIO_BATTERY_ADC). */
#if CYBEER_GPIO_BATTERY_ADC != 0
#error "Update BATTERY_ADC_CH for this GPIO (ESP32-C3 ADC1 ch0 is GPIO0)"
#endif
#define BATTERY_ADC_CH ADC_CHANNEL_0

#define CELL_V_EMPTY  3.3f
#define CELL_V_FULL   4.2f
#define DIVIDER_RATIO 2.0f
#define SAMPLE_PERIOD_MS 5000

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static bool s_cali_ok;

static int s_percent;
static float s_voltage_cell;
static portMUX_TYPE s_data_mux = portMUX_INITIALIZER_UNLOCKED;

static cybeer_battery_listener_t s_listener;

static bool battery_adc_calibration_init(adc_unit_t unit, adc_atten_t atten, adc_cali_handle_t *out_handle)
{
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cfg = {
        .unit_id = unit,
        .atten = atten,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cfg, out_handle) == ESP_OK) {
        return true;
    }
#endif
#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t cfg = {
        .unit_id = unit,
        .atten = atten,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_line_fitting(&cfg, out_handle) == ESP_OK) {
        return true;
    }
#endif
    *out_handle = NULL;
    return false;
}

static void sample_and_update(void)
{
    int raw = 0;
    if (adc_oneshot_read(s_adc, BATTERY_ADC_CH, &raw) != ESP_OK) {
        ESP_LOGW(TAG, "ADC read failed");
        return;
    }

    int mv = 0;
    if (s_cali_ok && s_cali) {
        if (adc_cali_raw_to_voltage(s_cali, raw, &mv) != ESP_OK) {
            mv = (raw * 3300) / 4095;
        }
    } else {
        mv = (raw * 3300) / 4095;
    }

    const float v_pin = (float)mv / 1000.0f;
    const float v_cell = v_pin * DIVIDER_RATIO;

    float pct = (v_cell - CELL_V_EMPTY) / (CELL_V_FULL - CELL_V_EMPTY) * 100.0f;
    if (pct < 0.f) {
        pct = 0.f;
    } else if (pct > 100.f) {
        pct = 100.f;
    }
    const int pi = (int)(pct + 0.5f);

    portENTER_CRITICAL(&s_data_mux);
    s_percent = pi;
    s_voltage_cell = v_cell;
    portEXIT_CRITICAL(&s_data_mux);

    if (s_listener) {
        s_listener(pi);
    }
}

static void battery_task(void *arg)
{
    (void)arg;
    /* First sample runs synchronously in cybeer_battery_init(); stagger subsequent reads. */
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(SAMPLE_PERIOD_MS));
        sample_and_update();
    }
}

void cybeer_battery_register_listener(cybeer_battery_listener_t fn)
{
    s_listener = fn;
}

int cybeer_battery_get_percent(void)
{
    portENTER_CRITICAL(&s_data_mux);
    const int p = s_percent;
    portEXIT_CRITICAL(&s_data_mux);
    return p;
}

float cybeer_battery_get_voltage(void)
{
    portENTER_CRITICAL(&s_data_mux);
    const float v = s_voltage_cell;
    portEXIT_CRITICAL(&s_data_mux);
    return v;
}

esp_err_t cybeer_battery_init(void)
{
    s_listener = NULL;
    s_percent = 0;
    s_voltage_cell = CELL_V_EMPTY;
    s_cali_ok = false;
    s_cali = NULL;

    adc_oneshot_unit_init_cfg_t ucfg = {
        .unit_id = ADC_UNIT_1,
    };
    esp_err_t err = adc_oneshot_new_unit(&ucfg, &s_adc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_new_unit: %s", esp_err_to_name(err));
        return err;
    }

    adc_oneshot_chan_cfg_t chcfg = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = ADC_ATTEN_DB_12,
    };
    err = adc_oneshot_config_channel(s_adc, BATTERY_ADC_CH, &chcfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_config_channel: %s", esp_err_to_name(err));
        adc_oneshot_del_unit(s_adc);
        s_adc = NULL;
        return err;
    }

    s_cali_ok = battery_adc_calibration_init(ADC_UNIT_1, ADC_ATTEN_DB_12, &s_cali);
    if (s_cali_ok) {
        ESP_LOGI(TAG, "ADC calibration enabled");
    } else {
        ESP_LOGW(TAG, "ADC calibration unavailable; using linear estimate");
    }

    ESP_LOGI(TAG, "Battery ADC ADC1 GPIO%u, atten 12 dB, sample every %d ms", (unsigned)CYBEER_GPIO_BATTERY_ADC,
             SAMPLE_PERIOD_MS);

    const BaseType_t ok = xTaskCreate(battery_task, "cybeer_battery", 3072, NULL, tskIDLE_PRIORITY + 2, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "battery_task create failed");
        adc_oneshot_del_unit(s_adc);
        s_adc = NULL;
        return ESP_FAIL;
    }

    sample_and_update();

    return ESP_OK;
}
