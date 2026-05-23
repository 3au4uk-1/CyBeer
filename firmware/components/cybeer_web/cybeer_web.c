#include "cybeer_web.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cybeer_ota.h"
#include "cybeer_battery.h"
#include "cybeer_config.h"
#include "cybeer_fsm.h"
#include "cybeer_led.h"
#include "cybeer_ws.h"
#include "cybeer_storage.h"
#include "cybeer_tournament.h"
#include "cybeer_wifi.h"
#include "cybeer_power.h"

#include <strings.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
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
    httpd_resp_set_type(req, "application/json; charset=utf-8");
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
    wifi_ap_record_t ap_info;
    if (cybeer_wifi_sta_connected() && esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        cJSON_AddStringToObject(wifi, "ssid", (const char *)ap_info.ssid);
        cJSON_AddNumberToObject(wifi, "rssi", (double)ap_info.rssi);
    } else {
        cJSON_AddStringToObject(wifi, "ssid", "");
        cJSON_AddNumberToObject(wifi, "rssi", 0);
    }
    cJSON_AddBoolToObject(wifi, "apFallback", cybeer_wifi_ap_is_fallback());
    cJSON_AddItemToObject(root, "wifi", wifi);
    cJSON_AddNumberToObject(root, "ledCount", (double)led_count);
    cJSON_AddNumberToObject(root, "ledBrightness", (double)led_bright);
    cJSON_AddBoolToObject(root, "adminPinConfigured", cybeer_nvs_admin_pin_is_configured());
    cJSON_AddStringToObject(root, "firmwareVersion", cybeer_firmware_version());
    cJSON_AddStringToObject(root, "unclaimedRunId", unclaimed);
    if (unclaimed[0] != '\0') {
        cybeer_run_t unclaimed_run;
        if (cybeer_storage_get_run(unclaimed, &unclaimed_run) == ESP_OK) {
            cJSON_AddNumberToObject(root, "unclaimedRunDurationUs",
                                    (double)unclaimed_run.duration_us);
        }
    }
    cybeer_tournament_fill_status_active_match(root);

    const char *power_mode = "normal";
    if (cybeer_power_is_idle()) {
        power_mode = "idle";
    } else if (cybeer_power_is_eco()) {
        power_mode = "eco";
    }
    cJSON_AddStringToObject(root, "powerMode", power_mode);

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

/** /api/participants/{id}/stats or /runs — middle '*' is not supported by httpd_uri_match_wildcard. */
static bool parse_participant_sub_path(const char *path, char pid_out[40], const char **suffix_out)
{
    const char *pfx = "/api/participants/";
    if (strncmp(path, pfx, strlen(pfx)) != 0) {
        return false;
    }
    const char *rest = path + strlen(pfx);
    const char *slash = strchr(rest, '/');
    if (!slash || slash == rest) {
        return false;
    }
    size_t id_len = (size_t)(slash - rest);
    if (id_len == 0 || id_len >= 40 || strchr(rest, '/') != slash) {
        return false;
    }
    memcpy(pid_out, rest, id_len);
    pid_out[id_len] = '\0';
    *suffix_out = slash;
    return true;
}

static esp_err_t h_get_participant_stats_for_pid(httpd_req_t *req, const char *pid)
{
    cybeer_stats_t st = { 0 };
    (void)cybeer_storage_get_participant_stats(pid, &st);

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }
    cJSON_AddStringToObject(root, "participantId", pid);
    cJSON_AddNumberToObject(root, "runCount", (double)st.count);
    cJSON_AddNumberToObject(root, "bestDurationUs", (double)st.best_us);
    cJSON_AddNumberToObject(root, "worstDurationUs", (double)st.worst_us);
    cJSON_AddNumberToObject(root, "avgDurationUs", (double)st.avg_us);
    cJSON_AddNumberToObject(root, "lastDurationUs", (double)st.last_us);
    return json_send(req, root);
}

static esp_err_t read_http_body(httpd_req_t *req, char *buf, size_t cap);
static esp_err_t send_json_text(httpd_req_t *req, const char *http_status, const char *json_body);

