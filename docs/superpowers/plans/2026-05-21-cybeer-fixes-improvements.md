# CyBeer Fixes & Improvements Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix critical bugs (UAF, claim flow, provisioning page), improve reliability (WS state, STA reconnect, header fix), and add quality-of-life features (SNTP, leaderboard, ping/pong, LED setup, hybrid claim UI).

**Architecture:** Mostly local fixes within existing `cybeer_*` components. One new embedded HTML page for Wi-Fi setup. New `/api/setup/scan` and `/api/leaderboard` endpoints. Frontend claim page reworked to hybrid dropdown+input.

**Tech Stack:** ESP-IDF 5.x, C, cJSON, esp_wifi scan API, esp_sntp, vanilla HTML/JS frontend.

**Spec:** `docs/superpowers/specs/2026-05-21-cybeer-fixes-improvements.md`

---

## File Map (created/modified by end of plan)

| Path | Change | Responsibility |
|------|--------|----------------|
| `firmware/components/cybeer_web/cybeer_web.c` | Modify | Fix UAF, add leaderboard endpoint |
| `firmware/components/cybeer_storage/cybeer_storage.c` | Modify | Auto-create participant on claim |
| `firmware/components/cybeer_storage/include/cybeer_storage.h` | Modify | Add cJSON forward decl, new function signature |
| `firmware/components/cybeer_web/cybeer_ws.c` | Modify | Initial state on connect, WS ping |
| `firmware/components/cybeer_wifi/cybeer_wifi.c` | Modify | STA reconnect, SNTP init, scan endpoint, serve setup.html, LED setup |
| `firmware/components/cybeer_wifi/include/cybeer_wifi.h` | Modify | Declare new scan handler |
| `firmware/components/cybeer_wifi/CMakeLists.txt` | Modify | Add cybeer_led dependency |
| `firmware/components/cybeer_web/cybeer_setup_html.h` | Create | Embedded setup.html as const char[] |
| `frontend/setup.html` | Create | Source for provisioning page |
| `frontend/claim.html` | Modify | Hybrid dropdown + input UI |
| `frontend/app.js` | Modify | Claim logic rework |
| `README.md` | Modify | Frontend build pipeline docs |

---

## Phase 1 — Critical Bug Fixes

### Task 1: Fix Use-After-Free in PUT /api/settings

**Files:**
- Modify: `firmware/components/cybeer_web/cybeer_web.c` (function `h_put_settings`, ~line 652-689)

- [ ] **Step 1: Identify the bug location**

Open `firmware/components/cybeer_web/cybeer_web.c` and find function `h_put_settings`. The bug is at lines 666-674:

```c
const cJSON *jlc = cJSON_GetObjectItemCaseSensitive(root, "ledCount");
const cJSON *jbr = cJSON_GetObjectItemCaseSensitive(root, "brightness");
cJSON_Delete(root);  // BUG: frees jlc and jbr

if (!cJSON_IsNumber(jlc) || !cJSON_IsNumber(jbr)) {  // UAF
```

- [ ] **Step 2: Fix — move cJSON_Delete after value extraction**

Replace the section in `h_put_settings` (from the `cJSON_GetObjectItemCaseSensitive` calls through the range check) with:

```c
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
```

- [ ] **Step 3: Verify build**

```bash
cd firmware
idf.py build
```

Expected: `Project build complete. To flash, run: idf.py flash`

- [ ] **Step 4: Commit**

```bash
git add firmware/components/cybeer_web/cybeer_web.c
git commit -m "fix: use-after-free in PUT /api/settings"
```

---

### Task 2: Auto-create participant on claim by name

**Files:**
- Modify: `firmware/components/cybeer_storage/cybeer_storage.c` (function `cybeer_storage_claim_run`)

- [ ] **Step 1: Add create_participant_by_name_locked helper**

Add this function in `cybeer_storage.c` right after the existing `resolve_participant_by_name_locked` function (around line 500):

```c
static esp_err_t create_participant_by_name_locked(const char *name, char pid_out[37])
{
    cJSON *parts = parse_array_file_locked(PATH_PARTICIPANTS);
    if (!parts) {
        parts = cJSON_CreateArray();
        if (!parts) {
            return ESP_ERR_NO_MEM;
        }
    }

    cybeer_format_uuid_v4(pid_out);

    cJSON *p = cJSON_CreateObject();
    if (!p) {
        cJSON_Delete(parts);
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(p, "id", pid_out);
    cJSON_AddStringToObject(p, "name", name);
    char ts[32];
    cybeer_storage_iso8601_now(ts);
    cJSON_AddStringToObject(p, "createdAt", ts);
    cJSON_AddItemToArray(parts, p);

    esp_err_t err = persist_json_locked(PATH_PARTICIPANTS, parts);
    cJSON_Delete(parts);
    return err;
}
```

