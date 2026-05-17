#include "cybeer_tournament.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

static const char *TAG = "cybeer_tournament";

static unsigned ceil_pow2_u(unsigned x)
{
    if (x <= 1) {
        return 2u;
    }
    unsigned p = 2u;
    while (p < x) {
        p <<= 1u;
    }
    return p;
}

static void strcpyz(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
}

static cJSON *ensure_obj_string(cJSON *o, const char *key)
{
    cJSON *j = cJSON_GetObjectItemCaseSensitive(o, key);
    if (cJSON_IsString(j)) {
        if (!j->valuestring) {
            cJSON_SetValuestring(j, "");
        }
        return j;
    }
    if (j) {
        cJSON_DeleteItemFromObject(o, key);
    }
    cJSON_AddStringToObject(o, key, "");
    return cJSON_GetObjectItemCaseSensitive(o, key);
}

static bool str_nonempty(const char *s)
{
    return s != NULL && s[0] != '\0';
}

static cJSON *find_tournament_with_id(cJSON *tournaments_arr, const char *tid)
{
    if (!tournaments_arr || !tid || !tid[0]) {
        return NULL;
    }
    cJSON *t = NULL;
    cJSON_ArrayForEach(t, tournaments_arr)
    {
        if (!cJSON_IsObject(t)) {
            continue;
        }
        const cJSON *jid = cJSON_GetObjectItemCaseSensitive(t, "id");
        if (cJSON_IsString(jid) && jid->valuestring && strcmp(jid->valuestring, tid) == 0) {
            return t;
        }
    }
    return NULL;
}

static cJSON *find_match_in_tournament(cJSON *tournament, const char *match_id)
{
    if (!tournament || !match_id || !match_id[0]) {
        return NULL;
    }
    cJSON *matches = cJSON_GetObjectItemCaseSensitive(tournament, "matches");
    if (!matches || !cJSON_IsArray(matches)) {
        return NULL;
    }
    cJSON *m = NULL;
    cJSON_ArrayForEach(m, matches)
    {
        const cJSON *jid = cJSON_GetObjectItemCaseSensitive(m, "id");
        if (cJSON_IsString(jid) && jid->valuestring && strcmp(jid->valuestring, match_id) == 0) {
            return m;
        }
    }
    return NULL;
}

static esp_err_t load_pair(cJSON **tors, cJSON **active_out)
{
    ESP_RETURN_ON_ERROR(cybeer_storage_load_tournaments_cjson(tors), TAG, "load t");
    return cybeer_storage_load_active_tournament_cjson(active_out);
}

static esp_err_t save_both(cJSON *tors, cJSON *active)
{
    ESP_RETURN_ON_ERROR(cybeer_storage_save_tournaments_cjson(tors), TAG, "save tors");
    if (active != NULL) {
        ESP_RETURN_ON_ERROR(cybeer_storage_save_active_tournament_cjson(active), TAG, "save act");
    }
    return ESP_OK;
}

