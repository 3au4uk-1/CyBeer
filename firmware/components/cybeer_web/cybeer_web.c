#include "cybeer_web.h"

#include <stdio.h>
#include <string.h>

#include "cybeer_battery.h"
#include "cybeer_fsm.h"
#include "cybeer_ws.h"
#include "cybeer_storage.h"
#include "cybeer_wifi.h"

#include <strings.h>

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include <sys/stat.h>

static const char *TAG = "cybeer_web";

#define WWW_ROOT "/littlefs/www"

static httpd_handle_t s_server;

static const char *fsm_state_str(cybeer_state_t s)
{
    switch (s) {
    case CYBEER_STATE_PREP:
        return "PREP";
    case CYBEER_STATE_RUNNING:
        return "RUNNING";
    case CYBEER_STATE_FINISHED:
        return "FINISHED";
    case CYBEER_STATE_READY:
        return "READY";
    default:
        return "UNKNOWN";
    }
}

static esp_err_t json_send(httpd_req_t *req, cJSON *root)
{
    char *printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!printed) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_send(req, "{}", HTTPD_RESP_USE_STRLEN);
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t e = httpd_resp_send(req, printed, HTTPD_RESP_USE_STRLEN);
    free(printed);
    return e;
}

static esp_err_t h_get_status(httpd_req_t *req)
{
    cybeer_fsm_snapshot_t fsm = cybeer_fsm_snapshot();
    uint8_t led_count = 0;
    uint8_t led_bright = 0;
    (void)cybeer_nvs_get_led_settings(&led_count, &led_bright);

    char unclaimed[40] = "";
    (void)cybeer_storage_get_latest_unclaimed_run_id(unclaimed, sizeof(unclaimed));

    char sta_ip[16] = "";
    cybeer_wifi_get_sta_ip_str(sta_ip, sizeof(sta_ip));

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }
    cJSON_AddStringToObject(root, "state", fsm_state_str(fsm.state));
    cJSON_AddNumberToObject(root, "batteryPercent", (double)cybeer_battery_get_percent());
    cJSON *wifi = cJSON_CreateObject();
    if (!wifi) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }
    cJSON_AddBoolToObject(wifi, "sta", cybeer_wifi_sta_connected());
    cJSON_AddBoolToObject(wifi, "ap", cybeer_wifi_is_started());
    cJSON_AddStringToObject(wifi, "staIp", sta_ip);
    cJSON_AddItemToObject(root, "wifi", wifi);
    cJSON_AddNumberToObject(root, "ledCount", (double)led_count);
    cJSON_AddStringToObject(root, "firmwareVersion", "1.0.0");
    cJSON_AddStringToObject(root, "unclaimedRunId", unclaimed);
    cJSON_AddItemToObject(root, "activeMatch", cJSON_CreateNull());

    return json_send(req, root);
}

static esp_err_t h_get_runs(httpd_req_t *req)
{
    const char *raw = cybeer_storage_runs_json();
    cJSON *arr = cJSON_Parse(raw && raw[0] ? raw : "[]");
    if (!arr || !cJSON_IsArray(arr)) {
        if (arr) {
            cJSON_Delete(arr);
        }
        cJSON *empty = cJSON_CreateArray();
        return json_send(req, empty);
    }

    int n = cJSON_GetArraySize(arr);
    int start = n > 50 ? n - 50 : 0;
    cJSON *out = cJSON_CreateArray();
    if (!out) {
        cJSON_Delete(arr);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }
    for (int i = start; i < n; i++) {
        const cJSON *item = cJSON_GetArrayItem(arr, i);
        if (!item) {
            continue;
        }
        cJSON *cpy = cJSON_Duplicate((cJSON *)item, true);
        if (cpy) {
            cJSON_AddItemToArray(out, cpy);
        }
    }
    cJSON_Delete(arr);
    return json_send(req, out);
}

static esp_err_t h_get_participants(httpd_req_t *req)
{
    const char *raw = cybeer_storage_participants_json();
    cJSON *root = cJSON_Parse(raw && raw[0] ? raw : "[]");
    if (!root) {
        root = cJSON_CreateArray();
    }
    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        root = cJSON_CreateArray();
    }
    return json_send(req, root);
}