- [ ] **Step 2: Modify cybeer_storage_claim_run to auto-create on NOT_FOUND**

In `cybeer_storage_claim_run`, replace the block after `resolve_participant_by_name_locked`:

Current code (around line 513-518):
```c
    if (lookup != ESP_OK) {
        give_mtx();
        return lookup;
    }
```

Replace with:
```c
    if (lookup == ESP_ERR_NOT_FOUND) {
        lookup = create_participant_by_name_locked(name_or_pid, pid);
    }
    if (lookup != ESP_OK) {
        give_mtx();
        return lookup;
    }
```

- [ ] **Step 3: Verify build**

```bash
cd firmware
idf.py build
```

Expected: `Project build complete.`

- [ ] **Step 4: Commit**

```bash
git add firmware/components/cybeer_storage/cybeer_storage.c
git commit -m "fix: auto-create participant when claiming by new name"
```

---

### Task 3: Wi-Fi provisioning page with scan

**Files:**
- Create: `frontend/setup.html`
- Create: `firmware/components/cybeer_web/cybeer_setup_html.h`
- Modify: `firmware/components/cybeer_wifi/cybeer_wifi.c`
- Modify: `firmware/components/cybeer_wifi/include/cybeer_wifi.h`

- [ ] **Step 1: Create frontend/setup.html**

Create `frontend/setup.html`:

```html
<!DOCTYPE html>
<html lang="ru">
<head>
<meta charset="utf-8"/>
<meta name="viewport" content="width=device-width,initial-scale=1"/>
<title>CyBeer — Wi-Fi Setup</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:system-ui,sans-serif;background:#1a1a2e;color:#e0e0e0;min-height:100vh;display:flex;align-items:center;justify-content:center}
.card{background:#16213e;border-radius:12px;padding:2rem;max-width:380px;width:90%;box-shadow:0 4px 24px rgba(0,0,0,.4)}
h1{font-size:1.4rem;margin-bottom:1rem;color:#4fc3f7;text-align:center}
label{display:block;margin-top:1rem;font-size:.9rem;color:#aaa}
select,input[type=password],input[type=text]{width:100%;padding:.6rem;margin-top:.3rem;border:1px solid #333;border-radius:6px;background:#0f3460;color:#fff;font-size:1rem}
button{width:100%;padding:.7rem;margin-top:1.2rem;border:none;border-radius:6px;font-size:1rem;cursor:pointer;font-weight:600}
.btn-scan{background:#1a936f;color:#fff}
.btn-scan:disabled{background:#555;cursor:wait}
.btn-connect{background:#4fc3f7;color:#111}
.btn-connect:disabled{background:#555;color:#999;cursor:not-allowed}
#msg{margin-top:1rem;text-align:center;font-size:.9rem;min-height:1.2em}
.err{color:#ef5350}
.ok{color:#66bb6a}
</style>
</head>
<body>
<div class="card">
  <h1>CyBeer Wi-Fi Setup</h1>
  <button class="btn-scan" id="btnScan">Scan Networks</button>
  <label for="ssid">Network</label>
  <select id="ssid" disabled><option value="">— scan first —</option></select>
  <label for="pass">Password</label>
  <input type="password" id="pass" placeholder="(leave empty for open)"/>
  <button class="btn-connect" id="btnConnect" disabled>Connect</button>
  <div id="msg"></div>
</div>
<script>
(function(){
  var sel=document.getElementById('ssid');
  var btn=document.getElementById('btnScan');
  var btnC=document.getElementById('btnConnect');
  var pass=document.getElementById('pass');
  var msg=document.getElementById('msg');

  btn.addEventListener('click',function(){
    btn.disabled=true;btn.textContent='Scanning...';msg.textContent='';
    fetch('/api/setup/scan').then(function(r){return r.json()}).then(function(list){
      sel.innerHTML='';
      if(!Array.isArray(list)||list.length===0){
        sel.innerHTML='<option value="">(no networks found)</option>';
        sel.disabled=true;btnC.disabled=true;
      } else {
        for(var i=0;i<list.length;i++){
          var o=document.createElement('option');
          o.value=list[i].ssid;
          o.textContent=list[i].ssid+' ('+list[i].rssi+' dBm, '+list[i].auth+')';
          sel.appendChild(o);
        }
        sel.disabled=false;btnC.disabled=false;
      }
    }).catch(function(e){
      msg.textContent='Scan failed: '+e;msg.className='err';
    }).finally(function(){btn.disabled=false;btn.textContent='Scan Networks'});
  });

  btnC.addEventListener('click',function(){
    var ssid=sel.value;
    if(!ssid){msg.textContent='Select a network';msg.className='err';return}
    btnC.disabled=true;msg.textContent='Connecting...';msg.className='';
    fetch('/api/setup/wifi',{method:'POST',headers:{'Content-Type':'application/json'},
      body:JSON.stringify({ssid:ssid,password:pass.value})
    }).then(function(r){return r.json()}).then(function(j){
      if(j.ok){msg.textContent='Connected! Rebooting...';msg.className='ok'}
      else{msg.textContent='Error: '+(j.error||'unknown');msg.className='err';btnC.disabled=false}
    }).catch(function(e){msg.textContent='Error: '+e;msg.className='err';btnC.disabled=false});
  });
})();
</script>
</body>
</html>
```