esp_err_t cybeer_tournament_generate_into(cJSON *tournament_root)
{
    ESP_RETURN_ON_FALSE(tournament_root && cJSON_IsObject(tournament_root), ESP_ERR_INVALID_ARG, TAG,
                        "tor");

    const cJSON *pids_arr = cJSON_GetObjectItemCaseSensitive(tournament_root, "participantIds");
    ESP_RETURN_ON_FALSE(pids_arr && cJSON_IsArray(pids_arr), ESP_ERR_INVALID_ARG, TAG,
                        "participantIds array");

    unsigned n = (unsigned)cJSON_GetArraySize(pids_arr);
    ESP_RETURN_ON_FALSE(n >= 1, ESP_ERR_INVALID_ARG, TAG, "need participants");

    unsigned cap = ceil_pow2_u(n);
    char(*padded)[37] = (char(*)[37])calloc(cap, sizeof(*padded));
    ESP_RETURN_ON_FALSE(padded, ESP_ERR_NO_MEM, TAG, "padded");

    for (unsigned i = 0; i < cap; i++) {
        padded[i][0] = '\0';
    }
    for (unsigned i = 0; i < n; i++) {
        const cJSON *pj = cJSON_GetArrayItem(pids_arr, (int)i);
        const char *s = (cJSON_IsString(pj) && pj->valuestring) ? pj->valuestring : "";
        strcpyz(padded[i], sizeof(padded[i]), s);
    }

    unsigned layers = 0u;
    for (unsigned tmp = cap; tmp > 1u; tmp >>= 1u) {
        layers++;
    }

    cJSON_DeleteItemFromObject(tournament_root, "matches");
    cJSON *matches_arr = cJSON_CreateArray();
    ESP_RETURN_ON_FALSE(matches_arr, ESP_ERR_NO_MEM, TAG, "arr");

    typedef struct {
        char id[37];
        unsigned level;
        unsigned index;
        char participantAId[37];
        char participantBId[37];
        char advanceToMatchId[37];
        char winnerGoesToSlot[4];
    } match_spec_t;

    unsigned num_levels = layers;
    size_t specs_cap = ((size_t)cap * layers) + 8u;
    match_spec_t *specs_buf = (match_spec_t *)calloc(specs_cap, sizeof(match_spec_t));
    ESP_GOTO_ON_FALSE(specs_buf != NULL, ESP_ERR_NO_MEM, fail_padded, TAG, "specs");

    match_spec_t **levels = (match_spec_t **)calloc(num_levels ? num_levels : 1u, sizeof(match_spec_t *));
    ESP_GOTO_ON_FALSE(levels != NULL, ESP_ERR_NO_MEM, fail_specs_buf, TAG, "levels");

    size_t alloc_off = 0;
    if (layers == 0) {
        cJSON_Delete(matches_arr);
        free(levels);
        free(specs_buf);
        free(padded);
        return ESP_ERR_INVALID_STATE;
    }
    for (unsigned level = 0; level < num_levels; level++) {
        levels[level] = specs_buf + alloc_off;
        unsigned mx = cap >> (level + 1u);
        alloc_off += mx;
        for (unsigned mi = 0; mi < mx; mi++) {
            match_spec_t *sp = levels[level] + mi;
            cybeer_format_uuid_v4(sp->id);
            sp->level = level;
            sp->index = mi;
            memset(sp->participantAId, 0, sizeof(sp->participantAId));
            memset(sp->participantBId, 0, sizeof(sp->participantBId));
            memset(sp->advanceToMatchId, 0, sizeof(sp->advanceToMatchId));
            strcpyz(sp->winnerGoesToSlot, sizeof(sp->winnerGoesToSlot), (mi % 2u == 0u) ? "A" : "B");

            if (level < num_levels - 1u) {
                unsigned pi = mi >> 1u;
                strcpyz(sp->advanceToMatchId, sizeof(sp->advanceToMatchId), levels[level + 1u][pi].id);
            }

            if (level == 0u) {
                unsigned li = mi * 2u;
                unsigned ri = mi * 2u + 1u;
                strcpyz(sp->participantAId, sizeof(sp->participantAId), padded[li]);
                strcpyz(sp->participantBId, sizeof(sp->participantBId), padded[ri]);
            }

            cJSON *m = cJSON_CreateObject();
            ESP_GOTO_ON_FALSE(m != NULL, ESP_ERR_NO_MEM, fail_levels, TAG, "match");
            cJSON_AddStringToObject(m, "id", sp->id);
            cJSON_AddNumberToObject(m, "column", (double)(num_levels - 1u - level));
            cJSON_AddNumberToObject(m, "index", (double)mi);
            cJSON_AddStringToObject(m, "participantAId", sp->participantAId);
            cJSON_AddStringToObject(m, "participantBId", sp->participantBId);
            cJSON_AddStringToObject(m, "runIdA", "");
            cJSON_AddStringToObject(m, "runIdB", "");
            cJSON_AddStringToObject(m, "winnerParticipantId", "");
            if (level < num_levels - 1u) {
                cJSON_AddStringToObject(m, "advanceToMatchId", sp->advanceToMatchId);
                cJSON_AddStringToObject(m, "winnerGoesToSlot", sp->winnerGoesToSlot);
            } else {
                cJSON_AddStringToObject(m, "advanceToMatchId", "");
                cJSON_AddNullToObject(m, "winnerGoesToSlot");
            }
            cJSON_AddItemToArray(matches_arr, m);
        }
    }

    cJSON_AddItemToObject(tournament_root, "matches", matches_arr);
    free(levels);
    free(specs_buf);
    free(padded);

    ESP_LOGI(TAG, "generated bracket levels=%u cap=%u participants=%u", layers, cap, n);
    return ESP_OK;

fail_levels:
    free(levels);
fail_specs_buf:
    free(specs_buf);
fail_padded:
    cJSON_Delete(matches_arr);
    free(padded);
    return ESP_FAIL;
}

