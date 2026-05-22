# CyBeer Overhaul — план реализации

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Исправить проблемы прошивки, переработать веб-интерфейс, добавить spectator mode, турнирную админку и звук финиша.

**Architecture:** Два прохода — сначала firmware (C, ESP-IDF), потом frontend (vanilla HTML/CSS/JS). Firmware отдаёт REST API + WebSocket; frontend на LittleFS. Без фреймворков, без сборщиков.

**Tech Stack:** ESP-IDF 5.x/6.x (C), ESP32-C3, cJSON, LittleFS, vanilla JS, Web Audio API.

**Spec:** `docs/superpowers/specs/2026-05-22-cybeer-overhaul-design.md`

**Замечание:** NTP-синхронизация (п.1.2 спека) уже реализована в `cybeer_wifi.c:203-210` — SNTP запускается при получении IP в STA. Отдельной задачи не требуется.

---

## Карта файлов

### Firmware (изменяемые)

| Файл | Что меняется |
|------|-------------|
| `firmware/CMakeLists.txt` | Добавить VERSION в `project()` |
| `firmware/components/cybeer_web/cybeer_web.c` | FW version макрос, leaderboard фильтр, новые handlers (participant runs, PIN verify, tournaments list, unclaimedRunDurationUs), LED flag вызовы |
| `firmware/components/cybeer_web/cybeer_ws.c` | WS max clients 4→10 |
| `firmware/components/cybeer_led/cybeer_led.c` | Unclaimed flag, подиум 3→6с |
| `firmware/components/cybeer_led/include/cybeer_led.h` | Новая функция `cybeer_led_set_unclaimed_flag` |
| `firmware/main/app_main.c` | Вызов `cybeer_led_set_unclaimed_flag(true)` при сохранении рана |

### Frontend (изменяемые)

| Файл | Что меняется |
|------|-------------|
| `frontend/app.js` | Shared topbar, WS-логика, звук, лидерборд, claim UX |
| `frontend/app.css` | Стили для таймера, навигации, spectator |
| `frontend/index.html` | Новая структура: таймер + лидерборд |
| `frontend/claim.html` | Топбар, русский, показ времени заезда |
| `frontend/player.html` | Полная переделка: карточка + статистика + заезды |
| `frontend/admin.html` | PIN-валидация, LED из API, турнирная секция |
| `frontend/admin.js` | PIN verify, LED prefill, турнирные формы |
| `frontend/tournament.html` | Подсветка матчей, чемпион, русский |

### Frontend (новые)

| Файл | Назначение |
|------|-----------|
| `frontend/spectator.html` | TV/spectator mode (leaderboard + tournament режимы) |

---

## Проход 1: Firmware

### Task 1: Firmware version из сборки

**Files:**
- Modify: `firmware/CMakeLists.txt:3`
- Modify: `firmware/components/cybeer_web/cybeer_web.c:99`

- [ ] **Step 1: Добавить VERSION в CMakeLists.txt**

В `firmware/CMakeLists.txt` заменить:

```c
project(cybeer)
```

на:

```c
project(cybeer VERSION 1.1.0)
```

ESP-IDF определит макрос `PROJECT_VER` как `"1.1.0"`.

- [ ] **Step 2: Использовать PROJECT_VER в статусе**

В `firmware/components/cybeer_web/cybeer_web.c` заменить строку:

```c
    cJSON_AddStringToObject(root, "firmwareVersion", "1.0.0");
```

на:

```c
#ifndef PROJECT_VER
#define PROJECT_VER "unknown"
#endif
    cJSON_AddStringToObject(root, "firmwareVersion", PROJECT_VER);
```

Примечание: `#ifndef` ставим перед функцией `h_get_status` (или в начале файла) — один раз. Внутри функции остаётся только `cJSON_AddStringToObject`.

- [ ] **Step 3: Собрать и проверить**

```bash
cd firmware
idf.py build
```

Ожидаем: сборка проходит без ошибок.

- [ ] **Step 4: Commit**

```bash
git add firmware/CMakeLists.txt firmware/components/cybeer_web/cybeer_web.c
git commit -m "fix: use PROJECT_VER instead of hardcoded firmware version"
```

---

### Task 2: WebSocket лимит 4 → 10

**Files:**
- Modify: `firmware/components/cybeer_web/cybeer_ws.c:18`

- [ ] **Step 1: Изменить константу**

В `firmware/components/cybeer_web/cybeer_ws.c` заменить:

```c
#define CYBEER_WS_MAX_CLIENTS 4
```

на:

```c
#define CYBEER_WS_MAX_CLIENTS 10
```

- [ ] **Step 2: Собрать**

```bash
cd firmware && idf.py build
```

- [ ] **Step 3: Commit**

```bash
git add firmware/components/cybeer_web/cybeer_ws.c
git commit -m "fix: increase WebSocket max clients from 4 to 10"
```

---

### Task 3: LED unclaimed flag + подиум 6с

**Files:**
- Modify: `firmware/components/cybeer_led/include/cybeer_led.h`
- Modify: `firmware/components/cybeer_led/cybeer_led.c`
- Modify: `firmware/main/app_main.c`
- Modify: `firmware/components/cybeer_web/cybeer_web.c`

- [ ] **Step 1: Добавить функцию в заголовок**

В `firmware/components/cybeer_led/include/cybeer_led.h` добавить перед закрывающим `#endif` (или после последнего прототипа):

```c
void cybeer_led_set_unclaimed_flag(bool has_unclaimed);
```

Также добавить `#include <stdbool.h>` в начало файла, если его ещё нет. (Он уже есть через `stdint.h` косвенно, но явный include безопаснее.)

- [ ] **Step 2: Реализовать флаг и убрать storage-чтение из LED**

В `firmware/components/cybeer_led/cybeer_led.c`:

Добавить статическую переменную после `s_podium_until_us`:

```c
static bool s_has_unclaimed_run;
```

Добавить функцию в конце файла (перед `cybeer_led_task_tick` или после `cybeer_led_set_fx`):

```c
void cybeer_led_set_unclaimed_flag(bool has_unclaimed)
{
    s_has_unclaimed_run = has_unclaimed;
}
```

В `render_frame()`, заменить блок:

```c
        char unclaimed[40];
        if (cybeer_storage_get_latest_unclaimed_run_id(unclaimed, sizeof(unclaimed)) == ESP_OK
            && unclaimed[0] != '\0') {
            return render_claim_pending(now_us);
        }
        return render_ambient(now_us);
```

на:

```c
        if (s_has_unclaimed_run) {
            return render_claim_pending(now_us);
        }
        return render_ambient(now_us);
```

Также убрать `#include "cybeer_storage.h"` из `cybeer_led.c`, если он больше не используется (проверить нет ли других вызовов storage в этом файле — `load_led_settings` использует `cybeer_nvs_get_led_settings`, который тоже в `cybeer_storage.h`, так что оставляем include).

- [ ] **Step 3: Увеличить подиум до 6 секунд**

В том же файле `cybeer_led.c`, заменить:

```c
        s_podium_until_us = now + (int64_t)3000000;
```

на:

```c
        s_podium_until_us = now + (int64_t)6000000;
```

- [ ] **Step 4: Выставить флаг при сохранении рана**

В `firmware/main/app_main.c`, в функции `on_finished_placeholder`, после строки:

```c
        cybeer_ws_on_run_finished(run.id, run.duration_us);
```

добавить:

```c
        cybeer_led_set_unclaimed_flag(true);
```

- [ ] **Step 5: Сбросить флаг при claim и data reset**

В `firmware/components/cybeer_web/cybeer_web.c`:

В функции `h_post_claim`, после строки `cybeer_ws_broadcast_leaderboard_update();` (внутри блока `if (err == ESP_OK)`), добавить:

```c
        cybeer_led_set_unclaimed_flag(false);
```

В функции `h_delete_admin_data_reset`, после строки `esp_err_t err = cybeer_storage_reset_all_data();`, внутри блока успеха, добавить:

```c
    cybeer_led_set_unclaimed_flag(false);
```

Также добавить `#include "cybeer_led.h"` в начало `cybeer_web.c` (если не подключен — проверить; он уже есть в includes).

- [ ] **Step 6: Собрать**

```bash
cd firmware && idf.py build
```

- [ ] **Step 7: Commit**

```bash
git add firmware/components/cybeer_led/ firmware/main/app_main.c firmware/components/cybeer_web/cybeer_web.c
git commit -m "fix: cache unclaimed-run flag in RAM, increase podium to 6s"
```

---

### Task 4: Leaderboard фильтрация duration ≤ 0

