# WebUI UX Improvements — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix participant name display bug, add admin power control buttons (reboot/eco/sleep), and add participant self-rename from player page.

**Architecture:** ESP32-C3 firmware (ESP-IDF, esp_http_server) serves a vanilla HTML/JS/CSS frontend from LittleFS. New features add HTTP handlers in `cybeer_web.c`, storage logic in `cybeer_storage.c`, power control in `cybeer_power.c`, and frontend UI in HTML/JS files.

**Tech Stack:** ESP-IDF (C), esp_http_server, cJSON, LittleFS, vanilla HTML/CSS/JS

---

## File Structure

| File | Responsibility |
|------|---------------|
| `firmware/components/cybeer_web/cybeer_web.c` | HTTP handlers for all REST API endpoints |
| `firmware/components/cybeer_storage/cybeer_storage.c` | JSON persistence for participants/runs |
| `firmware/components/cybeer_storage/include/cybeer_storage.h` | Public storage API declarations |
| `firmware/components/cybeer_power/cybeer_power.c` | Power management (sleep, eco mode) |
| `firmware/components/cybeer_power/include/cybeer_power.h` | Public power API declarations |
| `frontend/admin.html` | Admin panel page structure |
| `frontend/admin.js` | Admin panel logic and API calls |
| `frontend/player.html` | Player profile page with rename UI |

---

### Task 1: Debug and fix participant name display bug

**Files:**
- Modify: `firmware/components/cybeer_web/cybeer_web.c` (h_post_claim handler, ~line 284-294)
- Modify: `firmware/components/cybeer_storage/cybeer_storage.c` (claim_run function, ~line 555-614)

The screenshots show that after claiming a run, the participant name appears as a UUID prefix ("58961787…"). The leaderboard API at `/api/leaderboard` joins names from `participants.json`, but returns empty `participantName` if the participant isn't found. The frontend fallback `participantName()` then shows `pid.slice(0, 8) + "…"`.

Root cause hypothesis: The `h_post_claim` handler sends the HTTP response and broadcasts `leaderboardUpdate` **after** `cybeer_storage_claim_run` returns ESP_OK, so persistence should be complete. However, `cybeer_storage_participants_json()` uses a static buffer — if it's called concurrently or before the file is re-read, stale data may be returned.

- [ ] **Step 1: Add logging to confirm name reaches storage**

In `firmware/components/cybeer_storage/cybeer_storage.c`, add a log statement in `create_participant_by_name_locked` after the participant is created:

```c
// In create_participant_by_name_locked, after cJSON_AddStringToObject(p, "name", name):
ESP_LOGI(TAG, "created participant name='%s' id='%s'", name, pid_out);
```

- [ ] **Step 2: Inspect `cybeer_storage_participants_json()` for stale-cache bug**

Find the function `cybeer_storage_participants_json()` — it likely reads the file into a static buffer. Check if `persist_json_locked` invalidates this cache. If `participants_json()` returns a cached version that was read before the new participant was written, the leaderboard will not find the name.

Look for a pattern like:
```c
static char s_participants_buf[...];
const char *cybeer_storage_participants_json(void) {
    // If this reads from file every time: OK
    // If this returns s_participants_buf without re-reading: BUG
}
```

- [ ] **Step 3: Fix the stale-cache issue**

If `cybeer_storage_participants_json()` caches without invalidation, add invalidation after persist. The fix depends on the implementation found in Step 2. Two possible fixes:

**Option A** — if it caches a static buffer, invalidate after write:
```c
static bool s_participants_dirty = true;

// In persist_json_locked (or after successful write to PATH_PARTICIPANTS):
s_participants_dirty = true;

// In cybeer_storage_participants_json:
if (s_participants_dirty) {
    // re-read from file
    s_participants_dirty = false;
}
```

**Option B** — if it always reads from file but there's a timing issue, ensure the leaderboard handler reads inside the mutex or after the write is complete (it currently reads outside the mutex via `cybeer_storage_participants_json()`).