static esp_err_t h_post_claim(httpd_req_t *req)
{
    char body[520];
    if (read_http_body(req, body, sizeof(body)) != ESP_OK) {
        return send_json_text(req, "400 Bad Request", "{\"error\":\"body\"}");
    }

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        return send_json_text(req, "400 Bad Request", "{\"error\":\"json\"}");
    }

    char run_id[40] = { 0 };
    const cJSON *jrid = cJSON_GetObjectItemCaseSensitive(root, "runId");
    if (!cJSON_IsString(jrid) || !jrid->valuestring || jrid->valuestring[0] == '\0'
        || strlen(jrid->valuestring) >= sizeof(run_id)) {
        cJSON_Delete(root);
        return send_json_text(req, "400 Bad Request", "{\"error\":\"runId\"}");
    }
    strncpy(run_id, jrid->valuestring, sizeof(run_id) - 1);

    char claim_arg[96] = { 0 };
    bool by_pid = false;
    const cJSON *jname = cJSON_GetObjectItemCaseSensitive(root, "name");
    const cJSON *jpid = cJSON_GetObjectItemCaseSensitive(root, "participantId");
    /* Prefer explicit name over participantId; copy before cJSON_Delete. */
    if (cJSON_IsString(jname) && jname->valuestring && jname->valuestring[0] != '\0') {
        strncpy(claim_arg, jname->valuestring, sizeof(claim_arg) - 1);
        by_pid = false;
    } else if (cJSON_IsString(jpid) && jpid->valuestring && jpid->valuestring[0] != '\0') {
        strncpy(claim_arg, jpid->valuestring, sizeof(claim_arg) - 1);
        by_pid = true;
    }
    cJSON_Delete(root);

    if (claim_arg[0] == '\0') {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"error\":\"name or participantId\"}", HTTPD_RESP_USE_STRLEN);
    }

    esp_err_t err = cybeer_storage_claim_run(run_id, claim_arg, by_pid);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    if (err == ESP_OK) {
        cybeer_run_t claimed_run;
        memset(&claimed_run, 0, sizeof(claimed_run));
        (void)cybeer_storage_get_run(run_id, &claimed_run);
        if (claimed_run.participant_id[0] != '\0'
            && cybeer_storage_run_qualifies_podium_led(&claimed_run)) {
            cybeer_led_set_fx(CYBEER_LED_FX_PODIUM);
        }
        (void)cybeer_tournament_notify_run_claimed(run_id);
        cybeer_ws_broadcast_leaderboard_update();
        cybeer_led_set_unclaimed_flag(false);

        char pname[128] = { 0 };
        if (claimed_run.participant_id[0] != '\0') {
            (void)cybeer_storage_get_participant_name(claimed_run.participant_id, pname, sizeof(pname));
        }

        cJSON *resp = cJSON_CreateObject();
        if (!resp) {
            return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
        }
        cJSON_AddBoolToObject(resp, "ok", true);
        if (claimed_run.participant_id[0] != '\0') {
            cJSON_AddStringToObject(resp, "participantId", claimed_run.participant_id);
            cJSON_AddStringToObject(resp, "participantName", pname);
        }
        return json_send(req, resp);
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

#define ADMIN_HDR_PIN "X-Admin-Pin"
#define ADMIN_BODY_MAX 4096

static esp_err_t send_json_text(httpd_req_t *req, const char *http_status, const char *json_body)
{
    httpd_resp_set_status(req, http_status);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    return httpd_resp_send(req, json_body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t read_http_body(httpd_req_t *req, char *buf, size_t cap)
{
    ESP_RETURN_ON_FALSE(buf && cap > 8, ESP_ERR_INVALID_ARG, TAG, "cap");
    if (req->content_len <= 0 || req->content_len >= (ssize_t)(cap)) {
        return ESP_FAIL;
    }
    size_t need = (size_t)req->content_len;
    size_t got = 0;
    while (got < need) {
        int n = httpd_req_recv(req, buf + got, need - got);
        if (n < 0) {
            return ESP_FAIL;
        }
        if (n == 0) {
            break;
        }
        got += (size_t)n;
    }
    if (got != need) {
        return ESP_FAIL;
    }
    buf[got] = '\0';
    return ESP_OK;
}

static esp_err_t require_admin_pin(httpd_req_t *req)
{
    if (!cybeer_nvs_admin_pin_is_configured()) {
        return send_json_text(req, "403 Forbidden",
                              "{\"error\":\"admin pin not configured; default is 1111\"}");
    }
    char pin_raw[96];
    if (httpd_req_get_hdr_value_str(req, ADMIN_HDR_PIN, pin_raw, sizeof(pin_raw)) != ESP_OK) {
        return send_json_text(req, "401 Unauthorized", "{\"error\":\"missing X-Admin-Pin header\"}");
    }
    if (cybeer_admin_verify_pin(pin_raw) != ESP_OK) {
        return send_json_text(req, "401 Unauthorized", "{\"error\":\"invalid pin\"}");
    }
    return ESP_OK;
}

static esp_err_t h_get_participant_runs_for_pid(httpd_req_t *req, const char *pid)
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
    for (int i = n - 1; i >= start; i--) {
        const cJSON *item = cJSON_GetArrayItem(arr, i);
        if (!item) {
            continue;
        }
        const cJSON *jpid = cJSON_GetObjectItemCaseSensitive(item, "participant_id");
        if (!cJSON_IsString(jpid) || !jpid->valuestring) {
            continue;
        }
        if (strcmp(jpid->valuestring, pid) != 0) {
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

static esp_err_t h_get_participant_sub(httpd_req_t *req)
{
    char path[160];
    copy_path_no_query(req->uri, path, sizeof(path));
    char pid[40];
    const char *suffix = NULL;
    if (!parse_participant_sub_path(path, pid, &suffix)) {
        return send_json_text(req, "404 Not Found", "{\"error\":\"uri\"}");
    }
    if (strcmp(suffix, "/stats") == 0) {
        return h_get_participant_stats_for_pid(req, pid);
    }
    if (strcmp(suffix, "/runs") == 0) {
        return h_get_participant_runs_for_pid(req, pid);
    }
    return send_json_text(req, "404 Not Found", "{\"error\":\"uri\"}");
}

static esp_err_t h_post_admin_pin_verify(httpd_req_t *req)
{
    esp_err_t g = require_admin_pin(req);
    if (g != ESP_OK) {
        return g;
    }
    return send_json_text(req, "200 OK", "{\"ok\":true}");
}

static esp_err_t h_post_admin_pin_change(httpd_req_t *req)
{
    esp_err_t g = require_admin_pin(req);
    if (g != ESP_OK) {
        return g;
    }
    char body[ADMIN_BODY_MAX];
    if (read_http_body(req, body, sizeof(body)) != ESP_OK) {
        return send_json_text(req, "400 Bad Request", "{\"error\":\"body\"}");
    }
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        return send_json_text(req, "400 Bad Request", "{\"error\":\"json\"}");
    }
    const cJSON *jnew = cJSON_GetObjectItemCaseSensitive(root, "newPin");
    const char *new_pin = (cJSON_IsString(jnew) && jnew->valuestring) ? jnew->valuestring : NULL;
    cJSON_Delete(root);
    if (!new_pin || strlen(new_pin) < 4 || strlen(new_pin) > 32) {
        return send_json_text(req, "400 Bad Request", "{\"error\":\"newPin length 4-32\"}");
    }

    char pin_raw[96];
    if (httpd_req_get_hdr_value_str(req, ADMIN_HDR_PIN, pin_raw, sizeof(pin_raw)) != ESP_OK) {
        return send_json_text(req, "401 Unauthorized", "{\"error\":\"missing pin header\"}");
    }
    if (cybeer_admin_pin_change(pin_raw, new_pin) != ESP_OK) {
        return send_json_text(req, "401 Unauthorized", "{\"error\":\"invalid pin\"}");
    }
    return send_json_text(req, "200 OK", "{\"ok\":true}");
}

static esp_err_t h_post_admin_pin_reset_default(httpd_req_t *req)
{
    (void)req;
    if (cybeer_admin_pin_reset_to_default() != ESP_OK) {
        return send_json_text(req, "500 Internal Server Error", "{\"error\":\"nvs\"}");
    }
    ESP_LOGW(TAG, "admin PIN reset to factory default");
    return send_json_text(req, "200 OK", "{\"ok\":true,\"pin\":\"1111\"}");
}

static esp_err_t h_get_admin_tournaments(httpd_req_t *req)
{
    esp_err_t g = require_admin_pin(req);
    if (g != ESP_OK) {
        return g;
    }
    const char *raw = cybeer_storage_tournaments_json();
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

static bool admin_parse_run_id(const char *path_no_query, char id_out[40])
{
    const char *pfx = "/api/admin/runs/";
    if (strncmp(path_no_query, pfx, strlen(pfx)) != 0) {
        return false;
    }
    const char *id = path_no_query + strlen(pfx);
    if (id[0] == '\0' || strchr(id, '/') != NULL) {
        return false;
    }
    strncpy(id_out, id, 39);
    id_out[39] = '\0';
    return id_out[0] != '\0';
}

static void merge_run_optional_json(const cJSON *obj, cybeer_run_t *run)
{
    const cJSON *j;
    j = cJSON_GetObjectItemCaseSensitive(obj, "id");
    if (cJSON_IsString(j) && j->valuestring) {
        strncpy(run->id, j->valuestring, sizeof(run->id) - 1);
        run->id[sizeof(run->id) - 1] = '\0';
    }
    j = cJSON_GetObjectItemCaseSensitive(obj, "participant_id");
    if (cJSON_IsString(j) && j->valuestring) {
        strncpy(run->participant_id, j->valuestring, sizeof(run->participant_id) - 1);
        run->participant_id[sizeof(run->participant_id) - 1] = '\0';
    }
    j = cJSON_GetObjectItemCaseSensitive(obj, "duration_us");
    if (cJSON_IsNumber(j)) {
        run->duration_us = (int64_t)j->valuedouble;
    }
    j = cJSON_GetObjectItemCaseSensitive(obj, "finished_at");
    if (cJSON_IsString(j) && j->valuestring) {
        strncpy(run->finished_at, j->valuestring, sizeof(run->finished_at) - 1);
        run->finished_at[sizeof(run->finished_at) - 1] = '\0';
    }
    j = cJSON_GetObjectItemCaseSensitive(obj, "claimed");
    if (cJSON_IsBool(j)) {
        run->claimed = cJSON_IsTrue(j);
    }
    j = cJSON_GetObjectItemCaseSensitive(obj, "tournament_match_id");
    if (cJSON_IsString(j) && j->valuestring) {
        strncpy(run->tournament_match_id, j->valuestring, sizeof(run->tournament_match_id) - 1);
        run->tournament_match_id[sizeof(run->tournament_match_id) - 1] = '\0';
    }
}

static esp_err_t h_post_admin_pin_setup(httpd_req_t *req)
{
    if (cybeer_nvs_admin_pin_is_configured()) {
        return send_json_text(req, "409 Conflict", "{\"error\":\"admin pin already set\"}");
    }
    char body[ADMIN_BODY_MAX];
    if (read_http_body(req, body, sizeof(body)) != ESP_OK) {
        return send_json_text(req, "400 Bad Request", "{\"error\":\"body\"}");
    }
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        return send_json_text(req, "400 Bad Request", "{\"error\":\"json\"}");
    }
    const cJSON *jpin = cJSON_GetObjectItemCaseSensitive(root, "pin");
    const char *pin = (cJSON_IsString(jpin) && jpin->valuestring) ? jpin->valuestring : NULL;
    cJSON_Delete(root);
    if (!pin || strlen(pin) < 4 || strlen(pin) > 32) {
        return send_json_text(req, "400 Bad Request", "{\"error\":\"pin length 4-32\"}");
    }
    esp_err_t err = cybeer_admin_pin_first_setup(pin);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"error\":\"nvs\"}", HTTPD_RESP_USE_STRLEN);
    }
    return send_json_text(req, "200 OK", "{\"ok\":true}");
}

static esp_err_t h_post_admin_runs(httpd_req_t *req)
{
    esp_err_t g = require_admin_pin(req);
    if (g != ESP_OK) {
        return g;
    }
    char body[ADMIN_BODY_MAX];
    if (read_http_body(req, body, sizeof(body)) != ESP_OK) {
        return send_json_text(req, "400 Bad Request", "{\"error\":\"body\"}");
    }
    cJSON *root = cJSON_Parse(body);
    if (!root || !cJSON_IsObject(root)) {
        if (root) {
            cJSON_Delete(root);
        }
        return send_json_text(req, "400 Bad Request", "{\"error\":\"json object\"}");
    }
    cybeer_run_t run = { 0 };
    run.participant_id[0] = '\0';
    run.tournament_match_id[0] = '\0';
    run.claimed = false;
    merge_run_optional_json(root, &run);
    cJSON_Delete(root);
    if (run.id[0] == '\0') {
        cybeer_format_uuid_v4(run.id);
    }
    if (run.finished_at[0] == '\0') {
        cybeer_storage_iso8601_now(run.finished_at);
    }

    esp_err_t err = cybeer_storage_add_run_manual(&run);
    if (err == ESP_OK) {
        (void)cybeer_tournament_notify_run_saved(&run);
        cJSON *resp = cJSON_CreateObject();
        if (!resp) {
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        }
        cJSON_AddStringToObject(resp, "id", run.id);
        return json_send(req, resp);
    }
    if (err == ESP_ERR_INVALID_STATE) {
        return send_json_text(req, "409 Conflict", "{\"error\":\"duplicate run id\"}");
    }
    return send_json_text(req, "500 Internal Server Error", "{\"error\":\"storage\"}");
}

static esp_err_t h_patch_admin_run(httpd_req_t *req)
{
    esp_err_t g = require_admin_pin(req);
    if (g != ESP_OK) {
        return g;
    }
    char path[160];
    copy_path_no_query(req->uri, path, sizeof(path));
    char run_id[40];
    if (!admin_parse_run_id(path, run_id)) {
        return send_json_text(req, "400 Bad Request", "{\"error\":\"uri\"}");
    }
    cybeer_run_t cur;
    if (cybeer_storage_get_run(run_id, &cur) != ESP_OK) {
        return send_json_text(req, "404 Not Found", "{\"error\":\"unknown run\"}");
    }
    char body[ADMIN_BODY_MAX];
    if (read_http_body(req, body, sizeof(body)) != ESP_OK) {
        return send_json_text(req, "400 Bad Request", "{\"error\":\"body\"}");
    }
    cJSON *root = cJSON_Parse(body);
    if (!root || !cJSON_IsObject(root)) {
        if (root) {
            cJSON_Delete(root);
        }
        return send_json_text(req, "400 Bad Request", "{\"error\":\"json object\"}");
    }
    strncpy(cur.id, run_id, sizeof(cur.id) - 1);
    cur.id[sizeof(cur.id) - 1] = '\0';
    merge_run_optional_json(root, &cur);
    /* Do not replace id via body on PATCH — keep URI id */
    strncpy(cur.id, run_id, sizeof(cur.id) - 1);
    cur.id[sizeof(cur.id) - 1] = '\0';
    cJSON_Delete(root);

    esp_err_t err = cybeer_storage_update_run(run_id, &cur);
    if (err == ESP_OK) {
        return send_json_text(req, "200 OK", "{\"ok\":true}");
    }
    return send_json_text(req, "500 Internal Server Error", "{\"error\":\"storage\"}");
}

static esp_err_t h_delete_admin_run(httpd_req_t *req)
{
    esp_err_t g = require_admin_pin(req);
    if (g != ESP_OK) {
        return g;
    }
    char path[160];
    copy_path_no_query(req->uri, path, sizeof(path));
    char run_id[40];
    if (!admin_parse_run_id(path, run_id)) {
        return send_json_text(req, "400 Bad Request", "{\"error\":\"uri\"}");
    }
    esp_err_t err = cybeer_storage_delete_run(run_id);
    if (err == ESP_OK) {
        return send_json_text(req, "200 OK", "{\"ok\":true}");
    }
    if (err == ESP_ERR_NOT_FOUND) {
        return send_json_text(req, "404 Not Found", "{\"error\":\"unknown run\"}");
    }
    return send_json_text(req, "500 Internal Server Error", "{\"error\":\"storage\"}");
}

static esp_err_t h_delete_admin_data_reset(httpd_req_t *req)
{
    esp_err_t g = require_admin_pin(req);
    if (g != ESP_OK) {
        return g;
    }
    esp_err_t err = cybeer_storage_reset_all_data();
    if (err != ESP_OK) {
        return send_json_text(req, "500 Internal Server Error", "{\"error\":\"storage\"}");
    }
    cybeer_led_set_unclaimed_flag(false);
    return send_json_text(req, "200 OK", "{\"ok\":true}");
}

static esp_err_t h_post_admin_reboot(httpd_req_t *req)
{
    esp_err_t g = require_admin_pin(req);
    if (g != ESP_OK) {
        return g;
    }
    esp_err_t sent = send_json_text(req, "200 OK", "{\"ok\":true}");
    if (sent == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    }
    return sent;
}

static esp_err_t h_post_admin_power_eco(httpd_req_t *req)
{
    esp_err_t g = require_admin_pin(req);
    if (g != ESP_OK) {
        return g;
    }
    bool eco = cybeer_power_toggle_eco();
    char resp[32];
    snprintf(resp, sizeof(resp), "{\"eco\":%s}", eco ? "true" : "false");
    return send_json_text(req, "200 OK", resp);
}

static esp_err_t h_post_admin_power_sleep(httpd_req_t *req)
{
    esp_err_t g = require_admin_pin(req);
    if (g != ESP_OK) {
        return g;
    }
    esp_err_t sent = send_json_text(req, "200 OK", "{\"ok\":true,\"msg\":\"sleeping\"}");
    if (sent == ESP_OK) {
        cybeer_power_trigger_sleep();
    }
    return sent;
}

static bool parse_participant_id_path(const char *path, char pid_out[40])
{
    const char *pfx = "/api/participants/";
    if (strncmp(path, pfx, strlen(pfx)) != 0) {
        return false;
    }
    const char *pid = path + strlen(pfx);
    size_t len = strlen(pid);
    if (len == 0 || len >= 40 || strchr(pid, '/') != NULL) {
        return false;
    }
    snprintf(pid_out, 40, "%s", pid);
    return true;
}

static esp_err_t h_post_participants(httpd_req_t *req)
{
    char body[260];
    if (read_http_body(req, body, sizeof(body)) != ESP_OK) {
        return send_json_text(req, "400 Bad Request", "{\"error\":\"body\"}");
    }

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        return send_json_text(req, "400 Bad Request", "{\"error\":\"json\"}");
    }
    const cJSON *jname = cJSON_GetObjectItemCaseSensitive(root, "name");
    if (!cJSON_IsString(jname) || !jname->valuestring || jname->valuestring[0] == '\0') {
        cJSON_Delete(root);
        return send_json_text(req, "400 Bad Request", "{\"error\":\"name required\"}");
    }
    if (strlen(jname->valuestring) > 32) {
        cJSON_Delete(root);
        return send_json_text(req, "400 Bad Request", "{\"error\":\"name max 32 chars\"}");
    }

    char name_copy[96];
    strncpy(name_copy, jname->valuestring, sizeof(name_copy) - 1);
    cJSON_Delete(root);

    char pid[37] = { 0 };
    esp_err_t err = cybeer_storage_create_participant(name_copy, pid);
    if (err == ESP_OK) {
        cybeer_ws_broadcast_leaderboard_update();
        cJSON *resp = cJSON_CreateObject();
        if (!resp) {
            return send_json_text(req, "200 OK", "{\"ok\":true}");
        }
        cJSON_AddBoolToObject(resp, "ok", true);
        cJSON_AddStringToObject(resp, "id", pid);
        cJSON_AddStringToObject(resp, "name", name_copy);
        return json_send(req, resp);
    }
    if (err == ESP_ERR_INVALID_STATE) {
        return send_json_text(req, "409 Conflict", "{\"error\":\"name_taken\"}");
    }
    if (err == ESP_ERR_INVALID_ARG) {
        return send_json_text(req, "400 Bad Request", "{\"error\":\"bad name\"}");
    }
    return send_json_text(req, "500 Internal Server Error", "{\"error\":\"storage\"}");
}

static esp_err_t h_delete_participant(httpd_req_t *req)
{
    char path[160];
    copy_path_no_query(req->uri, path, sizeof(path));
    char pid[40];
    if (!parse_participant_id_path(path, pid)) {
        return send_json_text(req, "400 Bad Request", "{\"error\":\"uri\"}");
    }

    esp_err_t err = cybeer_storage_delete_participant(pid);
    if (err == ESP_OK) {
        cybeer_ws_broadcast_leaderboard_update();
        return send_json_text(req, "200 OK", "{\"ok\":true}");
    }
    if (err == ESP_ERR_NOT_FOUND) {
        return send_json_text(req, "404 Not Found", "{\"error\":\"not_found\"}");
    }
    if (err == ESP_ERR_INVALID_STATE) {
        return send_json_text(req, "409 Conflict", "{\"error\":\"has_runs\"}");
    }
    return send_json_text(req, "500 Internal Server Error", "{\"error\":\"storage\"}");
}

static esp_err_t h_patch_participant(httpd_req_t *req)
{
    char path[160];
    copy_path_no_query(req->uri, path, sizeof(path));
    char pid[40];
    if (!parse_participant_id_path(path, pid)) {
        return send_json_text(req, "400 Bad Request", "{\"error\":\"uri\"}");
    }

    if (req->content_len <= 0 || req->content_len > 256) {
        return send_json_text(req, "400 Bad Request", "{\"error\":\"body\"}");
    }
    char body[260];
    int r = httpd_req_recv(req, body, (size_t)req->content_len);
    if (r <= 0) {
        return send_json_text(req, "400 Bad Request", "{\"error\":\"recv\"}");
    }
    body[r] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        return send_json_text(req, "400 Bad Request", "{\"error\":\"json\"}");
    }
    const cJSON *jname = cJSON_GetObjectItemCaseSensitive(root, "name");
    if (!cJSON_IsString(jname) || !jname->valuestring || jname->valuestring[0] == '\0') {
        cJSON_Delete(root);
        return send_json_text(req, "400 Bad Request", "{\"error\":\"name required\"}");
    }
    if (strlen(jname->valuestring) > 32) {
        cJSON_Delete(root);
        return send_json_text(req, "400 Bad Request", "{\"error\":\"name max 32 chars\"}");
    }

    esp_err_t err = cybeer_storage_rename_participant(pid, jname->valuestring);
    cJSON_Delete(root);

    if (err == ESP_OK) {
        cybeer_ws_broadcast_leaderboard_update();
        return send_json_text(req, "200 OK", "{\"ok\":true}");
    }
    if (err == ESP_ERR_NOT_FOUND) {
        return send_json_text(req, "404 Not Found", "{\"error\":\"not_found\"}");
    }
    if (err == ESP_ERR_INVALID_STATE) {
        return send_json_text(req, "409 Conflict", "{\"error\":\"name_taken\"}");
    }
    return send_json_text(req, "500 Internal Server Error", "{\"error\":\"storage\"}");
}

