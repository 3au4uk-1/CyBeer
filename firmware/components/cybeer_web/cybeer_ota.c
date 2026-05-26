#include "cybeer_ota.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cybeer_config.h"
#include "cybeer_led.h"
#include "cybeer_storage.h"
#include "cybeer_ws.h"

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/private/sha256.h"

static const char *TAG = "cybeer_ota";

const char *cybeer_firmware_version(void)
{
    return esp_app_get_description()->version;
}

#define OTA_BUF_SIZE           4096
#define OTA_STACK_SIZE         (1024 * 16)
#define OTA_MANIFEST_MAX_SIZE  2048
#define OTA_MAX_FIRMWARE_SIZE  0x140000
#define OTA_MAX_LITTLEFS_SIZE  0x30000
#define OTA_MAX_BUNDLE_SIZE    (CYBEER_OTA_HEADER_SIZE + OTA_MAX_FIRMWARE_SIZE + OTA_MAX_LITTLEFS_SIZE)
#define OTA_URL_MAX_LEN        512

static SemaphoreHandle_t s_ota_mx;

typedef struct {
    bool active;
    int percent;
    char stage[16];
    char error[64];
} ota_status_t;

static ota_status_t s_status;

typedef struct {
    cybeer_ota_header_t hdr;
    bool header_parsed;
    uint8_t header_buf[CYBEER_OTA_HEADER_SIZE];
    size_t header_received;

    esp_ota_handle_t ota_handle;
    const esp_partition_t *ota_part;
    const esp_partition_t *fs_part;
    bool ota_begun;
    bool fw_finalized;
    bool fs_prepared;

    mbedtls_sha256_context fw_sha_ctx;
    mbedtls_sha256_context fs_sha_ctx;
    size_t fw_written;
    size_t fs_written;
    size_t total_size;
} ota_stream_ctx_t;

typedef struct {
    char url[OTA_URL_MAX_LEN];
} ota_download_args_t;

typedef struct {
    httpd_req_t *req;
    size_t content_len;
} ota_upload_args_t;

static int s_last_log_pct = -1;

static void ota_wdt_reset(void)
{
#if CONFIG_ESP_TASK_WDT_EN
    (void)esp_task_wdt_reset();
#endif
}

static void ota_task_wdt_subscribe(void)
{
#if CONFIG_ESP_TASK_WDT_EN
    (void)esp_task_wdt_add(NULL);
#endif
}

static void ota_task_wdt_unsubscribe(void)
{
#if CONFIG_ESP_TASK_WDT_EN
    (void)esp_task_wdt_delete(NULL);
#endif
}

static esp_err_t ota_stream_finalize_firmware(ota_stream_ctx_t *ctx);
static esp_err_t ota_prepare_littlefs_write(ota_stream_ctx_t *ctx);

#define OTA_MAX_REDIRECTS 5

/**
 * Open an HTTP request while manually following 3xx redirects.
 *
 * The streaming API (`esp_http_client_open` + `esp_http_client_read`) does NOT
 * honour `max_redirection_count`; that option only applies to
 * `esp_http_client_perform()`. GitHub release downloads always go through a
 * 302 hop to `release-assets.githubusercontent.com`, so without this helper the
 * OTA "download from GitHub" path bails out with HTTP 302.
 */
static esp_err_t ota_http_open_following_redirects(esp_http_client_handle_t client,
                                                   int *out_status,
                                                   int *out_content_len)
{
    for (int hop = 0; hop <= OTA_MAX_REDIRECTS; hop++) {
        esp_err_t err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            return err;
        }

        (void)esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);

        if (status == 301 || status == 302 || status == 303 ||
            status == 307 || status == 308) {
            ESP_LOGI(TAG, "OTA HTTP %d, following redirect (hop %d)", status, hop + 1);
            esp_err_t r = esp_http_client_set_redirection(client);
            esp_http_client_close(client);
            if (r != ESP_OK) {
                ESP_LOGE(TAG, "redirect parse failed: %s", esp_err_to_name(r));
                return r;
            }
            ota_wdt_reset();
            continue;
        }

        if (out_status) {
            *out_status = status;
        }
        if (out_content_len) {
            *out_content_len = esp_http_client_get_content_length(client);
        }
        return ESP_OK;
    }

    ESP_LOGE(TAG, "too many redirects (>%d)", OTA_MAX_REDIRECTS);
    return ESP_FAIL;
}