**Files:**
- Modify: `firmware/components/cybeer_web/cybeer_web.c` (функция `h_get_leaderboard`)

- [ ] **Step 1: Добавить фильтр**

В `h_get_leaderboard`, в цикле `cJSON_ArrayForEach(item, arr)`, после проверки `if (!cJSON_IsTrue(jcl)) { continue; }`, добавить:

```c
        if (cJSON_IsNumber(jdur) && jdur->valuedouble <= 0) {
            continue;
        }
```

Этот код должен быть после объявления `jdur` (`const cJSON *jdur = ...`), но до `r.duration_us = ...`. Проще всего вставить сразу после строки `if (!cJSON_IsTrue(jcl)) { continue; }`, переместив объявление `jdur` выше.

Итоговый порядок внутри цикла:

```c
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
```

- [ ] **Step 2: Собрать**

```bash
cd firmware && idf.py build
```

- [ ] **Step 3: Commit**

```bash
git add firmware/components/cybeer_web/cybeer_web.c
git commit -m "fix: filter out runs with duration<=0 from leaderboard"
```

---

### Task 5: Новые API-эндпоинты

**Files:**
- Modify: `firmware/components/cybeer_web/cybeer_web.c`

Добавляем три новых handler'а и регистрируем их.

- [ ] **Step 1: Handler — GET /api/participants/:id/runs**

В `cybeer_web.c`, добавить новую функцию (после `h_get_participant_stats`):

```c
static esp_err_t h_get_participant_runs(httpd_req_t *req)
{
    char path[160];
    copy_path_no_query(req->uri, path, sizeof(path));

    const char *pfx = "/api/participants/";
    if (strncmp(path, pfx, strlen(pfx)) != 0) {
        return send_json_text(req, "400 Bad Request", "{\"error\":\"uri\"}");
    }
    const char *rest = path + strlen(pfx);
    const char *suf = strstr(rest, "/runs");
    if (!suf || strcmp(suf, "/runs") != 0) {
        return send_json_text(req, "400 Bad Request", "{\"error\":\"uri\"}");
    }
    size_t id_len = (size_t)(suf - rest);
    if (id_len == 0 || id_len >= 40) {
        return send_json_text(req, "400 Bad Request", "{\"error\":\"id\"}");
    }
    char pid[40];
    memcpy(pid, rest, id_len);
    pid[id_len] = '\0';
    if (strchr(pid, '/') != NULL) {
        return send_json_text(req, "400 Bad Request", "{\"error\":\"id\"}");
    }

    const char *raw = cybeer_storage_runs_json();
    cJSON *arr = cJSON_Parse(raw && raw[0] ? raw : "[]");
    if (!arr || !cJSON_IsArray(arr)) {
        if (arr) cJSON_Delete(arr);
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
        if (!item) continue;
        const cJSON *jpid = cJSON_GetObjectItemCaseSensitive(item, "participant_id");
        if (!cJSON_IsString(jpid) || !jpid->valuestring) continue;
        if (strcmp(jpid->valuestring, pid) != 0) continue;
        cJSON *cpy = cJSON_Duplicate((cJSON *)item, true);
        if (cpy) cJSON_AddItemToArray(out, cpy);
    }
    cJSON_Delete(arr);
    return json_send(req, out);
}
```

- [ ] **Step 2: Handler — POST /api/admin/pin/verify**

```c
static esp_err_t h_post_admin_pin_verify(httpd_req_t *req)
{
    esp_err_t g = require_admin_pin(req);
    if (g != ESP_OK) {
        return g;
    }
    return send_json_text(req, "200 OK", "{\"ok\":true}");
}
```

- [ ] **Step 3: Handler — GET /api/admin/tournaments**

```c
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
```

- [ ] **Step 4: Добавить unclaimedRunDurationUs в h_get_status**

В функции `h_get_status`, после строки `cJSON_AddStringToObject(root, "unclaimedRunId", unclaimed);`, добавить:

```c
    if (unclaimed[0] != '\0') {
        cybeer_run_t unclaimed_run;
        if (cybeer_storage_get_run(unclaimed, &unclaimed_run) == ESP_OK) {
            cJSON_AddNumberToObject(root, "unclaimedRunDurationUs",
                                    (double)unclaimed_run.duration_us);
        }
    }
```

- [ ] **Step 5: Зарегистрировать новые URI**

В функции `cybeer_web_start`, добавить объявления URI (перед `httpd_uri_t u_static`):

```c
    httpd_uri_t u_part_runs = {
        .uri = "/api/participants/*/runs",
        .method = HTTP_GET,
        .handler = h_get_participant_runs,
        .user_ctx = NULL
    };
    httpd_uri_t u_admin_pin_verify = {
        .uri = "/api/admin/pin/verify",
        .method = HTTP_POST,
        .handler = h_post_admin_pin_verify,
        .user_ctx = NULL
    };
    httpd_uri_t u_admin_tournaments_list = {
        .uri = "/api/admin/tournaments",
        .method = HTTP_GET,
        .handler = h_get_admin_tournaments,
        .user_ctx = NULL
    };
```

И добавить регистрацию в цепочку `httpd_register_uri_handler`:

```c
        || httpd_register_uri_handler(s_server, &u_part_runs) != ESP_OK
        || httpd_register_uri_handler(s_server, &u_admin_pin_verify) != ESP_OK
        || httpd_register_uri_handler(s_server, &u_admin_tournaments_list) != ESP_OK
```

**Важно:** `u_admin_tournaments_list` (GET `/api/admin/tournaments`) должен быть зарегистрирован **до** `u_tor_create` (POST `/api/admin/tournaments`), иначе wildcard может перехватить. Поскольку методы разные (GET vs POST), порядок не критичен, но лучше рядом.

- [ ] **Step 6: Собрать**

```bash
cd firmware && idf.py build
```

- [ ] **Step 7: Commit**

```bash
git add firmware/components/cybeer_web/cybeer_web.c
git commit -m "feat: add participant runs, PIN verify, tournaments list APIs + unclaimedRunDurationUs"
```

---

## Проход 2: Frontend

### Task 6: Общий CSS — обновление стилей

**Files:**
- Modify: `frontend/app.css`

- [ ] **Step 1: Добавить стили для нового таймера, навигации, claim, player**

Добавить в конец `frontend/app.css`:

```css
/* === Navigation === */
nav {
  display: flex;
  gap: 1rem;
  flex-wrap: wrap;
}

nav a.active {
  color: var(--gold);
  text-decoration: underline;
}

/* === Live timer (large) === */
.timer-hero {
  text-align: center;
  padding: 1.5rem 1rem;
  margin-bottom: 1rem;
}

.timer-hero .timer-value {
  font-size: 3.5rem;
  font-weight: 700;
  font-variant-numeric: tabular-nums;
  letter-spacing: 0.04em;
  color: var(--gold);
  line-height: 1.1;
}

.timer-hero .timer-label {
  font-size: 1rem;
  color: var(--muted);
  margin-top: 0.35rem;
}

.timer-hero.state-prep .timer-value { color: var(--ok); }
.timer-hero.state-running .timer-value { color: var(--amber); }
.timer-hero.state-finished .timer-value { color: var(--gold); }
.timer-hero.state-ready .timer-value { color: var(--muted); }

@keyframes pulse-amber {
  0%, 100% { opacity: 1; }
  50% { opacity: 0.7; }
}
.timer-hero.state-running .timer-value {
  animation: pulse-amber 1s ease-in-out infinite;
}

/* === Leaderboard === */
.leaderboard td:first-child {
  font-weight: 700;
  color: var(--amber);
  width: 2.5rem;
}

.leaderboard a {
  color: var(--foam);
  text-decoration: none;
}
.leaderboard a:hover {
  color: var(--gold);
  text-decoration: underline;
}

/* === Player card === */
.player-card {
  display: flex;
  flex-direction: column;
  gap: 0.5rem;
  margin-bottom: 1rem;
}
.player-card h2 {
  margin: 0;
  color: var(--gold);
}
.player-card .player-rank {
  font-size: 0.95rem;
  color: var(--muted);
}
.stat-grid {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(120px, 1fr));
  gap: 0.75rem;
  margin-bottom: 1rem;
}
.stat-item {
  background: var(--bg);
  border: 1px solid var(--stout);
  border-radius: 8px;
  padding: 0.75rem;
  text-align: center;
}
.stat-item .stat-val {
  font-size: 1.3rem;
  font-weight: 700;
  color: var(--gold);
}
.stat-item .stat-label {
  font-size: 0.8rem;
  color: var(--muted);
  margin-top: 0.2rem;
}

/* === Claim === */
.claim-panel select {
  display: block;
  width: 100%;
  max-width: 420px;
  margin-top: 0.35rem;
  padding: 0.45rem 0.5rem;
  border-radius: 6px;
  border: 1px solid var(--stout);
  background: var(--bg);
  color: var(--foam);
}
.claim-result {
  font-size: 1.8rem;
  font-weight: 700;
  color: var(--gold);
  margin: 0.5rem 0;
}

/* === Tournament enhancements === */
.bracket-match.active-match {
  border-color: var(--gold);
  box-shadow: 0 0 8px rgba(232, 180, 74, 0.3);
  animation: pulse-border 2s ease-in-out infinite;
}
@keyframes pulse-border {
  0%, 100% { box-shadow: 0 0 8px rgba(232, 180, 74, 0.3); }
  50% { box-shadow: 0 0 16px rgba(232, 180, 74, 0.5); }
}
.champion-card {
  text-align: center;
  padding: 1rem;
  margin-bottom: 1rem;
  background: linear-gradient(135deg, rgba(232, 180, 74, 0.15), rgba(199, 128, 45, 0.1));
  border: 2px solid var(--gold);
  border-radius: 12px;
}
.champion-card .champion-name {
  font-size: 1.5rem;
  font-weight: 700;
  color: var(--gold);
}

/* === Admin tournament section === */
.participant-checkboxes {
  display: flex;
  flex-wrap: wrap;
  gap: 0.5rem;
  margin: 0.5rem 0;
}
.participant-checkboxes label {
  display: inline-flex;
  align-items: center;
  gap: 0.3rem;
  background: var(--bg);
  border: 1px solid var(--stout);
  border-radius: 6px;
  padding: 0.35rem 0.6rem;
  font-size: 0.85rem;
  cursor: pointer;
}
.participant-checkboxes input:checked + span {
  color: var(--gold);
}

/* === Sound toggle === */
.sound-toggle {
  position: fixed;
  bottom: 1rem;
  right: 1rem;
  background: var(--stout);
  border: 1px solid var(--amber);
  border-radius: 50%;
  width: 48px;
  height: 48px;
  display: flex;
  align-items: center;
  justify-content: center;
  cursor: pointer;
  font-size: 1.3rem;
  color: var(--amber);
  z-index: 100;
}
.sound-toggle.active {
  background: var(--amber);
  color: var(--bg);
}
```

