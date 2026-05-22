# WiFi Fallback + Admin WiFi Panel + Mobile Admin UX — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make ESP start in STA-only mode with fallback to AP after 5 failures, add WiFi management to admin panel, and make admin forms mobile-friendly.

**Architecture:** Firmware WiFi logic changes from always-APSTA to STA-first with runtime fallback. Frontend gets a new WiFi section in admin panel, accordion layout for all sections, and mobile media queries. One new field added to `/api/status` (SSID + RSSI).

**Tech Stack:** ESP-IDF (C), vanilla HTML/CSS/JS, LittleFS

---

## File Structure

| File | Action | Responsibility |
|------|--------|----------------|
| `firmware/components/cybeer_wifi/cybeer_wifi.c` | Modify | STA-only boot, fallback AP activation after 5 failures |
| `firmware/components/cybeer_wifi/include/cybeer_wifi.h` | Modify | New public API: `cybeer_wifi_ap_is_fallback()` |
| `firmware/components/cybeer_web/cybeer_web.c` | Modify | Add `ssid`, `rssi`, `apFallback` to `/api/status` wifi object |
| `frontend/admin.html` | Modify | WiFi section, `<details>/<summary>` accordions, remove textarea `cols` |
| `frontend/admin.js` | Modify | WiFi functions: status, scan, connect, forget |
| `frontend/app.css` | Modify | `<details>/<summary>` styles, `@media (max-width: 600px)` block |

---

### Task 1: WiFi Fallback — STA-only Boot with AP Fallback

**Files:**
- Modify: `firmware/components/cybeer_wifi/cybeer_wifi.c:578-664` (`cybeer_wifi_start()`)
- Modify: `firmware/components/cybeer_wifi/cybeer_wifi.c:58-93` (`schedule_sta_reconnect()`)
- Modify: `firmware/components/cybeer_wifi/cybeer_wifi.c:152-182` (`wifi_event()`)
- Modify: `firmware/components/cybeer_wifi/include/cybeer_wifi.h`

- [ ] **Step 1: Add fallback state variable and threshold constant**

In `cybeer_wifi.c`, add after line 51 (`static int s_sta_retry_count;`):

```c
#define STA_FALLBACK_AP_THRESHOLD 5
static bool s_fallback_ap_active;
```

- [ ] **Step 2: Create `activate_fallback_ap()` function**

Add new static function before `cybeer_wifi_start()` (around line 570). This function switches from STA-only to APSTA mode at runtime:

```c
static esp_err_t activate_fallback_ap(void)
{
    if (s_fallback_ap_active) {
        return ESP_OK;
    }

    ESP_LOGW(TAG, "STA failed %d times, activating fallback AP", STA_FALLBACK_AP_THRESHOLD);

    if (s_ap_netif == NULL) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        if (s_ap_netif == NULL) {
            ESP_LOGE(TAG, "fallback: ap netif create failed");
            return ESP_FAIL;
        }
        esp_err_t err = ap_netif_configure_dhcps(s_ap_netif);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "fallback: dhcps config failed: %s", esp_err_to_name(err));
            return err;
        }
    }

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "fallback: mode apsta");

    build_ap_ssid(s_ap_ssid, sizeof(s_ap_ssid));
    wifi_config_t ap_cfg = { 0 };
    strlcpy((char *)ap_cfg.ap.ssid, s_ap_ssid, sizeof(ap_cfg.ap.ssid));
    ap_cfg.ap.ssid_len = (uint8_t)strlen((char *)ap_cfg.ap.ssid);
    ap_cfg.ap.channel = 1;
    ap_cfg.ap.max_connection = 10;
    ap_cfg.ap.beacon_interval = 100;
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    ap_cfg.ap.password[0] = '\0';

    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg), TAG, "fallback: cfg ap");
    (void)esp_wifi_set_inactive_time(WIFI_IF_AP, 65535);

    start_captive_dns_task();
    cybeer_led_set_fx(CYBEER_LED_FX_WIFI_SETUP);

    s_fallback_ap_active = true;
    ESP_LOGI(TAG, "Fallback AP active: SSID \"%s\" @ 192.168.4.1", s_ap_ssid);
    return ESP_OK;
}
```