- [ ] **Step 2: Generate embedded header from setup.html**

Create `firmware/components/cybeer_web/cybeer_setup_html.h`. This file contains the setup.html content as a C string constant. Use the following approach — store it as a raw string:

```c
#pragma once

static const char CYBEER_SETUP_HTML[] =
    "<!DOCTYPE html>"
    "<html lang=\"ru\">"
    "<head>"
    "<meta charset=\"utf-8\"/>"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"/>"
    "<title>CyBeer — Wi-Fi Setup</title>"
    "<style>"
    "*{box-sizing:border-box;margin:0;padding:0}"
    "body{font-family:system-ui,sans-serif;background:#1a1a2e;color:#e0e0e0;min-height:100vh;display:flex;align-items:center;justify-content:center}"
    ".card{background:#16213e;border-radius:12px;padding:2rem;max-width:380px;width:90%;box-shadow:0 4px 24px rgba(0,0,0,.4)}"
    "h1{font-size:1.4rem;margin-bottom:1rem;color:#4fc3f7;text-align:center}"
    "label{display:block;margin-top:1rem;font-size:.9rem;color:#aaa}"
    "select,input[type=password],input[type=text]{width:100%;padding:.6rem;margin-top:.3rem;border:1px solid #333;border-radius:6px;background:#0f3460;color:#fff;font-size:1rem}"
    "button{width:100%;padding:.7rem;margin-top:1.2rem;border:none;border-radius:6px;font-size:1rem;cursor:pointer;font-weight:600}"
    ".btn-scan{background:#1a936f;color:#fff}"
    ".btn-scan:disabled{background:#555;cursor:wait}"
    ".btn-connect{background:#4fc3f7;color:#111}"
    ".btn-connect:disabled{background:#555;color:#999;cursor:not-allowed}"
    "#msg{margin-top:1rem;text-align:center;font-size:.9rem;min-height:1.2em}"
    ".err{color:#ef5350}"
    ".ok{color:#66bb6a}"
    "</style>"
    "</head>"
    "<body>"
    "<div class=\"card\">"
    "<h1>CyBeer Wi-Fi Setup</h1>"
    "<button class=\"btn-scan\" id=\"btnScan\">Scan Networks</button>"
    "<label for=\"ssid\">Network</label>"
    "<select id=\"ssid\" disabled><option value=\"\">— scan first —</option></select>"
    "<label for=\"pass\">Password</label>"
    "<input type=\"password\" id=\"pass\" placeholder=\"(leave empty for open)\"/>"
    "<button class=\"btn-connect\" id=\"btnConnect\" disabled>Connect</button>"
    "<div id=\"msg\"></div>"
    "</div>"
    "<script>"
    "(function(){"
    "var sel=document.getElementById('ssid');"
    "var btn=document.getElementById('btnScan');"
    "var btnC=document.getElementById('btnConnect');"
    "var pass=document.getElementById('pass');"
    "var msg=document.getElementById('msg');"
    "btn.addEventListener('click',function(){"
    "btn.disabled=true;btn.textContent='Scanning...';msg.textContent='';"
    "fetch('/api/setup/scan').then(function(r){return r.json()}).then(function(list){"
    "sel.innerHTML='';"
    "if(!Array.isArray(list)||list.length===0){"
    "sel.innerHTML='<option value=\"\">(no networks found)</option>';"
    "sel.disabled=true;btnC.disabled=true;"
    "}else{"
    "for(var i=0;i<list.length;i++){"
    "var o=document.createElement('option');"
    "o.value=list[i].ssid;"
    "o.textContent=list[i].ssid+' ('+list[i].rssi+' dBm, '+list[i].auth+')';"
    "sel.appendChild(o);}"
    "sel.disabled=false;btnC.disabled=false;}"
    "}).catch(function(e){"
    "msg.textContent='Scan failed: '+e;msg.className='err';"
    "}).finally(function(){btn.disabled=false;btn.textContent='Scan Networks'});"
    "});"
    "btnC.addEventListener('click',function(){"
    "var ssid=sel.value;"
    "if(!ssid){msg.textContent='Select a network';msg.className='err';return;}"
    "btnC.disabled=true;msg.textContent='Connecting...';msg.className='';"
    "fetch('/api/setup/wifi',{method:'POST',headers:{'Content-Type':'application/json'},"
    "body:JSON.stringify({ssid:ssid,password:pass.value})"
    "}).then(function(r){return r.json()}).then(function(j){"
    "if(j.ok){msg.textContent='Connected! Rebooting...';msg.className='ok';}"
    "else{msg.textContent='Error: '+(j.error||'unknown');msg.className='err';btnC.disabled=false;}"
    "}).catch(function(e){msg.textContent='Error: '+e;msg.className='err';btnC.disabled=false;});"
    "});"
    "})();"
    "</script>"
    "</body>"
    "</html>";
```