esp_err_t cybeer_tournament_create_named(const char *name, const cJSON *participant_ids_array, char tournament_id_out[37])
{
    ESP_RETURN_ON_FALSE(name && participant_ids_array && cJSON_IsArray(participant_ids_array) && tournament_id_out,
                         ESP_ERR_INVALID_ARG, TAG, "args");
    ESP_RETURN_ON_FALSE(cJSON_GetArraySize(participant_ids_array) >= 1, ESP_ERR_INVALID_ARG, TAG,
                        "participants");

    cJSON *tors = NULL;
    esp_err_t err = cybeer_storage_load_tournaments_cjson(&tors);
    ESP_RETURN_ON_ERROR(err, TAG, "load tors");

    ESP_GOTO_ON_FALSE(tors != NULL && cJSON_IsArray(tors), ESP_FAIL, cleanup, TAG, "tors");

    cybeer_format_uuid_v4(tournament_id_out);

    cJSON *t = cJSON_CreateObject();
    ESP_GOTO_ON_FALSE(t != NULL, ESP_ERR_NO_MEM, cleanup, TAG, "t object");

    cJSON_AddStringToObject(t, "id", tournament_id_out);
    cJSON_AddStringToObject(t, "name", name);
    cJSON_AddStringToObject(t, "status", "draft");

    cJSON *pids_dup = cJSON_Duplicate((cJSON *)participant_ids_array, true);
    ESP_GOTO_ON_FALSE(pids_dup != NULL, ESP_ERR_NO_MEM, cleanup_t, TAG, "dup pids");
    cJSON_AddItemToObject(t, "participantIds", pids_dup);

    err = cybeer_tournament_generate_into(t);
    if (err != ESP_OK) {
        cJSON_Delete(t);
        cJSON_Delete(tors);
        return err;
    }

    cJSON_AddItemToArray(tors, t);
    err = cybeer_storage_save_tournaments_cjson(tors);

    ESP_LOGI(TAG, "created tournament id=%s", tournament_id_out);
    cJSON_Delete(tors);
    return err;

cleanup_t:
    cJSON_Delete(t);
cleanup:
    cJSON_Delete(tors);
    return ESP_FAIL;
}

static esp_err_t clear_active_binding(cJSON *active)
{
    cJSON_DeleteItemFromObject(active, "tournamentId");
    cJSON_AddStringToObject(active, "tournamentId", "");
    cJSON_DeleteItemFromObject(active, "pendingMatchId");
    cJSON_AddStringToObject(active, "pendingMatchId", "");
    cJSON_DeleteItemFromObject(active, "pendingSlot");
    cJSON_AddStringToObject(active, "pendingSlot", "");
    return ESP_OK;
}

static const char *tournament_active_id_locked(cJSON *active)
{
    const cJSON *j = cJSON_GetObjectItemCaseSensitive(active, "tournamentId");
    return (cJSON_IsString(j) && str_nonempty(j->valuestring)) ? j->valuestring : "";
}

esp_err_t cybeer_tournament_start_by_id(const char *tournament_id)
{
    ESP_RETURN_ON_FALSE(tournament_id && tournament_id[0], ESP_ERR_INVALID_ARG, TAG, "tid");

    cJSON *tors = NULL;
    cJSON *active = NULL;
    esp_err_t err = load_pair(&tors, &active);
    ESP_RETURN_ON_ERROR(err, TAG, "load");

    ESP_GOTO_ON_FALSE(tournament_active_id_locked(active)[0] == '\0', ESP_ERR_INVALID_STATE, cleanup, TAG,
                      "already active tournament");

    cJSON *t = find_tournament_with_id(tors, tournament_id);
    ESP_GOTO_ON_FALSE(t != NULL, ESP_ERR_NOT_FOUND, cleanup, TAG, "missing tor");

    const cJSON *jst = cJSON_GetObjectItemCaseSensitive(t, "status");
    const char *st = (cJSON_IsString(jst) && jst->valuestring) ? jst->valuestring : "";
    ESP_GOTO_ON_FALSE(strcmp(st, "draft") == 0, ESP_ERR_INVALID_STATE, cleanup, TAG, "not draft");

    cJSON_DeleteItemFromObject(t, "status");
    cJSON_AddStringToObject(t, "status", "active");

    cJSON_DeleteItemFromObject(active, "tournamentId");
    cJSON_AddStringToObject(active, "tournamentId", tournament_id);
    cJSON_DeleteItemFromObject(active, "pendingMatchId");
    cJSON_AddStringToObject(active, "pendingMatchId", "");
    cJSON_DeleteItemFromObject(active, "pendingSlot");
    cJSON_AddStringToObject(active, "pendingSlot", "");

    err = save_both(tors, active);

    cJSON_Delete(tors);
    cJSON_Delete(active);

    if (err == ESP_OK) {
        (void)cybeer_tournament_notify_run_claimed("");
        ESP_LOGI(TAG, "started tournament %s", tournament_id);
    }

    return err;

cleanup:
    cJSON_Delete(tors);
    cJSON_Delete(active);
    return err;
}