- [ ] **Step 4: Verify the fix by building firmware**

```bash
cd firmware
idf.py build
```

Expected: Build succeeds with no errors.

- [ ] **Step 5: Commit**

```bash
git add firmware/components/cybeer_storage/cybeer_storage.c
git commit -m "fix: participant name cache invalidation after claim"
```

---

### Task 2: Fix player page stats and runs endpoints

**Files:**
- Modify: `firmware/components/cybeer_web/cybeer_web.c` (h_get_participant_stats, h_get_participant_runs)

The player page screenshot shows "Ошибка загрузки статистики" and "Ошибка" in run history. The handlers at `/api/participants/*/stats` and `/api/participants/*/runs` use wildcard URI matching. The `h_get_participant_stats` handler returns `{"error":"storage"}` with 500 if `cybeer_storage_get_participant_stats` fails. This function fails if `parse_array_file_locked(PATH_RUNS)` returns NULL — which happens if `runs.json` is empty or malformed.

- [ ] **Step 1: Fix stats handler to return zeros instead of 500 when no runs exist**

In `firmware/components/cybeer_web/cybeer_web.c`, modify `h_get_participant_stats` to handle the case where storage returns an error gracefully (the participant may simply have no claimed runs yet):

```c
static esp_err_t h_get_participant_stats(httpd_req_t *req)
{
    char path[160];
    copy_path_no_query(req->uri, path, sizeof(path));
    char pid[40];
    if (!parse_participant_stats_path(path, pid)) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"error\":\"uri\"}", HTTPD_RESP_USE_STRLEN);
    }

    cybeer_stats_t st = { 0 };
    esp_err_t err = cybeer_storage_get_participant_stats(pid, &st);
    if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "{\"error\":\"storage\"}", HTTPD_RESP_USE_STRLEN);
    }

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
```

- [ ] **Step 2: Fix `cybeer_storage_get_participant_stats` to return OK with zeros when no runs match**

In `firmware/components/cybeer_storage/cybeer_storage.c`, the function currently returns `ESP_FAIL` if `parse_array_file_locked(PATH_RUNS)` returns NULL. Fix it to return ESP_OK with zeros if the file simply doesn't exist yet:

```c
esp_err_t cybeer_storage_get_participant_stats(const char *pid, cybeer_stats_t *out)
{
    ESP_RETURN_ON_FALSE(pid && out, ESP_ERR_INVALID_ARG, TAG, "args");
    memset(out, 0, sizeof(*out));

    ESP_RETURN_ON_ERROR(take_mtx(), TAG, "take");

    cJSON *runs = parse_array_file_locked(PATH_RUNS);
    if (!runs) {
        give_mtx();
        return ESP_OK;  // No runs file yet — return zeros
    }

    // ... rest of existing code unchanged ...
}
```

- [ ] **Step 3: Build and verify**

```bash
cd firmware
idf.py build
```

Expected: Build succeeds.

- [ ] **Step 4: Commit**

```bash
git add firmware/components/cybeer_web/cybeer_web.c firmware/components/cybeer_storage/cybeer_storage.c
git commit -m "fix: player stats/runs return zeros instead of 500 when no data"
```

---

### Task 3: Add eco mode and sleep-trigger to cybeer_power

**Files:**
- Modify: `firmware/components/cybeer_power/cybeer_power.c`
- Modify: `firmware/components/cybeer_power/include/cybeer_power.h`

- [ ] **Step 1: Add eco mode API declarations to header**

In `firmware/components/cybeer_power/include/cybeer_power.h`, add after the existing declarations:

```c
/** Toggle eco mode: blanks display + LEDs, Wi-Fi stays active. Returns new state. */
bool cybeer_power_toggle_eco(void);

/** Get current eco mode state. */
bool cybeer_power_is_eco(void);

/** Trigger immediate light sleep (call from HTTP handler after sending response). */
void cybeer_power_trigger_sleep(void);
```