static bool export_wants_csv(const char *full_uri)
{
    const char *q = strchr(full_uri, '?');
    if (!q) {
        return false;
    }
    q++;
    char buf[144];
    snprintf(buf, sizeof(buf), "%s", q);
    char *t = strtok(buf, "&");
    while (t) {
        char *eq = strchr(t, '=');
        if (eq) {
            *eq = '\0';
            const char *val = eq + 1;
            if (strcasecmp(t, "format") == 0 && strcasecmp(val, "csv") == 0) {
                return true;
            }
            if (strcasecmp(t, "format") == 0 && strcasecmp(val, "json") == 0) {
                return false;
            }
        }
        t = strtok(NULL, "&");
    }
    return false;
}

static esp_err_t h_get_export(httpd_req_t *req)
{
    esp_err_t g = require_admin_pin(req);
    if (g != ESP_OK) {
        return g;
    }
    bool csv_mode = export_wants_csv(req->uri);

    const char *raw_runs = cybeer_storage_runs_json();
    const char *raw_parts = cybeer_storage_participants_json();

    if (!csv_mode) {
        cJSON *runs = cJSON_Parse(raw_runs && raw_runs[0] ? raw_runs : "[]");
        if (!runs) {
            runs = cJSON_CreateArray();
        }
        if (!cJSON_IsArray(runs)) {
            cJSON_Delete(runs);
            runs = cJSON_CreateArray();
        }
        cJSON *parts = cJSON_Parse(raw_parts && raw_parts[0] ? raw_parts : "[]");
        if (!parts) {
            parts = cJSON_CreateArray();
        }
        if (!cJSON_IsArray(parts)) {
            cJSON_Delete(parts);
            parts = cJSON_CreateArray();
        }

        cJSON *pack = cJSON_CreateObject();
        if (!pack) {
            cJSON_Delete(runs);
            cJSON_Delete(parts);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        }
        cJSON_AddItemToObject(pack, "runs", runs);
        cJSON_AddItemToObject(pack, "participants", parts);
        esp_err_t e = json_send(req, pack);
        return e;
    }

    cJSON *arr = cJSON_Parse(raw_runs && raw_runs[0] ? raw_runs : "[]");
    if (!arr || !cJSON_IsArray(arr)) {
        if (arr) {
            cJSON_Delete(arr);
        }
        arr = cJSON_CreateArray();
    }

    httpd_resp_set_status(req, "200 OK");
    httpd_resp_set_type(req, "text/csv; charset=utf-8");
    esp_err_t e = httpd_resp_sendstr_chunk(req, "id,participant_id,duration_us,finished_at,claimed,tournament_match_id\r\n");
    if (e != ESP_OK) {
        cJSON_Delete(arr);
        return e;
    }

    char line_buf[576];
    cJSON *it = NULL;
    cJSON_ArrayForEach(it, arr)
    {
        cybeer_run_t r;
        memset(&r, 0, sizeof(r));
        merge_run_optional_json(it, &r);

        snprintf(line_buf, sizeof(line_buf), "%s,%s,%lld,%s,%s,%s\r\n", r.id, r.participant_id,
                 (long long)r.duration_us, r.finished_at, r.claimed ? "true" : "false", r.tournament_match_id);
        e = httpd_resp_sendstr_chunk(req, line_buf);
        if (e != ESP_OK) {
            break;
        }
    }

    if (e == ESP_OK) {
        e = httpd_resp_send_chunk(req, NULL, 0);
    }
    cJSON_Delete(arr);
    return e;
}

