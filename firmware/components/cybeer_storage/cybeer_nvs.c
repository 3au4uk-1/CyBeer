#include "cybeer_storage.h"

#include <string.h>

#include "cybeer_config.h"
#include "esp_check.h"
#include "esp_random.h"
#include "mbedtls/md.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "cybeer_nvs";

static const char *const CY_NS = "cybeer";

static const char *const KEY_WIFI_SSID = "wifi_sta_ssid";
static const char *const KEY_WIFI_PASS = "wifi_sta_pass";
static const char *const KEY_ADMIN_HASH = "admin_pin_hash";
static const char *const KEY_ADMIN_SALT = "admin_pin_salt";
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

    uint8_t c = CYBEER_LED_COUNT_DEFAULT;
    uint8_t b = CYBEER_LED_BRIGHTNESS_DEFAULT;

    nvs_handle_t h;
    esp_err_t open_err = nvs_open(CY_NS, NVS_READONLY, &h);
    if (open_err != ESP_OK) {
        *led_count = c;
        *led_brightness = b;
        /* Treat missing NVS partition / namespace same as unreadable → defaults. */
        return (open_err == ESP_ERR_NVS_NOT_FOUND || open_err == ESP_ERR_NVS_NO_FREE_PAGES)
                   ? ESP_OK
                   : open_err;
    }
    esp_err_t e1 = nvs_get_u8(h, KEY_LED_COUNT, &c);
    if (e1 != ESP_OK && e1 != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(h);
        return e1;
    }
    esp_err_t e2 = nvs_get_u8(h, KEY_LED_BRIGHT, &b);
    if (e2 != ESP_OK && e2 != ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(h);
        return e2;
    }
    nvs_close(h);

    if (c == 0 || c > CYBEER_LED_COUNT_MAX) {
        c = CYBEER_LED_COUNT_DEFAULT;
    }
    if (b == 0) {
        b = CYBEER_LED_BRIGHTNESS_DEFAULT;
    }

    *led_count = c;
    *led_brightness = b;
    return ESP_OK;
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

esp_err_t cybeer_nvs_get_admin_pin_salt(uint8_t salt_out[CYBEER_ADMIN_PIN_SALT_LEN])
{
    ESP_RETURN_ON_FALSE(salt_out, ESP_ERR_INVALID_ARG, TAG, "null");

    nvs_handle_t h;
    esp_err_t err = nvs_open(CY_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return err;
    }
    size_t len = CYBEER_ADMIN_PIN_SALT_LEN;
    err = nvs_get_blob(h, KEY_ADMIN_SALT, salt_out, &len);
    nvs_close(h);
    if (err == ESP_OK && len != CYBEER_ADMIN_PIN_SALT_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }
    return err;
}

bool cybeer_nvs_admin_pin_is_configured(void)
{
    uint8_t hash[CYBEER_ADMIN_PIN_HASH_LEN];
    return cybeer_nvs_get_admin_pin_hash(hash) == ESP_OK;
}

static esp_err_t pin_hash_derive(const uint8_t salt[CYBEER_ADMIN_PIN_SALT_LEN], const char *pin,
                                 uint8_t out[CYBEER_ADMIN_PIN_HASH_LEN])
{
    size_t pn = strlen(pin);
    if (pn > 96) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t in[CYBEER_ADMIN_PIN_SALT_LEN + 96];
    memcpy(in, salt, CYBEER_ADMIN_PIN_SALT_LEN);
    memcpy(in + CYBEER_ADMIN_PIN_SALT_LEN, pin, pn);
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!md || mbedtls_md(md, in, CYBEER_ADMIN_PIN_SALT_LEN + pn, out) != 0) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static bool buf_eq_ct(const uint8_t *a, const uint8_t *b, size_t n)
{
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) {
        d |= (uint8_t)(a[i] ^ b[i]);
    }
    return d == 0;
}

esp_err_t cybeer_admin_pin_first_setup(const char *pin)
{
    ESP_RETURN_ON_FALSE(pin && pin[0], ESP_ERR_INVALID_ARG, TAG, "pin");
    if (cybeer_nvs_admin_pin_is_configured()) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t salt[CYBEER_ADMIN_PIN_SALT_LEN];
    esp_fill_random(salt, sizeof(salt));

    uint8_t hash[CYBEER_ADMIN_PIN_HASH_LEN];
    ESP_RETURN_ON_ERROR(pin_hash_derive(salt, pin, hash), TAG, "sha256");

    nvs_handle_t h;
    esp_err_t err = nvs_open(CY_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(h, KEY_ADMIN_SALT, salt, CYBEER_ADMIN_PIN_SALT_LEN);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, KEY_ADMIN_HASH, hash, CYBEER_ADMIN_PIN_HASH_LEN);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

esp_err_t cybeer_admin_verify_pin(const char *pin)
{
    ESP_RETURN_ON_FALSE(pin && pin[0], ESP_ERR_INVALID_ARG, TAG, "pin");

    uint8_t salt[CYBEER_ADMIN_PIN_SALT_LEN];
    uint8_t stored[CYBEER_ADMIN_PIN_HASH_LEN];
    if (cybeer_nvs_get_admin_pin_salt(salt) != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    if (cybeer_nvs_get_admin_pin_hash(stored) != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t got[CYBEER_ADMIN_PIN_HASH_LEN];
    esp_err_t herr = pin_hash_derive(salt, pin, got);
    if (herr != ESP_OK) {
        return herr;
    }
    if (!buf_eq_ct(got, stored, CYBEER_ADMIN_PIN_HASH_LEN)) {
        return ESP_FAIL;
    }
    return ESP_OK;
}
