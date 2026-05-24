#include "cybeer_sync.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "cybeer_config.h"
#include "cybeer_wifi.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "cybeer_sync";

static SemaphoreHandle_t s_sync_mtx;
static bool s_sync_started;

static esp_err_t sync_take(void)
{
    ESP_RETURN_ON_FALSE(s_sync_mtx, ESP_ERR_INVALID_STATE, TAG, "mutex");
    if (xSemaphoreTake(s_sync_mtx, pdMS_TO_TICKS(5000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static void sync_give(void)
{
    xSemaphoreGive(s_sync_mtx);
}

static esp_err_t read_text_file(const char *path, char **out_text)
{
    *out_text = NULL;
    FILE *f = fopen(path, "rb");
    if (!f) {
        return ESP_ERR_NOT_FOUND;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return ESP_FAIL;
    }
    long sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return ESP_FAIL;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return ESP_FAIL;
    }

    size_t cap = (size_t)sz + 1;
    char *buf = (char *)malloc(cap);
    if (!buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';
    *out_text = buf;
    return ESP_OK;
}

static esp_err_t write_text_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        return ESP_FAIL;
    }
    size_t len = strlen(text);
    size_t wr = fwrite(text, 1, len, f);
    fclose(f);
    if (wr != len) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t load_queue_locked(cJSON **out_arr)
{
    *out_arr = NULL;
    char *raw = NULL;
    esp_err_t err = read_text_file(CYBEER_SYNC_QUEUE_PATH, &raw);
    if (err == ESP_ERR_NOT_FOUND) {
        cJSON *empty = cJSON_CreateArray();
        if (!empty) {
            return ESP_ERR_NO_MEM;
        }
        char *printed = cJSON_PrintUnformatted(empty);
        cJSON_Delete(empty);
        if (!printed) {
            return ESP_ERR_NO_MEM;
        }
        err = write_text_file(CYBEER_SYNC_QUEUE_PATH, printed);
        free(printed);
        if (err != ESP_OK) {
            return err;
        }
        raw = (char *)malloc(3);
        if (!raw) {
            return ESP_ERR_NO_MEM;
        }
        memcpy(raw, "[]", 3);
    } else if (err != ESP_OK) {
        return err;
    }

    cJSON *arr = cJSON_Parse(raw);
    free(raw);
    if (!arr || !cJSON_IsArray(arr)) {
        if (arr) {
            cJSON_Delete(arr);
        }
        return ESP_FAIL;
    }

    *out_arr = arr;
    return ESP_OK;
}

static esp_err_t save_queue_locked(cJSON *arr)
{
    char *printed = cJSON_PrintUnformatted(arr);
    if (!printed) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = write_text_file(CYBEER_SYNC_QUEUE_PATH, printed);
    free(printed);
    return err;
}

static cJSON *queue_event_from_run(const cybeer_run_t *run)
{
    cJSON *event = cJSON_CreateObject();
    if (!event) {
        return NULL;
    }
    cJSON_AddStringToObject(event, "type", "run");
    cJSON_AddStringToObject(event, "id", run->id);
    cJSON_AddStringToObject(event, "participant_id", run->participant_id);
    cJSON_AddNumberToObject(event, "duration_us", (double)run->duration_us);
    cJSON_AddStringToObject(event, "finished_at", run->finished_at);
    cJSON_AddBoolToObject(event, "claimed", run->claimed);
    cJSON_AddStringToObject(event, "tournament_match_id", run->tournament_match_id);
    return event;
}

static cJSON *queue_event_from_participant(const char *device_id, const char *name)
{
    cJSON *event = cJSON_CreateObject();
    if (!event) {
        return NULL;
    }
    cJSON_AddStringToObject(event, "type", "participant");
    cJSON_AddStringToObject(event, "device_id", device_id);
    cJSON_AddStringToObject(event, "name", name);
    return event;
}

static esp_err_t queue_append_event(cJSON *event)
{
    ESP_RETURN_ON_FALSE(event, ESP_ERR_INVALID_ARG, TAG, "event");
    ESP_RETURN_ON_ERROR(sync_take(), TAG, "take");

    cJSON *arr = NULL;
    esp_err_t err = load_queue_locked(&arr);
    if (err != ESP_OK) {
        cJSON_Delete(event);
        sync_give();
        return err;
    }
    cJSON_AddItemToArray(arr, event);
    err = save_queue_locked(arr);
    cJSON_Delete(arr);
    sync_give();
    return err;
}

static esp_err_t http_get_json(const char *url, cJSON **out_root)
{
    *out_root = NULL;
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .user_agent = "CyBeer/Sync",
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_header(client, "x-api-key", CYBEER_SYNC_API_KEY);
    esp_http_client_set_header(client, "Authorization", "Bearer " CYBEER_SYNC_API_KEY);
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }

    (void)esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status < 200 || status >= 300) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return status == 404 ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }

    int content_length = esp_http_client_get_content_length(client);
    size_t cap = (content_length > 0 ? (size_t)content_length : 4096U) + 1U;
    char *body = (char *)malloc(cap);
    if (!body) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_NO_MEM;
    }

    size_t total = 0;
    for (;;) {
        if (total + 512 >= cap) {
            size_t new_cap = cap * 2U;
            char *grown = (char *)realloc(body, new_cap);
            if (!grown) {
                free(body);
                esp_http_client_close(client);
                esp_http_client_cleanup(client);
                return ESP_ERR_NO_MEM;
            }
            body = grown;
            cap = new_cap;
        }
        int rd = esp_http_client_read(client, body + total, (int)(cap - total - 1U));
        if (rd < 0) {
            free(body);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }
        if (rd == 0) {
            break;
        }
        total += (size_t)rd;
    }
    body[total] = '\0';

    cJSON *root = cJSON_Parse(body);
    free(body);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (!root) {
        return ESP_FAIL;
    }
    *out_root = root;
    return ESP_OK;
}