- [ ] **Step 2: Commit**

```bash
git add frontend/app.css
git commit -m "style: add CSS for timer hero, leaderboard, player card, tournament, spectator"
```

---

### Task 7: app.js — shared topbar + WS + sound + leaderboard

**Files:**
- Modify: `frontend/app.js`

Это самый большой файл. Полная перезапись `app.js`.

- [ ] **Step 1: Перезаписать app.js**

Заменить содержимое `frontend/app.js` на:

```js
(() => {
  /* ========== Утилиты ========== */

  function formatDuration(us) {
    if (typeof us !== "number" || !Number.isFinite(us)) return "—";
    const ms = Math.floor(us / 1000);
    const s = Math.floor(ms / 1000);
    const cs = Math.floor((ms % 1000) / 10);
    return s + "." + String(cs).padStart(2, "0") + "с";
  }

  function formatDate(iso) {
    if (!iso) return "—";
    try {
      const d = new Date(iso);
      if (isNaN(d.getTime())) return iso;
      return d.toLocaleDateString("ru-RU", { day: "numeric", month: "short", hour: "2-digit", minute: "2-digit" });
    } catch (_) { return iso; }
  }

  const nameByPid = new Map();

  function participantName(pid) {
    if (!pid) return "—";
    return nameByPid.get(pid) || pid.slice(0, 8) + "…";
  }

  async function loadParticipants() {
    try {
      const r = await fetch("/api/participants");
      const data = await r.json();
      nameByPid.clear();
      if (Array.isArray(data)) {
        for (const p of data) {
          if (p && p.id && p.name) nameByPid.set(p.id, p.name);
        }
      }
    } catch (_) {}
  }

  /* ========== Shared topbar ========== */

  function injectTopbar() {
    const page = window.__CYBEER_PAGE__;
    if (page === "setup" || page === "spectator") return;

    const header = document.querySelector("header.topbar");
    if (!header) return;

    const links = [
      { href: "/", label: "Главная", page: "index" },
      { href: "/claim.html", label: "Заявить заезд", page: "claim" },
      { href: "/tournament.html", label: "Турнир", page: "tournament" },
      { href: "/admin.html", label: "Админка", page: "admin" },
    ];

    const nav = header.querySelector("nav");
    if (nav) {
      nav.innerHTML = "";
      for (const l of links) {
        const a = document.createElement("a");
        a.href = l.href;
        a.textContent = l.label;
        if (l.page === page) a.classList.add("active");
        nav.appendChild(a);
      }
    }
  }

  /* ========== Battery ========== */

  function applyBatteryPercent(pct) {
    const wrap = document.getElementById("batteryWrap");
    const icon = document.getElementById("batteryIcon");
    const label = document.getElementById("batteryPct");
    if (!wrap || !icon || !label) return;
    if (typeof pct !== "number" || !Number.isFinite(pct)) {
      label.textContent = "—";
      icon.textContent = "🔋";
      wrap.classList.remove("bat-low", "bat-mid", "bat-high");
      return;
    }
    const p = Math.max(0, Math.min(100, Math.round(pct)));
    label.textContent = p + "%";
    icon.textContent = p < 20 ? "🪫" : "🔋";
    wrap.classList.remove("bat-low", "bat-mid", "bat-high");
    if (p < 20) wrap.classList.add("bat-low");
    else if (p < 60) wrap.classList.add("bat-mid");
    else wrap.classList.add("bat-high");
    wrap.title = "Батарея " + p + "%";
  }

  /* ========== FSM state ========== */

  function renderStatus(s) {
    const badge = document.getElementById("stateBadge");
    const ver = document.getElementById("fwVer");
    if (badge) {
      const labels = { PREP: "ГОТОВ", RUNNING: "ИДЁТ", FINISHED: "ФИНИШ", READY: "ОЖИДАНИЕ" };
      badge.textContent = labels[s.state] || s.state || "—";
    }
    applyBatteryPercent(s.batteryPercent);
    if (ver) ver.textContent = s.firmwareVersion ? "fw " + s.firmwareVersion : "";
  }

  /* ========== Timer hero ========== */

  let currentFsmState = "PREP";

  function updateTimerHero(state, durationUs) {
    const hero = document.getElementById("timerHero");
    const val = document.getElementById("timerValue");
    const lbl = document.getElementById("timerLabel");
    if (!hero || !val) return;

    hero.className = "timer-hero panel state-" + state.toLowerCase();
    currentFsmState = state;

    const labels = {
      PREP: "Готов к старту",
      RUNNING: "",
      FINISHED: "Ожидает заявки",
      READY: "Последний результат",
    };
    if (lbl) lbl.textContent = labels[state] || "";

    if (state === "PREP") {
      val.textContent = "0.00с";
    } else if (state === "RUNNING") {
      if (typeof durationUs === "number") val.textContent = formatDuration(durationUs);
    } else if (state === "FINISHED" || state === "READY") {
      if (typeof durationUs === "number") val.textContent = formatDuration(durationUs);
    }
  }

  /* ========== Leaderboard ========== */

  async function loadLeaderboard() {
    const body = document.getElementById("leaderboardBody");
    if (!body) return;
    try {
      await loadParticipants();
      const r = await fetch("/api/leaderboard?limit=20");
      const data = await r.json();
      body.innerHTML = "";
      if (!Array.isArray(data) || data.length === 0) {
        body.innerHTML = '<tr><td colspan="4" style="text-align:center;color:var(--muted)">Пока нет заездов</td></tr>';
        return;
      }
      const frag = document.createDocumentFragment();
      for (const row of data) {
        const tr = document.createElement("tr");
        const name = row.participantName || participantName(row.participantId);
        const pid = row.participantId || "";
        const nameLink = pid
          ? '<a href="/player.html?id=' + encodeURIComponent(pid) + '">' + name + "</a>"
          : name;
        tr.innerHTML =
          "<td>" + row.rank + "</td>" +
          "<td>" + nameLink + "</td>" +
          "<td>" + formatDuration(row.durationUs) + "</td>" +
          "<td>" + formatDate(row.finishedAt) + "</td>";
        frag.appendChild(tr);
      }
      body.appendChild(frag);
    } catch (_) {}
  }

  /* ========== Sound ========== */

  let audioCtx = null;
  let soundEnabled = false;

  function ensureAudioContext() {
    if (!audioCtx) {
      audioCtx = new (window.AudioContext || window.webkitAudioContext)();
    }
    if (audioCtx.state === "suspended") {
      audioCtx.resume();
    }
  }

  function playFanfare() {
    if (!soundEnabled || !audioCtx) return;
    const notes = [523.25, 659.25, 783.99]; // C5, E5, G5
    const now = audioCtx.currentTime;
    for (let i = 0; i < notes.length; i++) {
      const osc = audioCtx.createOscillator();
      const gain = audioCtx.createGain();
      osc.type = "sine";
      osc.frequency.value = notes[i];
      gain.gain.setValueAtTime(0.3, now + i * 0.25);
      gain.gain.exponentialRampToValueAtTime(0.001, now + i * 0.25 + 0.8);
      osc.connect(gain);
      gain.connect(audioCtx.destination);
      osc.start(now + i * 0.25);
      osc.stop(now + i * 0.25 + 0.8);
    }
  }

  function enableSound() {
    ensureAudioContext();
    soundEnabled = true;
  }

  document.addEventListener("click", function onFirstClick() {
    ensureAudioContext();
    document.removeEventListener("click", onFirstClick);
  }, { once: true });

  /* ========== WebSocket ========== */

  let lastFinishedDurationUs = null;

  function connectLiveWs() {
    const proto = window.location.protocol === "https:" ? "wss:" : "ws:";
    const url = proto + "//" + window.location.host + "/ws";
    let ws;
    let reconnectTimer;

    function scheduleReconnect() {
      if (reconnectTimer) return;
      reconnectTimer = window.setTimeout(function () {
        reconnectTimer = null;
        connectLiveWs();
      }, 2000);
    }

    try { ws = new WebSocket(url); } catch (_) { scheduleReconnect(); return; }

    ws.onopen = function () {
      try { ws.send("."); } catch (_) {}
    };

    ws.onmessage = function (ev) {
      let msg;
      try { msg = JSON.parse(ev.data); } catch (_) { return; }
      if (!msg || typeof msg.type !== "string") return;

      if (msg.type === "timer" && typeof msg.elapsedUs === "number") {
        updateTimerHero("RUNNING", msg.elapsedUs);
        return;
      }
      if (msg.type === "state" && typeof msg.state === "string") {
        currentFsmState = msg.state;
        const badge = document.getElementById("stateBadge");
        const labels = { PREP: "ГОТОВ", RUNNING: "ИДЁТ", FINISHED: "ФИНИШ", READY: "ОЖИДАНИЕ" };
        if (badge) badge.textContent = labels[msg.state] || msg.state;
        if (msg.state === "PREP") {
          updateTimerHero("PREP", 0);
        } else if (msg.state !== "RUNNING") {
          updateTimerHero(msg.state, lastFinishedDurationUs);
        }
        return;
      }
      if (msg.type === "runFinished") {
        lastFinishedDurationUs = msg.durationUs;
        updateTimerHero("FINISHED", msg.durationUs);
        playFanfare();
        loadLeaderboard();

        const targetEl = document.getElementById("targetRunTime");
        if (targetEl && typeof msg.durationUs === "number") {
          targetEl.textContent = formatDuration(msg.durationUs);
        }
        return;
      }
      if (msg.type === "leaderboardUpdate") {
        loadLeaderboard();
        return;
      }
      if (msg.type === "battery" && typeof msg.percent === "number") {
        applyBatteryPercent(msg.percent);
        return;
      }
    };

    ws.onclose = function () { scheduleReconnect(); };
    ws.onerror = function () { try { ws.close(); } catch (_) {} };
  }

  /* ========== Claim page ========== */

  async function tickClaimTarget() {
    const timeEl = document.getElementById("targetRunTime");
    const idEl = document.getElementById("targetRunId");
    try {
      const rs = await fetch("/api/status");
      const st = await rs.json();
      renderStatus(st);
      if (timeEl) {
        timeEl.textContent = st.unclaimedRunDurationUs
          ? formatDuration(st.unclaimedRunDurationUs)
          : "(нет)";
      }
      if (idEl) idEl.textContent = st.unclaimedRunId || "";
    } catch (_) {
      if (timeEl) timeEl.textContent = "(ошибка)";
    }
  }

  function initClaimForm() {
    const form = document.getElementById("claimForm");
    const msg = document.getElementById("claimMsg");
    const sel = document.getElementById("claimSelect");
    const nameInput = document.getElementById("claimName");
    if (!form) return;

    fetch("/api/participants")
      .then(function (r) { return r.json(); })
      .then(function (data) {
        if (!Array.isArray(data)) return;
        for (const p of data) {
          if (!p || !p.id || !p.name) continue;
          const opt = document.createElement("option");
          opt.value = p.id;
          opt.textContent = p.name;
          sel.appendChild(opt);
        }
      })
      .catch(function () {});

    sel.addEventListener("change", function () {
      if (sel.value) {
        nameInput.value = "";
        nameInput.disabled = true;
      } else {
        nameInput.disabled = false;
      }
    });

    form.addEventListener("submit", async function (ev) {
      ev.preventDefault();
      if (msg) { msg.textContent = ""; msg.classList.remove("err"); }
      const rs = await fetch("/api/status");
      const status = await rs.json();
      const runId = status.unclaimedRunId;
      const runDuration = status.unclaimedRunDurationUs;
      if (!runId) {
        if (msg) { msg.textContent = "Нет незаявленного заезда."; msg.classList.add("err"); }
        return;
      }

      let body;
      if (sel.value) {
        body = { participantId: sel.value };
      } else {
        const name = nameInput?.value?.trim();
        if (!name) {
          if (msg) { msg.textContent = "Выберите участника или введите имя."; msg.classList.add("err"); }
          return;
        }
        body = { name: name };
      }

      try {
        const resp = await fetch("/api/runs/" + encodeURIComponent(runId) + "/claim", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify(body),
        });
        const txt = await resp.text();
        if (!resp.ok) {
          if (msg) { msg.textContent = txt || resp.statusText; msg.classList.add("err"); }
          return;
        }
        if (msg) {
          msg.textContent = "Заявлено! Ваш результат: " + formatDuration(runDuration);
          msg.classList.remove("err");
        }
        await tickClaimTarget();
      } catch (e) {
        if (msg) { msg.textContent = String(e); msg.classList.add("err"); }
      }
    });
  }

  /* ========== Boot ========== */

  function bootPage() {
    injectTopbar();
    const page = window.__CYBEER_PAGE__;

    if (page === "index") {
      loadLeaderboard();
      connectLiveWs();

      fetch("/api/status").then(function (r) { return r.json(); }).then(function (st) {
        renderStatus(st);
        updateTimerHero(st.state || "PREP", null);
      }).catch(function () {});
    } else if (page === "claim") {
      tickClaimTarget();
      setInterval(tickClaimTarget, 5000);
      initClaimForm();
      connectLiveWs();
    }
  }

  window.__cybeer = {
    formatDuration: formatDuration,
    formatDate: formatDate,
    loadParticipants: loadParticipants,
    participantName: participantName,
    nameByPid: nameByPid,
    enableSound: enableSound,
    playFanfare: playFanfare,
    connectLiveWs: connectLiveWs,
    updateTimerHero: updateTimerHero,
    loadLeaderboard: loadLeaderboard,
  };

  bootPage();
})();
```

