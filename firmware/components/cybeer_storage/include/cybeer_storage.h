#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

struct cJSON;
typedef struct cJSON cJSON;

#define CYBEER_WIFI_SSID_MAX 33
#define CYBEER_WIFI_PASS_MAX 65
#define CYBEER_ADMIN_PIN_HASH_LEN 32
#define CYBEER_ADMIN_PIN_SALT_LEN 16

typedef struct {
    char id[37];
    char participant_id[37];
    int64_t duration_us;
    char finished_at[32];
    bool claimed;
    char tournament_match_id[37];
} cybeer_run_t;

typedef struct {
    int count;
    int64_t best_us;
    int64_t worst_us;
    int64_t avg_us;
    int64_t last_us;
} cybeer_stats_t;

esp_err_t cybeer_storage_init(void);

esp_err_t cybeer_nvs_get_wifi(char *ssid_out, size_t ssid_max, char *pass_out, size_t pass_max);
esp_err_t cybeer_nvs_set_wifi(const char *ssid, const char *pass);
/** Erases stored STA credentials (both SSID and password keys). */
esp_err_t cybeer_nvs_clear_wifi(void);
esp_err_t cybeer_nvs_get_led_settings(uint8_t *led_count, uint8_t *led_brightness);
esp_err_t cybeer_nvs_set_led_count(uint8_t count);
esp_err_t cybeer_nvs_set_led_brightness(uint8_t brightness);
esp_err_t cybeer_nvs_get_admin_pin_hash(uint8_t hash_out[CYBEER_ADMIN_PIN_HASH_LEN]);
esp_err_t cybeer_nvs_set_admin_pin_hash(const uint8_t hash[CYBEER_ADMIN_PIN_HASH_LEN]);
esp_err_t cybeer_nvs_get_admin_pin_salt(uint8_t salt_out[CYBEER_ADMIN_PIN_SALT_LEN]);
/** True when `admin_pin_hash` exists (first-time PIN setup applies before this). */
bool cybeer_nvs_admin_pin_is_configured(void);
/** Saves random salt + SHA256(salt||pin); fails with ESP_ERR_INVALID_STATE if PIN already configured. */
esp_err_t cybeer_admin_pin_first_setup(const char *pin);
esp_err_t cybeer_admin_verify_pin(const char *pin);

/** RFC 4122 UUID v4 (uses esp_fill_random). */
void cybeer_format_uuid_v4(char out[37]);

/** Wall-clock ISO8601 UTC if time sync exists; otherwise epoch-based placeholder. */
void cybeer_storage_iso8601_now(char buf[32]);

esp_err_t cybeer_storage_add_run(const cybeer_run_t *run);
esp_err_t cybeer_storage_add_run_manual(cybeer_run_t *run);
esp_err_t cybeer_storage_get_run(const char *run_id, cybeer_run_t *out);
esp_err_t cybeer_storage_delete_run(const char *run_id);
esp_err_t cybeer_storage_update_run(const char *run_id, const cybeer_run_t *run);
esp_err_t cybeer_storage_reset_all_data(void);
esp_err_t cybeer_storage_claim_run(const char *run_id, const char *name_or_pid, bool by_participant_id);
esp_err_t cybeer_storage_get_latest_unclaimed_run_id(char *out_id, size_t out_len);
esp_err_t cybeer_storage_get_participant_stats(const char *pid, cybeer_stats_t *out);

/** True if claimed run is global top-3 (by lowest duration_us) or personal best for participant_id. */
bool cybeer_storage_run_qualifies_podium_led(const cybeer_run_t *run);

/** Static buffers; copy before calling again or taking the filesystem mutex elsewhere. */
const char *cybeer_storage_runs_json(void);
const char *cybeer_storage_participants_json(void);

const char *cybeer_storage_tournaments_json(void);
const char *cybeer_storage_active_tournament_json(void);

/** Parse/copy helper: returns malloc'd array from tournaments.json — caller frees with cJSON_Delete. */
esp_err_t cybeer_storage_load_tournaments_cjson(cJSON **out_arr);
esp_err_t cybeer_storage_save_tournaments_cjson(cJSON *arr);

/** Same for active_tournament.json root object ({}) — caller frees. */
esp_err_t cybeer_storage_load_active_tournament_cjson(cJSON **out_obj);
esp_err_t cybeer_storage_save_active_tournament_cjson(cJSON *obj);
