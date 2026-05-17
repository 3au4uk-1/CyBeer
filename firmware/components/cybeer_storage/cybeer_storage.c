#include "cybeer_storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define CY_ROOT "/littlefs"
#define CY_DATA CY_ROOT "/data"
#define PATH_PARTICIPANTS CY_DATA "/participants.json"
#define PATH_RUNS CY_DATA "/runs.json"
#define PATH_TOURNAMENTS CY_DATA "/tournaments.json"
#define PATH_ACTIVE_T CY_DATA "/active_tournament.json"

#define JSON_BUF_SZ 16384

static const char *TAG = "cybeer_storage";

static SemaphoreHandle_t s_fs_mtx;

static char s_runs_json_buf[JSON_BUF_SZ];
static char s_participants_json_buf[JSON_BUF_SZ];
/** Only used while `s_fs_mtx` is held (avoid large stack frames). */
static char s_json_scratch[JSON_BUF_SZ];

static esp_err_t take_mtx(void)
{
    ESP_RETURN_ON_FALSE(s_fs_mtx, ESP_ERR_INVALID_STATE, TAG, "not initialized");
    if (!xSemaphoreTake(s_fs_mtx, pdMS_TO_TICKS(5000))) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static void give_mtx(void)
{
    xSemaphoreGive(s_fs_mtx);
}

static esp_err_t read_full_locked(const char *path, char *buf, size_t buf_sz, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return ESP_FAIL;
    }
    size_t n = fread(buf, 1, buf_sz - 1, f);
    fclose(f);
    buf[n] = '\0';
    if (out_len) {
        *out_len = n;
    }
    return ESP_OK;
}