static esp_err_t h_put_settings(httpd_req_t *req)
{
    esp_err_t g = require_admin_pin(req);
    if (g != ESP_OK) {
        return g;
    }
    char body[ADMIN_BODY_MAX];
    if (read_http_body(req, body, sizeof(body)) != ESP_OK) {
        return send_json_text(req, "400 Bad Request", "{\"error\":\"body\"}");
    }
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        return send_json_text(req, "400 Bad Request", "{\"error\":\"json\"}");
    }
    const cJSON *jlc = cJSON_GetObjectItemCaseSensitive(root, "ledCount");
    const cJSON *jbr = cJSON_GetObjectItemCaseSensitive(root, "brightness");

    if (!cJSON_IsNumber(jlc) || !cJSON_IsNumber(jbr)) {
        cJSON_Delete(root);
        return send_json_text(req, "400 Bad Request", "{\"error\":\"ledCount and brightness numbers\"}");
    }
    int led = (int)jlc->valuedouble;
    int br = (int)jbr->valuedouble;
    cJSON_Delete(root);

    if (led < 1 || led > CYBEER_LED_COUNT_MAX || br < 1 || br > 255) {
        return send_json_text(req, "400 Bad Request", "{\"error\":\"out of range\"}");
    }

    if (cybeer_nvs_set_led_count((uint8_t)led) != ESP_OK || cybeer_nvs_set_led_brightness((uint8_t)br) != ESP_OK) {
        return send_json_text(req, "500 Internal Server Error", "{\"error\":\"nvs\"}");
    }

    if (send_json_text(req, "200 OK", "{\"ok\":true,\"restarting\":true}") != ESP_OK) {
        /* still restart — response may truncate */
    }
    vTaskDelay(pdMS_TO_TICKS(250));
    esp_restart();
    return ESP_OK;
}