- [ ] **Step 3: Add scan endpoint and update h_get_setup**

In `firmware/components/cybeer_wifi/cybeer_wifi.c`:

Add at the top of the file (after existing includes):

```c
#include "cybeer_setup_html.h"
```

Replace `h_get_setup` with:

```c
static esp_err_t h_get_setup(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, CYBEER_SETUP_HTML, HTTPD_RESP_USE_STRLEN);
}
```

Add a new handler for scan (before `bind_setup_handlers`):

```c
static const char *auth_mode_str(wifi_auth_mode_t mode)
{
    switch (mode) {
    case WIFI_AUTH_OPEN:
        return "Open";
    case WIFI_AUTH_WEP:
        return "WEP";
    case WIFI_AUTH_WPA_PSK:
        return "WPA";
    case WIFI_AUTH_WPA2_PSK:
        return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:
        return "WPA/WPA2";
    case WIFI_AUTH_WPA3_PSK:
        return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:
        return "WPA2/WPA3";
    default:
        return "Other";
    }
}

static esp_err_t h_get_scan(httpd_req_t *req)
{
    wifi_scan_config_t scan_cfg = {
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_PASSIVE,
        .scan_time.passive = 300,
    };
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_send(req, "[]", HTTPD_RESP_USE_STRLEN);
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count > 20) {
        ap_count = 20;
    }

    wifi_ap_record_t *ap_list = NULL;
    if (ap_count > 0) {
        ap_list = (wifi_ap_record_t *)malloc(ap_count * sizeof(wifi_ap_record_t));
        if (!ap_list) {
            esp_wifi_scan_get_ap_records(&ap_count, NULL);
            httpd_resp_set_type(req, "application/json");
            return httpd_resp_send(req, "[]", HTTPD_RESP_USE_STRLEN);
        }
        esp_wifi_scan_get_ap_records(&ap_count, ap_list);
    }

    cJSON *arr = cJSON_CreateArray();
    if (!arr) {
        free(ap_list);
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "[]", HTTPD_RESP_USE_STRLEN);
    }

    for (uint16_t i = 0; i < ap_count; i++) {
        cJSON *item = cJSON_CreateObject();
        if (!item) {
            continue;
        }
        cJSON_AddStringToObject(item, "ssid", (const char *)ap_list[i].ssid);
        cJSON_AddNumberToObject(item, "rssi", (double)ap_list[i].rssi);
        cJSON_AddStringToObject(item, "auth", auth_mode_str(ap_list[i].authmode));
        cJSON_AddItemToArray(arr, item);
    }
    free(ap_list);

    char *printed = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (!printed) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "[]", HTTPD_RESP_USE_STRLEN);
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t e = httpd_resp_send(req, printed, HTTPD_RESP_USE_STRLEN);
    free(printed);
    return e;
}
```

- [ ] **Step 4: Register scan endpoint in bind_setup_handlers**

In `bind_setup_handlers`, add the scan URI after `u_api_wifi`:

```c
httpd_uri_t u_scan = {
    .uri = "/api/setup/scan", .method = HTTP_GET, .handler = h_get_scan, .user_ctx = NULL
};
```

And register it:

```c
ESP_RETURN_ON_ERROR(httpd_register_uri_handler(h, &u_scan), TAG, "uri /api/setup/scan");
```

- [ ] **Step 5: Add include path for cybeer_setup_html.h**

The header is in `firmware/components/cybeer_web/` but included from `cybeer_wifi.c`. Move it to a shared location or adjust the include. Simplest: place in `firmware/components/cybeer_wifi/` since that's where it's used.

Rename the file to: `firmware/components/cybeer_wifi/cybeer_setup_html.h`

And the include in `cybeer_wifi.c` remains: `#include "cybeer_setup_html.h"`

Since `cybeer_wifi/CMakeLists.txt` has `INCLUDE_DIRS "include"`, and the `.h` is at component root, add `.` to INCLUDE_DIRS:

In `firmware/components/cybeer_wifi/CMakeLists.txt`, change to include current dir:

```cmake
idf_component_register(SRCS "cybeer_wifi.c"
                    INCLUDE_DIRS "include" "."
                    REQUIRES esp_wifi esp_netif esp_event nvs_flash lwip mdns esp_http_server cybeer_storage cybeer_config json)
```