- [ ] **Step 2: Commit**

```bash
git add frontend/app.js
git commit -m "feat: rewrite app.js — shared topbar, leaderboard, RU, sound, timer hero"
```

---

### Task 8: index.html — лидерборд + таймер

**Files:**
- Modify: `frontend/index.html`

- [ ] **Step 1: Перезаписать index.html**

```html
<!DOCTYPE html>
<html lang="ru">
<head>
  <meta charset="UTF-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <title>CyBeer</title>
  <link rel="stylesheet" href="/app.css" />
</head>
<body>
  <header class="topbar">
    <div class="brand">
      <span class="logo" aria-hidden="true">🍺</span>
      <h1>CyBeer</h1>
    </div>
    <div class="status-line">
      <div class="battery" id="batteryWrap" title="Батарея">
        <span class="battery-icon" id="batteryIcon" aria-hidden="true">🔋</span>
        <span id="batteryPct">—</span>
      </div>
      <span id="stateBadge" class="badge">…</span>
      <span class="ver" id="fwVer"></span>
    </div>
    <nav></nav>
  </header>

  <main>
    <section class="panel timer-hero" id="timerHero">
      <div class="timer-value" id="timerValue">0.00с</div>
      <div class="timer-label" id="timerLabel">Готов к старту</div>
    </section>

    <section class="panel">
      <h2>Лидерборд</h2>
      <div class="table-wrap">
        <table class="leaderboard">
          <thead>
            <tr>
              <th>#</th>
              <th>Имя</th>
              <th>Время</th>
              <th>Дата</th>
            </tr>
          </thead>
          <tbody id="leaderboardBody"></tbody>
        </table>
      </div>
    </section>
  </main>

  <script>window.__CYBEER_PAGE__ = "index";</script>
  <script src="/app.js"></script>
</body>
</html>
```