static void ota_set_status(const char *stage, int percent, const char *error)
{
    s_status.active = (error == NULL) && strcmp(stage, "done") != 0 && strcmp(stage, "idle") != 0;
    s_status.percent = percent;

    strncpy(s_status.stage, stage, sizeof(s_status.stage) - 1);
    s_status.stage[sizeof(s_status.stage) - 1] = '\0';

    if (error) {
        strncpy(s_status.error, error, sizeof(s_status.error) - 1);
        s_status.error[sizeof(s_status.error) - 1] = '\0';
    } else {
        s_status.error[0] = '\0';
    }
}

static void ota_broadcast_progress(const char *stage, int percent)
{
    ota_set_status(stage, percent, NULL);

    if (percent >= s_last_log_pct + 5 || s_last_log_pct < 0) {
        ESP_LOGI(TAG, "progress: %s %d%%", stage, percent);
        s_last_log_pct = percent;
    }

    char msg[96];
    int n = snprintf(msg, sizeof(msg),
                     "{\"type\":\"ota_progress\",\"percent\":%d,\"stage\":\"%s\"}",
                     percent, stage);
    if (n > 0) {
        cybeer_ws_broadcast_text(msg, (size_t)n);
    }
}

static void ota_broadcast_done(void)
{
    ota_set_status("done", 100, NULL);
    const char *msg = "{\"type\":\"ota_done\"}";
    cybeer_ws_broadcast_text(msg, strlen(msg));
}

static void ota_broadcast_error(const char *message)
{
    ota_set_status("error", 0, message);
    ESP_LOGE(TAG, "failed: %s", message);

    char msg[140];
    int n = snprintf(msg, sizeof(msg),
                     "{\"type\":\"ota_error\",\"message\":\"%s\"}", message);
    if (n > 0) {
        cybeer_ws_broadcast_text(msg, (size_t)n);
    }
}

static bool parse_header(const uint8_t *data, cybeer_ota_header_t *hdr)
{
    if (memcmp(data, CYBEER_OTA_MAGIC, 4) != 0) {
        return false;
    }

    hdr->header_version = data[4];
    if (hdr->header_version != 1) {
        return false;
    }

    memcpy(&hdr->firmware_size, data + 5, 4);
    memcpy(&hdr->littlefs_size, data + 9, 4);
    memcpy(hdr->firmware_sha256, data + 13, 32);
    memcpy(hdr->littlefs_sha256, data + 45, 32);
    memcpy(hdr->version, data + 77, 32);
    hdr->version[31] = '\0';

    if (hdr->firmware_size == 0 || hdr->firmware_size > OTA_MAX_FIRMWARE_SIZE) {
        return false;
    }
    if (hdr->littlefs_size == 0 || hdr->littlefs_size > OTA_MAX_LITTLEFS_SIZE) {
        return false;
    }
    return true;
}

static bool ota_partitions_ready(void)
{
    const esp_partition_t *otadata = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                              ESP_PARTITION_SUBTYPE_DATA_OTA,
                                                              NULL);
    if (!otadata) {
        ESP_LOGE(TAG, "otadata partition missing — reflash partition table via USB");
        return false;
    }
    if (!esp_ota_get_next_update_partition(NULL)) {
        ESP_LOGE(TAG, "no OTA app slot available");
        return false;
    }
    return true;
}

static const char *ota_err_user_message(esp_err_t err)
{
    switch (err) {
    case ESP_ERR_INVALID_ARG:
        return "invalid bundle format";
    case ESP_ERR_INVALID_CRC:
        return "SHA-256 mismatch";
    case ESP_ERR_INVALID_SIZE:
        return "file too large for device";
    case ESP_ERR_NOT_FOUND:
        return "OTA not configured (missing otadata). Reflash via USB";
    case ESP_ERR_INVALID_STATE:
        return "download incomplete";
    case ESP_FAIL:
        return "download failed";
    default:
        return "flash write error";
    }
}