static esp_err_t http_post_json_ok(const char *url, const char *body)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 12000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .user_agent = "CyBeer/Sync",
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "x-api-key", CYBEER_SYNC_API_KEY);
    esp_http_client_set_header(client, "Authorization", "Bearer " CYBEER_SYNC_API_KEY);
    esp_http_client_set_post_field(client, body, (int)strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (status >= 200 && status < 300) {
        return ESP_OK;
    }
    return ESP_FAIL;
}

static cJSON *extract_array_from_dump(cJSON *root, const char *key)
{
    if (!root) {
        return NULL;
    }
    cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsArray(arr)) {
        return arr;
    }
    cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (cJSON_IsObject(data)) {
        arr = cJSON_GetObjectItemCaseSensitive(data, key);
        if (cJSON_IsArray(arr)) {
            return arr;
        }
    }
    return NULL;
}

static void parse_run_from_dump_item(const cJSON *item, cybeer_run_t *run)
{
    memset(run, 0, sizeof(*run));
    const cJSON *j = NULL;

    j = cJSON_GetObjectItemCaseSensitive(item, "id");
    if (cJSON_IsString(j) && j->valuestring) {
        strncpy(run->id, j->valuestring, sizeof(run->id) - 1);
    }

    j = cJSON_GetObjectItemCaseSensitive(item, "participant_id");
    if (!cJSON_IsString(j)) {
        j = cJSON_GetObjectItemCaseSensitive(item, "participantId");
    }
    if (cJSON_IsString(j) && j->valuestring) {
        strncpy(run->participant_id, j->valuestring, sizeof(run->participant_id) - 1);
    }

    j = cJSON_GetObjectItemCaseSensitive(item, "duration_us");
    if (!cJSON_IsNumber(j)) {
        j = cJSON_GetObjectItemCaseSensitive(item, "durationUs");
    }
    if (cJSON_IsNumber(j)) {
        run->duration_us = (int64_t)j->valuedouble;
    }

    j = cJSON_GetObjectItemCaseSensitive(item, "finished_at");
    if (!cJSON_IsString(j)) {
        j = cJSON_GetObjectItemCaseSensitive(item, "finishedAt");
    }
    if (cJSON_IsString(j) && j->valuestring) {
        strncpy(run->finished_at, j->valuestring, sizeof(run->finished_at) - 1);
    }

    j = cJSON_GetObjectItemCaseSensitive(item, "claimed");
    if (cJSON_IsBool(j)) {
        run->claimed = cJSON_IsTrue(j);
    }

    j = cJSON_GetObjectItemCaseSensitive(item, "tournament_match_id");
    if (!cJSON_IsString(j)) {
        j = cJSON_GetObjectItemCaseSensitive(item, "tournamentMatchId");
    }
    if (cJSON_IsString(j) && j->valuestring) {
        strncpy(run->tournament_match_id, j->valuestring, sizeof(run->tournament_match_id) - 1);
    }
}