static esp_err_t h_get_tournaments_active(httpd_req_t *req)
{
    cJSON *env = NULL;
    if (cybeer_tournament_get_active_envelope_json(&env) != ESP_OK || !env) {
        return send_json_text(req, "500 Internal Server Error", "{\"error\":\"tournament envelope\"}");
    }
    return json_send(req, env);
}

static bool parse_admin_tournament_start(const char *path, char uuid_out[40])
{
    const char *pfx = "/api/admin/tournaments/";
    if (strncmp(path, pfx, strlen(pfx)) != 0) {
        return false;
    }
    const char *rest = path + strlen(pfx);
    const char *suf = strstr(rest, "/start");
    if (!suf || strcmp(suf, "/start") != 0) {
        return false;
    }
    size_t id_len = (size_t)(suf - rest);
    if (id_len == 0 || id_len >= 40) {
        return false;
    }
    memcpy(uuid_out, rest, id_len);
    uuid_out[id_len] = '\0';
    return strchr(uuid_out, '/') == NULL;
}

static bool parse_admin_match_assign(const char *path, char uuid_out[40])
{
    const char *pfx = "/api/admin/tournaments/matches/";
    if (strncmp(path, pfx, strlen(pfx)) != 0) {
        return false;
    }
    const char *rest = path + strlen(pfx);
    const char *suf = strstr(rest, "/assign");
    if (!suf || strcmp(suf, "/assign") != 0) {
        return false;
    }
    size_t id_len = (size_t)(suf - rest);
    if (id_len == 0 || id_len >= 40) {
        return false;
    }
    memcpy(uuid_out, rest, id_len);
    uuid_out[id_len] = '\0';
    return strchr(uuid_out, '/') == NULL;
}