static esp_err_t ota_stream_init(ota_stream_ctx_t *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    mbedtls_sha256_init(&ctx->fw_sha_ctx);
    mbedtls_sha256_init(&ctx->fs_sha_ctx);

    int rc = mbedtls_sha256_starts(&ctx->fw_sha_ctx, 0);
    if (rc != 0) {
        return ESP_FAIL;
    }
    rc = mbedtls_sha256_starts(&ctx->fs_sha_ctx, 0);
    if (rc != 0) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t ota_stream_feed(ota_stream_ctx_t *ctx, const uint8_t *data, size_t len)
{
    size_t offset = 0;

    if (!ctx->header_parsed) {
        size_t need = CYBEER_OTA_HEADER_SIZE - ctx->header_received;
        size_t take = len < need ? len : need;
        memcpy(ctx->header_buf + ctx->header_received, data, take);
        ctx->header_received += take;
        offset += take;

        if (ctx->header_received < CYBEER_OTA_HEADER_SIZE) {
            return ESP_OK;
        }

        if (!parse_header(ctx->header_buf, &ctx->hdr)) {
            return ESP_ERR_INVALID_ARG;
        }
        ctx->header_parsed = true;
        ctx->total_size = ctx->hdr.firmware_size + ctx->hdr.littlefs_size;

        if (!ota_partitions_ready()) {
            return ESP_ERR_NOT_FOUND;
        }

        ctx->ota_part = esp_ota_get_next_update_partition(NULL);
        if (!ctx->ota_part) {
            return ESP_ERR_NOT_FOUND;
        }
        if (ctx->hdr.firmware_size > ctx->ota_part->size) {
            return ESP_ERR_INVALID_SIZE;
        }

        esp_err_t err = esp_ota_begin(ctx->ota_part, ctx->hdr.firmware_size, &ctx->ota_handle);
        if (err != ESP_OK) {
            return err;
        }
        ctx->ota_begun = true;

        ctx->fs_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                ESP_PARTITION_SUBTYPE_ANY, "littlefs");
        if (!ctx->fs_part) {
            esp_ota_abort(ctx->ota_handle);
            return ESP_ERR_NOT_FOUND;
        }
        if (ctx->hdr.littlefs_size > ctx->fs_part->size) {
            esp_ota_abort(ctx->ota_handle);
            return ESP_ERR_INVALID_SIZE;
        }

        ESP_LOGI(TAG, "bundle v%s: firmware=%lu fs=%lu -> slot %s",
                 ctx->hdr.version,
                 (unsigned long)ctx->hdr.firmware_size,
                 (unsigned long)ctx->hdr.littlefs_size,
                 ctx->ota_part->label);
        s_last_log_pct = -1;
        cybeer_led_set_fx(CYBEER_LED_FX_OTA_WRITE);
    }

    while (offset < len && ctx->fw_written < ctx->hdr.firmware_size) {
        size_t remaining_fw = ctx->hdr.firmware_size - ctx->fw_written;
        size_t chunk = len - offset;
        if (chunk > remaining_fw) {
            chunk = remaining_fw;
        }

        esp_err_t err = esp_ota_write(ctx->ota_handle, data + offset, chunk);
        if (err != ESP_OK) {
            return err;
        }
        if (mbedtls_sha256_update(&ctx->fw_sha_ctx, data + offset, chunk) != 0) {
            return ESP_FAIL;
        }

        ctx->fw_written += chunk;
        offset += chunk;
        int pct = (int)((ctx->fw_written * 80ULL) / ctx->total_size);
        ota_broadcast_progress("firmware", pct);
        if ((ctx->fw_written & 0x7FFF) == 0) {
            taskYIELD();
        }
    }

    if (ctx->fw_written == ctx->hdr.firmware_size && !ctx->fw_finalized) {
        esp_err_t err = ota_stream_finalize_firmware(ctx);
        if (err != ESP_OK) {
            return err;
        }
        err = ota_prepare_littlefs_write(ctx);
        if (err != ESP_OK) {
            return err;
        }
    }

    while (offset < len && ctx->fs_written < ctx->hdr.littlefs_size) {
        size_t remaining_fs = ctx->hdr.littlefs_size - ctx->fs_written;
        size_t chunk = len - offset;
        if (chunk > remaining_fs) {
            chunk = remaining_fs;
        }

        esp_err_t err = esp_partition_write(ctx->fs_part, ctx->fs_written, data + offset, chunk);
        if (err != ESP_OK) {
            return err;
        }
        if (mbedtls_sha256_update(&ctx->fs_sha_ctx, data + offset, chunk) != 0) {
            return ESP_FAIL;
        }

        ctx->fs_written += chunk;
        offset += chunk;
        int pct = 80 + (int)((ctx->fs_written * 20ULL) / ctx->hdr.littlefs_size);
        ota_broadcast_progress("littlefs", pct);
        if ((ctx->fs_written & 0x3FFF) == 0) {
            taskYIELD();
        }
    }

    if (offset < len) {
        return ESP_ERR_INVALID_SIZE;
    }

    return ESP_OK;
}

