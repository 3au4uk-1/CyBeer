#include "cybeer_battery.h"
#include "cybeer_display.h"
#include "cybeer_fsm.h"
#include "cybeer_led.h"
#include "cybeer_storage.h"
#include "cybeer_web.h"
#include "cybeer_wifi.h"
#include "cybeer_switch.h"
#include "cybeer_timer.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"

static const char *TAG = "cybeer";

static void on_finished_placeholder(int64_t duration_us, void *user_ctx)
{
    (void)user_ctx;

    cybeer_run_t run = { 0 };
    cybeer_format_uuid_v4(run.id);
    run.participant_id[0] = '\0';
    run.duration_us = duration_us;
    cybeer_storage_iso8601_now(run.finished_at);
    run.claimed = false;
    run.tournament_match_id[0] = '\0';

    esp_err_t err = cybeer_storage_add_run(&run);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "storage_add_run failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "run saved id=%s duration_us=%lld", run.id, (long long)run.duration_us);
        cybeer_ws_on_run_finished(run.id, run.duration_us);
    }
}

static void display_task(void *pvParameters)
{
    (void)pvParameters;

    cybeer_state_t fsm_prev = CYBEER_STATE_PREP;

    for (;;) {
        const int64_t now = esp_timer_get_time();

        cybeer_switch_state_t sw = { 0 };
        cybeer_switch_poll(now, &sw);
        if (sw.pressed_stable) {
            cybeer_fsm_on_switch_stable(sw.pressed, now);
        }

        cybeer_fsm_snapshot_t snap = cybeer_fsm_snapshot();
        if (snap.state != fsm_prev) {
            cybeer_ws_broadcast_state();
        }
        cybeer_ws_timer_tick(now);

        switch (snap.state) {
        case CYBEER_STATE_RUNNING:
            cybeer_display_show_us(cybeer_timer_elapsed_us(now));
            break;
        case CYBEER_STATE_PREP:
            cybeer_display_show_zeros();
            break;
        case CYBEER_STATE_READY:
        case CYBEER_STATE_FINISHED:
            cybeer_display_show_us(snap.finished_duration_us);
            break;
        default:
            break;
        }

        /*
         * Stub: when claimed flow exists, FINISHED timeline can yield CLAIM_PENDING in cybeer_led.c
         * (CYBEER_LED_POST_FINISH_CLAIM_PENDING). M1 completes FINISHED animation then ambient.
         */
        cybeer_led_fx_t led_fx = CYBEER_LED_FX_AMBIENT;
        switch (snap.state) {
        case CYBEER_STATE_PREP:
            led_fx = CYBEER_LED_FX_ARMED;
            break;
        case CYBEER_STATE_RUNNING:
            led_fx = CYBEER_LED_FX_RUNNING;
            break;
        case CYBEER_STATE_READY:
            led_fx = CYBEER_LED_FX_AMBIENT;
            break;
        case CYBEER_STATE_FINISHED:
            if (fsm_prev != CYBEER_STATE_FINISHED) {
                cybeer_led_set_fx(CYBEER_LED_FX_FINISHED);
            }
            break;
        default:
            break;
        }
        if (snap.state != CYBEER_STATE_FINISHED) {
            cybeer_led_set_fx(led_fx);
        }
        fsm_prev = snap.state;

        cybeer_led_task_tick(now);

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(cybeer_storage_init());
    ESP_ERROR_CHECK(cybeer_battery_init());
    ESP_LOGI(TAG, "CyBeer boot");

    cybeer_switch_init();
    cybeer_display_init();
    cybeer_led_init();

    cybeer_fsm_callbacks_t cb = {
        .on_finished = on_finished_placeholder,
        .user_ctx = NULL,
    };
    cybeer_fsm_init(&cb);
    cybeer_fsm_reset_to_prep(esp_timer_get_time());

    ESP_ERROR_CHECK(cybeer_wifi_init());
    ESP_ERROR_CHECK(cybeer_web_start());
    ESP_ERROR_CHECK(cybeer_wifi_start());

    const BaseType_t ok = xTaskCreate(display_task, "display_task", 4096, NULL, tskIDLE_PRIORITY + 1, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "display_task create failed");
    }
}