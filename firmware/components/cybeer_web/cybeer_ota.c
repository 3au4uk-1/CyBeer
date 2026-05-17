#include "cybeer_ota.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cybeer_config.h"
#include "cybeer_storage.h"

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_server.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"

static const char *TAG = "cybeer_ota";

#define CYBEER_OTA_MAX_FIRMWARE 0x130000
#define CYBEER_OTA_URL_MAX      512
#define CYBEER_OTA_STACK        (1024 * 10)

static SemaphoreHandle_t s_ota_mx;

static bool find_double_crlf(const uint8_t *data, size_t len, size_t *out_off)
{
    for (size_t i = 0; i + 4 <= len; i++) {
        if (data[i] == '\r' && data[i + 1] == '\n' && data[i + 2] == '\r' && data[i + 3] == '\n') {
            *out_off = i;
            return true;
        }
    }
    return false;
}

static bool find_multipart_end(const uint8_t *data, size_t len, const char *boundary, size_t *out_idx)
{
    size_t bl = strlen(boundary);
    if (len < 4 + bl) {
        return false;
    }
    for (size_t i = 0; i + 4 + bl <= len; i++) {
        if (data[i] != '\r' || data[i + 1] != '\n' || data[i + 2] != '-' || data[i + 3] != '-') {
            continue;
        }
        if (memcmp(data + i + 4, boundary, bl) != 0) {
            continue;
        }
        size_t after = i + 4 + bl;
        if (after + 2 <= len && data[after] == '-' && data[after + 1] == '-') {
            *out_idx = i;
            return true;
        }
        if (after + 2 <= len && data[after] == '\r' && data[after + 1] == '\n') {
            *out_idx = i;
            return true;
        }
    }
    return false;
}

static bool admin_pin_ok(httpd_req_t *req)
{
    uint8_t stored[CYBEER_ADMIN_PIN_HASH_LEN];
    if (cybeer_nvs_get_admin_pin_hash(stored) != ESP_OK) {
        return false;
    }

    char hdr[CYBEER_ADMIN_PIN_MAX_LEN + 8];
    if (httpd_req_get_hdr_value_str(req, "X-Admin-Pin", hdr, sizeof(hdr)) != ESP_OK) {
        return false;
    }

    while (hdr[0] == ' ' || hdr[0] == '\t') {
        memmove(hdr, hdr + 1, strlen(hdr));
    }
    size_t n = strlen(hdr);
    while (n > 0 && (hdr[n - 1] == ' ' || hdr[n - 1] == '\t')) {
        hdr[--n] = '\0';
    }
    if (n == 0 || n > CYBEER_ADMIN_PIN_MAX_LEN) {
        return false;
    }

    uint8_t digest[CYBEER_ADMIN_PIN_HASH_LEN];
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, (const unsigned char *)hdr, n);
    mbedtls_sha256_finish(&ctx, digest);
    mbedtls_sha256_free(&ctx);

    return memcmp(digest, stored, CYBEER_ADMIN_PIN_HASH_LEN) == 0;
}

static esp_err_t send_json_err(httpd_req_t *req, const char *status, const char *json)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t require_admin(httpd_req_t *req)
{
    if (!admin_pin_ok(req)) {
        return send_json_err(req, "403 Forbidden",
                             "{\"error\":\"forbidden: set admin PIN in NVS or bad X-Admin-Pin\"}");
    }
    return ESP_OK;
}

static void ota_https_task(void *arg)
{
    char *url = (char *)arg;
    esp_http_client_config_t http_cfg = {
        .url = url,
        .timeout_ms = 60000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    esp_err_t err = esp_https_ota(&ota_cfg);
    ESP_LOGI(TAG, "HTTPS OTA: %s", esp_err_to_name(err));
    free(url);

    if (err == ESP_OK) {
        esp_restart();
    }
    xSemaphoreGive(s_ota_mx);
    vTaskDelete(NULL);
}

static esp_err_t h_post_ota_url(httpd_req_t *req)
{
    if (require_admin(req) != ESP_OK) {
        return ESP_OK;
    }

    if (req->content_len <= 0 || req->content_len > 768) {
        return send_json_err(req, "400 Bad Request", "{\"error\":\"body\"}");
    }

    if (xSemaphoreTake(s_ota_mx, 0) != pdTRUE) {
        return send_json_err(req, "409 Conflict", "{\"error\":\"ota busy\"}");
    }

    size_t n = (size_t)req->content_len;
    char body[780];
    int r = httpd_req_recv(req, body, n);
    if (r <= 0) {
        xSemaphoreGive(s_ota_mx);
        return send_json_err(req, "400 Bad Request", "{\"error\":\"recv\"}");
    }
    body[r] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        xSemaphoreGive(s_ota_mx);
        return send_json_err(req, "400 Bad Request", "{\"error\":\"json\"}");
    }

    const cJSON *ju = cJSON_GetObjectItemCaseSensitive(root, "url");
    const char *url_in = (cJSON_IsString(ju) && ju->valuestring) ? ju->valuestring : NULL;
    char *url_copy = NULL;
    if (url_in && strncmp(url_in, "https://", 8) == 0 && strlen(url_in) <= CYBEER_OTA_URL_MAX) {
        url_copy = strdup(url_in);
    }
    cJSON_Delete(root);

    if (!url_copy) {
        xSemaphoreGive(s_ota_mx);
        return send_json_err(req, "400 Bad Request", "{\"error\":\"bad url\"}");
    }

    BaseType_t ok = xTaskCreate(ota_https_task, "cybeer_https_ota", CYBEER_OTA_STACK, url_copy,
                                tskIDLE_PRIORITY + 5, NULL);
    if (ok != pdPASS) {
        free(url_copy);
        xSemaphoreGive(s_ota_mx);
        return send_json_err(req, "500 Internal Server Error", "{\"error\":\"task\"}");
    }

    httpd_resp_set_status(req, "202 Accepted");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true,\"status\":\"started\"}", HTTPD_RESP_USE_STRLEN);
}