static esp_err_t write_full_locked(const char *path, const char *data)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "open write fail %s", path);
        return ESP_FAIL;
    }
    size_t len = strlen(data);
    size_t w = fwrite(data, 1, len, f);
    fclose(f);
    if (w != len) {
        ESP_LOGE(TAG, "short write %s", path);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t ensure_json_file_locked(const char *path, const char *initial)
{
    struct stat st;
    if (stat(path, &st) == 0) {
        return ESP_OK;
    }
    return write_full_locked(path, initial);
}

static esp_err_t ensure_layout_locked(void)
{
    struct stat st;
    if (stat(CY_ROOT, &st) != 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (stat(CY_DATA, &st) != 0) {
        if (mkdir(CY_DATA, 0775) != 0) {
            ESP_LOGE(TAG, "mkdir " CY_DATA " failed");
            return ESP_FAIL;
        }
    }
    ESP_RETURN_ON_ERROR(ensure_json_file_locked(PATH_PARTICIPANTS, "[]"), TAG, "participants init");
    ESP_RETURN_ON_ERROR(ensure_json_file_locked(PATH_RUNS, "[]"), TAG, "runs init");
    ESP_RETURN_ON_ERROR(ensure_json_file_locked(PATH_TOURNAMENTS, "[]"), TAG, "tournaments init");
    ESP_RETURN_ON_ERROR(ensure_json_file_locked(PATH_ACTIVE_T, "{}"), TAG, "active init");
    return ESP_OK;
}

void cybeer_format_uuid_v4(char out[37])
{
    uint8_t b[16];
    esp_fill_random(b, sizeof(b));
    b[6] = (uint8_t)((b[6] & 0x0f) | 0x40);
    b[8] = (uint8_t)((b[8] & 0x3f) | 0x80);
    snprintf(out, 37,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0], b[1], b[2],
             b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
}

void cybeer_storage_iso8601_now(char buf[32])
{
    time_t t = time(NULL);
    struct tm tm;
    gmtime_r(&t, &tm);
    if (strftime(buf, 32, "%Y-%m-%dT%H:%M:%SZ", &tm) == 0) {
        snprintf(buf, 32, "1970-01-01T00:00:00Z");
    }
}

esp_err_t cybeer_storage_init(void)
{
    if (!s_fs_mtx) {
        s_fs_mtx = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_fs_mtx, ESP_ERR_NO_MEM, TAG, "mutex");
    }

    esp_vfs_littlefs_conf_t conf = {
        .base_path = CY_ROOT,
        .partition_label = "littlefs",
        .format_if_mount_failed = true,
        .dont_mount = false,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    ESP_RETURN_ON_ERROR(err, TAG, "littlefs mount");

    ESP_RETURN_ON_ERROR(take_mtx(), TAG, "take");
    err = ensure_layout_locked();
    give_mtx();
    ESP_RETURN_ON_ERROR(err, TAG, "layout");

    return ESP_OK;
}

static cJSON *parse_array_file_locked(const char *path)
{
    size_t len = 0;
    if (read_full_locked(path, s_json_scratch, sizeof(s_json_scratch), &len) != ESP_OK) {
        return NULL;
    }
    cJSON *root = cJSON_Parse(s_json_scratch);
    if (!root || !cJSON_IsArray(root)) {
        if (root) {
            cJSON_Delete(root);
        }
        return NULL;
    }
    return root;
}

static esp_err_t persist_json_locked(const char *path, cJSON *root)
{
    char *printed = cJSON_PrintUnformatted(root);
    if (!printed) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = write_full_locked(path, printed);
    free(printed);
    return err;
}

static void run_from_json(const cJSON *obj, cybeer_run_t *out)
{
    memset(out, 0, sizeof(*out));
    const cJSON *j;

    j = cJSON_GetObjectItemCaseSensitive(obj, "id");
    if (cJSON_IsString(j) && j->valuestring) {
        strncpy(out->id, j->valuestring, sizeof(out->id) - 1);
    }
    j = cJSON_GetObjectItemCaseSensitive(obj, "participant_id");
    if (cJSON_IsString(j) && j->valuestring) {
        strncpy(out->participant_id, j->valuestring, sizeof(out->participant_id) - 1);
    }
    j = cJSON_GetObjectItemCaseSensitive(obj, "duration_us");
    if (cJSON_IsNumber(j)) {
        out->duration_us = (int64_t)j->valuedouble;
    }
    j = cJSON_GetObjectItemCaseSensitive(obj, "finished_at");
    if (cJSON_IsString(j) && j->valuestring) {
        strncpy(out->finished_at, j->valuestring, sizeof(out->finished_at) - 1);
    }
    j = cJSON_GetObjectItemCaseSensitive(obj, "claimed");
    if (cJSON_IsBool(j)) {
        out->claimed = cJSON_IsTrue(j);
    }
    j = cJSON_GetObjectItemCaseSensitive(obj, "tournament_match_id");
    if (cJSON_IsString(j) && j->valuestring) {
        strncpy(out->tournament_match_id, j->valuestring, sizeof(out->tournament_match_id) - 1);
    }
}

static void json_apply_run(cJSON *obj, const cybeer_run_t *run)
{
    cJSON_DeleteItemFromObject(obj, "id");
    cJSON_DeleteItemFromObject(obj, "participant_id");
    cJSON_DeleteItemFromObject(obj, "duration_us");
    cJSON_DeleteItemFromObject(obj, "finished_at");
    cJSON_DeleteItemFromObject(obj, "claimed");
    cJSON_DeleteItemFromObject(obj, "tournament_match_id");
    cJSON_AddStringToObject(obj, "id", run->id);
    cJSON_AddStringToObject(obj, "participant_id", run->participant_id);
    cJSON_AddNumberToObject(obj, "duration_us", (double)run->duration_us);
    cJSON_AddStringToObject(obj, "finished_at", run->finished_at);
    cJSON_AddBoolToObject(obj, "claimed", run->claimed);
    cJSON_AddStringToObject(obj, "tournament_match_id", run->tournament_match_id);
}

static cJSON *run_to_json_new(const cybeer_run_t *run)
{
    cJSON *o = cJSON_CreateObject();
    if (!o) {
        return NULL;
    }
    cJSON_AddStringToObject(o, "id", run->id);
    cJSON_AddStringToObject(o, "participant_id", run->participant_id);
    cJSON_AddNumberToObject(o, "duration_us", (double)run->duration_us);
    cJSON_AddStringToObject(o, "finished_at", run->finished_at);
    cJSON_AddBoolToObject(o, "claimed", run->claimed);
    cJSON_AddStringToObject(o, "tournament_match_id", run->tournament_match_id);
    return o;
}

esp_err_t cybeer_storage_add_run(const cybeer_run_t *run)
{
    ESP_RETURN_ON_FALSE(run && run->id[0], ESP_ERR_INVALID_ARG, TAG, "run/id");

    ESP_RETURN_ON_ERROR(take_mtx(), TAG, "take");

    cJSON *runs = parse_array_file_locked(PATH_RUNS);
    if (!runs) {
        give_mtx();
        return ESP_FAIL;
    }

    cJSON *item = run_to_json_new(run);
    if (!item) {
        cJSON_Delete(runs);
        give_mtx();
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddItemToArray(runs, item);

    esp_err_t err = persist_json_locked(PATH_RUNS, runs);
    cJSON_Delete(runs);
    give_mtx();
    return err;
}

static esp_err_t resolve_participant_by_name_locked(const char *name, char pid_out[37])
{
    cJSON *parts = parse_array_file_locked(PATH_PARTICIPANTS);
    if (!parts) {
        return ESP_FAIL;
    }

    const cJSON *p = NULL;
    cJSON_ArrayForEach(p, parts)
    {
        const cJSON *jn = cJSON_GetObjectItemCaseSensitive(p, "name");
        const cJSON *jid = cJSON_GetObjectItemCaseSensitive(p, "id");
        if (cJSON_IsString(jn) && jn->valuestring && strcmp(jn->valuestring, name) == 0 && cJSON_IsString(jid)
            && jid->valuestring) {
            snprintf(pid_out, 37, "%s", jid->valuestring);
            cJSON_Delete(parts);
            return ESP_OK;
        }
    }
    cJSON_Delete(parts);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t cybeer_storage_claim_run(const char *run_id, const char *name_or_pid, bool by_participant_id)
{
    ESP_RETURN_ON_FALSE(run_id && name_or_pid, ESP_ERR_INVALID_ARG, TAG, "args");

    ESP_RETURN_ON_ERROR(take_mtx(), TAG, "take");

    char pid[37] = { 0 };
    esp_err_t lookup = ESP_OK;
    if (by_participant_id) {
        snprintf(pid, sizeof(pid), "%s", name_or_pid);
    } else {
        lookup = resolve_participant_by_name_locked(name_or_pid, pid);
    }

    if (lookup != ESP_OK) {
        give_mtx();
        return lookup;
    }

    cJSON *runs = parse_array_file_locked(PATH_RUNS);
    if (!runs) {
        give_mtx();
        return ESP_FAIL;
    }

    bool found = false;
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, runs)
    {
        cybeer_run_t r;
        run_from_json(item, &r);
        if (strcmp(r.id, run_id) != 0) {
            continue;
        }
        found = true;
        if (r.claimed) {
            cJSON_Delete(runs);
            give_mtx();
            return ESP_ERR_INVALID_STATE;
        }
        r.claimed = true;
        strncpy(r.participant_id, pid, sizeof(r.participant_id) - 1);
        json_apply_run(item, &r);
        break;
    }

    if (!found) {
        cJSON_Delete(runs);
        give_mtx();
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t err = persist_json_locked(PATH_RUNS, runs);
    cJSON_Delete(runs);
    give_mtx();
    return err;
}

esp_err_t cybeer_storage_get_latest_unclaimed_run_id(char *out_id, size_t out_len)
{
    ESP_RETURN_ON_FALSE(out_id && out_len > 0, ESP_ERR_INVALID_ARG, TAG, "buf");

    ESP_RETURN_ON_ERROR(take_mtx(), TAG, "take");

    cJSON *runs = parse_array_file_locked(PATH_RUNS);
    if (!runs) {
        give_mtx();
        return ESP_FAIL;
    }

    esp_err_t ret = ESP_ERR_NOT_FOUND;
    int count = cJSON_GetArraySize(runs);
    for (int i = count - 1; i >= 0; i--) {
        cJSON *item = cJSON_GetArrayItem(runs, i);
        cybeer_run_t r;
        run_from_json(item, &r);
        if (!r.claimed && r.id[0]) {
            strncpy(out_id, r.id, out_len - 1);
            out_id[out_len - 1] = '\0';
            ret = ESP_OK;
            break;
        }
    }
    cJSON_Delete(runs);
    give_mtx();
    return ret;
}

esp_err_t cybeer_storage_get_participant_stats(const char *pid, cybeer_stats_t *out)
{
    ESP_RETURN_ON_FALSE(pid && out, ESP_ERR_INVALID_ARG, TAG, "args");
    memset(out, 0, sizeof(*out));

    ESP_RETURN_ON_ERROR(take_mtx(), TAG, "take");

    cJSON *runs = parse_array_file_locked(PATH_RUNS);
    if (!runs) {
        give_mtx();
        return ESP_FAIL;
    }

    int64_t sum = 0;
    int64_t best = INT64_MAX;
    int64_t worst = INT64_MIN;
    int64_t last = 0;
    int n = 0;

    cJSON *item = NULL;
    cJSON_ArrayForEach(item, runs)
    {
        cybeer_run_t r;
        run_from_json(item, &r);
        if (strcmp(r.participant_id, pid) != 0) {
            continue;
        }
        n++;
        sum += r.duration_us;
        if (r.duration_us < best) {
            best = r.duration_us;
        }
        if (r.duration_us > worst) {
            worst = r.duration_us;
        }
        last = r.duration_us;
    }
    cJSON_Delete(runs);
    give_mtx();

    out->count = n;
    out->last_us = last;
    if (n > 0) {
        out->best_us = best;
        out->worst_us = worst;
        out->avg_us = sum / n;
    }
    return ESP_OK;
}

const char *cybeer_storage_runs_json(void)
{
    if (take_mtx() != ESP_OK) {
        return "";
    }
    esp_err_t err = read_full_locked(PATH_RUNS, s_runs_json_buf, sizeof(s_runs_json_buf), NULL);
    if (err != ESP_OK) {
        s_runs_json_buf[0] = '\0';
    }
    give_mtx();
    return s_runs_json_buf;
}

const char *cybeer_storage_participants_json(void)
{
    if (take_mtx() != ESP_OK) {
        return "";
    }
    esp_err_t err =
        read_full_locked(PATH_PARTICIPANTS, s_participants_json_buf, sizeof(s_participants_json_buf), NULL);
    if (err != ESP_OK) {
        s_participants_json_buf[0] = '\0';
    }
    give_mtx();
    return s_participants_json_buf;
}