static esp_err_t ota_stream_finalize_firmware(ota_stream_ctx_t *ctx)
{
    if (ctx->fw_written != ctx->hdr.firmware_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t digest[32];
    if (mbedtls_sha256_finish(&ctx->fw_sha_ctx, digest) != 0) {
        esp_ota_abort(ctx->ota_handle);
        return ESP_FAIL;
    }
    if (memcmp(digest, ctx->hdr.firmware_sha256, sizeof(digest)) != 0) {
        ESP_LOGE(TAG, "firmware SHA-256 mismatch");
        esp_ota_abort(ctx->ota_handle);
        return ESP_ERR_INVALID_CRC;
    }

    esp_err_t err = esp_ota_end(ctx->ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        return err;
    }

    ctx->fw_finalized = true;
    ctx->ota_begun = false;
    ESP_LOGI(TAG, "firmware verified and committed to %s", ctx->ota_part->label);
    return ESP_OK;
}

static esp_err_t ota_prepare_littlefs_write(ota_stream_ctx_t *ctx)
{
    if (ctx->fs_prepared) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "preparing LittleFS write (%lu bytes)", (unsigned long)ctx->hdr.littlefs_size);
    ota_broadcast_progress("littlefs", 80);
    ota_wdt_reset();

    esp_err_t err = cybeer_storage_unmount_for_ota();
    if (err != ESP_OK) {
        return err;
    }

    ota_wdt_reset();
    err = esp_partition_erase_range(ctx->fs_part, 0, ctx->fs_part->size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LittleFS erase failed: %s", esp_err_to_name(err));
        return err;
    }

    ctx->fs_prepared = true;
    ESP_LOGI(TAG, "LittleFS erased, writing image");
    return ESP_OK;
}

static esp_err_t ota_stream_finish(ota_stream_ctx_t *ctx)
{
    if (!ctx->header_parsed) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!ctx->fw_finalized) {
        esp_err_t err = ota_stream_finalize_firmware(ctx);
        if (err != ESP_OK) {
            return err;
        }
        err = ota_prepare_littlefs_write(ctx);
        if (err != ESP_OK) {
            return err;
        }
    }

    if (ctx->fs_written != ctx->hdr.littlefs_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t digest[32];
    if (mbedtls_sha256_finish(&ctx->fs_sha_ctx, digest) != 0) {
        return ESP_FAIL;
    }
    if (memcmp(digest, ctx->hdr.littlefs_sha256, sizeof(digest)) != 0) {
        ESP_LOGE(TAG, "LittleFS SHA-256 mismatch");
        return ESP_ERR_INVALID_CRC;
    }

    ESP_LOGI(TAG, "LittleFS verified, switching boot partition to %s", ctx->ota_part->label);
    return esp_ota_set_boot_partition(ctx->ota_part);
}

static void ota_stream_cleanup(ota_stream_ctx_t *ctx)
{
    mbedtls_sha256_free(&ctx->fw_sha_ctx);
    mbedtls_sha256_free(&ctx->fs_sha_ctx);
}

static bool admin_pin_ok(httpd_req_t *req)
{
    char hdr[CYBEER_ADMIN_PIN_MAX_LEN + 8];
    if (httpd_req_get_hdr_value_str(req, "X-Admin-Pin", hdr, sizeof(hdr)) != ESP_OK) {
        return false;
    }
    size_t n = strlen(hdr);
    while (n > 0 && (hdr[n - 1] == ' ' || hdr[n - 1] == '\t')) {
        hdr[--n] = '\0';
    }
    char *p = hdr;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p == '\0') {
        return false;
    }
    return cybeer_admin_verify_pin(p) == ESP_OK;
}