static void restore_dump_once(void)
{
    if (cybeer_storage_runs_count() > 0) {
        return;
    }

    char url[192];
    snprintf(url, sizeof(url), "%s/dump", CYBEER_SYNC_BASE_URL);

    cJSON *root = NULL;
    esp_err_t err = http_get_json(url, &root);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "dump unavailable: %s", esp_err_to_name(err));
        return;
    }

    cJSON *participants = extract_array_from_dump(root, "participants");
    if (participants) {
        cJSON *it = NULL;
        cJSON_ArrayForEach(it, participants)
        {
            cJSON *jid = cJSON_GetObjectItemCaseSensitive(it, "id");
            if (!cJSON_IsString(jid)) {
                jid = cJSON_GetObjectItemCaseSensitive(it, "device_id");
            }
            if (!cJSON_IsString(jid) || !jid->valuestring || !jid->valuestring[0]) {
                continue;
            }
            cJSON *jname = cJSON_GetObjectItemCaseSensitive(it, "name");
            if (!cJSON_IsString(jname) || !jname->valuestring || !jname->valuestring[0]) {
                continue;
            }
            (void)cybeer_storage_upsert_participant(jid->valuestring, jname->valuestring);
        }
    }

    cJSON *runs = extract_array_from_dump(root, "runs");
    if (runs) {
        cJSON *it = NULL;
        cJSON_ArrayForEach(it, runs)
        {
            if (!cJSON_IsObject(it)) {
                continue;
            }
            cybeer_run_t run;
            parse_run_from_dump_item(it, &run);
            if (!run.id[0]) {
                continue;
            }
            if (!run.finished_at[0]) {
                cybeer_storage_iso8601_now(run.finished_at);
            }
            (void)cybeer_storage_add_run_if_not_exists(&run);
        }
    }

    cJSON_Delete(root);
}

static void flush_queue_once(void)
{
    if (!cybeer_wifi_sta_connected()) {
        return;
    }

    if (sync_take() != ESP_OK) {
        return;
    }
    cJSON *arr = NULL;
    esp_err_t err = load_queue_locked(&arr);
    if (err != ESP_OK) {
        sync_give();
        return;
    }
    if (cJSON_GetArraySize(arr) == 0) {
        cJSON_Delete(arr);
        sync_give();
        return;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        cJSON_Delete(arr);
        sync_give();
        return;
    }
    cJSON *events = cJSON_Duplicate(arr, true);
    if (!events) {
        cJSON_Delete(root);
        cJSON_Delete(arr);
        sync_give();
        return;
    }
    cJSON_AddItemToObject(root, "events", events);
    char *payload = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!payload) {
        cJSON_Delete(arr);
        sync_give();
        return;
    }

    char url[192];
    snprintf(url, sizeof(url), "%s/sync", CYBEER_SYNC_BASE_URL);
    err = http_post_json_ok(url, payload);
    free(payload);

    if (err == ESP_OK) {
        cJSON *empty = cJSON_CreateArray();
        if (empty) {
            (void)save_queue_locked(empty);
            cJSON_Delete(empty);
        }
        ESP_LOGI(TAG, "sync push ok (%d events)", cJSON_GetArraySize(arr));
    } else {
        ESP_LOGW(TAG, "sync push failed: %s", esp_err_to_name(err));
    }

    cJSON_Delete(arr);
    sync_give();
}

static void sync_task(void *arg)
{
    (void)arg;
    restore_dump_once();
    for (;;) {
        flush_queue_once();
        vTaskDelay(pdMS_TO_TICKS(CYBEER_SYNC_INTERVAL_MS));
    }
}

esp_err_t cybeer_sync_init(void)
{
    if (!s_sync_mtx) {
        s_sync_mtx = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_sync_mtx, ESP_ERR_NO_MEM, TAG, "mutex");
    }
    if (s_sync_started) {
        return ESP_OK;
    }

    BaseType_t ok = xTaskCreate(sync_task, "cybeer_sync", 8192, NULL, tskIDLE_PRIORITY + 1, NULL);
    if (ok != pdPASS) {
        return ESP_FAIL;
    }
    s_sync_started = true;
    return ESP_OK;
}

esp_err_t cybeer_sync_enqueue_run(const cybeer_run_t *run)
{
    ESP_RETURN_ON_FALSE(run && run->id[0], ESP_ERR_INVALID_ARG, TAG, "run");
    cJSON *event = queue_event_from_run(run);
    if (!event) {
        return ESP_ERR_NO_MEM;
    }
    return queue_append_event(event);
}

esp_err_t cybeer_sync_enqueue_participant(const char *device_id, const char *name)
{
    ESP_RETURN_ON_FALSE(device_id && device_id[0] && name && name[0], ESP_ERR_INVALID_ARG, TAG, "args");
    cJSON *event = queue_event_from_participant(device_id, name);
    if (!event) {
        return ESP_ERR_NO_MEM;
    }
    return queue_append_event(event);
}
