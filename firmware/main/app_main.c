#include "nvs_flash.h"
#include "esp_log.h"

static const char *TAG = "cybeer";

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_LOGI(TAG, "CyBeer boot");
}