- [ ] **Step 6: Verify build**

```bash
cd firmware
idf.py build
```

Expected: `Project build complete.`

- [ ] **Step 7: Commit**

```bash
git add frontend/setup.html firmware/components/cybeer_wifi/cybeer_setup_html.h firmware/components/cybeer_wifi/cybeer_wifi.c firmware/components/cybeer_wifi/CMakeLists.txt
git commit -m "feat: Wi-Fi provisioning page with network scan"
```

---

## Phase 2 — Reliability Fixes

### Task 4: WebSocket sends initial state on connect

**Files:**
- Modify: `firmware/components/cybeer_web/cybeer_ws.c` (function `h_ws`, around line 201-216)

- [ ] **Step 1: Add initial state + timer send after clients_add**

In `h_ws()`, after the block that sends battery info on `clients_add(fd)`, add state and timer messages. Replace the `if (clients_add(fd))` block:

```c
if (clients_add(fd)) {
    /* Send battery */
    const int pct = cybeer_battery_get_percent();
    cJSON *bat = cJSON_CreateObject();
    if (bat) {
        cJSON_AddStringToObject(bat, "type", "battery");
        cJSON_AddNumberToObject(bat, "percent", pct);
        char *bp = cJSON_PrintUnformatted(bat);
        cJSON_Delete(bat);
        if (bp) {
            send_text_payload_to_fd(fd, bp, strlen(bp));
            free(bp);
        }
    }
    s_last_battery_pct_sent = pct;

    /* Send current FSM state */
    cybeer_fsm_snapshot_t snap = cybeer_fsm_snapshot();
    cJSON *st = cJSON_CreateObject();
    if (st) {
        cJSON_AddStringToObject(st, "type", "state");
        cJSON_AddStringToObject(st, "state", fsm_state_str(snap.state));
        char *sp = cJSON_PrintUnformatted(st);
        cJSON_Delete(st);
        if (sp) {
            send_text_payload_to_fd(fd, sp, strlen(sp));
            free(sp);
        }
    }

    /* If running, send current elapsed */
    if (snap.state == CYBEER_STATE_RUNNING) {
        int64_t now = esp_timer_get_time();
        int64_t elapsed = cybeer_timer_elapsed_us(now);
        cJSON *tm = cJSON_CreateObject();
        if (tm) {
            cJSON_AddStringToObject(tm, "type", "timer");
            cJSON_AddNumberToObject(tm, "elapsedUs", (double)elapsed);
            char *tp = cJSON_PrintUnformatted(tm);
            cJSON_Delete(tm);
            if (tp) {
                send_text_payload_to_fd(fd, tp, strlen(tp));
                free(tp);
            }
        }
    }
}
```

- [ ] **Step 2: Add missing include for esp_timer**

At the top of `cybeer_ws.c`, add (if not already present):

```c
#include "esp_timer.h"
```

- [ ] **Step 3: Verify build**

```bash
cd firmware
idf.py build
```

Expected: `Project build complete.`

- [ ] **Step 4: Commit**

```bash
git add firmware/components/cybeer_web/cybeer_ws.c
git commit -m "fix: send FSM state and timer to new WebSocket clients"
```

---

### Task 5: Wi-Fi STA reconnect on disconnect

**Files:**
- Modify: `firmware/components/cybeer_wifi/cybeer_wifi.c` (function `wifi_event`)

- [ ] **Step 1: Add reconnect state variables**

Add after the existing static variables (around line 39-40):

```c
static int s_sta_retry_count;
static const int STA_RECONNECT_MAX_DELAY_MS = 30000;
```

- [ ] **Step 2: Update wifi_event to reconnect on disconnect**

Replace the `wifi_event` function:

```c
static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;

    if (id == WIFI_EVENT_STA_START) {
        s_sta_retry_count = 0;
        (void)esp_wifi_connect();
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        s_sta_has_ip = false;
        s_sta_ip_str[0] = '\0';
        s_sta_retry_count++;
        int delay_ms = 2000;
        for (int i = 1; i < s_sta_retry_count && delay_ms < STA_RECONNECT_MAX_DELAY_MS; i++) {
            delay_ms *= 2;
        }
        if (delay_ms > STA_RECONNECT_MAX_DELAY_MS) {
            delay_ms = STA_RECONNECT_MAX_DELAY_MS;
        }
        ESP_LOGW(TAG, "STA disconnected, reconnect attempt #%d in %d ms", s_sta_retry_count, delay_ms);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
        (void)esp_wifi_connect();
    }
}
```

- [ ] **Step 3: Reset retry count on successful IP**

In `ip_event` function, after `s_sta_has_ip = true;` add:

```c
s_sta_retry_count = 0;
```

- [ ] **Step 4: Verify build**

```bash
cd firmware
idf.py build
```

Expected: `Project build complete.`

