#include "cybeer_display.h"
#include "cybeer_fsm.h"
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
    (void)duration_us;
    (void)user_ctx;
}

static void display_task(void *pvParameters)
{
    (void)pvParameters;

    for (;;) {
        const int64_t now = esp_timer_get_time();

        cybeer_switch_state_t sw = { 0 };
        cybeer_switch_poll(now, &sw);
        if (sw.pressed_stable) {
            cybeer_fsm_on_switch_stable(sw.pressed, now);
        }

        cybeer_fsm_snapshot_t snap = cybeer_fsm_snapshot();
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

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_LOGI(TAG, "CyBeer boot");

    cybeer_switch_init();
    cybeer_display_init();

    cybeer_fsm_callbacks_t cb = {
        .on_finished = on_finished_placeholder,
        .user_ctx = NULL,
    };
    cybeer_fsm_init(&cb);
    cybeer_fsm_reset_to_prep(esp_timer_get_time());

    const BaseType_t ok = xTaskCreate(display_task, "display_task", 4096, NULL, tskIDLE_PRIORITY + 1, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "display_task create failed");
    }
}