esp_err_t cybeer_tournament_assign_next_bind(const char *match_id, const char *slot_a_or_b)
{
    ESP_RETURN_ON_FALSE(match_id && match_id[0] && slot_a_or_b, ESP_ERR_INVALID_ARG, TAG, "bind args");
    ESP_RETURN_ON_FALSE(strcmp(slot_a_or_b, "A") == 0 || strcmp(slot_a_or_b, "B") == 0, ESP_ERR_INVALID_ARG,
                        TAG, "slot");

    cJSON *tors = NULL;
    cJSON *active = NULL;
    esp_err_t err = load_pair(&tors, &active);
    ESP_RETURN_ON_ERROR(err, TAG, "load pair");

    const char *tid = tournament_active_id_locked(active);
    ESP_GOTO_ON_FALSE(tid[0], ESP_ERR_INVALID_STATE, cleanup, TAG, "no active");

    cJSON *t = find_tournament_with_id(tors, tid);
    ESP_GOTO_ON_FALSE(t != NULL, ESP_ERR_INVALID_STATE, cleanup, TAG, "lost tor");

    cJSON *m = find_match_in_tournament(t, match_id);
    ESP_GOTO_ON_FALSE(m != NULL, ESP_ERR_NOT_FOUND, cleanup, TAG, "match");

    ensure_obj_string(m, "runIdA");
    ensure_obj_string(m, "runIdB");

    const cJSON *rsa = cJSON_GetObjectItemCaseSensitive(m, "runIdA");
    const cJSON *rsb = cJSON_GetObjectItemCaseSensitive(m, "runIdB");

    if (slot_a_or_b[0] == 'A') {
        ESP_GOTO_ON_FALSE(!(cJSON_IsString(rsa) && str_nonempty(rsa->valuestring)), ESP_ERR_INVALID_STATE,
                            cleanup, TAG, "slot A taken");
        const cJSON *pa = cJSON_GetObjectItemCaseSensitive(m, "participantAId");
        ESP_GOTO_ON_FALSE(cJSON_IsString(pa) && str_nonempty(pa->valuestring), ESP_ERR_INVALID_STATE,
                            cleanup, TAG, "no seed A");
    } else {
        ESP_GOTO_ON_FALSE(!(cJSON_IsString(rsb) && str_nonempty(rsb->valuestring)), ESP_ERR_INVALID_STATE,
                            cleanup, TAG, "slot B taken");
        const cJSON *pb = cJSON_GetObjectItemCaseSensitive(m, "participantBId");
        ESP_GOTO_ON_FALSE(cJSON_IsString(pb) && str_nonempty(pb->valuestring), ESP_ERR_INVALID_STATE,
                            cleanup, TAG, "no seed B");
    }

    cJSON_DeleteItemFromObject(active, "pendingMatchId");
    cJSON_AddStringToObject(active, "pendingMatchId", match_id);
    cJSON_DeleteItemFromObject(active, "pendingSlot");
    cJSON_AddStringToObject(active, "pendingSlot", slot_a_or_b);

    err = save_both(tors, active);

    ESP_LOGI(TAG, "assign pending match=%s slot=%s", match_id, slot_a_or_b);

cleanup:
    cJSON_Delete(tors);
    cJSON_Delete(active);
    return err;
}

static int column_of_match(const cJSON *m)
{
    const cJSON *jc = cJSON_GetObjectItemCaseSensitive(m, "column");
    if (cJSON_IsNumber(jc)) {
        return (int)jc->valuedouble;
    }
    return 0;
}

static esp_err_t try_finalize_match(cJSON *tors, cJSON *tournament, cJSON *m, cJSON *active);
static void sweep_tournament(cJSON *tors, cJSON *tournament, cJSON *active);