static esp_err_t send_json_err(httpd_req_t *req, const char *status, const char *json)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t require_admin(httpd_req_t *req)
{
    if (!cybeer_nvs_admin_pin_is_configured()) {
        return send_json_err(req, "403 Forbidden",
                             "{\"error\":\"admin pin not configured\"}");
    }
    if (!admin_pin_ok(req)) {
        return send_json_err(req, "401 Unauthorized",
                             "{\"error\":\"invalid pin\"}");
    }
    return ESP_OK;
}

static esp_err_t fetch_manifest(char **out_manifest)
{
    *out_manifest = NULL;

    esp_http_client_config_t cfg = {
        .url = CYBEER_OTA_MANIFEST_URL,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .user_agent = "CyBeer/OTA",
        .max_redirection_count = 10,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }

    int status = 0;
    int content_length = 0;
    esp_err_t err = ota_http_open_following_redirects(client, &status, &content_length);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }
    if (status != 200) {
        ESP_LOGE(TAG, "manifest HTTP %d", status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    if (content_length <= 0 || content_length > OTA_MANIFEST_MAX_SIZE) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_INVALID_SIZE;
    }

    char *manifest = calloc(1, (size_t)content_length + 1);
    if (!manifest) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    size_t total_read = 0;
    while (total_read < (size_t)content_length) {
        int r = esp_http_client_read(client, manifest + total_read,
                                     content_length - (int)total_read);
        if (r <= 0) {
            free(manifest);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }
        total_read += (size_t)r;
    }

    manifest[total_read] = '\0';
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    *out_manifest = manifest;
    return ESP_OK;
}

static bool version_is_newer(const char *remote, const char *local)
{
    int rm = 0, rn = 0, rp = 0;
    int lm = 0, ln = 0, lp = 0;

    if (sscanf(remote, "%d.%d.%d", &rm, &rn, &rp) != 3) {
        return false;
    }
    if (sscanf(local, "%d.%d.%d", &lm, &ln, &lp) != 3) {
        return false;
    }
    if (rm != lm) {
        return rm > lm;
    }
    if (rn != ln) {
        return rn > ln;
    }
    return rp > lp;
}

static esp_err_t h_get_ota_check(httpd_req_t *req)
{
    if (require_admin(req) != ESP_OK) {
        return ESP_OK;
    }

    char *manifest = NULL;
    esp_err_t err = fetch_manifest(&manifest);
    if (err != ESP_OK) {
        return send_json_err(req, "502 Bad Gateway", "{\"error\":\"manifest fetch failed\"}");
    }

    cJSON *root = cJSON_Parse(manifest);
    free(manifest);
    if (!root) {
        return send_json_err(req, "502 Bad Gateway", "{\"error\":\"manifest parse\"}");
    }

    const char *ver = cJSON_GetStringValue(cJSON_GetObjectItem(root, "version"));
    const char *changelog = cJSON_GetStringValue(cJSON_GetObjectItem(root, "changelog"));
    cJSON *size_item = cJSON_GetObjectItem(root, "size");

    cJSON *resp = cJSON_CreateObject();
    const char *current = cybeer_firmware_version();
    cJSON_AddStringToObject(resp, "currentVersion", current);
    cJSON_AddStringToObject(resp, "remoteVersion", ver ? ver : "");
    cJSON_AddBoolToObject(resp, "available", ver && version_is_newer(ver, current));
    cJSON_AddStringToObject(resp, "changelog", changelog ? changelog : "");
    if (cJSON_IsNumber(size_item)) {
        cJSON_AddNumberToObject(resp, "size", size_item->valuedouble);
    }
    cJSON_Delete(root);

    char *printed = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    if (!printed) {
        return send_json_err(req, "500 Internal Server Error", "{\"error\":\"json print\"}");
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, printed, HTTPD_RESP_USE_STRLEN);
    free(printed);
    return ESP_OK;
}