- [ ] **Step 2: Implement eco mode and trigger_sleep in cybeer_power.c**

In `firmware/components/cybeer_power/cybeer_power.c`, add a static flag and implement the functions. Add before `cybeer_power_note_activity`:

```c
static bool s_eco_mode = false;

bool cybeer_power_is_eco(void)
{
    return s_eco_mode;
}

bool cybeer_power_toggle_eco(void)
{
    s_eco_mode = !s_eco_mode;
    if (s_eco_mode) {
        ESP_LOGI(TAG, "eco mode ON — blanking display and LEDs");
        cybeer_display_blank();
        cybeer_led_prepare_sleep();
    } else {
        ESP_LOGI(TAG, "eco mode OFF — restoring display");
        cybeer_display_show_zeros();
        cybeer_led_set_fx(CYBEER_LED_FX_IDLE);
    }
    return s_eco_mode;
}

void cybeer_power_trigger_sleep(void)
{
    ESP_LOGI(TAG, "sleep triggered via HTTP — entering in 500ms");
    vTaskDelay(pdMS_TO_TICKS(500));
    enter_idle_sleep_until_unlocked();
}
```

- [ ] **Step 3: Check eco flag in FSM display/LED calls**

The main FSM loop (in `app_main.c`) calls display and LED functions. We need to suppress those when eco mode is active. Add a check at the top of the relevant calls, OR export `cybeer_power_is_eco()` and check it wherever display/LED updates happen.

Find in `firmware/main/app_main.c` where `cybeer_display_*` and `cybeer_led_set_fx` are called and wrap with:
```c
if (!cybeer_power_is_eco()) {
    cybeer_display_show_time_us(elapsed);
    // ... LED updates ...
}
```

- [ ] **Step 4: Exit eco mode on physical button press**

In the main FSM loop (app_main.c), when a button press is detected and eco mode is active, exit eco first:
```c
if (cybeer_power_is_eco()) {
    cybeer_power_toggle_eco();  // turns eco OFF, restores display
    cybeer_power_note_activity();
    continue;  // consume the press without starting a run
}
```

- [ ] **Step 5: Add `cybeer_led.h` include if needed for `CYBEER_LED_FX_IDLE`**

Verify that `cybeer_power.c` already includes `cybeer_led.h` (it does — line 5 of existing file shows `#include "cybeer_led.h"`). Also verify that `CYBEER_LED_FX_IDLE` is defined. If not, use whichever idle/standby effect constant exists.

- [ ] **Step 6: Build and verify**

```bash
cd firmware
idf.py build
```

Expected: Build succeeds.

- [ ] **Step 7: Commit**

```bash
git add firmware/components/cybeer_power/cybeer_power.c firmware/components/cybeer_power/include/cybeer_power.h firmware/main/app_main.c
git commit -m "feat: add eco mode toggle and HTTP-triggered sleep to power management"
```

---

### Task 4: Add power control HTTP endpoints (reboot, eco, sleep)

**Files:**
- Modify: `firmware/components/cybeer_web/cybeer_web.c`

- [ ] **Step 1: Add reboot handler**

In `firmware/components/cybeer_web/cybeer_web.c`, add the handler function (place it near other admin handlers, e.g., after `h_delete_admin_data_reset`):

```c
static esp_err_t h_post_admin_reboot(httpd_req_t *req)
{
    esp_err_t g = require_admin_pin(req);
    if (g != ESP_OK) {
        return g;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return ESP_OK;
}
```

- [ ] **Step 2: Add eco mode handler**

```c
static esp_err_t h_post_admin_power_eco(httpd_req_t *req)
{
    esp_err_t g = require_admin_pin(req);
    if (g != ESP_OK) {
        return g;
    }
    bool new_state = cybeer_power_toggle_eco();
    httpd_resp_set_type(req, "application/json");
    char resp[32];
    snprintf(resp, sizeof(resp), "{\"eco\":%s}", new_state ? "true" : "false");
    return httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
}
```