static esp_err_t set_winner_line(cJSON *child_match, const char *winner_pid)
{
    cJSON_DeleteItemFromObject(child_match, "winnerParticipantId");
    cJSON_AddStringToObject(child_match, "winnerParticipantId", winner_pid ? winner_pid : "");
    return ESP_OK;
}

static esp_err_t mark_champion_and_clear_active(cJSON *tors, cJSON *active, cJSON *tournament,
                                                const char *winner_pid)
{
    ESP_RETURN_ON_FALSE(active != NULL, ESP_ERR_INVALID_STATE, TAG, "active");

    cJSON_DeleteItemFromObject(tournament, "status");
    cJSON_AddStringToObject(tournament, "status", "complete");
    cJSON_DeleteItemFromObject(tournament, "championParticipantId");
    cJSON_AddStringToObject(tournament, "championParticipantId", winner_pid ? winner_pid : "");

    clear_active_binding(active);

    ESP_LOGI(TAG, "tournament complete champion=%s", winner_pid ? winner_pid : "");
    return save_both(tors, active);
}

static esp_err_t push_winner_to_parent(cJSON *tors, cJSON *tournament, cJSON *child_match,
                                      const char *winner_pid, cJSON *active)
{
    const cJSON *padv = cJSON_GetObjectItemCaseSensitive(child_match, "advanceToMatchId");
    if (!cJSON_IsString(padv) || !padv->valuestring || !padv->valuestring[0]) {
        return mark_champion_and_clear_active(tors, active, tournament, winner_pid);
    }

    const cJSON *pslot = cJSON_GetObjectItemCaseSensitive(child_match, "winnerGoesToSlot");
    const char *slot = (cJSON_IsString(pslot) && pslot->valuestring) ? pslot->valuestring : "A";

    cJSON *parent = find_match_in_tournament(tournament, padv->valuestring);
    ESP_RETURN_ON_FALSE(parent != NULL, ESP_ERR_NOT_FOUND, TAG, "parent");

    const char *k = (strcmp(slot, "B") == 0) ? "participantBId" : "participantAId";
    cJSON_DeleteItemFromObject(parent, k);
    cJSON_AddStringToObject(parent, k, winner_pid ? winner_pid : "");

    sweep_tournament(tors, tournament, active);
    return ESP_OK;
}

static esp_err_t try_finalize_match(cJSON *tors, cJSON *tournament, cJSON *m, cJSON *active)
{
    const cJSON *jw = cJSON_GetObjectItemCaseSensitive(m, "winnerParticipantId");
    if (cJSON_IsString(jw) && str_nonempty(jw->valuestring)) {
        return ESP_ERR_INVALID_STATE;
    }

    ensure_obj_string(m, "participantAId");
    ensure_obj_string(m, "participantBId");
    const cJSON *pa = cJSON_GetObjectItemCaseSensitive(m, "participantAId");
    const cJSON *pb = cJSON_GetObjectItemCaseSensitive(m, "participantBId");
    bool ha = cJSON_IsString(pa) && str_nonempty(pa->valuestring);
    bool hb = cJSON_IsString(pb) && str_nonempty(pb->valuestring);

    ensure_obj_string(m, "runIdA");
    ensure_obj_string(m, "runIdB");
    const cJSON *rsa = cJSON_GetObjectItemCaseSensitive(m, "runIdA");
    const cJSON *rsb = cJSON_GetObjectItemCaseSensitive(m, "runIdB");
    bool rha = cJSON_IsString(rsa) && str_nonempty(rsa->valuestring);
    bool rhb = cJSON_IsString(rsb) && str_nonempty(rsb->valuestring);

    if (ha && !hb && !rha && !rhb) {
        ESP_RETURN_ON_ERROR(set_winner_line(m, pa->valuestring), TAG, "set w");
        return push_winner_to_parent(tors, tournament, m, pa->valuestring, active);
    }
    if (!ha && hb && !rha && !rhb) {
        ESP_RETURN_ON_ERROR(set_winner_line(m, pb->valuestring), TAG, "set w");
        return push_winner_to_parent(tors, tournament, m, pb->valuestring, active);
    }
    if (!(ha && hb)) {
        return ESP_FAIL;
    }
    ESP_RETURN_ON_FALSE(rha && rhb, ESP_FAIL, TAG, "wait runs");

    cybeer_run_t ra = { 0 };
    cybeer_run_t rb = { 0 };
    ESP_RETURN_ON_FALSE(rsa && rsa->valuestring && rsb && rsb->valuestring, ESP_FAIL, TAG, "runs str");
    ESP_RETURN_ON_ERROR(cybeer_storage_get_run(rsa->valuestring, &ra), TAG, "run a");
    ESP_RETURN_ON_ERROR(cybeer_storage_get_run(rsb->valuestring, &rb), TAG, "run b");

    ESP_RETURN_ON_FALSE(ra.claimed && rb.claimed, ESP_FAIL, TAG, "wait claimed");

    cybeer_run_t *wr = NULL;
    if (ra.duration_us < rb.duration_us) {
        wr = &ra;
    } else if (ra.duration_us > rb.duration_us) {
        wr = &rb;
    } else {
        wr = (strcmp(ra.id, rb.id) <= 0) ? &ra : &rb;
    }
    const char *chosen_pid = (wr->participant_id[0] != '\0') ? wr->participant_id : "";

    ESP_RETURN_ON_ERROR(set_winner_line(m, chosen_pid), TAG, "set w");
    return push_winner_to_parent(tors, tournament, m, chosen_pid, active);
}