- [ ] **Step 2: Commit**

```bash
git add frontend/index.html
git commit -m "feat: index.html — timer hero + leaderboard, Russian"
```

---

### Task 9: claim.html — топбар, русский, время заезда

**Files:**
- Modify: `frontend/claim.html`

- [ ] **Step 1: Перезаписать claim.html**

```html
<!DOCTYPE html>
<html lang="ru">
<head>
  <meta charset="UTF-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <title>CyBeer — Заявить заезд</title>
  <link rel="stylesheet" href="/app.css" />
</head>
<body>
  <header class="topbar">
    <div class="brand">
      <span class="logo" aria-hidden="true">🍺</span>
      <h1>CyBeer</h1>
    </div>
    <div class="status-line">
      <div class="battery" id="batteryWrap" title="Батарея">
        <span class="battery-icon" id="batteryIcon" aria-hidden="true">🔋</span>
        <span id="batteryPct">—</span>
      </div>
      <span id="stateBadge" class="badge">…</span>
      <span class="ver" id="fwVer"></span>
    </div>
    <nav></nav>
  </header>

  <main>
    <section class="panel claim-panel">
      <h2>Заявить заезд</h2>
      <p>Незаявленный заезд: <strong class="claim-result" id="targetRunTime">(загрузка...)</strong></p>
      <p class="hint" id="targetRunId" style="font-size:0.75rem;color:var(--muted)"></p>

      <form id="claimForm">
        <label for="claimSelect">Выбрать участника:</label>
        <select id="claimSelect">
          <option value="">— Новый участник —</option>
        </select>
        <label for="claimName">Или введите новое имя:</label>
        <input type="text" id="claimName" placeholder="Имя нового участника" />
        <button type="submit">Заявить</button>
      </form>
      <p id="claimMsg" class="msg"></p>
    </section>
  </main>

  <script>window.__CYBEER_PAGE__ = "claim";</script>
  <script src="/app.js"></script>
</body>
</html>
```

- [ ] **Step 2: Commit**

```bash
git add frontend/claim.html
git commit -m "feat: claim.html — topbar, Russian, show run duration"
```

---

### Task 10: player.html — полная переделка

**Files:**
- Modify: `frontend/player.html`

- [ ] **Step 1: Перезаписать player.html**

```html
<!DOCTYPE html>
<html lang="ru">
<head>
  <meta charset="UTF-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <title>CyBeer — Игрок</title>
  <link rel="stylesheet" href="/app.css" />
</head>
<body>
  <header class="topbar">
    <div class="brand">
      <span class="logo" aria-hidden="true">🍺</span>
      <h1>CyBeer</h1>
    </div>
    <div class="status-line">
      <div class="battery" id="batteryWrap" title="Батарея">
        <span class="battery-icon" id="batteryIcon" aria-hidden="true">🔋</span>
        <span id="batteryPct">—</span>
      </div>
      <span id="stateBadge" class="badge">…</span>
      <span class="ver" id="fwVer"></span>
    </div>
    <nav></nav>
  </header>

  <main>
    <section class="panel" id="playerPanel">
      <div class="player-card">
        <h2 id="playerName">Загрузка…</h2>
        <div class="player-rank" id="playerRank"></div>
      </div>

      <div class="stat-grid" id="statGrid"></div>

      <h3>История заездов</h3>
      <div class="table-wrap">
        <table>
          <thead>
            <tr><th>Дата</th><th>Время</th></tr>
          </thead>
          <tbody id="playerRunsBody"></tbody>
        </table>
      </div>
    </section>
  </main>

  <script>window.__CYBEER_PAGE__ = "player";</script>
  <script src="/app.js"></script>
  <script>
  (function () {
    var cy = window.__cybeer;
    var p = new URLSearchParams(location.search);
    var id = p.get("id");
    if (!id) {
      document.getElementById("playerName").textContent = "Не указан ID участника";
      return;
    }

    cy.loadParticipants().then(function () {
      var name = cy.nameByPid.get(id) || id.slice(0, 12) + "…";
      document.getElementById("playerName").textContent = name;
      document.title = "CyBeer — " + name;
    });

    fetch("/api/participants/" + encodeURIComponent(id) + "/stats")
      .then(function (r) { return r.json(); })
      .then(function (st) {
        var grid = document.getElementById("statGrid");
        var items = [
          { label: "Заездов", val: st.runCount || 0 },
          { label: "Рекорд", val: cy.formatDuration(st.bestDurationUs) },
          { label: "Среднее", val: cy.formatDuration(st.avgDurationUs) },
          { label: "Худшее", val: cy.formatDuration(st.worstDurationUs) },
          { label: "Последний", val: cy.formatDuration(st.lastDurationUs) },
        ];
        grid.innerHTML = "";
        for (var i = 0; i < items.length; i++) {
          var div = document.createElement("div");
          div.className = "stat-item";
          div.innerHTML = '<div class="stat-val">' + items[i].val + '</div><div class="stat-label">' + items[i].label + "</div>";
          grid.appendChild(div);
        }
      })
      .catch(function () {
        document.getElementById("statGrid").innerHTML = '<p class="hint">Ошибка загрузки статистики</p>';
      });

    fetch("/api/leaderboard?limit=50")
      .then(function (r) { return r.json(); })
      .then(function (lb) {
        if (!Array.isArray(lb)) return;
        for (var i = 0; i < lb.length; i++) {
          if (lb[i].participantId === id) {
            document.getElementById("playerRank").textContent = "Место в лидерборде: #" + lb[i].rank;
            break;
          }
        }
      }).catch(function () {});

    fetch("/api/participants/" + encodeURIComponent(id) + "/runs")
      .then(function (r) { return r.json(); })
      .then(function (runs) {
        var body = document.getElementById("playerRunsBody");
        if (!Array.isArray(runs) || runs.length === 0) {
          body.innerHTML = '<tr><td colspan="2" style="color:var(--muted)">Нет заездов</td></tr>';
          return;
        }
        body.innerHTML = "";
        for (var i = 0; i < runs.length; i++) {
          var tr = document.createElement("tr");
          tr.innerHTML = "<td>" + cy.formatDate(runs[i].finished_at) + "</td><td>" + cy.formatDuration(runs[i].duration_us) + "</td>";
          body.appendChild(tr);
        }
      })
      .catch(function () {
        document.getElementById("playerRunsBody").innerHTML = '<tr><td colspan="2">Ошибка</td></tr>';
      });
  })();
  </script>
</body>
</html>
```

- [ ] **Step 2: Commit**

```bash
git add frontend/player.html
git commit -m "feat: player.html — stats card, run history, Russian"
```

---

### Task 11: admin.html + admin.js — PIN verify, LED prefill, турнирная секция

**Files:**
- Modify: `frontend/admin.html`
- Modify: `frontend/admin.js`

- [ ] **Step 1: Обновить admin.html — добавить турнирную секцию, русский язык**

В `frontend/admin.html`, заменить содержимое `<section id="adminPanel">` (от `<section class="panel hidden" id="adminPanel">` до закрывающего `</section>` перед `</main>`):

