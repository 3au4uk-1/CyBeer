#include "cybeer_battery.h"
#include "cybeer_display.h"
#include "cybeer_fsm.h"
#include "cybeer_led.h"
#include "cybeer_storage.h"
#include "cybeer_tournament.h"
#include "cybeer_web.h"
#include "cybeer_wifi.h"
#include "cybeer_switch.h"
#include "cybeer_timer.h"
#include "esp_err.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"

static const char *TAG = "cybeer";

typedef struct {
    int64_t duration_us;
} run_save_msg_t;

static QueueHandle_t s_run_save_q;

/** Persist run + WS/LED side effects off the display/FSM task (LittleFS writes block Wi-Fi). */
static void persist_finished_run(int64_t duration_us)
{
    /* Let LED flash / state WS drain before flash write + runFinished burst. */
    vTaskDelay(pdMS_TO_TICKS(120));

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
        return;
    }
    ESP_LOGI(TAG, "run saved id=%s duration_us=%lld", run.id, (long long)run.duration_us);
    (void)cybeer_tournament_notify_run_saved(&run);
    cybeer_ws_on_run_finished(run.id, run.duration_us);
    cybeer_led_set_unclaimed_flag(true);
}

static void run_save_task(void *pvParameters)
{
    (void)pvParameters;
    run_save_msg_t msg;

    for (;;) {
        if (xQueueReceive(s_run_save_q, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        persist_finished_run(msg.duration_us);
    }
}

static void on_finished_placeholder(int64_t duration_us, void *user_ctx)
{
    (void)user_ctx;
    if (s_run_save_q == NULL) {
        persist_finished_run(duration_us);
        return;
    }

    const run_save_msg_t msg = { .duration_us = duration_us };
    if (xQueueSend(s_run_save_q, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "run save queue full, persisting inline");
        persist_finished_run(duration_us);
    }
}

static bool display_update_due(int64_t now_us, int64_t *last_us, int64_t period_us)
{
    if (*last_us == 0 || (now_us - *last_us) >= period_us) {
        *last_us = now_us;
        return true;
    }
    return false;
}

static void display_task(void *pvParameters)
{
    (void)pvParameters;

    cybeer_state_t fsm_prev = CYBEER_STATE_PREP;
    int64_t last_display_us = 0;

    for (;;) {
        const int64_t now = esp_timer_get_time();

        cybeer_switch_state_t sw = { 0 };
        cybeer_switch_poll(now, &sw);
        if (sw.pressed_stable) {
            cybeer_fsm_on_switch_stable(sw.pressed, now);
        }

        cybeer_fsm_snapshot_t snap = cybeer_fsm_snapshot();
        if (snap.state != fsm_prev) {
            if (snap.state == CYBEER_STATE_FINISHED || fsm_prev == CYBEER_STATE_RUNNING
                || snap.state == CYBEER_STATE_RUNNING) {
                cybeer_ws_broadcast_state_deferred(80000);
            } else {
                cybeer_ws_broadcast_state();
            }
            last_display_us = 0;
            if (snap.state == CYBEER_STATE_PREP) {
                cybeer_display_show_zeros();
                last_display_us = now;
            }
        }
        cybeer_ws_timer_tick(now);

        switch (snap.state) {
        case CYBEER_STATE_RUNNING:
            if (display_update_due(now, &last_display_us, 100000)) {
                cybeer_display_show_us(cybeer_timer_elapsed_us(now));
            }
            break;
        case CYBEER_STATE_READY:
        case CYBEER_STATE_FINISHED:
            if (display_update_due(now, &last_display_us, 250000)) {
                cybeer_display_show_us(snap.finished_duration_us);
            }
            break;
        default:
            break;
        }

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

        const uint32_t loop_ms = (snap.state == CYBEER_STATE_RUNNING) ? 33 : 25;
        vTaskDelay(pdMS_TO_TICKS(loop_ms));
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());

    esp_err_t mv = esp_ota_mark_app_valid_cancel_rollback();
    if (mv != ESP_OK && mv != ESP_ERR_NOT_SUPPORTED && mv != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "esp_ota_mark_app_valid_cancel_rollback: %s", esp_err_to_name(mv));
    }

    ESP_ERROR_CHECK(cybeer_storage_init());
    ESP_ERROR_CHECK(cybeer_battery_init());
    ESP_LOGI(TAG, "CyBeer boot");

    s_run_save_q = xQueueCreate(2, sizeof(run_save_msg_t));
    if (s_run_save_q != NULL) {
        const BaseType_t save_ok =
            xTaskCreate(run_save_task, "run_save", 6144, NULL, tskIDLE_PRIORITY + 2, NULL);
        if (save_ok != pdPASS) {
            ESP_LOGW(TAG, "run_save task create failed, saves run on display task");
            vQueueDelete(s_run_save_q);
            s_run_save_q = NULL;
        }
    } else {
        ESP_LOGW(TAG, "run save queue create failed");
    }

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