- [ ] **Step 5: Commit**

```bash
git add firmware/components/cybeer_wifi/cybeer_wifi.c
git commit -m "fix: STA reconnect with exponential backoff on disconnect"
```

---

### Task 6: Fix cybeer_storage.h missing cJSON forward declaration

**Files:**
- Modify: `firmware/components/cybeer_storage/include/cybeer_storage.h`

- [ ] **Step 1: Add cJSON forward declaration**

At the top of `cybeer_storage.h`, after the existing `#include` lines and before the typedefs, add:

```c
struct cJSON;
typedef struct cJSON cJSON;
```

This goes after `#include "esp_err.h"` and before the `cybeer_run_t` typedef.

- [ ] **Step 2: Verify build**

```bash
cd firmware
idf.py build
```

Expected: `Project build complete.`

- [ ] **Step 3: Commit**

```bash
git add firmware/components/cybeer_storage/include/cybeer_storage.h
git commit -m "fix: add cJSON forward declaration to cybeer_storage.h"
```

---

## Phase 3 — Improvements

### Task 7: SNTP time synchronization

**Files:**
- Modify: `firmware/components/cybeer_wifi/cybeer_wifi.c` (function `ip_event`)
- Modify: `firmware/components/cybeer_wifi/CMakeLists.txt` (if esp_sntp needs explicit REQUIRES)

- [ ] **Step 1: Add SNTP include and init flag**

At the top of `cybeer_wifi.c`, add:

```c
#include "esp_sntp.h"
```

Add a static flag after the existing statics:

```c
static bool s_sntp_started;
```

- [ ] **Step 2: Initialize SNTP in ip_event after got_ip**

In the `ip_event` function, after the mDNS setup block (after `s_mdns_started = true;`), add:

```c
    if (!s_sntp_started) {
        esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "pool.ntp.org");
        esp_sntp_setservername(1, "time.google.com");
        esp_sntp_init();
        s_sntp_started = true;
        ESP_LOGI(TAG, "SNTP started");
    }
```

- [ ] **Step 3: Verify build**

```bash
cd firmware
idf.py build
```

Expected: `Project build complete.` (esp_sntp is part of lwip which is already a dependency)

- [ ] **Step 4: Commit**

```bash
git add firmware/components/cybeer_wifi/cybeer_wifi.c
git commit -m "feat: SNTP time sync for correct timestamps"
```

---

### Task 8: Leaderboard endpoint

**Files:**
- Modify: `firmware/components/cybeer_web/cybeer_web.c`

- [ ] **Step 1: Add leaderboard handler**

Add the following function before `cybeer_web_start()`:

```c
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
        const cJSON *jid = cJSON_GetObjectItemCaseSensitive(item, "id");
        const cJSON *jpid = cJSON_GetObjectItemCaseSensitive(item, "participant_id");
        const cJSON *jdur = cJSON_GetObjectItemCaseSensitive(item, "duration_us");
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
```

- [ ] **Step 2: Register leaderboard endpoint**

In `cybeer_web_start()`, add the URI definition alongside the other definitions:

```c
httpd_uri_t u_leaderboard = {
    .uri = "/api/leaderboard", .method = HTTP_GET, .handler = h_get_leaderboard, .user_ctx = NULL
};
```

Add its registration in the chain (before the static handler registration):

```c
|| httpd_register_uri_handler(s_server, &u_leaderboard) != ESP_OK
```

- [ ] **Step 3: Add stdlib.h include if not present** (for `qsort`, `atoi`)

Verify `#include <stdlib.h>` is at the top of `cybeer_web.c`. It should already be there via other includes, but confirm.

- [ ] **Step 4: Verify build**

```bash
cd firmware
idf.py build
```

Expected: `Project build complete.`

- [ ] **Step 5: Commit**

```bash
git add firmware/components/cybeer_web/cybeer_web.c
git commit -m "feat: GET /api/leaderboard endpoint sorted by fastest"
```

---

### Task 9: WebSocket ping for dead connection detection

**Files:**
- Modify: `firmware/components/cybeer_web/cybeer_ws.c`

- [ ] **Step 1: Add ping interval tracking**

Add a static variable after `s_last_battery_pct_sent`:

```c
static int64_t s_last_ping_us;
#define WS_PING_INTERVAL_US (15 * 1000000LL)
```

- [ ] **Step 2: Add ping logic in timer_tick**

In `cybeer_ws_timer_tick()`, add at the **beginning** of the function (before the RUNNING check):