```html
    <section class="panel hidden" id="adminPanel">
      <h2>Ручной заезд</h2>
      <form id="addRunForm">
        <label>ID заезда <span class="hint">(пусто = авто)</span>
          <input type="text" name="id" placeholder="авто" maxlength="38" />
        </label>
        <label>ID участника (UUID или пусто)
          <input type="text" name="participant_id" maxlength="38" />
        </label>
        <label>Длительность (мкс)
          <input type="number" name="duration_us" value="0" />
        </label>
        <label>Время финиша (ISO UTC)
          <input type="text" name="finished_at" placeholder="авто если пусто" />
        </label>
        <label>
          <input type="checkbox" name="claimed" /> Заявлен
        </label>
        <label>ID матча турнира (опционально)
          <input type="text" name="tournament_match_id" maxlength="38" />
        </label>
        <button type="submit">Добавить заезд</button>
      </form>

      <h2>Редактировать / удалить заезд</h2>
      <form id="patchRunForm">
        <label>ID заезда
          <input type="text" name="patch_id" maxlength="38" required />
        </label>
        <textarea name="patch_json" rows="5" cols="72" spellcheck="false" placeholder='Частичный JSON, напр. {"participant_id":"","claimed":true}'></textarea>
        <button type="submit">PATCH</button>
      </form>
      <form id="deleteRunForm">
        <label>ID заезда
          <input type="text" name="delete_id" maxlength="38" required />
        </label>
        <button type="submit" class="danger">Удалить</button>
      </form>

      <h2>Данные</h2>
      <button type="button" id="resetBtn">Очистить все заезды и участников</button>

      <h2>Экспорт</h2>
      <form id="exportForm">
        <label>Формат</label>
        <select id="exportFmt">
          <option value="json">JSON</option>
          <option value="csv">CSV</option>
        </select>
        <button type="submit">Скачать</button>
      </form>

      <h2>LED-лента</h2>
      <form id="settingsForm">
        <label>Количество LED (1–64)
          <input type="number" name="ledCount" min="1" max="64" id="ledCountInput" />
        </label>
        <label>Яркость (1–255)
          <input type="number" name="brightness" min="1" max="255" id="ledBrightnessInput" />
        </label>
        <p class="hint">Устройство перезагрузится после сохранения.</p>
        <button type="submit">Сохранить и перезагрузить</button>
      </form>

      <h2>Турнир</h2>
      <form id="tournamentCreateForm">
        <label>Название турнира
          <input type="text" id="torName" placeholder="Кубок CyBeer" required />
        </label>
        <p class="hint">Участники:</p>
        <div class="participant-checkboxes" id="torParticipants"></div>
        <button type="submit">Создать турнир</button>
      </form>

      <h3>Старт турнира</h3>
      <form id="tournamentStartForm">
        <label>Турнир
          <select id="torSelect"><option value="">— загрузка —</option></select>
        </label>
        <button type="submit">Старт</button>
      </form>

      <h3>Назначить слот</h3>
      <form id="tournamentAssignForm">
        <label>ID матча
          <input type="text" id="assignMatchId" maxlength="38" required />
        </label>
        <label>Слот
          <select id="assignSlot">
            <option value="A">A</option>
            <option value="B">B</option>
          </select>
        </label>
        <button type="submit">Привязать следующий заезд</button>
      </form>

    </section>
```

Также обновить секции `setupPanel` и `unlockPanel` на русский:

Секция `setupPanel` — заменить `<h2>First-time admin PIN</h2>` на `<h2>Первая настройка PIN</h2>`, hint на `<p class="hint">SHA-256 хеш с солью хранится на устройстве.</p>`, кнопку на `Сохранить PIN`, label на `Новый PIN`.

Секция `unlockPanel` — заменить `<h2>Unlock admin</h2>` на `<h2>Вход в админку</h2>`, hint на `<p class="hint">PIN сохраняется в сессии этой вкладки.</p>`, кнопку на `Запомнить PIN`, `Clear PIN from session` на `Забыть PIN`.

- [ ] **Step 2: Обновить admin.js — PIN verify, LED prefill, турниры**

Заменить содержимое `frontend/admin.js`:

```js
const PIN_SESSION_KEY = "cybeer_admin_pin";

function pinHeaders() {
  const p = sessionStorage.getItem(PIN_SESSION_KEY);
  return p ? { "X-Admin-Pin": p } : {};
}

function combineHeaders(extra) {
  return { ...(extra || {}), ...pinHeaders(), "Content-Type": "application/json" };
}

function showMsg(txt, isErr) {
  const el = document.getElementById("adminMsg");
  if (el) {
    el.textContent = txt;
    el.classList.toggle("err", !!isErr);
  }
}

async function fetchStatus() {
  const res = await fetch("/api/status");
  if (!res.ok) throw new Error("status " + res.status);
  return res.json();
}

function applyVisibility(cfg) {
  const setup = document.getElementById("setupPanel");
  const unlock = document.getElementById("unlockPanel");
  const admin = document.getElementById("adminPanel");
  const hasPinStored = !!sessionStorage.getItem(PIN_SESSION_KEY);
  const pinConfigured = !!cfg.adminPinConfigured;

  if (!pinConfigured) {
    setup.classList.remove("hidden");
    unlock.classList.add("hidden");
    admin.classList.add("hidden");
    return;
  }
  setup.classList.add("hidden");
  if (!hasPinStored) {
    unlock.classList.remove("hidden");
    admin.classList.add("hidden");
    return;
  }
  unlock.classList.add("hidden");
  admin.classList.remove("hidden");

  prefillLedSettings(cfg);
  loadTournamentParticipants();
  loadTournamentsList();
}

function prefillLedSettings(cfg) {
  const lc = document.getElementById("ledCountInput");
  const lb = document.getElementById("ledBrightnessInput");
  if (lc && cfg.ledCount) lc.value = cfg.ledCount;
  if (lb && cfg.ledBrightness) lb.value = cfg.ledBrightness;
}

async function loadTournamentParticipants() {
  try {
    const r = await fetch("/api/participants");
    const data = await r.json();
    const wrap = document.getElementById("torParticipants");
    if (!wrap || !Array.isArray(data)) return;
    wrap.innerHTML = "";
    for (const p of data) {
      if (!p || !p.id || !p.name) continue;
      const lbl = document.createElement("label");
      lbl.innerHTML = '<input type="checkbox" name="tor_pid" value="' + p.id + '"> <span>' + p.name + "</span>";
      wrap.appendChild(lbl);
    }
  } catch (_) {}
}

async function loadTournamentsList() {
  try {
    const r = await fetch("/api/admin/tournaments", { headers: pinHeaders() });
    const data = await r.json();
    const sel = document.getElementById("torSelect");
    if (!sel || !Array.isArray(data)) return;
    sel.innerHTML = '<option value="">— выберите —</option>';
    for (const t of data) {
      if (!t || !t.id) continue;
      const opt = document.createElement("option");
      opt.value = t.id;
      opt.textContent = (t.name || t.id.slice(0, 8)) + " [" + (t.status || "?") + "]";
      sel.appendChild(opt);
    }
  } catch (_) {}
}

/* PIN setup */
document.getElementById("setupPinForm").addEventListener("submit", async (ev) => {
  ev.preventDefault();
  const fd = new FormData(ev.target);
  const pin = String(fd.get("pin1") || "").trim();
  showMsg("");
  try {
    const res = await fetch("/api/admin/pin/setup", {
      method: "POST", headers: combineHeaders({}), body: JSON.stringify({ pin }),
    });
    if (!res.ok) { showMsg(await res.text(), true); return; }
    showMsg("PIN сохранён. Перезагрузка.");
    sessionStorage.removeItem(PIN_SESSION_KEY);
    await fetchStatus().then(applyVisibility);
  } catch (e) { showMsg(String(e.message || e), true); }
});

/* PIN unlock with verification */
document.getElementById("unlockForm").addEventListener("submit", async (ev) => {
  ev.preventDefault();
  const pin = String(document.getElementById("unlockPin").value || "").trim();
  if (pin.length < 4) { showMsg("PIN слишком короткий", true); return; }

  try {
    const res = await fetch("/api/admin/pin/verify", {
      method: "POST", headers: { "X-Admin-Pin": pin },
    });
    if (!res.ok) {
      showMsg("Неверный PIN", true);
      return;
    }
    sessionStorage.setItem(PIN_SESSION_KEY, pin);
    showMsg("PIN принят.");
    await fetchStatus().then(applyVisibility);
  } catch (e) { showMsg(String(e.message || e), true); }
});

document.getElementById("clearPinBtn").addEventListener("click", async () => {
  sessionStorage.removeItem(PIN_SESSION_KEY);
  showMsg("");
  await fetchStatus().then(applyVisibility);
});

/* Manual run */
document.getElementById("addRunForm").addEventListener("submit", async (ev) => {
  ev.preventDefault();
  showMsg("");
  const fd = new FormData(ev.target);
  const body = {
    participant_id: String(fd.get("participant_id") || "").trim(),
    duration_us: Number(fd.get("duration_us") || 0),
    finished_at: String(fd.get("finished_at") || "").trim(),
    claimed: !!fd.get("claimed"),
    tournament_match_id: String(fd.get("tournament_match_id") || "").trim(),
  };
  const id = String(fd.get("id") || "").trim();
  if (id) body.id = id;
  const res = await fetch("/api/admin/runs", {
    method: "POST", headers: combineHeaders({}), body: JSON.stringify(body),
  });
  const txt = await res.text();
  showMsg(res.ok ? "Заезд добавлен: " + txt : txt, !res.ok);
});

/* Patch run */
document.getElementById("patchRunForm").addEventListener("submit", async (ev) => {
  ev.preventDefault();
  showMsg("");
  const fd = new FormData(ev.target);
  const id = String(fd.get("patch_id") || "").trim();
  let patch;
  try { patch = JSON.parse(fd.get("patch_json")); } catch (_) {
    showMsg("Тело PATCH должно быть валидным JSON", true); return;
  }
  const res = await fetch("/api/admin/runs/" + encodeURIComponent(id), {
    method: "PATCH", headers: combineHeaders({}), body: JSON.stringify(patch),
  });
  const txt = await res.text();
  showMsg(res.ok ? "OK " + txt : txt, !res.ok);
});

/* Delete run */
document.getElementById("deleteRunForm").addEventListener("submit", async (ev) => {
  ev.preventDefault();
  showMsg("");
  const fd = new FormData(ev.target);
  const id = String(fd.get("delete_id") || "").trim();
  if (!window.confirm("Удалить заезд " + id + "?")) return;
  const res = await fetch("/api/admin/runs/" + encodeURIComponent(id), {
    method: "DELETE", headers: pinHeaders(),
  });
  const txt = await res.text();
  showMsg(res.ok ? "OK " + txt : txt, !res.ok);
});

/* Reset data */
document.getElementById("resetBtn").addEventListener("click", async () => {
  showMsg("");
  if (!window.confirm("Очистить все заезды, участников и турниры?")) return;
  const res = await fetch("/api/admin/data/reset", { method: "DELETE", headers: pinHeaders() });
  const txt = await res.text();
  showMsg(res.ok ? "Сброс OK " + txt : txt, !res.ok);
});

/* Export */
document.getElementById("exportForm").addEventListener("submit", async (ev) => {
  ev.preventDefault();
  showMsg("");
  const fmt = document.getElementById("exportFmt").value;
  const res = await fetch("/api/export?format=" + encodeURIComponent(fmt), { headers: pinHeaders() });
  if (!res.ok) { showMsg((await res.text()) || "ошибка экспорта", true); return; }
  const blob = await res.blob();
  const a = document.createElement("a");
  a.href = URL.createObjectURL(blob);
  a.download = fmt === "csv" ? "cybeer-export.csv" : "cybeer-export.json";
  a.click();
  URL.revokeObjectURL(a.href);
  showMsg("Скачивание начато.");
});

/* LED settings */
document.getElementById("settingsForm").addEventListener("submit", async (ev) => {
  ev.preventDefault();
  showMsg("");
  const fd = new FormData(ev.target);
  const body = JSON.stringify({
    ledCount: Number(fd.get("ledCount")),
    brightness: Number(fd.get("brightness")),
  });
  try {
    const res = await fetch("/api/settings", { method: "PUT", headers: combineHeaders({}), body });
    const txt = await res.text();
    if (!res.ok) { showMsg(txt, true); return; }
    showMsg("Сохранено; устройство перезагружается…");
  } catch (e) { showMsg(String(e.message || e), true); }
});

/* Tournament create */
document.getElementById("tournamentCreateForm").addEventListener("submit", async (ev) => {
  ev.preventDefault();
  showMsg("");
  const name = document.getElementById("torName").value.trim();
  const checks = document.querySelectorAll('#torParticipants input[name="tor_pid"]:checked');
  const pids = Array.from(checks).map(function (c) { return c.value; });
  if (!name || pids.length < 2) {
    showMsg("Нужно имя и минимум 2 участника", true); return;
  }
  const res = await fetch("/api/admin/tournaments", {
    method: "POST", headers: combineHeaders({}),
    body: JSON.stringify({ name: name, participantIds: pids }),
  });
  const txt = await res.text();
  showMsg(res.ok ? "Турнир создан: " + txt : txt, !res.ok);
  if (res.ok) loadTournamentsList();
});

/* Tournament start */
document.getElementById("tournamentStartForm").addEventListener("submit", async (ev) => {
  ev.preventDefault();
  showMsg("");
  const tid = document.getElementById("torSelect").value;
  if (!tid) { showMsg("Выберите турнир", true); return; }
  const res = await fetch("/api/admin/tournaments/" + encodeURIComponent(tid) + "/start", {
    method: "POST", headers: pinHeaders(),
  });
  const txt = await res.text();
  showMsg(res.ok ? "Турнир запущен" : txt, !res.ok);
  if (res.ok) loadTournamentsList();
});

/* Tournament assign */
document.getElementById("tournamentAssignForm").addEventListener("submit", async (ev) => {
  ev.preventDefault();
  showMsg("");
  const mid = document.getElementById("assignMatchId").value.trim();
  const slot = document.getElementById("assignSlot").value;
  if (!mid) { showMsg("Укажите ID матча", true); return; }
  const res = await fetch("/api/admin/tournaments/matches/" + encodeURIComponent(mid) + "/assign", {
    method: "POST", headers: combineHeaders({}),
    body: JSON.stringify({ slot: slot }),
  });
  const txt = await res.text();
  showMsg(res.ok ? "Слот назначен" : txt, !res.ok);
});

/* Boot */
fetchStatus()
  .then(applyVisibility)
  .catch(function (e) { showMsg("Не удалось связаться с устройством: " + (e.message || e), true); });
```

