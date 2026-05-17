#pragma once

#include "cJSON.h"
#include "cybeer_storage.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Populate `matches` on `tournament` (object with participantIds[]) using single elimination.
 * Participant list is padded to a power of two with empty opponents (bye).
 */
esp_err_t cybeer_tournament_generate_into(cJSON *tournament_root);

/** Creates a draft bracket from C string ids (`tournament_id_out` receives the new UUID). */
esp_err_t cybeer_tournament_generate(const char *const *participant_ids, size_t n, char tournament_id_out[37]);

esp_err_t cybeer_tournament_create_named(const char *name, const cJSON *participant_ids_array, char tournament_id_out[37]);

esp_err_t cybeer_tournament_start_by_id(const char *tournament_id);

esp_err_t cybeer_tournament_assign_next_bind(const char *match_id, const char *slot_a_or_b);

esp_err_t cybeer_tournament_notify_run_saved(const cybeer_run_t *run);
esp_err_t cybeer_tournament_notify_run_claimed(const char *run_id);

esp_err_t cybeer_tournament_get_active_envelope_json(cJSON **out_root);

/** Replaces `"activeMatch": null` semantics by adding/updating activeMatch on status_root. */
void cybeer_tournament_fill_status_active_match(cJSON *status_root);

#ifdef __cplusplus
}
#endif