static esp_err_t h_post_admin_tournaments_create(httpd_req_t *req)
{
    esp_err_t g = require_admin_pin(req);
    if (g != ESP_OK) {
        return g;
    }
    char body[ADMIN_BODY_MAX];
    if (read_http_body(req, body, sizeof(body)) != ESP_OK) {
        return send_json_text(req, "400 Bad Request", "{\"error\":\"body\"}");
    }
    cJSON *root = cJSON_Parse(body);
    if (!root || !cJSON_IsObject(root)) {
        if (root) {
            cJSON_Delete(root);
        }
        return send_json_text(req, "400 Bad Request", "{\"error\":\"json object\"}");
    }
    const cJSON *jnm = cJSON_GetObjectItemCaseSensitive(root, "name");
    const cJSON *jp = cJSON_GetObjectItemCaseSensitive(root, "participantIds");
    const char *nm = (cJSON_IsString(jnm) && jnm->valuestring) ? jnm->valuestring : "";
    if (!nm[0] || !jp || !cJSON_IsArray(jp) || cJSON_GetArraySize(jp) <= 0) {
        cJSON_Delete(root);
        return send_json_text(req, "400 Bad Request", "{\"error\":\"name and participantIds[]\"}");
    }
    char tid[37];
    esp_err_t e = cybeer_tournament_create_named(nm, jp, tid);
    cJSON_Delete(root);
    if (e != ESP_OK) {
        return send_json_text(req, "500 Internal Server Error", "{\"error\":\"generate\"}");
    }
    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }
    cJSON_AddStringToObject(resp, "id", tid);
    return json_send(req, resp);
}

static esp_err_t h_post_admin_tournament_start(httpd_req_t *req)
{
    esp_err_t g = require_admin_pin(req);
    if (g != ESP_OK) {
        return g;
    }
    char path[160];
    copy_path_no_query(req->uri, path, sizeof(path));
    char tid[40];
    if (!parse_admin_tournament_start(path, tid)) {
        return send_json_text(req, "400 Bad Request", "{\"error\":\"uri\"}");
    }
    esp_err_t e = cybeer_tournament_start_by_id(tid);
    if (e == ESP_OK) {
        return send_json_text(req, "200 OK", "{\"ok\":true}");
    }
    if (e == ESP_ERR_NOT_FOUND) {
        return send_json_text(req, "404 Not Found", "{\"error\":\"tournament id not found\"}");
    }
    if (e == ESP_ERR_INVALID_STATE) {
        return send_json_text(req, "409 Conflict",
                              "{\"error\":\"cannot start (already active, not draft, or invalid)\"}");
    }
    return send_json_text(req, "500 Internal Server Error", "{\"error\":\"storage\"}");
}