/** 0 = multipart firmware complete; 1 = need more bytes; -1 = error */
static int feed_boundary_stream(const uint8_t *chunk, size_t chunk_len, const char *boundary,
                                esp_ota_handle_t oh, uint8_t *tail, size_t *tail_len)
{
    const size_t bl = strlen(boundary);
    const size_t max_pat = bl + 16;

    uint8_t work[4096 + 256];
    if (*tail_len + chunk_len > sizeof(work)) {
        return -1;
    }
    memcpy(work, tail, *tail_len);
    memcpy(work + *tail_len, chunk, chunk_len);
    size_t wlen = *tail_len + chunk_len;

    size_t end_idx = 0;
    if (find_multipart_end(work, wlen, boundary, &end_idx)) {
        if (end_idx > 0 && esp_ota_write(oh, work, end_idx) != ESP_OK) {
            return -1;
        }
        *tail_len = 0;
        return 0;
    }

    if (wlen <= max_pat) {
        memcpy(tail, work, wlen);
        *tail_len = wlen;
        return 1;
    }

    size_t safe = wlen - max_pat;
    if (esp_ota_write(oh, work, safe) != ESP_OK) {
        return -1;
    }
    memcpy(tail, work + safe, max_pat);
    *tail_len = max_pat;
    return 1;
}

static const char *find_boundary_attr(const char *ct)
{
    const char *p = ct;
    while (*p) {
        if (strncasecmp(p, "boundary=", 9) == 0) {
            return p + 9;
        }
        p++;
    }
    return NULL;
}

static void drain_req(httpd_req_t *req, size_t *received, size_t total)
{
    uint8_t drain[256];
    while (*received < total) {
        size_t w = sizeof(drain);
        if (total - *received < w) {
            w = total - *received;
        }
        int r = httpd_req_recv(req, drain, w);
        if (r <= 0) {
            break;
        }
        *received += (size_t)r;
    }
}