- [ ] **Step 3: Commit**

```bash
git add frontend/admin.html frontend/admin.js
git commit -m "feat: admin — PIN verify, LED prefill, tournament management, Russian"
```

---

### Task 12: tournament.html — подсветка матчей, чемпион, русский

**Files:**
- Modify: `frontend/tournament.html`

- [ ] **Step 1: Обновить tournament.html**

Ключевые изменения в inline-скрипте `tournament.html`:

1. В `renderMatchCard`: добавить класс `active-match` если `pending && pending.matchId === m.id`
2. Чемпион: перед сеткой, если `tor.championParticipantId`, отрисовать блок `.champion-card`
3. Русский язык: заменить "Round" на "Раунд", "Loading bracket" на "Загрузка сетки", "Match" на "Матч", "Winner" на "Победитель", "No active tournament" на "Нет активного турнира", "Refreshes every 8s" на "Обновляется каждые 8 секунд", "Champion" на "Чемпион"
4. Навигация: `<nav></nav>` (пустой, заполнится из `app.js`)

Полную перезапись этого файла делать не буду в плане, т.к. он уже большой. Конкретные замены:

В `renderMatchCard`: заменить `el.className = "bracket-match" + (won ? " win" : "");` на:

```js
        const isActive = pending && pending.matchId === m.id;
        el.className = "bracket-match" + (won ? " win" : "") + (isActive ? " active-match" : "");
```

В `tick()`, после `wrap.innerHTML = "";`, добавить блок чемпиона:

```js
          const champ = tor.championParticipantId;
          if (champ) {
            const card = document.createElement("div");
            card.className = "champion-card";
            card.innerHTML = '<div style="font-size:0.9rem;color:var(--muted)">🏆 Чемпион</div><div class="champion-name">' + pname(champ) + '</div>';
            wrap.parentElement.insertBefore(card, wrap);
          }
```

Заменить английские строки на русские.

Добавить `<script src="/app.js"></script>` перед inline `<script>` и установить `window.__CYBEER_PAGE__ = "tournament";`.

- [ ] **Step 2: Commit**

```bash
git add frontend/tournament.html
git commit -m "feat: tournament.html — active match highlight, champion card, Russian"
```

---

### Task 13: spectator.html — новая страница

**Files:**
- Create: `frontend/spectator.html`

- [ ] **Step 1: Создать spectator.html**