static void sweep_tournament(cJSON *tors, cJSON *tournament, cJSON *active)
{
    int guard = 0;
    bool changed = false;
    do {
        changed = false;
        cJSON *matches = cJSON_GetObjectItemCaseSensitive(tournament, "matches");
        if (!matches || !cJSON_IsArray(matches)) {
            return;
        }

        int max_col = -1;
        cJSON *mm = NULL;
        cJSON_ArrayForEach(mm, matches)
        {
            int cc = column_of_match(mm);
            if (cc > max_col) {
                max_col = cc;
            }
        }
        if (max_col < 0) {
            max_col = 0;
        }

        for (int col = max_col; col >= 0; col--) {
            cJSON *mx = NULL;
            cJSON_ArrayForEach(mx, matches)
            {
                if (column_of_match(mx) != col) {
                    continue;
                }
                if (ESP_OK == try_finalize_match(tors, tournament, mx, active)) {
                    changed = true;
                }
            }
        }
        guard++;
    } while (changed && guard < 96);
}

esp_err_t cybeer_tournament_generate(const char *const *participant_ids, size_t n, char tournament_id_out[37])
{
    ESP_RETURN_ON_FALSE(participant_ids && n > 0 && tournament_id_out, ESP_ERR_INVALID_ARG, TAG, "args");

    cJSON *arr = cJSON_CreateArray();
    ESP_RETURN_ON_FALSE(arr, ESP_ERR_NO_MEM, TAG, "arr");
    for (size_t i = 0; i < n; i++) {
        const char *p = participant_ids[i];
        if (!p) {
            cJSON_Delete(arr);
            return ESP_ERR_INVALID_ARG;
        }
        cJSON *s = cJSON_CreateString(p);
        if (!s) {
            cJSON_Delete(arr);
            return ESP_ERR_NO_MEM;
        }
        cJSON_AddItemToArray(arr, s);
    }
    esp_err_t e = cybeer_tournament_create_named("Tournament", arr, tournament_id_out);
    cJSON_Delete(arr);
    return e;
}