- [ ] **Step 3: Add sleep handler**

```c
static esp_err_t h_post_admin_power_sleep(httpd_req_t *req)
{
    esp_err_t g = require_admin_pin(req);
    if (g != ESP_OK) {
        return g;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true,\"msg\":\"sleeping\"}", HTTPD_RESP_USE_STRLEN);
    cybeer_power_trigger_sleep();
    return ESP_OK;
}
```

- [ ] **Step 4: Add `powerMode` field to status endpoint**

In `h_get_status` (around line 104), add:
```c
cJSON_AddStringToObject(root, "powerMode", cybeer_power_is_eco() ? "eco" : "normal");
```

- [ ] **Step 5: Register the new URI handlers**

In the `cybeer_web_start` function, add URI definitions and registrations. Add the definitions near the existing admin URI defs:

```c
httpd_uri_t u_admin_reboot = {
    .uri = "/api/admin/reboot", .method = HTTP_POST, .handler = h_post_admin_reboot, .user_ctx = NULL
};
httpd_uri_t u_admin_power_eco = {
    .uri = "/api/admin/power/eco", .method = HTTP_POST, .handler = h_post_admin_power_eco, .user_ctx = NULL
};
httpd_uri_t u_admin_power_sleep = {
    .uri = "/api/admin/power/sleep", .method = HTTP_POST, .handler = h_post_admin_power_sleep, .user_ctx = NULL
};
```

Add registrations to the chain (before the final `|| httpd_register_uri_handler(s_server, &u_static) != ESP_OK`):
```c
|| httpd_register_uri_handler(s_server, &u_admin_reboot) != ESP_OK
|| httpd_register_uri_handler(s_server, &u_admin_power_eco) != ESP_OK
|| httpd_register_uri_handler(s_server, &u_admin_power_sleep) != ESP_OK
```

- [ ] **Step 6: Add `#include "cybeer_power.h"` if not already present**

Check the includes at the top of `cybeer_web.c`. Add `#include "cybeer_power.h"` if missing. Also ensure `#include "freertos/task.h"` is included for `vTaskDelay`.

- [ ] **Step 7: Build and verify**

```bash
cd firmware
idf.py build
```

Expected: Build succeeds.

- [ ] **Step 8: Commit**

```bash
git add firmware/components/cybeer_web/cybeer_web.c
git commit -m "feat: add admin API endpoints for reboot, eco mode, and sleep"
```

---

### Task 5: Add rename participant storage function

**Files:**
- Modify: `firmware/components/cybeer_storage/cybeer_storage.c`
- Modify: `firmware/components/cybeer_storage/include/cybeer_storage.h`

- [ ] **Step 1: Declare the function in the header**

In `firmware/components/cybeer_storage/include/cybeer_storage.h`, add after the `cybeer_storage_claim_run` declaration:

```c
/** Rename participant. Returns ESP_ERR_NOT_FOUND if pid unknown, ESP_ERR_INVALID_STATE if name taken. */
esp_err_t cybeer_storage_rename_participant(const char *participant_id, const char *new_name);
```

- [ ] **Step 2: Implement the function**

In `firmware/components/cybeer_storage/cybeer_storage.c`, add after `cybeer_storage_claim_run`:

```c
esp_err_t cybeer_storage_rename_participant(const char *participant_id, const char *new_name)
{
    ESP_RETURN_ON_FALSE(participant_id && new_name && new_name[0], ESP_ERR_INVALID_ARG, TAG, "args");

    ESP_RETURN_ON_ERROR(take_mtx(), TAG, "take");

    cJSON *parts = parse_array_file_locked(PATH_PARTICIPANTS);
    if (!parts) {
        give_mtx();
        return ESP_FAIL;
    }

    cJSON *target = NULL;
    cJSON *p = NULL;
    cJSON_ArrayForEach(p, parts)
    {
        const cJSON *jid = cJSON_GetObjectItemCaseSensitive(p, "id");
        const cJSON *jn = cJSON_GetObjectItemCaseSensitive(p, "name");
        if (cJSON_IsString(jid) && jid->valuestring) {
            if (strcmp(jid->valuestring, participant_id) == 0) {
                target = p;
            } else if (cJSON_IsString(jn) && jn->valuestring && strcmp(jn->valuestring, new_name) == 0) {
                cJSON_Delete(parts);
                give_mtx();
                return ESP_ERR_INVALID_STATE;  // name taken by another participant
            }
        }
    }

    if (!target) {
        cJSON_Delete(parts);
        give_mtx();
        return ESP_ERR_NOT_FOUND;
    }

    cJSON *jname = cJSON_GetObjectItemCaseSensitive(target, "name");
    if (cJSON_IsString(jname)) {
        cJSON_SetValuestring(jname, new_name);
    } else {
        cJSON_AddStringToObject(target, "name", new_name);
    }

    esp_err_t err = persist_json_locked(PATH_PARTICIPANTS, parts);
    cJSON_Delete(parts);
    give_mtx();

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "renamed participant %s -> '%s'", participant_id, new_name);
    }
    return err;
}
```

- [ ] **Step 3: Build and verify**

```bash
cd firmware
idf.py build
```

Expected: Build succeeds.

- [ ] **Step 4: Commit**

```bash
git add firmware/components/cybeer_storage/cybeer_storage.c firmware/components/cybeer_storage/include/cybeer_storage.h
git commit -m "feat: add cybeer_storage_rename_participant function"
```

---

### Task 6: Add rename participant HTTP endpoint

**Files:**
- Modify: `firmware/components/cybeer_web/cybeer_web.c`

- [ ] **Step 1: Add the PATCH handler**

In `firmware/components/cybeer_web/cybeer_web.c`, add the handler (place near other participant handlers):

```c
static esp_err_t h_patch_participant(httpd_req_t *req)
{
    char path[160];
    copy_path_no_query(req->uri, path, sizeof(path));

    const char *pfx = "/api/participants/";
    if (strncmp(path, pfx, strlen(pfx)) != 0) {
        return send_json_text(req, "400 Bad Request", "{\"error\":\"uri\"}");
    }
    const char *pid = path + strlen(pfx);
    if (strlen(pid) == 0 || strlen(pid) >= 40 || strchr(pid, '/') != NULL) {
        return send_json_text(req, "400 Bad Request", "{\"error\":\"id\"}");
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
    const char *new_name = jname->valuestring;
    size_t name_len = strlen(new_name);
    if (name_len > 32) {
        cJSON_Delete(root);
        return send_json_text(req, "400 Bad Request", "{\"error\":\"name max 32 chars\"}");
    }

    esp_err_t err = cybeer_storage_rename_participant(pid, new_name);
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
```

- [ ] **Step 2: Register the URI handler**

Add the URI definition:
```c
httpd_uri_t u_part_rename = {
    .uri = "/api/participants/*", .method = HTTP_PATCH, .handler = h_patch_participant, .user_ctx = NULL
};
```

Add registration in the chain (before the static file catch-all):
```c
|| httpd_register_uri_handler(s_server, &u_part_rename) != ESP_OK
```

- [ ] **Step 3: Build and verify**

```bash
cd firmware
idf.py build
```

Expected: Build succeeds.

- [ ] **Step 4: Commit**

```bash
git add firmware/components/cybeer_web/cybeer_web.c
git commit -m "feat: add PATCH /api/participants/:id endpoint for rename"
```

---

### Task 7: Frontend — Admin power control buttons

**Files:**
- Modify: `frontend/admin.html`
- Modify: `frontend/admin.js`

- [ ] **Step 1: Add power control section to admin.html**

In `frontend/admin.html`, add a new `<details>` section before the closing `</section>` tag (before line 178 `</section>`), after the OTA section:

```html
      <details open>
        <summary>Управление устройством</summary>
        <div class="power-controls">
          <button type="button" id="rebootBtn" class="danger">Перезагрузить</button>
          <button type="button" id="ecoBtn">Энергосбережение</button>
          <button type="button" id="sleepBtn">Сон</button>
        </div>
        <p class="hint" id="powerStatus"></p>
      </details>
```

- [ ] **Step 2: Add power button styles to admin.html**

In the existing `<style>` block in admin.html (line 181), add:

```css
.power-controls { display: flex; gap: 0.5rem; flex-wrap: wrap; margin-bottom: 0.5rem; }
.power-controls button { flex: 1; min-width: 8rem; }
#ecoBtn.active { background: var(--amber); color: #000; border-color: var(--amber); }
```

- [ ] **Step 3: Add power control logic to admin.js**

At the end of `frontend/admin.js`, add:

```javascript
(function initPowerControls() {
  const rebootBtn = document.getElementById("rebootBtn");
  const ecoBtn = document.getElementById("ecoBtn");
  const sleepBtn = document.getElementById("sleepBtn");
  const powerStatus = document.getElementById("powerStatus");

  if (!rebootBtn) return;

  async function updatePowerStatus() {
    try {
      const res = await fetch("/api/status");
      const st = await res.json();
      if (st.powerMode === "eco") {
        ecoBtn.classList.add("active");
        ecoBtn.textContent = "Энергосбережение (вкл)";
        if (powerStatus) powerStatus.textContent = "Режим: Энергосбережение";
      } else {
        ecoBtn.classList.remove("active");
        ecoBtn.textContent = "Энергосбережение";
        if (powerStatus) powerStatus.textContent = "";
      }
    } catch (_) {}
  }

  rebootBtn.addEventListener("click", async function () {
    try {
      await fetch("/api/admin/reboot", { method: "POST", headers: pinHeaders() });
      showMsg("Устройство перезагружается…");
    } catch (_) {
      showMsg("Ошибка отправки команды", true);
    }
  });

  ecoBtn.addEventListener("click", async function () {
    try {
      const res = await fetch("/api/admin/power/eco", { method: "POST", headers: pinHeaders() });
      const data = await res.json();
      if (data.eco) {
        ecoBtn.classList.add("active");
        ecoBtn.textContent = "Энергосбережение (вкл)";
        showMsg("Энергосбережение включено");
        if (powerStatus) powerStatus.textContent = "Режим: Энергосбережение";
      } else {
        ecoBtn.classList.remove("active");
        ecoBtn.textContent = "Энергосбережение";
        showMsg("Энергосбережение выключено");
        if (powerStatus) powerStatus.textContent = "";
      }
    } catch (_) {
      showMsg("Ошибка отправки команды", true);
    }
  });

  sleepBtn.addEventListener("click", async function () {
    try {
      await fetch("/api/admin/power/sleep", { method: "POST", headers: pinHeaders() });
      showMsg("Устройство уснуло. Для пробуждения нажмите кнопку 3 раза.");
    } catch (_) {
      showMsg("Ошибка отправки команды", true);
    }
  });

  updatePowerStatus();
})();
```

- [ ] **Step 4: Build and verify frontend loads**

Open `frontend/admin.html` in a browser (or verify syntax). Ensure no JS errors in the console.

- [ ] **Step 5: Commit**

```bash
git add frontend/admin.html frontend/admin.js
git commit -m "feat: add power control buttons (reboot, eco, sleep) to admin UI"
```

---

### Task 8: Frontend — Player rename UI

**Files:**
- Modify: `frontend/player.html`

- [ ] **Step 1: Add rename UI elements to player.html**

In `frontend/player.html`, modify the player card section (line 28-31) to add an edit button next to the name:

```html
      <div class="player-card">
        <h2 id="playerName">Загрузка…</h2>
        <button type="button" id="renameBtn" class="btn-edit" hidden>Изменить имя</button>
        <div id="renameForm" hidden>
          <input type="text" id="renameInput" maxlength="32" placeholder="Новое имя" />
          <button type="button" id="renameSave">Сохранить</button>
          <button type="button" id="renameCancel">Отмена</button>
          <p class="hint err" id="renameMsg"></p>
        </div>
        <div class="player-rank" id="playerRank"></div>
      </div>
```

- [ ] **Step 2: Add rename styles**

Add a `<style>` block to player.html (before the script tag):

```html
<style>
  .btn-edit { font-size: 0.8rem; margin-left: 0.5rem; padding: 0.2rem 0.6rem; }
  #renameForm { margin: 0.5rem 0; display: flex; flex-wrap: wrap; gap: 0.4rem; align-items: center; }
  #renameForm input { flex: 1; min-width: 10rem; }
  #renameMsg { width: 100%; margin: 0; }
</style>
```

- [ ] **Step 3: Add rename logic in the player page script**

In the inline `<script>` block in `frontend/player.html`, after the participants load (after `document.title = "CyBeer — " + name;` around line 62), add rename initialization:

```javascript
    var renameBtn = document.getElementById("renameBtn");
    var renameForm = document.getElementById("renameForm");
    var renameInput = document.getElementById("renameInput");
    var renameSave = document.getElementById("renameSave");
    var renameCancel = document.getElementById("renameCancel");
    var renameMsg = document.getElementById("renameMsg");

    renameBtn.hidden = false;

    renameBtn.addEventListener("click", function () {
      renameBtn.hidden = true;
      renameForm.hidden = false;
      renameInput.value = document.getElementById("playerName").textContent;
      renameInput.focus();
      renameMsg.textContent = "";
    });

    renameCancel.addEventListener("click", function () {
      renameForm.hidden = true;
      renameBtn.hidden = false;
      renameMsg.textContent = "";
    });

    renameSave.addEventListener("click", async function () {
      var newName = renameInput.value.trim();
      if (!newName) {
        renameMsg.textContent = "Имя не может быть пустым";
        return;
      }
      renameMsg.textContent = "";
      try {
        var res = await fetch("/api/participants/" + encodeURIComponent(id), {
          method: "PATCH",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ name: newName }),
        });
        if (res.ok) {
          document.getElementById("playerName").textContent = newName;
          document.title = "CyBeer — " + newName;
          renameForm.hidden = true;
          renameBtn.hidden = false;
        } else {
          var data = await res.json().catch(function () { return {}; });
          if (data.error === "name_taken") {
            renameMsg.textContent = "Это имя уже занято";
          } else if (data.error === "not_found") {
            renameMsg.textContent = "Участник не найден";
          } else {
            renameMsg.textContent = "Ошибка: " + (data.error || res.status);
          }
        }
      } catch (e) {
        renameMsg.textContent = "Ошибка сети";
      }
    });
```

- [ ] **Step 4: Verify HTML structure is valid**

Check that the HTML has no unclosed tags and the script block is syntactically correct.

- [ ] **Step 5: Commit**

```bash
git add frontend/player.html
git commit -m "feat: add inline rename UI to player page"
```

---

### Task 9: Final build and integration test

**Files:** None (verification only)

- [ ] **Step 1: Full firmware build**

```bash
cd firmware
idf.py build
```

Expected: Build succeeds with no errors and no new warnings.

- [ ] **Step 2: Flash and test on device (manual)**

```bash
cd firmware
idf.py -p COM_PORT flash monitor
```

Test checklist:
1. Claim a run with a new name → verify leaderboard shows the name (not UUID)
2. Open player page → verify stats and run history load without errors
3. Rename participant from player page → verify leaderboard updates
4. Admin → Reboot button works
5. Admin → Eco mode toggles display/LED
6. Admin → Sleep puts device to sleep, triple-tap wakes

- [ ] **Step 3: Final commit (if any integration fixes needed)**

```bash
git add -A
git commit -m "fix: integration adjustments after testing"
```
