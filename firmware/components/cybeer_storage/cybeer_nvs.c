#include "cybeer_storage.h"

#include <string.h>

#include "esp_check.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "cybeer_nvs";

static const char *const CY_NS = "cybeer";

static const char *const KEY_WIFI_SSID = "wifi_sta_ssid";
static const char *const KEY_WIFI_PASS = "wifi_sta_pass";
static const char *const KEY_ADMIN_HASH = "admin_pin_hash";
static const char *const KEY_LED_COUNT = "led_count";
static const char *const KEY_LED_BRIGHT = "led_brightness";

esp_err_t cybeer_nvs_get_wifi(char *ssid_out, size_t ssid_max, char *pass_out, size_t pass_max)
{
    ESP_RETURN_ON_FALSE(ssid_out && ssid_max && pass_out && pass_max, ESP_ERR_INVALID_ARG, TAG,
                        "null/out_max");

    nvs_handle_t h;
    esp_err_t err = nvs_open(CY_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }

    size_t ssid_len = ssid_max;
    size_t pass_len = pass_max;
    err = nvs_get_str(h, KEY_WIFI_SSID, ssid_out, &ssid_len);
    if (err != ESP_OK) {
        nvs_close(h);
        return err;
    }
    err = nvs_get_str(h, KEY_WIFI_PASS, pass_out, &pass_len);
    nvs_close(h);
    return err;
}

esp_err_t cybeer_nvs_set_wifi(const char *ssid, const char *pass)
{
    ESP_RETURN_ON_FALSE(ssid, ESP_ERR_INVALID_ARG, TAG, "null ssid");
    const char *pass_str = pass ? pass : "";

    nvs_handle_t h;
    esp_err_t err = nvs_open(CY_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, KEY_WIFI_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(h, KEY_WIFI_PASS, pass_str);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t cybeer_nvs_clear_wifi(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(CY_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_erase_key(h, KEY_WIFI_SSID);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err != ESP_OK) {
        nvs_close(h);
        return err;
    }
    err = nvs_erase_key(h, KEY_WIFI_PASS);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err != ESP_OK) {
        nvs_close(h);
        return err;
    }
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t cybeer_nvs_get_led_settings(uint8_t *led_count, uint8_t *led_brightness)
{
    ESP_RETURN_ON_FALSE(led_count && led_brightness, ESP_ERR_INVALID_ARG, TAG, "null");

    nvs_handle_t h;
    esp_err_t err = nvs_open(CY_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_get_u8(h, KEY_LED_COUNT, led_count);
    if (err != ESP_OK) {
        nvs_close(h);
        return err;
    }
    err = nvs_get_u8(h, KEY_LED_BRIGHT, led_brightness);
    nvs_close(h);
    return err;
}

esp_err_t cybeer_nvs_set_led_count(uint8_t count)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(CY_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(h, KEY_LED_COUNT, count);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t cybeer_nvs_set_led_brightness(uint8_t brightness)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(CY_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(h, KEY_LED_BRIGHT, brightness);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t cybeer_nvs_get_admin_pin_hash(uint8_t hash_out[CYBEER_ADMIN_PIN_HASH_LEN])
{
    ESP_RETURN_ON_FALSE(hash_out, ESP_ERR_INVALID_ARG, TAG, "null");

    nvs_handle_t h;
    esp_err_t err = nvs_open(CY_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }
    size_t len = CYBEER_ADMIN_PIN_HASH_LEN;
    err = nvs_get_blob(h, KEY_ADMIN_HASH, hash_out, &len);
    nvs_close(h);
    if (err == ESP_OK && len != CYBEER_ADMIN_PIN_HASH_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }
    return err;
}

esp_err_t cybeer_nvs_set_admin_pin_hash(const uint8_t hash[CYBEER_ADMIN_PIN_HASH_LEN])
{
    ESP_RETURN_ON_FALSE(hash, ESP_ERR_INVALID_ARG, TAG, "null");

    nvs_handle_t h;
    esp_err_t err = nvs_open(CY_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(h, KEY_ADMIN_HASH, hash, CYBEER_ADMIN_PIN_HASH_LEN);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}