- [ ] **Step 3: Modify `schedule_sta_reconnect()` to trigger fallback**

Replace the current `schedule_sta_reconnect()` function. The only change is adding the fallback check after incrementing `s_sta_retry_count`:

```c
static void schedule_sta_reconnect(void)
{
    uint32_t delay_ms = STA_RECONNECT_BASE_MS << s_sta_retry_count;
    if (delay_ms > STA_RECONNECT_MAX_DELAY_MS) {
        delay_ms = STA_RECONNECT_MAX_DELAY_MS;
    }
    s_sta_retry_count++;

    if (s_sta_retry_count >= STA_FALLBACK_AP_THRESHOLD && !s_fallback_ap_active) {
        (void)activate_fallback_ap();
    }

    if (s_sta_reconnect_timer == NULL) {
        const esp_timer_create_args_t args = {
            .callback = &sta_reconnect_timer_cb,
            .name = "sta_reconn",
        };
        if (esp_timer_create(&args, &s_sta_reconnect_timer) != ESP_OK) {
            ESP_LOGE(TAG, "STA reconnect timer create failed");
            (void)esp_wifi_connect();
            return;
        }
    }

    (void)esp_timer_stop(s_sta_reconnect_timer);
    esp_err_t err = esp_timer_start_once(s_sta_reconnect_timer, (uint64_t)delay_ms * 1000ULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "STA reconnect timer start failed: %s", esp_err_to_name(err));
        (void)esp_wifi_connect();
        return;
    }
    ESP_LOGI(TAG, "STA reconnect scheduled in %lu ms", (unsigned long)delay_ms);
}
```

- [ ] **Step 4: Modify `cybeer_wifi_start()` — STA-only when credentials exist**

Replace the `have_sta` branch in `cybeer_wifi_start()`. When STA credentials exist, start in STA-only mode (no AP, no captive DNS):

Replace the block from `s_ap_netif = esp_netif_create_default_wifi_ap();` through `start_captive_dns_task();` with:

```c
    if (have_sta) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
        ESP_RETURN_ON_FALSE(s_sta_netif != NULL, ESP_FAIL, TAG, "sta netif");

        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "mode sta");

        wifi_config_t sta_cfg = { 0 };
        strlcpy((char *)sta_cfg.sta.ssid, sta_ssid, sizeof(sta_cfg.sta.ssid));
        strlcpy((char *)sta_cfg.sta.password, sta_pass, sizeof(sta_cfg.sta.password));
        sta_cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
        if (sta_pass[0] == '\0') {
            sta_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
        } else {
            sta_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        }

        ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg), TAG, "cfg sta");
    } else {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        ESP_RETURN_ON_FALSE(s_ap_netif != NULL, ESP_FAIL, TAG, "ap netif");
        ESP_RETURN_ON_ERROR(ap_netif_configure_dhcps(s_ap_netif), TAG, "ap ip/dhcp");

        wifi_config_t ap_cfg = { 0 };
        build_ap_ssid(s_ap_ssid, sizeof(s_ap_ssid));
        strlcpy((char *)ap_cfg.ap.ssid, s_ap_ssid, sizeof(ap_cfg.ap.ssid));
        ap_cfg.ap.ssid_len = (uint8_t)strlen((char *)ap_cfg.ap.ssid);
        ap_cfg.ap.channel = 1;
        ap_cfg.ap.max_connection = 10;
        ap_cfg.ap.beacon_interval = 100;
        ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
        ap_cfg.ap.password[0] = '\0';

        s_sta_netif = NULL;
        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "mode ap");
        ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg), TAG, "cfg ap");
    }

    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG, "wifi_ps_none");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi_start");

    if (have_sta) {
        ESP_LOGI(TAG, "STA-only mode, joining: %s", sta_ssid);
    } else {
        (void)esp_wifi_set_inactive_time(WIFI_IF_AP, 65535);
        ESP_LOGI(TAG, "SoftAP SSID:%s IP 192.168.4.1/24 (open)", s_ap_ssid);
        ESP_LOGI(TAG, "Provisioning mode (no STA credentials)");
        cybeer_led_set_fx(CYBEER_LED_FX_WIFI_SETUP);
        start_captive_dns_task();
    }
```