static esp_err_t h_post_admin_tournament_assign(httpd_req_t *req)
{
    esp_err_t g = require_admin_pin(req);
    if (g != ESP_OK) {
        return g;
    }
    char path[160];
    copy_path_no_query(req->uri, path, sizeof(path));
    char mid[40];
    if (!parse_admin_match_assign(path, mid)) {
        return send_json_text(req, "400 Bad Request", "{\"error\":\"uri\"}");
    }
    char body[ADMIN_BODY_MAX];
    if (read_http_body(req, body, sizeof(body)) != ESP_OK) {
        return send_json_text(req, "400 Bad Request", "{\"error\":\"body\"}");
    }
    cJSON *root = cJSON_Parse(body);
    if (!root || !cJSON_IsObject(root)) {
        if (root) {
            cJSON_Delete(root);
        }
        return send_json_text(req, "400 Bad Request", "{\"error\":\"json object\"}");
    }
    const cJSON *jsl = cJSON_GetObjectItemCaseSensitive(root, "slot");
    const char *slot = (cJSON_IsString(jsl) && jsl->valuestring) ? jsl->valuestring : "";
    cJSON_Delete(root);
    if (strcmp(slot, "A") != 0 && strcmp(slot, "B") != 0) {
        return send_json_text(req, "400 Bad Request", "{\"error\":\"slot A or B\"}");
    }
    esp_err_t e = cybeer_tournament_assign_next_bind(mid, slot);
    if (e == ESP_OK) {
        return send_json_text(req, "200 OK", "{\"ok\":true}");
    }
    if (e == ESP_ERR_NOT_FOUND) {
        return send_json_text(req, "404 Not Found", "{\"error\":\"match\"}");
    }
    if (e == ESP_ERR_INVALID_STATE || e == ESP_ERR_INVALID_ARG) {
        return send_json_text(req, "409 Conflict",
                              "{\"error\":\"assignment not allowed (busy slot / no tournament / seeds)\"}");
    }
    return send_json_text(req, "500 Internal Server Error", "{\"error\":\"storage\"}");
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

static int compare_runs_by_duration(const void *a, const void *b)
{
    const cybeer_run_t *ra = (const cybeer_run_t *)a;
    const cybeer_run_t *rb = (const cybeer_run_t *)b;
    if (ra->duration_us < rb->duration_us) {
        return -1;
    }
    if (ra->duration_us > rb->duration_us) {
        return 1;
    }
    return 0;
}

static esp_err_t h_get_leaderboard(httpd_req_t *req)
{
    int limit = 20;
    char query[64];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char val[8];
        if (httpd_query_key_value(query, "limit", val, sizeof(val)) == ESP_OK) {
            int v = atoi(val);
            if (v > 0 && v <= 50) {
                limit = v;
            }
        }
    }

    const char *raw_runs = cybeer_storage_runs_json();
    cJSON *arr = cJSON_Parse(raw_runs && raw_runs[0] ? raw_runs : "[]");
    if (!arr || !cJSON_IsArray(arr)) {
        if (arr) {
            cJSON_Delete(arr);
        }
        cJSON *empty = cJSON_CreateArray();
        return json_send(req, empty);
    }

    int n = cJSON_GetArraySize(arr);
    cybeer_run_t *claimed = (cybeer_run_t *)malloc((size_t)n * sizeof(cybeer_run_t));
    if (!claimed) {
        cJSON_Delete(arr);
        cJSON *empty = cJSON_CreateArray();
        return json_send(req, empty);
    }

    int nc = 0;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, arr)
    {
        cybeer_run_t r;
        memset(&r, 0, sizeof(r));
        const cJSON *jcl = cJSON_GetObjectItemCaseSensitive(item, "claimed");
        if (!cJSON_IsTrue(jcl)) {
            continue;
        }
        const cJSON *jdur = cJSON_GetObjectItemCaseSensitive(item, "duration_us");
        if (!cJSON_IsNumber(jdur) || jdur->valuedouble <= 0) {
            continue;
        }
        const cJSON *jid = cJSON_GetObjectItemCaseSensitive(item, "id");
        const cJSON *jpid = cJSON_GetObjectItemCaseSensitive(item, "participant_id");
        const cJSON *jfa = cJSON_GetObjectItemCaseSensitive(item, "finished_at");
        if (cJSON_IsString(jid) && jid->valuestring) {
            strncpy(r.id, jid->valuestring, sizeof(r.id) - 1);
        }
        if (cJSON_IsString(jpid) && jpid->valuestring) {
            strncpy(r.participant_id, jpid->valuestring, sizeof(r.participant_id) - 1);
        }
        if (cJSON_IsNumber(jdur)) {
            r.duration_us = (int64_t)jdur->valuedouble;
        }
        if (cJSON_IsString(jfa) && jfa->valuestring) {
            strncpy(r.finished_at, jfa->valuestring, sizeof(r.finished_at) - 1);
        }
        r.claimed = true;
        claimed[nc++] = r;
    }
    cJSON_Delete(arr);

    if (nc > 1) {
        qsort(claimed, (size_t)nc, sizeof(cybeer_run_t), compare_runs_by_duration);
    }

    const char *raw_parts = cybeer_storage_participants_json();
    cJSON *parts = cJSON_Parse(raw_parts && raw_parts[0] ? raw_parts : "[]");

    cJSON *out = cJSON_CreateArray();
    if (!out) {
        free(claimed);
        if (parts) {
            cJSON_Delete(parts);
        }
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }

    int cnt = nc < limit ? nc : limit;
    for (int i = 0; i < cnt; i++) {
        cJSON *entry = cJSON_CreateObject();
        if (!entry) {
            continue;
        }
        cJSON_AddNumberToObject(entry, "rank", (double)(i + 1));
        cJSON_AddStringToObject(entry, "participantId", claimed[i].participant_id);

        const char *pname = "";
        if (parts && cJSON_IsArray(parts)) {
            cJSON *p = NULL;
            cJSON_ArrayForEach(p, parts)
            {
                const cJSON *pid = cJSON_GetObjectItemCaseSensitive(p, "id");
                if (cJSON_IsString(pid) && pid->valuestring
                    && strcmp(pid->valuestring, claimed[i].participant_id) == 0) {
                    const cJSON *pn = cJSON_GetObjectItemCaseSensitive(p, "name");
                    if (cJSON_IsString(pn) && pn->valuestring) {
                        pname = pn->valuestring;
                    }
                    break;
                }
            }
        }
        cJSON_AddStringToObject(entry, "participantName", pname);
        cJSON_AddNumberToObject(entry, "durationUs", (double)claimed[i].duration_us);
        cJSON_AddStringToObject(entry, "finishedAt", claimed[i].finished_at);
        cJSON_AddItemToArray(out, entry);
    }

    free(claimed);
    if (parts) {
        cJSON_Delete(parts);
    }
    return json_send(req, out);
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
    /* Default max_uri_handlers is 8; we register 30+ routes (API + setup + OTA + WS + static). */
    cfg.max_uri_handlers = 40;
    /* Default stack_size is 4096; several handlers use ADMIN_BODY_MAX (4096) on stack. */
    cfg.stack_size = 10240;
    esp_err_t err = httpd_start(&s_server, &cfg);
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
    httpd_uri_t u_parts_post = {
        .uri = "/api/participants", .method = HTTP_POST, .handler = h_post_participants, .user_ctx = NULL
    };
    httpd_uri_t u_leaderboard = {
        .uri = "/api/leaderboard", .method = HTTP_GET, .handler = h_get_leaderboard, .user_ctx = NULL
    };
    httpd_uri_t u_part_sub = {
        .uri = "/api/participants/*",
        .method = HTTP_GET,
        .handler = h_get_participant_sub,
        .user_ctx = NULL
    };
    httpd_uri_t u_claim = { .uri = "/api/claim", .method = HTTP_POST, .handler = h_post_claim, .user_ctx = NULL };
    httpd_uri_t u_admin_pin = {
        .uri = "/api/admin/pin/setup", .method = HTTP_POST, .handler = h_post_admin_pin_setup, .user_ctx = NULL
    };
    httpd_uri_t u_admin_pin_verify = {
        .uri = "/api/admin/pin/verify",
        .method = HTTP_POST,
        .handler = h_post_admin_pin_verify,
        .user_ctx = NULL
    };
    httpd_uri_t u_admin_pin_change = {
        .uri = "/api/admin/pin/change",
        .method = HTTP_POST,
        .handler = h_post_admin_pin_change,
        .user_ctx = NULL
    };
    httpd_uri_t u_admin_pin_reset_default = {
        .uri = "/api/admin/pin/reset-default",
        .method = HTTP_POST,
        .handler = h_post_admin_pin_reset_default,
        .user_ctx = NULL
    };
    httpd_uri_t u_admin_runs = {
        .uri = "/api/admin/runs", .method = HTTP_POST, .handler = h_post_admin_runs, .user_ctx = NULL
    };
    httpd_uri_t u_admin_run_patch = {
        .uri = "/api/admin/runs/*", .method = HTTP_PATCH, .handler = h_patch_admin_run, .user_ctx = NULL
    };
    httpd_uri_t u_admin_run_del = {
        .uri = "/api/admin/runs/*", .method = HTTP_DELETE, .handler = h_delete_admin_run, .user_ctx = NULL
    };
    httpd_uri_t u_admin_reset = {
        .uri = "/api/admin/data/reset",
        .method = HTTP_DELETE,
        .handler = h_delete_admin_data_reset,
        .user_ctx = NULL
    };
    httpd_uri_t u_export = { .uri = "/api/export", .method = HTTP_GET, .handler = h_get_export, .user_ctx = NULL };
    httpd_uri_t u_settings_put = {
        .uri = "/api/settings", .method = HTTP_PUT, .handler = h_put_settings, .user_ctx = NULL
    };

    httpd_uri_t u_tor_active_pub = {
        .uri = "/api/tournaments/active",
        .method = HTTP_GET,
        .handler = h_get_tournaments_active,
        .user_ctx = NULL
    };
    httpd_uri_t u_tor_assign = { .uri = "/api/admin/tournaments/matches/*/assign",
                                  .method = HTTP_POST,
                                  .handler = h_post_admin_tournament_assign,
                                  .user_ctx = NULL };
    httpd_uri_t u_tor_start = {
        .uri = "/api/admin/tournaments/*/start",
        .method = HTTP_POST,
        .handler = h_post_admin_tournament_start,
        .user_ctx = NULL
    };
    httpd_uri_t u_tor_create = {
        .uri = "/api/admin/tournaments",
        .method = HTTP_POST,
        .handler = h_post_admin_tournaments_create,
        .user_ctx = NULL
    };
    httpd_uri_t u_admin_tournaments_list = {
        .uri = "/api/admin/tournaments",
        .method = HTTP_GET,
        .handler = h_get_admin_tournaments,
        .user_ctx = NULL
    };
    httpd_uri_t u_admin_reboot = {
        .uri = "/api/admin/reboot", .method = HTTP_POST, .handler = h_post_admin_reboot, .user_ctx = NULL
    };
    httpd_uri_t u_admin_power_eco = {
        .uri = "/api/admin/power/eco", .method = HTTP_POST, .handler = h_post_admin_power_eco, .user_ctx = NULL
    };
    httpd_uri_t u_admin_power_sleep = {
        .uri = "/api/admin/power/sleep", .method = HTTP_POST, .handler = h_post_admin_power_sleep, .user_ctx = NULL
    };
    httpd_uri_t u_part_rename = {
        .uri = "/api/participants/*", .method = HTTP_PATCH, .handler = h_patch_participant, .user_ctx = NULL
    };
    httpd_uri_t u_part_del = {
        .uri = "/api/participants/*", .method = HTTP_DELETE, .handler = h_delete_participant, .user_ctx = NULL
    };

    httpd_uri_t u_static = { .uri = "/*", .method = HTTP_GET, .handler = h_static, .user_ctx = NULL };

    if (httpd_register_uri_handler(s_server, &u_status) != ESP_OK || httpd_register_uri_handler(s_server, &u_runs) != ESP_OK
        || httpd_register_uri_handler(s_server, &u_parts) != ESP_OK
        || httpd_register_uri_handler(s_server, &u_parts_post) != ESP_OK
        || httpd_register_uri_handler(s_server, &u_leaderboard) != ESP_OK
        || httpd_register_uri_handler(s_server, &u_part_sub) != ESP_OK
        || httpd_register_uri_handler(s_server, &u_part_rename) != ESP_OK
        || httpd_register_uri_handler(s_server, &u_part_del) != ESP_OK
        || httpd_register_uri_handler(s_server, &u_claim) != ESP_OK
        || httpd_register_uri_handler(s_server, &u_tor_active_pub) != ESP_OK
        || httpd_register_uri_handler(s_server, &u_tor_assign) != ESP_OK
        || httpd_register_uri_handler(s_server, &u_tor_start) != ESP_OK
        || httpd_register_uri_handler(s_server, &u_tor_create) != ESP_OK || httpd_register_uri_handler(s_server, &u_admin_pin) != ESP_OK
        || httpd_register_uri_handler(s_server, &u_admin_pin_verify) != ESP_OK
        || httpd_register_uri_handler(s_server, &u_admin_pin_change) != ESP_OK
        || httpd_register_uri_handler(s_server, &u_admin_pin_reset_default) != ESP_OK
        || httpd_register_uri_handler(s_server, &u_admin_tournaments_list) != ESP_OK
        || httpd_register_uri_handler(s_server, &u_admin_runs) != ESP_OK
        || httpd_register_uri_handler(s_server, &u_admin_run_patch) != ESP_OK
        || httpd_register_uri_handler(s_server, &u_admin_run_del) != ESP_OK
        || httpd_register_uri_handler(s_server, &u_admin_reset) != ESP_OK
        || httpd_register_uri_handler(s_server, &u_export) != ESP_OK
        || httpd_register_uri_handler(s_server, &u_settings_put) != ESP_OK
        || httpd_register_uri_handler(s_server, &u_admin_reboot) != ESP_OK
        || httpd_register_uri_handler(s_server, &u_admin_power_eco) != ESP_OK
        || httpd_register_uri_handler(s_server, &u_admin_power_sleep) != ESP_OK
        || cybeer_ota_register_handlers(s_server) != ESP_OK || cybeer_ws_register(s_server) != ESP_OK
        || httpd_register_uri_handler(s_server, &u_static) != ESP_OK) {
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