static void ota_download_task(void *arg)
{
    ota_download_args_t *args = (ota_download_args_t *)arg;
    esp_err_t err = ESP_OK;

    ota_task_wdt_subscribe();
    ESP_LOGI(TAG, "download OTA started: %s", args->url);
    cybeer_led_set_fx(CYBEER_LED_FX_OTA_DOWNLOAD);
    ota_broadcast_progress("downloading", 0);

    esp_http_client_config_t cfg = {
        .url = args->url,
        .timeout_ms = 120000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = OTA_BUF_SIZE,
        .user_agent = "CyBeer/OTA",
        .max_redirection_count = 10,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ota_broadcast_error("http init failed");
        cybeer_led_set_fx(CYBEER_LED_FX_OTA_FAIL);
        free(args);
        xSemaphoreGive(s_ota_mx);
        ota_task_wdt_unsubscribe();
        vTaskDelete(NULL);
        return;
    }

    int status = 0;
    int content_len = 0;
    err = ota_http_open_following_redirects(client, &status, &content_len);
    if (err != ESP_OK) {
        ota_broadcast_error("cannot connect to server");
        cybeer_led_set_fx(CYBEER_LED_FX_OTA_FAIL);
        esp_http_client_cleanup(client);
        free(args);
        xSemaphoreGive(s_ota_mx);
        ota_task_wdt_unsubscribe();
        vTaskDelete(NULL);
        return;
    }

    if (status != 200) {
        ESP_LOGE(TAG, "bundle HTTP %d", status);
        ota_broadcast_error("bad HTTP status from server");
        cybeer_led_set_fx(CYBEER_LED_FX_OTA_FAIL);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        free(args);
        xSemaphoreGive(s_ota_mx);
        ota_task_wdt_unsubscribe();
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "HTTP %d, Content-Length: %d", status, content_len);

    ota_stream_ctx_t ctx;
    err = ota_stream_init(&ctx);
    if (err != ESP_OK) {
        ota_broadcast_error("sha init failed");
        cybeer_led_set_fx(CYBEER_LED_FX_OTA_FAIL);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        free(args);
        xSemaphoreGive(s_ota_mx);
        ota_task_wdt_unsubscribe();
        vTaskDelete(NULL);
        return;
    }

    uint8_t buf[OTA_BUF_SIZE];
    size_t downloaded = 0;
    int last_dl_pct = -1;
    while (err == ESP_OK) {
        ota_wdt_reset();
        int r = esp_http_client_read(client, (char *)buf, sizeof(buf));
        if (r < 0) {
            err = ESP_FAIL;
            break;
        }
        if (r == 0) {
            break;
        }
        downloaded += (size_t)r;
        vTaskDelay(1);
        if (content_len > 0) {
            int pct = (int)((downloaded * 10ULL) / (size_t)content_len);
            if (pct > 10) {
                pct = 10;
            }
            if (pct != last_dl_pct) {
                last_dl_pct = pct;
                ota_broadcast_progress("downloading", pct);
            }
        }
        err = ota_stream_feed(&ctx, buf, (size_t)r);
    }

    if (err == ESP_OK) {
        err = ota_stream_finish(&ctx);
    }

    ota_stream_cleanup(&ctx);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(args);

    if (err != ESP_OK) {
        const char *msg = ota_err_user_message(err);
        ESP_LOGE(TAG, "download OTA failed: %s (%s)", msg, esp_err_to_name(err));
        ota_broadcast_error(msg);
        cybeer_led_set_fx(CYBEER_LED_FX_OTA_FAIL);
        xSemaphoreGive(s_ota_mx);
        ota_task_wdt_unsubscribe();
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "download OTA complete, rebooting in 3s");
    cybeer_led_set_fx(CYBEER_LED_FX_OTA_OK);
    ota_broadcast_done();
    ota_task_wdt_unsubscribe();
    vTaskDelay(pdMS_TO_TICKS(3000));
    esp_restart();
}