- [ ] **Step 5: Add public API `cybeer_wifi_ap_is_fallback()`**

In `cybeer_wifi.c`, add after `cybeer_wifi_sta_credentials_configured()`:

```c
bool cybeer_wifi_ap_is_fallback(void)
{
    return s_fallback_ap_active;
}
```

In `cybeer_wifi.h`, add after line 14:

```c
/** True if AP was activated as fallback due to STA connection failure. */
bool cybeer_wifi_ap_is_fallback(void);
```

- [ ] **Step 6: Build firmware to verify compilation**

Run: `cd firmware && idf.py build`
Expected: Build succeeds with no errors.

- [ ] **Step 7: Commit**

```bash
git add firmware/components/cybeer_wifi/cybeer_wifi.c firmware/components/cybeer_wifi/include/cybeer_wifi.h
git commit -m "feat(wifi): STA-only boot with fallback AP after 5 failures"
```

---

### Task 2: Add SSID and RSSI to `/api/status`

**Files:**
- Modify: `firmware/components/cybeer_web/cybeer_web.c:68-111` (`h_get_status`)

- [ ] **Step 1: Add SSID and RSSI fields to wifi JSON object**

In `h_get_status()`, after the line `cJSON_AddStringToObject(wifi, "staIp", sta_ip);` (line 94), add:

```c
    wifi_ap_record_t ap_info;
    if (cybeer_wifi_sta_connected() && esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        cJSON_AddStringToObject(wifi, "ssid", (const char *)ap_info.ssid);
        cJSON_AddNumberToObject(wifi, "rssi", (double)ap_info.rssi);
    } else {
        cJSON_AddStringToObject(wifi, "ssid", "");
        cJSON_AddNumberToObject(wifi, "rssi", 0);
    }
    cJSON_AddBoolToObject(wifi, "apFallback", cybeer_wifi_ap_is_fallback());
```

The `esp_wifi.h` header is already included via `cybeer_wifi.h`. The type `wifi_ap_record_t` is defined in `esp_wifi_types.h` which is included transitively.

- [ ] **Step 2: Build firmware to verify compilation**

Run: `cd firmware && idf.py build`
Expected: Build succeeds. `/api/status` now returns `wifi.ssid`, `wifi.rssi`, `wifi.apFallback`.

- [ ] **Step 3: Commit**

```bash
git add firmware/components/cybeer_web/cybeer_web.c
git commit -m "feat(api): add ssid, rssi, apFallback to /api/status"
```

---

### Task 3: Admin HTML — WiFi Section + Accordions

**Files:**
- Modify: `frontend/admin.html`

- [ ] **Step 1: Replace admin panel content with accordion layout + WiFi section**

Replace the entire `<section class="panel" id="adminPanel">` block in `admin.html` with:

```html
    <section class="panel" id="adminPanel">
      <details id="wifiSection" open>
        <summary><h2>WiFi</h2></summary>
        <div id="wifiStatus" class="wifi-status">
          <p>Загрузка...</p>
        </div>
        <button type="button" id="wifiScanBtn" class="btn-scan">Сканировать сети</button>
        <label>Сеть
          <select id="wifiSsidSelect" disabled>
            <option value="">— сначала сканируйте —</option>
          </select>
        </label>
        <label>Пароль
          <input type="password" id="wifiPassInput" placeholder="пусто для открытой сети" />
        </label>
        <button type="button" id="wifiConnectBtn" disabled>Подключить</button>
        <button type="button" id="wifiForgetBtn" class="danger">Забыть сеть</button>
      </details>

      <details>
        <summary><h2>PIN админки</h2></summary>
        <p class="hint">На устройстве по умолчанию <strong>1111</strong>. PIN хранится в браузере и отправляется с каждым запросом.</p>
        <form id="changePinForm">
          <label>Новый PIN (4–32 символа)
            <input type="password" id="newPinInput" minlength="4" maxlength="32" autocomplete="new-password" required />
          </label>
          <button type="submit">Сменить PIN на устройстве</button>
        </form>
        <button type="button" id="resetPinBtn" class="danger">Сбросить PIN на устройстве → 1111</button>
      </details>

      <details>
        <summary><h2>Ручной заезд</h2></summary>
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
      </details>

      <details>
        <summary><h2>Редактировать / удалить заезд</h2></summary>
        <form id="patchRunForm">
          <label>ID заезда
            <input type="text" name="patch_id" maxlength="38" required />
          </label>
          <textarea name="patch_json" rows="5" spellcheck="false" placeholder='Частичный JSON, напр. {"participant_id":"","claimed":true}'></textarea>
          <button type="submit">PATCH</button>
        </form>
        <form id="deleteRunForm">
          <label>ID заезда
            <input type="text" name="delete_id" maxlength="38" required />
          </label>
          <button type="submit" class="danger">Удалить</button>
        </form>
      </details>

      <details>
        <summary><h2>Данные и экспорт</h2></summary>
        <button type="button" id="resetBtn">Очистить все заезды и участников</button>
        <form id="exportForm">
          <label>Формат</label>
          <select id="exportFmt">
            <option value="json">JSON</option>
            <option value="csv">CSV</option>
          </select>
          <button type="submit">Скачать</button>
        </form>
      </details>

      <details>
        <summary><h2>LED-лента</h2></summary>
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
      </details>

      <details>
        <summary><h2>Турнир</h2></summary>
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
      </details>
    </section>
```

Key changes vs current HTML:
- Each `<h2>` section wrapped in `<details>/<summary>`
- WiFi section added as first `<details>` with `open` attribute
- `textarea` no longer has `cols="72"`
- "Данные" and "Экспорт" merged into one section

- [ ] **Step 2: Commit**

```bash
git add frontend/admin.html
git commit -m "feat(admin): WiFi section + accordion layout"
```

---

### Task 4: Admin JS — WiFi Functions

**Files:**
- Modify: `frontend/admin.js`

- [ ] **Step 1: Add WiFi management functions**

Add the following block before the existing `initAdminPanel()` function (before line 54):