static void copy_path_no_query(const char *uri, char *out, size_t out_len)
{
    const char *q = strchr(uri, '?');
    size_t n = q ? (size_t)(q - uri) : strlen(uri);
    if (n >= out_len) {
        n = out_len - 1;
    }
    memcpy(out, uri, n);
    out[n] = '\0';
}

static esp_err_t h_post_claim(httpd_req_t *req)
{
    char path[160];
    copy_path_no_query(req->uri, path, sizeof(path));

    const char *prefix = "/api/runs/";
    if (strncmp(path, prefix, strlen(prefix)) != 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"error\":\"uri\"}", HTTPD_RESP_USE_STRLEN);
    }
    const char *id_start = path + strlen(prefix);
    const char *claim = strstr(id_start, "/claim");
    if (!claim || strcmp(claim, "/claim") != 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"error\":\"uri\"}", HTTPD_RESP_USE_STRLEN);
    }
    size_t id_len = (size_t)(claim - id_start);
    if (id_len == 0 || id_len >= 40) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"error\":\"id\"}", HTTPD_RESP_USE_STRLEN);
    }
    char run_id[40];
    memcpy(run_id, id_start, id_len);
    run_id[id_len] = '\0';

    if (req->content_len <= 0 || req->content_len > 512) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"error\":\"body\"}", HTTPD_RESP_USE_STRLEN);
    }
    size_t body_n = (size_t)req->content_len;
    char body[520];
    int r = httpd_req_recv(req, body, body_n);
    if (r <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"error\":\"recv\"}", HTTPD_RESP_USE_STRLEN);
    }
    body[r] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"error\":\"json\"}", HTTPD_RESP_USE_STRLEN);
    }

    const cJSON *jname = cJSON_GetObjectItemCaseSensitive(root, "name");
    const cJSON *jpid = cJSON_GetObjectItemCaseSensitive(root, "participantId");
    const char *arg = NULL;
    bool by_pid = false;
    if (cJSON_IsString(jpid) && jpid->valuestring && jpid->valuestring[0] != '\0') {
        arg = jpid->valuestring;
        by_pid = true;
    } else if (cJSON_IsString(jname) && jname->valuestring && jname->valuestring[0] != '\0') {
        arg = jname->valuestring;
        by_pid = false;
    }
    cJSON_Delete(root);

    if (!arg) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"error\":\"name or participantId\"}", HTTPD_RESP_USE_STRLEN);
    }

    esp_err_t err = cybeer_storage_claim_run(run_id, arg, by_pid);
    httpd_resp_set_type(req, "application/json");
    if (err == ESP_OK) {
        return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    }
    if (err == ESP_ERR_NOT_FOUND) {
        httpd_resp_set_status(req, "404 Not Found");
        return httpd_resp_send(req, "{\"error\":\"run or participant not found\"}", HTTPD_RESP_USE_STRLEN);
    }
    if (err == ESP_ERR_INVALID_STATE) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_send(req, "{\"error\":\"already claimed\"}", HTTPD_RESP_USE_STRLEN);
    }
    if (err == ESP_ERR_INVALID_ARG) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req, "{\"error\":\"bad args\"}", HTTPD_RESP_USE_STRLEN);
    }
    httpd_resp_set_status(req, "500 Internal Server Error");
    return httpd_resp_send(req, "{\"error\":\"storage\"}", HTTPD_RESP_USE_STRLEN);
}

static bool path_has_dotdot(const char *s)
{
    return strstr(s, "..") != NULL;
}