static esp_err_t h_post_ota_start(httpd_req_t *req)
{
    if (require_admin(req) != ESP_OK) {
        return ESP_OK;
    }

    if (xSemaphoreTake(s_ota_mx, 0) != pdTRUE) {
        return send_json_err(req, "409 Conflict", "{\"error\":\"ota busy\"}");
    }

    char *manifest = NULL;
    esp_err_t err = fetch_manifest(&manifest);
    if (err != ESP_OK) {
        xSemaphoreGive(s_ota_mx);
        return send_json_err(req, "502 Bad Gateway", "{\"error\":\"manifest fetch\"}");
    }

    cJSON *root = cJSON_Parse(manifest);
    free(manifest);
    if (!root) {
        xSemaphoreGive(s_ota_mx);
        return send_json_err(req, "502 Bad Gateway", "{\"error\":\"manifest parse\"}");
    }

    const char *url = cJSON_GetStringValue(cJSON_GetObjectItem(root, "url"));
    if (!url || strlen(url) >= OTA_URL_MAX_LEN || strncmp(url, "https://", 8) != 0) {
        cJSON_Delete(root);
        xSemaphoreGive(s_ota_mx);
        return send_json_err(req, "502 Bad Gateway", "{\"error\":\"bad url in manifest\"}");
    }

    ota_download_args_t *args = calloc(1, sizeof(*args));
    if (!args) {
        cJSON_Delete(root);
        xSemaphoreGive(s_ota_mx);
        return send_json_err(req, "500 Internal Server Error", "{\"error\":\"alloc\"}");
    }
    strncpy(args->url, url, sizeof(args->url) - 1);
    cJSON_Delete(root);

    ota_broadcast_progress("downloading", 0);

    BaseType_t ok = xTaskCreate(ota_download_task, "ota_dl", OTA_STACK_SIZE, args,
                                tskIDLE_PRIORITY + 5, NULL);
    if (ok != pdPASS) {
        free(args);
        xSemaphoreGive(s_ota_mx);
        return send_json_err(req, "500 Internal Server Error", "{\"error\":\"task create\"}");
    }

    httpd_resp_set_status(req, "202 Accepted");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true,\"status\":\"started\"}", HTTPD_RESP_USE_STRLEN);
}

static void ota_upload_task(void *arg)
{
    ota_upload_args_t *args = (ota_upload_args_t *)arg;
    httpd_req_t *req = args->req;
    const size_t content_len = args->content_len;
    free(args);

    ota_task_wdt_subscribe();
    ESP_LOGI(TAG, "upload OTA started (%u bytes)", (unsigned)content_len);

    httpd_resp_set_status(req, "202 Accepted");
    httpd_resp_set_type(req, "application/json");
    if (httpd_resp_send(req, "{\"ok\":true,\"status\":\"started\"}", HTTPD_RESP_USE_STRLEN) != ESP_OK) {
        ESP_LOGE(TAG, "upload: failed to send 202 response");
        httpd_req_async_handler_complete(req);
        xSemaphoreGive(s_ota_mx);
        ota_task_wdt_unsubscribe();
        vTaskDelete(NULL);
        return;
    }

    cybeer_led_set_fx(CYBEER_LED_FX_OTA_DOWNLOAD);
    ota_broadcast_progress("receiving", 0);

    ota_stream_ctx_t ctx;
    esp_err_t err = ota_stream_init(&ctx);
    if (err != ESP_OK) {
        cybeer_led_set_fx(CYBEER_LED_FX_OTA_FAIL);
        ota_broadcast_error("sha init failed");
        httpd_req_async_handler_complete(req);
        xSemaphoreGive(s_ota_mx);
        ota_task_wdt_unsubscribe();
        vTaskDelete(NULL);
        return;
    }

    uint8_t buf[OTA_BUF_SIZE];
    size_t received = 0;
    while (received < content_len && err == ESP_OK) {
        ota_wdt_reset();
        size_t want = content_len - received;
        if (want > sizeof(buf)) {
            want = sizeof(buf);
        }

        int r = httpd_req_recv(req, (char *)buf, want);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (r <= 0) {
            err = ESP_FAIL;
            break;
        }

        received += (size_t)r;
        err = ota_stream_feed(&ctx, buf, (size_t)r);
        if (received <= sizeof(buf) || (received & 0xFFFF) == 0) {
            int pct = (int)((received * 10ULL) / content_len);
            ota_broadcast_progress("receiving", pct);
        }
    }

    if (err == ESP_OK) {
        err = ota_stream_finish(&ctx);
    }
    ota_stream_cleanup(&ctx);
    httpd_req_async_handler_complete(req);

    if (err != ESP_OK) {
        const char *msg = ota_err_user_message(err);
        ESP_LOGE(TAG, "upload OTA failed: %s (%s)", msg, esp_err_to_name(err));
        cybeer_led_set_fx(CYBEER_LED_FX_OTA_FAIL);
        ota_broadcast_error(msg);
        xSemaphoreGive(s_ota_mx);
        ota_task_wdt_unsubscribe();
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "upload OTA complete, rebooting in 3s");
    cybeer_led_set_fx(CYBEER_LED_FX_OTA_OK);
    ota_broadcast_done();
    ota_task_wdt_unsubscribe();
    vTaskDelay(pdMS_TO_TICKS(3000));
    esp_restart();
}