```javascript
async function loadWifiStatus() {
  try {
    const cfg = await fetchStatus();
    const el = document.getElementById("wifiStatus");
    if (!el || !cfg.wifi) return;

    const w = cfg.wifi;
    let html = "";
    if (w.sta && w.ssid) {
      html += '<p><strong>Сеть:</strong> ' + w.ssid + '</p>';
      html += '<p><strong>IP:</strong> ' + (w.staIp || "—") + '</p>';
      if (w.rssi) html += '<p><strong>Сигнал:</strong> ' + w.rssi + ' dBm</p>';
    } else {
      html += '<p>Не подключена к WiFi</p>';
    }
    if (w.apFallback) {
      html += '<p class="hint" style="color:var(--amber)">Режим: Fallback AP</p>';
    }
    el.innerHTML = html;

    const sec = document.getElementById("wifiSection");
    if (sec && w.sta) sec.removeAttribute("open");
  } catch (_) {}
}

function scanWifi() {
  const btn = document.getElementById("wifiScanBtn");
  const sel = document.getElementById("wifiSsidSelect");
  const connectBtn = document.getElementById("wifiConnectBtn");
  btn.disabled = true;
  btn.textContent = "Сканирование...";
  showMsg("");

  fetch("/api/setup/scan")
    .then(function (r) { return r.json(); })
    .then(function (list) {
      sel.innerHTML = "";
      if (!Array.isArray(list) || list.length === 0) {
        sel.innerHTML = '<option value="">(сети не найдены)</option>';
        sel.disabled = true;
        connectBtn.disabled = true;
      } else {
        for (var i = 0; i < list.length; i++) {
          var o = document.createElement("option");
          o.value = list[i].ssid;
          o.textContent = list[i].ssid + " (" + list[i].rssi + " dBm, " + list[i].auth + ")";
          sel.appendChild(o);
        }
        sel.disabled = false;
        connectBtn.disabled = false;
      }
    })
    .catch(function (e) {
      showMsg("Ошибка сканирования: " + e, true);
    })
    .finally(function () {
      btn.disabled = false;
      btn.textContent = "Сканировать сети";
    });
}

function connectWifi() {
  var sel = document.getElementById("wifiSsidSelect");
  var pass = document.getElementById("wifiPassInput");
  var btn = document.getElementById("wifiConnectBtn");
  var ssid = sel.value;
  if (!ssid) { showMsg("Выберите сеть", true); return; }
  btn.disabled = true;
  showMsg("Подключение...");

  fetch("/api/setup/wifi", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ ssid: ssid, password: pass.value })
  })
    .then(function (r) { return r.json(); })
    .then(function (j) {
      if (j.ok) {
        showMsg("Подключено! Устройство перезагружается...");
      } else {
        showMsg("Ошибка: " + (j.error || "неизвестная"), true);
        btn.disabled = false;
      }
    })
    .catch(function (e) {
      showMsg("Ошибка: " + e, true);
      btn.disabled = false;
    });
}

function forgetWifi() {
  if (!window.confirm("Забыть текущую WiFi сеть? Устройство перезагрузится в режим точки доступа.")) return;
  showMsg("");
  fetch("/api/admin/wifi/forget", { method: "POST", headers: pinHeaders() })
    .then(function (r) {
      if (r.ok) {
        showMsg("Сеть забыта, устройство перезагружается...");
      } else {
        return r.text().then(function (t) { showMsg(t || "Ошибка", true); });
      }
    })
    .catch(function (e) { showMsg("Ошибка: " + e, true); });
}
```

- [ ] **Step 2: Wire up WiFi button event listeners**

Add after the `forgetWifi` function:

```javascript
document.getElementById("wifiScanBtn").addEventListener("click", scanWifi);
document.getElementById("wifiConnectBtn").addEventListener("click", connectWifi);
document.getElementById("wifiForgetBtn").addEventListener("click", forgetWifi);
```

- [ ] **Step 3: Add `loadWifiStatus()` to initialization**

In `initAdminPanel()` (currently at line 54), add the WiFi status call:

```javascript
function initAdminPanel() {
  loadWifiStatus();
  prefillLedSettingsFromStatus();
  loadTournamentParticipants();
  loadTournamentsList();
}
```

- [ ] **Step 4: Commit**

```bash
git add frontend/admin.js
git commit -m "feat(admin): WiFi scan, connect, forget functions"
```

---

### Task 5: CSS — Accordion Styles + Mobile Media Queries

**Files:**
- Modify: `frontend/app.css`

- [ ] **Step 1: Add `<details>/<summary>` styles**

Append to `app.css`, before any media queries (at the end of file, after `.sound-toggle.active`):