static void set_content_type(httpd_req_t *req, const char *path)
{
    const char *ext = strrchr(path, '.');
    if (!ext) {
        httpd_resp_set_type(req, "application/octet-stream");
        return;
    }
    if (strcasecmp(ext, ".html") == 0) {
        httpd_resp_set_type(req, "text/html; charset=utf-8");
    } else if (strcasecmp(ext, ".css") == 0) {
        httpd_resp_set_type(req, "text/css; charset=utf-8");
    } else if (strcasecmp(ext, ".js") == 0) {
        httpd_resp_set_type(req, "application/javascript; charset=utf-8");
    } else if (strcasecmp(ext, ".ico") == 0) {
        httpd_resp_set_type(req, "image/x-icon");
    } else if (strcasecmp(ext, ".svg") == 0) {
        httpd_resp_set_type(req, "image/svg+xml");
    } else {
        httpd_resp_set_type(req, "application/octet-stream");
    }
}

static esp_err_t send_path_file(httpd_req_t *req, const char *fs_path)
{
    FILE *f = fopen(fs_path, "rb");
    if (!f) {
        return ESP_FAIL;
    }
    set_content_type(req, fs_path);
    char buf[1024];
    size_t n;
    esp_err_t err = ESP_OK;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        err = httpd_resp_send_chunk(req, buf, n);
        if (err != ESP_OK) {
            break;
        }
    }
    fclose(f);
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, NULL, 0);
    }
    return err;
}

static esp_err_t h_static(httpd_req_t *req)
{
    if (req->method != HTTP_GET) {
        httpd_resp_set_status(req, "405 Method Not Allowed");
        return httpd_resp_send(req, "Method Not Allowed", HTTPD_RESP_USE_STRLEN);
    }

    char uri[256];
    copy_path_no_query(req->uri, uri, sizeof(uri));
    if (path_has_dotdot(uri)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req, "Bad path", HTTPD_RESP_USE_STRLEN);
    }

    char path[320];
    if (strcmp(uri, "/") == 0) {
        snprintf(path, sizeof(path), WWW_ROOT "/index.html");
    } else {
        snprintf(path, sizeof(path), WWW_ROOT "%s", uri);
    }

    struct stat st;
    if (stat(path, &st) != 0 || S_ISDIR(st.st_mode)) {
        snprintf(path, sizeof(path), WWW_ROOT "/index.html");
        if (stat(path, &st) != 0 || S_ISDIR(st.st_mode)) {
            httpd_resp_set_status(req, "404 Not Found");
            return httpd_resp_send(req, "Not Found", HTTPD_RESP_USE_STRLEN);
        }
    }

    esp_err_t err = send_path_file(req, path);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_send(req, "Send failed", HTTPD_RESP_USE_STRLEN);
    }
    return ESP_OK;
}

esp_err_t cybeer_web_start(void)
{
    if (s_server) {
        return ESP_OK;
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.lru_purge_enable = true;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    esp_err_t err = httpd_start(&cfg, &s_server);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    cybeer_wifi_register_setup_handlers(s_server);

    httpd_uri_t u_status = { .uri = "/api/status", .method = HTTP_GET, .handler = h_get_status, .user_ctx = NULL };
    httpd_uri_t u_runs = { .uri = "/api/runs", .method = HTTP_GET, .handler = h_get_runs, .user_ctx = NULL };
    httpd_uri_t u_parts = {
        .uri = "/api/participants", .method = HTTP_GET, .handler = h_get_participants, .user_ctx = NULL
    };
    httpd_uri_t u_claim = { .uri = "/api/runs/*/claim", .method = HTTP_POST, .handler = h_post_claim, .user_ctx = NULL };
    httpd_uri_t u_static = { .uri = "/*", .method = HTTP_GET, .handler = h_static, .user_ctx = NULL };

    if (httpd_register_uri_handler(s_server, &u_status) != ESP_OK || httpd_register_uri_handler(s_server, &u_runs) != ESP_OK
        || httpd_register_uri_handler(s_server, &u_parts) != ESP_OK || httpd_register_uri_handler(s_server, &u_claim) != ESP_OK
        || cybeer_ws_register(s_server) != ESP_OK || httpd_register_uri_handler(s_server, &u_static) != ESP_OK) {
        ESP_LOGE(TAG, "register uri failed");
        httpd_stop(s_server);
        s_server = NULL;
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "HTTP server on :80 (static " WWW_ROOT ")");
    return ESP_OK;
}

httpd_handle_t cybeer_web_get_server(void)
{
    return s_server;
}