```html
<!DOCTYPE html>
<html lang="ru">
<head>
  <meta charset="UTF-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <title>CyBeer — Spectator</title>
  <link rel="stylesheet" href="/app.css" />
  <style>
    body { overflow: hidden; cursor: default; }
    .spec-wrap { display: flex; flex-direction: column; height: 100vh; padding: 1rem; }
    .spec-timer { flex: 0 0 auto; text-align: center; padding: 2rem 0; }
    .spec-timer .timer-value { font-size: min(12vw, 10rem); font-weight: 700; font-variant-numeric: tabular-nums; color: var(--gold); line-height: 1; }
    .spec-timer .timer-label { font-size: 1.5rem; color: var(--muted); margin-top: 0.5rem; }
    .spec-timer.state-prep .timer-value { color: var(--ok); }
    .spec-timer.state-running .timer-value { color: var(--amber); animation: pulse-amber 1s ease-in-out infinite; }
    .spec-timer.state-finished .timer-value { color: var(--gold); }
    .spec-content { flex: 1 1 auto; overflow: hidden; }
    .spec-lb table { width: 100%; font-size: min(3vw, 1.5rem); }
    .spec-lb th, .spec-lb td { padding: 0.6rem 0.8rem; }
    .spec-lb td:first-child { font-weight: 700; color: var(--amber); width: 3rem; }
    .spec-controls { position: fixed; top: 1rem; right: 1rem; display: flex; gap: 0.5rem; z-index: 100; opacity: 0.3; transition: opacity 0.3s; }
    .spec-controls:hover { opacity: 1; }
    .spec-btn { background: var(--stout); border: 1px solid var(--amber); border-radius: 8px; padding: 0.4rem 0.8rem; color: var(--amber); cursor: pointer; font-size: 0.85rem; }
    .spec-btn.active { background: var(--amber); color: var(--bg); }

    .spec-bracket { display: flex; flex-direction: row; gap: 1rem; align-items: stretch; overflow-x: auto; height: 100%; }
    .spec-bracket .bracket-col { min-width: 14rem; }
    .spec-bracket .bracket-match { font-size: min(2.5vw, 1.1rem); }
  </style>
</head>
<body>
  <div class="spec-wrap">
    <div class="spec-timer" id="specTimer">
      <div class="timer-value" id="timerValue">0.00с</div>
      <div class="timer-label" id="timerLabel">Готов к старту</div>
    </div>
    <div class="spec-content" id="specContent">
      <div class="spec-lb" id="specLeaderboard">
        <table class="leaderboard">
          <thead><tr><th>#</th><th>Имя</th><th>Время</th></tr></thead>
          <tbody id="leaderboardBody"></tbody>
        </table>
      </div>
      <div class="spec-bracket" id="specBracket" style="display:none"></div>
    </div>
  </div>

  <div class="spec-controls">
    <button class="spec-btn" id="modeToggle">Турнир</button>
    <button class="spec-btn" id="soundToggle" title="Звук">🔇</button>
  </div>

  <script>window.__CYBEER_PAGE__ = "spectator";</script>
  <script src="/app.js"></script>
  <script>
  (function () {
    var cy = window.__cybeer;
    var params = new URLSearchParams(location.search);
    var mode = params.get("mode") || "leaderboard";
    var lbDiv = document.getElementById("specLeaderboard");
    var brDiv = document.getElementById("specBracket");
    var modeBtn = document.getElementById("modeToggle");
    var soundBtn = document.getElementById("soundToggle");

    function applyMode() {
      if (mode === "tournament") {
        lbDiv.style.display = "none";
        brDiv.style.display = "flex";
        modeBtn.textContent = "Лидерборд";
        modeBtn.classList.add("active");
        loadBracket();
      } else {
        lbDiv.style.display = "";
        brDiv.style.display = "none";
        modeBtn.textContent = "Турнир";
        modeBtn.classList.remove("active");
        cy.loadLeaderboard();
      }
    }

    modeBtn.addEventListener("click", function () {
      mode = mode === "leaderboard" ? "tournament" : "leaderboard";
      history.replaceState(null, "", "?mode=" + mode);
      applyMode();
    });

    soundBtn.addEventListener("click", function () {
      cy.enableSound();
      soundBtn.textContent = "🔊";
      soundBtn.classList.add("active");
    });

    /* Timer hero for spectator */
    var specTimer = document.getElementById("specTimer");
    window.__cybeer.updateTimerHero = function (state, durationUs) {
      var val = document.getElementById("timerValue");
      var lbl = document.getElementById("timerLabel");
      if (!specTimer || !val) return;
      specTimer.className = "spec-timer state-" + state.toLowerCase();
      var labels = { PREP: "Готов к старту", RUNNING: "", FINISHED: "Ожидает заявки", READY: "Последний результат" };
      if (lbl) lbl.textContent = labels[state] || "";
      if (state === "PREP") val.textContent = "0.00с";
      else if (typeof durationUs === "number") val.textContent = cy.formatDuration(durationUs);
    };

    /* Tournament bracket (simplified) */
    var nameByPid = cy.nameByPid;

    function shortId(s) {
      if (!s || typeof s !== "string") return "—";
      return s.length > 14 ? s.slice(0, 6) + "…" + s.slice(-4) : s;
    }

    function pname(pid) {
      if (!pid) return "bye";
      return nameByPid.get(pid) || shortId(pid);
    }

    async function loadBracket() {
      try {
        await cy.loadParticipants();
        var r = await fetch("/api/tournaments/active");
        var data = await r.json();
        var tor = data.tournament;
        var pending = data.pendingNextRun || null;
        brDiv.innerHTML = "";
        if (!tor || !Array.isArray(tor.matches)) {
          brDiv.innerHTML = '<p style="color:var(--muted);padding:2rem">Нет активного турнира</p>';
          return;
        }
        var cols = {};
        for (var mi = 0; mi < tor.matches.length; mi++) {
          var m = tor.matches[mi];
          var c = typeof m.column === "number" ? m.column : 0;
          if (!cols[c]) cols[c] = [];
          cols[c].push(m);
        }
        var order = Object.keys(cols).map(Number).sort(function (a, b) { return b - a; });
        for (var oi = 0; oi < order.length; oi++) {
          var col = order[oi];
          var colEl = document.createElement("div");
          colEl.className = "bracket-col";
          var h = document.createElement("h3");
          h.textContent = "Раунд " + (oi + 1);
          h.style.color = "var(--muted)";
          colEl.appendChild(h);
          var list = cols[col].slice().sort(function (a, b) { return (a.index || 0) - (b.index || 0); });
          for (var li = 0; li < list.length; li++) {
            var mm = list[li];
            var won = !!(mm.winnerParticipantId && String(mm.winnerParticipantId).length);
            var isActive = pending && pending.matchId === mm.id;
            var el = document.createElement("article");
            el.className = "bracket-match" + (won ? " win" : "") + (isActive ? " active-match" : "");
            el.innerHTML =
              '<div style="color:var(--muted);font-size:0.75rem">Матч ' + shortId(mm.id) + "</div>" +
              '<div style="margin:4px 0"><span style="color:var(--gold)">A</span> ' + pname(mm.participantAId) + "</div>" +
              '<div><span style="color:var(--gold)">B</span> ' + pname(mm.participantBId) + "</div>" +
              (won ? '<div style="margin-top:4px;color:var(--ok)">Победитель: ' + pname(mm.winnerParticipantId) + "</div>" : "");
            colEl.appendChild(el);
          }
          brDiv.appendChild(colEl);
        }
      } catch (_) {
        brDiv.innerHTML = '<p style="color:var(--muted);padding:2rem">Ошибка загрузки</p>';
      }
    }

    /* Auto-refresh bracket */
    if (mode === "tournament") setInterval(loadBracket, 8000);

    /* Hide cursor after 3s of inactivity */
    var cursorTimer;
    document.addEventListener("mousemove", function () {
      document.body.style.cursor = "default";
      clearTimeout(cursorTimer);
      cursorTimer = setTimeout(function () { document.body.style.cursor = "none"; }, 3000);
    });

    /* Boot */
    cy.connectLiveWs();
    applyMode();
    fetch("/api/status").then(function (r) { return r.json(); }).then(function (st) {
      cy.updateTimerHero(st.state || "PREP", null);
    }).catch(function () {});
  })();
  </script>
</body>
</html>
```

- [ ] **Step 2: Commit**

```bash
git add frontend/spectator.html
git commit -m "feat: spectator.html — TV mode with leaderboard/tournament toggle, sound"
```

---

### Task 14: Финальная сборка и проверка

- [ ] **Step 1: Полная сборка прошивки**

```bash
cd firmware && idf.py build
```

Ожидаем: сборка проходит без ошибок. LittleFS-образ пересоздаётся с новыми frontend-файлами.

- [ ] **Step 2: Проверить наличие всех файлов в frontend_dist**

```bash
ls firmware/frontend_dist/www/
```

Ожидаем: `index.html`, `claim.html`, `player.html`, `admin.html`, `tournament.html`, `spectator.html`, `setup.html`, `app.js`, `admin.js`, `app.css`.

- [ ] **Step 3: Commit всё вместе (если есть незакоммиченные файлы)**

```bash
git status
git add -A
git commit -m "chore: final build verification"
```

---

## Self-review checklist

- [x] **Spec coverage:** все пункты 1.1–1.10, 2.1–2.8 покрыты задачами
- [x] **NTP (1.2):** уже реализован — пропущен, отмечено в header
- [x] **Placeholder scan:** нет TBD/TODO
- [x] **Type consistency:** `formatDuration`, `loadLeaderboard`, `updateTimerHero`, `connectLiveWs` — одинаковые имена во всех файлах; API endpoints совпадают со спеком
- [x] **Файлы:** все пути точные, проверены по реальной структуре проекта