```c
if (s_hd && s_last_ping_us != 0 && (now_us - s_last_ping_us) >= WS_PING_INTERVAL_US) {
    s_last_ping_us = now_us;
    int fds[CYBEER_WS_MAX_CLIENTS];
    int n = clients_copy(fds, CYBEER_WS_MAX_CLIENTS);
    httpd_ws_frame_t ping = { .type = HTTPD_WS_TYPE_PING, .payload = NULL, .len = 0 };
    for (int i = 0; i < n; i++) {
        if (httpd_ws_send_frame_async(s_hd, fds[i], &ping) != ESP_OK) {
            clients_remove(fds[i]);
        }
    }
} else if (s_last_ping_us == 0 && s_hd) {
    s_last_ping_us = now_us;
}
```

- [ ] **Step 3: Initialize s_last_ping_us in cybeer_ws_register**

In `cybeer_ws_register`, after `s_last_timer_send_us = 0;` add:

```c
s_last_ping_us = 0;
```

- [ ] **Step 4: Verify build**

```bash
cd firmware
idf.py build
```

Expected: `Project build complete.`

- [ ] **Step 5: Commit**

```bash
git add firmware/components/cybeer_web/cybeer_ws.c
git commit -m "feat: WebSocket ping every 15s to detect dead connections"
```

---

### Task 10: LED WIFI_SETUP effect activation

**Files:**
- Modify: `firmware/components/cybeer_wifi/cybeer_wifi.c`
- Modify: `firmware/components/cybeer_wifi/CMakeLists.txt`

- [ ] **Step 1: Add cybeer_led dependency**

In `firmware/components/cybeer_wifi/CMakeLists.txt`, add `cybeer_led` to REQUIRES:

```cmake
idf_component_register(SRCS "cybeer_wifi.c"
                    INCLUDE_DIRS "include" "."
                    REQUIRES esp_wifi esp_netif esp_event nvs_flash lwip mdns esp_http_server cybeer_storage cybeer_config cybeer_led json)
```

- [ ] **Step 2: Include cybeer_led.h and activate effect**

In `cybeer_wifi.c`, add at the top:

```c
#include "cybeer_led.h"
```

In `cybeer_wifi_start()`, after the `ESP_LOGI(TAG, "Provisioning mode (no STA credentials)");` line, add:

```c
cybeer_led_set_fx(CYBEER_LED_FX_WIFI_SETUP);
```

- [ ] **Step 3: Verify build**

```bash
cd firmware
idf.py build
```

Expected: `Project build complete.`

- [ ] **Step 4: Commit**

```bash
git add firmware/components/cybeer_wifi/cybeer_wifi.c firmware/components/cybeer_wifi/CMakeLists.txt
git commit -m "feat: activate blue pulse LED effect during Wi-Fi provisioning"
```

---

### Task 11: Hybrid claim UI (dropdown + new name)

**Files:**
- Modify: `frontend/claim.html`
- Modify: `frontend/app.js`

- [ ] **Step 1: Update claim.html with hybrid form**

Replace the content of `frontend/claim.html` with:

```html
<!DOCTYPE html>
<html lang="ru">
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <title>CyBeer — Claim</title>
  <link rel="stylesheet" href="app.css" />
</head>
<body>
  <main class="container">
    <h1>Claim Run</h1>
    <p>Unclaimed run: <strong id="targetRunId">(loading...)</strong></p>
    <form id="claimForm">
      <label for="claimSelect">Выбрать участника:</label>
      <select id="claimSelect">
        <option value="">— Новый участник —</option>
      </select>
      <label for="claimName">Или введите новое имя:</label>
      <input type="text" id="claimName" placeholder="Имя нового участника" />
      <button type="submit">Claim</button>
    </form>
    <p id="claimMsg"></p>
    <p><a href="/">← Back</a></p>
  </main>
  <script>window.__CYBEER_PAGE__ = "claim";</script>
  <script src="app.js"></script>
</body>
</html>
```

- [ ] **Step 2: Update app.js claim logic for hybrid**

In `frontend/app.js`, replace the `initClaimForm` function:

```javascript
function initClaimForm() {
  const form = document.getElementById("claimForm");
  const msg = document.getElementById("claimMsg");
  const sel = document.getElementById("claimSelect");
  const nameInput = document.getElementById("claimName");
  if (!form) return;

  /* Load participants into dropdown */
  fetch("/api/participants")
    .then((r) => r.json())
    .then((data) => {
      if (!Array.isArray(data)) return;
      for (const p of data) {
        if (!p || !p.id || !p.name) continue;
        const opt = document.createElement("option");
        opt.value = p.id;
        opt.textContent = p.name;
        sel.appendChild(opt);
      }
    })
    .catch(() => {});

  /* Disable name input when existing participant selected */
  sel.addEventListener("change", () => {
    if (sel.value) {
      nameInput.value = "";
      nameInput.disabled = true;
    } else {
      nameInput.disabled = false;
    }
  });

  form.addEventListener("submit", async (ev) => {
    ev.preventDefault();
    if (msg) {
      msg.textContent = "";
      msg.classList.remove("err");
    }
    const rs = await fetch("/api/status");
    const status = await rs.json();
    const runId = status.unclaimedRunId;
    if (!runId) {
      if (msg) {
        msg.textContent = "No unclaimed run.";
        msg.classList.add("err");
      }
      return;
    }

    let body;
    if (sel.value) {
      body = { participantId: sel.value };
    } else {
      const name = nameInput?.value?.trim();
      if (!name) {
        if (msg) {
          msg.textContent = "Выберите участника или введите имя.";
          msg.classList.add("err");
        }
        return;
      }
      body = { name: name };
    }

    try {
      const resp = await fetch(
        `/api/runs/${encodeURIComponent(runId)}/claim`,
        {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify(body),
        }
      );
      const txt = await resp.text();
      if (!resp.ok) {
        if (msg) {
          msg.textContent = txt || resp.statusText;
          msg.classList.add("err");
        }
        return;
      }
      if (msg) msg.textContent = "Claimed!";
      await tickClaimTarget();
    } catch (e) {
      if (msg) {
        msg.textContent = String(e);
        msg.classList.add("err");
      }
    }
  });
}
```