esp_err_t cybeer_tournament_notify_run_saved(const cybeer_run_t *run)
{
    ESP_RETURN_ON_FALSE(run && run->id[0], ESP_ERR_INVALID_ARG, TAG, "run");

    cJSON *tors = NULL;
    cJSON *active = NULL;
    esp_err_t err = load_pair(&tors, &active);
    ESP_RETURN_ON_ERROR(err, TAG, "load");

    const char *tid = tournament_active_id_locked(active);
    if (!tid[0]) {
        cJSON_Delete(tors);
        cJSON_Delete(active);
        return ESP_OK;
    }

    cJSON *t = find_tournament_with_id(tors, tid);
    if (!t) {
        cJSON_Delete(tors);
        cJSON_Delete(active);
        return ESP_OK;
    }

    const cJSON *jpm = cJSON_GetObjectItemCaseSensitive(active, "pendingMatchId");
    const cJSON *jps = cJSON_GetObjectItemCaseSensitive(active, "pendingSlot");
    const char *pm = (cJSON_IsString(jpm) && jpm->valuestring) ? jpm->valuestring : "";
    const char *pslot = (cJSON_IsString(jps) && jps->valuestring) ? jps->valuestring : "";

    if (str_nonempty(pm) && str_nonempty(pslot)) {
        cJSON *m = find_match_in_tournament(t, pm);
        if (!m) {
            ESP_LOGW(TAG, "pending match unknown %s", pm);
        } else {
            ensure_obj_string(m, "runIdA");
            ensure_obj_string(m, "runIdB");
            cJSON *jra = cJSON_GetObjectItemCaseSensitive(m, "runIdA");
            cJSON *jrb = cJSON_GetObjectItemCaseSensitive(m, "runIdB");
            if (!jra || !jrb) {
                err = ESP_ERR_INVALID_STATE;
                goto err_cleanup;
            }

            if (pslot[0] == 'A') {
                if (cJSON_IsString(jra) && str_nonempty(jra->valuestring)) {
                    err = ESP_ERR_INVALID_STATE;
                    goto err_cleanup;
                }
                cJSON_DeleteItemFromObject(m, "runIdA");
                cJSON_AddStringToObject(m, "runIdA", run->id);
            } else {
                if (cJSON_IsString(jrb) && str_nonempty(jrb->valuestring)) {
                    err = ESP_ERR_INVALID_STATE;
                    goto err_cleanup;
                }
                cJSON_DeleteItemFromObject(m, "runIdB");
                cJSON_AddStringToObject(m, "runIdB", run->id);
            }

            cybeer_run_t patched = *run;
            strcpyz(patched.tournament_match_id, sizeof(patched.tournament_match_id), pm);
            (void)cybeer_storage_update_run(run->id, &patched);

            cJSON_DeleteItemFromObject(active, "pendingMatchId");
            cJSON_AddStringToObject(active, "pendingMatchId", "");
            cJSON_DeleteItemFromObject(active, "pendingSlot");
            cJSON_AddStringToObject(active, "pendingSlot", "");
        }
    }

    sweep_tournament(tors, t, active);
    err = save_both(tors, active);

err_cleanup:
    cJSON_Delete(tors);
    cJSON_Delete(active);
    return err;
}

esp_err_t cybeer_tournament_notify_run_claimed(const char *run_id)
{
    (void)run_id;

    cJSON *tors = NULL;
    cJSON *active = NULL;
    esp_err_t err = load_pair(&tors, &active);
    ESP_RETURN_ON_ERROR(err, TAG, "load");

    const char *tid = tournament_active_id_locked(active);
    if (!tid[0]) {
        cJSON_Delete(tors);
        cJSON_Delete(active);
        return ESP_OK;
    }

    cJSON *t = find_tournament_with_id(tors, tid);
    if (!t) {
        cJSON_Delete(tors);
        cJSON_Delete(active);
        return ESP_OK;
    }

    sweep_tournament(tors, t, active);
    err = save_both(tors, active);
    cJSON_Delete(tors);
    cJSON_Delete(active);
    return err;
}

esp_err_t cybeer_tournament_get_active_envelope_json(cJSON **out_root)
{
    ESP_RETURN_ON_FALSE(out_root, ESP_ERR_INVALID_ARG, TAG, "out");
    *out_root = NULL;

    cJSON *tors = NULL;
    cJSON *active = NULL;
    esp_err_t err = load_pair(&tors, &active);
    ESP_RETURN_ON_ERROR(err, TAG, "load");

    const char *tid = tournament_active_id_locked(active);
    if (!tid[0]) {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddNullToObject(root, "tournament");
        cJSON_AddNullToObject(root, "pendingNextRun");
        *out_root = root;
        cJSON_Delete(tors);
        cJSON_Delete(active);
        return ESP_OK;
    }

    cJSON *t = find_tournament_with_id(tors, tid);
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        cJSON_Delete(tors);
        cJSON_Delete(active);
        return ESP_ERR_NO_MEM;
    }

    if (t) {
        cJSON_AddItemToObject(root, "tournament", cJSON_Duplicate(t, true));
    } else {
        cJSON_AddNullToObject(root, "tournament");
    }

    const cJSON *jpm = cJSON_GetObjectItemCaseSensitive(active, "pendingMatchId");
    const cJSON *jps = cJSON_GetObjectItemCaseSensitive(active, "pendingSlot");
    const char *pm = (cJSON_IsString(jpm) && jpm->valuestring) ? jpm->valuestring : "";
    const char *ps = (cJSON_IsString(jps) && jps->valuestring) ? jps->valuestring : "";
    if (str_nonempty(pm) && str_nonempty(ps)) {
        cJSON *pend = cJSON_CreateObject();
        cJSON_AddStringToObject(pend, "matchId", pm);
        cJSON_AddStringToObject(pend, "slot", ps);
        cJSON_AddItemToObject(root, "pendingNextRun", pend);
    } else {
        cJSON_AddNullToObject(root, "pendingNextRun");
    }

    *out_root = root;

    cJSON_Delete(tors);
    cJSON_Delete(active);
    return ESP_OK;
}