static esp_err_t h_post_ota_upload(httpd_req_t *req)
{
    if (require_admin(req) != ESP_OK) {
        return ESP_OK;
    }

    if (req->content_len <= 0 || req->content_len > CYBEER_OTA_MAX_FIRMWARE) {
        return send_json_err(req, "400 Bad Request", "{\"error\":\"content length\"}");
    }

    if (xSemaphoreTake(s_ota_mx, 0) != pdTRUE) {
        return send_json_err(req, "409 Conflict", "{\"error\":\"ota busy\"}");
    }

    char ct[128];
    if (httpd_req_get_hdr_value_str(req, "Content-Type", ct, sizeof(ct)) != ESP_OK) {
        ct[0] = '\0';
    }

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        xSemaphoreGive(s_ota_mx);
        return send_json_err(req, "500 Internal Server Error", "{\"error\":\"no ota partition\"}");
    }

    esp_ota_handle_t oh = 0;
    esp_err_t err;
    bool multipart = (strncasecmp(ct, "multipart/form-data", 19) == 0);

    if (multipart) {
        err = esp_ota_begin(part, OTA_SIZE_UNKNOWN, &oh);
    } else {
        err = esp_ota_begin(part, (size_t)req->content_len, &oh);
    }

    if (err != ESP_OK) {
        xSemaphoreGive(s_ota_mx);
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
        return send_json_err(req, "500 Internal Server Error", "{\"error\":\"ota begin\"}");
    }

    const char *bptr = find_boundary_attr(ct);
    char boundary[80];
    boundary[0] = '\0';
    if (bptr) {
        size_t bi = 0;
        if (*bptr == '"') {
            bptr++;
            while (*bptr && *bptr != '"' && bi + 1 < sizeof(boundary)) {
                boundary[bi++] = *bptr++;
            }
            boundary[bi] = '\0';
        } else {
            while (*bptr && *bptr != ';' && *bptr != ' ' && *bptr != '\r' && bi + 1 < sizeof(boundary)) {
                boundary[bi++] = *bptr++;
            }
            boundary[bi] = '\0';
        }
    }

    if (multipart && boundary[0] == '\0') {
        esp_ota_abort(oh);
        xSemaphoreGive(s_ota_mx);
        return send_json_err(req, "400 Bad Request", "{\"error\":\"boundary\"}");
    }

    size_t received = 0;
    uint8_t tail[256];
    size_t tail_len = 0;

    if (multipart) {
        uint8_t pre[1536];
        size_t pre_len = 0;
        bool hdr_done = false;
        size_t body_off = 0;

        while (!hdr_done && pre_len < sizeof(pre)) {
            int r = httpd_req_recv(req, pre + pre_len, sizeof(pre) - pre_len);
            if (r <= 0) {
                esp_ota_abort(oh);
                xSemaphoreGive(s_ota_mx);
                return send_json_err(req, "400 Bad Request", "{\"error\":\"recv\"}");
            }
            pre_len += (size_t)r;
            received += (size_t)r;
            size_t hdr_end = 0;
            if (find_double_crlf(pre, pre_len, &hdr_end)) {
                hdr_done = true;
                body_off = hdr_end + 4;
            }
        }

        if (!hdr_done) {
            esp_ota_abort(oh);
            xSemaphoreGive(s_ota_mx);
            return send_json_err(req, "400 Bad Request", "{\"error\":\"multipart headers\"}");
        }

        if (body_off < pre_len) {
            int fr = feed_boundary_stream(pre + body_off, pre_len - body_off, boundary, oh, tail, &tail_len);
            if (fr == 0) {
                drain_req(req, &received, (size_t)req->content_len);
                goto ota_finish;
            }
            if (fr < 0) {
                esp_ota_abort(oh);
                xSemaphoreGive(s_ota_mx);
                return send_json_err(req, "400 Bad Request", "{\"error\":\"write\"}");
            }
        }

        while (received < (size_t)req->content_len) {
            uint8_t buf[4096];
            size_t want = sizeof(buf);
            if ((size_t)req->content_len - received < want) {
                want = (size_t)req->content_len - received;
            }
            int r = httpd_req_recv(req, buf, want);
            if (r <= 0) {
                esp_ota_abort(oh);
                xSemaphoreGive(s_ota_mx);
                return send_json_err(req, "400 Bad Request", "{\"error\":\"recv\"}");
            }
            received += (size_t)r;
            int fr = feed_boundary_stream(buf, (size_t)r, boundary, oh, tail, &tail_len);
            if (fr == 0) {
                drain_req(req, &received, (size_t)req->content_len);
                goto ota_finish;
            }
            if (fr < 0) {
                esp_ota_abort(oh);
                xSemaphoreGive(s_ota_mx);
                return send_json_err(req, "400 Bad Request", "{\"error\":\"write\"}");
            }
        }

        esp_ota_abort(oh);
        xSemaphoreGive(s_ota_mx);
        return send_json_err(req, "400 Bad Request", "{\"error\":\"multipart end\"}");
    }

    size_t remain = (size_t)req->content_len;
    uint8_t buf[4096];
    while (remain > 0) {
        size_t chunk = remain > sizeof(buf) ? sizeof(buf) : remain;
        int r = httpd_req_recv(req, buf, chunk);
        if (r <= 0) {
            esp_ota_abort(oh);
            xSemaphoreGive(s_ota_mx);
            return send_json_err(req, "400 Bad Request", "{\"error\":\"recv\"}");
        }
        err = esp_ota_write(oh, buf, (size_t)r);
        if (err != ESP_OK) {
            esp_ota_abort(oh);
            xSemaphoreGive(s_ota_mx);
            ESP_LOGE(TAG, "esp_ota_write: %s", esp_err_to_name(err));
            return send_json_err(req, "400 Bad Request", "{\"error\":\"write\"}");
        }
        remain -= (size_t)r;
    }

ota_finish:
    err = esp_ota_end(oh);
    if (err != ESP_OK) {
        xSemaphoreGive(s_ota_mx);
        ESP_LOGE(TAG, "esp_ota_end: %s", esp_err_to_name(err));
        return send_json_err(req, "400 Bad Request", "{\"error\":\"invalid image\"}");
    }

    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) {
        xSemaphoreGive(s_ota_mx);
        return send_json_err(req, "500 Internal Server Error", "{\"error\":\"set boot\"}");
    }

    httpd_resp_set_type(req, "application/json");
    (void)httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    vTaskDelay(pdMS_TO_TICKS(150));
    esp_restart();
    return ESP_OK;
}

esp_err_t cybeer_ota_register_handlers(httpd_handle_t server)
{
    if (!s_ota_mx) {
        s_ota_mx = xSemaphoreCreateMutex();
        if (!s_ota_mx) {
            return ESP_ERR_NO_MEM;
        }
        xSemaphoreGive(s_ota_mx);
    }

    httpd_uri_t u_url = {
        .uri = "/api/admin/ota/url",
        .method = HTTP_POST,
        .handler = h_post_ota_url,
        .user_ctx = NULL,
    };
    httpd_uri_t u_up = {
        .uri = "/api/admin/ota/upload",
        .method = HTTP_POST,
        .handler = h_post_ota_upload,
        .user_ctx = NULL,
    };

    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &u_url), TAG, "ota url");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &u_up), TAG, "ota upload");
    ESP_LOGI(TAG, "OTA admin endpoints registered");
    return ESP_OK;
}