- [ ] **Step 3: Remove old claimPid input reference**

The old `claim.html` had a `claimPid` input field. The new version uses `claimSelect` instead. Ensure the old `tickClaimTarget` function and `connectLiveWs` still work (they don't reference `claimPid`, so no change needed).

- [ ] **Step 4: Verify files are syntactically correct**

Open `frontend/claim.html` and `frontend/app.js` in a browser or validator to confirm no syntax errors.

- [ ] **Step 5: Commit**

```bash
git add frontend/claim.html frontend/app.js
git commit -m "feat: hybrid claim UI with participant dropdown and new name input"
```

---

### Task 12: Frontend build pipeline documentation

**Files:**
- Modify: `README.md`

- [ ] **Step 1: Add frontend build section to README**

Add the following section to `README.md` (after the "Сборка и прошивка" section):

```markdown
## Frontend (веб-интерфейс)

Исходники фронтенда лежат в `frontend/`. При сборке прошивки CMake автоматически синхронизирует файлы:

```
frontend/ → firmware/frontend_dist/www/ → LittleFS image → flash partition "littlefs"
```

### Обновление фронтенда

1. Отредактируйте файлы в `frontend/`
2. Пересоберите и прошейте:
   ```bash
   cd firmware
   idf.py build flash
   ```
   LittleFS-образ пересоздаётся автоматически при каждой сборке.

### Структура фронтенда

| Файл | Назначение |
|------|-----------|
| `index.html` | Главная: лидерборд, статус, live-таймер |
| `claim.html` | Привязка заезда к участнику |
| `player.html` | Статистика участника |
| `tournament.html` | Турнирная сетка |
| `admin.html` | Админка (PIN-защита) |
| `setup.html` | Wi-Fi provisioning (captive portal) |
| `app.js` | Общая логика (WS, claim, рендер) |
| `app.css` | Стили |
```

- [ ] **Step 2: Commit**

```bash
git add README.md
git commit -m "docs: add frontend build pipeline section to README"
```

---

### Task 13: Thread-safety documentation for static JSON buffers

**Files:**
- Modify: `firmware/components/cybeer_storage/cybeer_storage.c`

- [ ] **Step 1: Add documentation comment to cybeer_storage_runs_json**

Add a comment above `cybeer_storage_runs_json`:

```c
/**
 * Returns pointer to internal static buffer containing runs.json content.
 *
 * THREAD SAFETY: The returned pointer is valid only until the next call to
 * any cybeer_storage_*_json() function. Safe when httpd uses its default
 * single-threaded request handling (one handler runs at a time).
 * Do NOT call from multiple tasks concurrently.
 */
const char *cybeer_storage_runs_json(void)
```

Add the same style comment above `cybeer_storage_participants_json`, `cybeer_storage_tournaments_json`, and `cybeer_storage_active_tournament_json`.

- [ ] **Step 2: Commit**

```bash
git add firmware/components/cybeer_storage/cybeer_storage.c
git commit -m "docs: document thread-safety constraints on static JSON buffers"
```

---

## Spec Coverage Checklist

| Spec § | Task |
|--------|------|
| 2.1 UAF fix | Task 1 |
| 2.2 Claim auto-create | Task 2 |
| 2.3 /setup with scan | Task 3 |
| 3.1 WS initial state | Task 4 |
| 3.2 STA reconnect | Task 5 |
| 3.3 cJSON forward decl | Task 6 |
| 4.1 SNTP | Task 7 |
| 4.2 Leaderboard | Task 8 |
| 4.3 WS ping | Task 9 |
| 4.4 Frontend docs | Task 12 |
| 4.5 LED WIFI_SETUP | Task 10 |
| 4.6 Thread-safety | Task 13 |
| 4.7 Hybrid claim UI | Task 11 |