void cybeer_tournament_fill_status_active_match(cJSON *status_root)
{
    if (!status_root) {
        return;
    }

    cJSON *env = NULL;
    if (cybeer_tournament_get_active_envelope_json(&env) != ESP_OK || !env) {
        cJSON_DeleteItemFromObject(status_root, "activeMatch");
        cJSON_AddNullToObject(status_root, "activeMatch");
        return;
    }

    const cJSON *tor = cJSON_GetObjectItemCaseSensitive(env, "tournament");
    const cJSON *pend = cJSON_GetObjectItemCaseSensitive(env, "pendingNextRun");
    const cJSON *jpm =
        pend && cJSON_IsObject((cJSON *)pend) ? cJSON_GetObjectItemCaseSensitive(pend, "matchId") : NULL;
    const cJSON *jps =
        pend && cJSON_IsObject((cJSON *)pend) ? cJSON_GetObjectItemCaseSensitive(pend, "slot") : NULL;

    char match_id_focus[37] = { 0 };

    /* Prefer pending-bind target match for display */
    if (cJSON_IsString(jpm) && jpm->valuestring && jpm->valuestring[0]) {
        strcpyz(match_id_focus, sizeof(match_id_focus), jpm->valuestring);
    } else if (cJSON_IsObject((cJSON *)tor)) {
        cJSON *matches = cJSON_GetObjectItemCaseSensitive((cJSON *)tor, "matches");
        if (!matches || !cJSON_IsArray(matches)) {
            matches = NULL;
        }
        if (matches) {
            int pick_col = 1000000;
            cJSON *m = NULL;
            cJSON_ArrayForEach(m, matches)
            {
                const cJSON *jw = cJSON_GetObjectItemCaseSensitive(m, "winnerParticipantId");
                if (cJSON_IsString(jw) && str_nonempty(jw->valuestring)) {
                    continue;
                }
                const cJSON *pa = cJSON_GetObjectItemCaseSensitive(m, "participantAId");
                const cJSON *pb = cJSON_GetObjectItemCaseSensitive(m, "participantBId");
                bool ha = cJSON_IsString(pa) && str_nonempty(pa->valuestring);
                bool hb = cJSON_IsString(pb) && str_nonempty(pb->valuestring);
                if (!ha || !hb) {
                    continue;
                }
                int col = column_of_match(m);
                if (col < pick_col) {
                    pick_col = col;
                    const cJSON *jid = cJSON_GetObjectItemCaseSensitive(m, "id");
                    if (cJSON_IsString(jid) && jid->valuestring) {
                        strcpyz(match_id_focus, sizeof(match_id_focus), jid->valuestring);
                    }
                }
            }
        }
    }

    cJSON *am = cJSON_CreateObject();
    if (!am) {
        cJSON_Delete(env);
        return;
    }

    if (cJSON_IsObject((cJSON *)tor)) {
        const cJSON *jid = cJSON_GetObjectItemCaseSensitive(tor, "id");
        const cJSON *jnm = cJSON_GetObjectItemCaseSensitive(tor, "name");
        cJSON_AddStringToObject(am, "tournamentId", (jid && jid->valuestring) ? jid->valuestring : "");
        cJSON_AddStringToObject(am, "name", (jnm && jnm->valuestring) ? jnm->valuestring : "");
    } else {
        cJSON_AddStringToObject(am, "tournamentId", "");
        cJSON_AddStringToObject(am, "name", "");
    }

    if (str_nonempty(match_id_focus) && cJSON_IsObject((cJSON *)tor)) {
        cJSON *mj = find_match_in_tournament((cJSON *)tor, match_id_focus);
        if (mj) {
            cJSON_AddItemToObject(am, "match", cJSON_Duplicate(mj, true));
        }
    }

    if (cJSON_IsObject((cJSON *)pend)) {
        cJSON_AddItemToObject(am, "pendingNextRun", cJSON_Duplicate((cJSON *)pend, true));
    } else {
        cJSON_AddNullToObject(am, "pendingNextRun");
    }

    cJSON_DeleteItemFromObject(status_root, "activeMatch");
    cJSON_AddItemToObject(status_root, "activeMatch", am);
    cJSON_Delete(env);
}