static esp_err_t h_post_ota_upload(httpd_req_t *req)
{
    if (require_admin(req) != ESP_OK) {
        return ESP_OK;
    }

    size_t content_len = (size_t)req->content_len;
    if (content_len <= CYBEER_OTA_HEADER_SIZE || content_len > OTA_MAX_BUNDLE_SIZE) {
        return send_json_err(req, "400 Bad Request", "{\"error\":\"invalid size\"}");
    }

    if (xSemaphoreTake(s_ota_mx, 0) != pdTRUE) {
        return send_json_err(req, "409 Conflict", "{\"error\":\"ota busy\"}");
    }

    httpd_req_t *async_req = NULL;
    esp_err_t err = httpd_req_async_handler_begin(req, &async_req);
    if (err != ESP_OK || !async_req) {
        xSemaphoreGive(s_ota_mx);
        return send_json_err(req, "500 Internal Server Error", "{\"error\":\"async begin\"}");
    }

    ota_upload_args_t *args = calloc(1, sizeof(*args));
    if (!args) {
        httpd_req_async_handler_complete(async_req);
        xSemaphoreGive(s_ota_mx);
        return send_json_err(req, "500 Internal Server Error", "{\"error\":\"alloc\"}");
    }
    args->req = async_req;
    args->content_len = content_len;

    BaseType_t ok = xTaskCreate(ota_upload_task, "ota_up", OTA_STACK_SIZE, args,
                                tskIDLE_PRIORITY + 5, NULL);
    if (ok != pdPASS) {
        free(args);
        httpd_req_async_handler_complete(async_req);
        xSemaphoreGive(s_ota_mx);
        return send_json_err(req, "500 Internal Server Error", "{\"error\":\"task create\"}");
    }

    return ESP_OK;
}

static esp_err_t h_get_ota_status(httpd_req_t *req)
{
    if (require_admin(req) != ESP_OK) {
        return ESP_OK;
    }

    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        return send_json_err(req, "500 Internal Server Error", "{\"error\":\"alloc\"}");
    }
    cJSON_AddBoolToObject(resp, "active", s_status.active);
    cJSON_AddNumberToObject(resp, "percent", s_status.percent);
    cJSON_AddStringToObject(resp, "stage", s_status.stage);
    cJSON_AddStringToObject(resp, "error", s_status.error);

    char *printed = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    if (!printed) {
        return send_json_err(req, "500 Internal Server Error", "{\"error\":\"json print\"}");
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, printed, HTTPD_RESP_USE_STRLEN);
    free(printed);
    return ESP_OK;
}

esp_err_t cybeer_ota_register_handlers(httpd_handle_t server)
{
    if (!s_ota_mx) {
        s_ota_mx = xSemaphoreCreateMutex();
        if (!s_ota_mx) {
            return ESP_ERR_NO_MEM;
        }
    }

    memset(&s_status, 0, sizeof(s_status));
    ota_set_status("idle", 0, NULL);

    const httpd_uri_t handlers[] = {
        { .uri = "/api/admin/ota/check", .method = HTTP_GET, .handler = h_get_ota_check },
        { .uri = "/api/admin/ota/start", .method = HTTP_POST, .handler = h_post_ota_start },
        { .uri = "/api/admin/ota/upload", .method = HTTP_POST, .handler = h_post_ota_upload },
        { .uri = "/api/admin/ota/status", .method = HTTP_GET, .handler = h_get_ota_status },
    };

    for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); i++) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &handlers[i]), TAG,
                            "reg %s", handlers[i].uri);
    }

    ESP_LOGI(TAG, "OTA bundle endpoints registered");
    return ESP_OK;
}

bool cybeer_ota_is_active(void)
{
    return s_status.active;
}