```css
/* === Accordion (details/summary) === */
details {
  border-bottom: 1px solid var(--stout);
  padding-bottom: 0.5rem;
  margin-bottom: 0.5rem;
}

details:last-child {
  border-bottom: none;
}

details summary {
  cursor: pointer;
  list-style: none;
  display: flex;
  align-items: center;
  gap: 0.5rem;
  padding: 0.5rem 0;
  user-select: none;
}

details summary::-webkit-details-marker {
  display: none;
}

details summary::before {
  content: "▶";
  font-size: 0.7rem;
  color: var(--amber);
  transition: transform 0.2s;
}

details[open] summary::before {
  transform: rotate(90deg);
}

details summary h2 {
  margin: 0;
  font-size: 1.15rem;
}

/* === WiFi status block === */
.wifi-status {
  background: var(--bg);
  border: 1px solid var(--stout);
  border-radius: 8px;
  padding: 0.75rem;
  margin-bottom: 0.75rem;
  font-size: 0.9rem;
}

.wifi-status p {
  margin: 0.2rem 0;
}
```

- [ ] **Step 2: Add mobile media queries**

Append to `app.css` at the very end:

```css
/* === Mobile === */
@media (max-width: 600px) {
  main {
    padding: 0.75rem;
  }

  .panel {
    padding: 0.75rem;
    border-radius: 8px;
  }

  .topbar {
    padding: 0.75rem;
    gap: 0.5rem;
  }

  label {
    display: block;
    font-size: 0.95rem;
    margin-top: 0.75rem;
  }

  input[type="text"],
  input[type="password"],
  input[type="number"],
  select,
  textarea {
    width: 100%;
    padding: 0.6rem;
    font-size: 1rem;
    border-radius: 6px;
    border: 1px solid var(--stout);
    background: var(--bg);
    color: var(--foam);
  }

  button {
    width: 100%;
    min-height: 44px;
    font-size: 1rem;
    margin-top: 0.5rem;
    padding: 0.6rem;
    border-radius: 8px;
    border: 1px solid var(--amber);
    background: var(--stout);
    color: var(--foam);
    cursor: pointer;
  }

  button[type="submit"] {
    background: linear-gradient(180deg, var(--amber), #a06320);
    color: #1a120c;
    font-weight: 700;
    border: none;
  }

  button.danger {
    border-color: var(--danger);
    color: var(--danger);
  }

  .btn-scan {
    background: var(--stout);
    border-color: var(--ok);
    color: var(--ok);
  }

  details summary h2 {
    font-size: 1.05rem;
  }

  .participant-checkboxes {
    gap: 0.4rem;
  }

  .participant-checkboxes label {
    font-size: 0.8rem;
    padding: 0.3rem 0.5rem;
    margin-top: 0;
  }
}
```

- [ ] **Step 3: Commit**

```bash
git add frontend/app.css
git commit -m "feat(css): accordion styles + mobile media queries for admin"
```

---

### Task 6: Sync Frontend + Final Build

- [ ] **Step 1: Verify frontend files are consistent**

Check that `frontend/admin.html`, `frontend/admin.js`, and `frontend/app.css` have no syntax issues by opening them in a browser (connect to device or use a local HTTP server).

- [ ] **Step 2: Full firmware build (syncs frontend → LittleFS)**

Run: `cd firmware && idf.py build`
Expected: Build succeeds. The build pipeline auto-syncs `frontend/` → `firmware/frontend_dist/www/` → LittleFS image.

- [ ] **Step 3: Flash and test**

Run: `cd firmware && idf.py flash monitor`

Manual verification checklist:
1. Boot with STA credentials → device starts in STA-only mode (no AP visible in WiFi scan from phone)
2. If STA network is unreachable → after ~30-60s, `CyBeer-XXXXXX` AP appears, blue LED pulses
3. Open `http://192.168.4.1/admin` → WiFi section visible at top, accordion sections work
4. Scan networks → list appears in dropdown
5. Connect to a network → device reboots, connects
6. On phone: admin page forms are touch-friendly, sections collapse/expand

- [ ] **Step 4: Final commit if any fixes needed**

```bash
git add -A
git commit -m "fix: post-test adjustments"
```
