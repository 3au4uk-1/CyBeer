# CyBeer Fixes & Improvements — Design Spec

**Date:** 2026-05-21  
**Status:** Draft  
**Scope:** Bug fixes + reliability improvements + quality-of-life enhancements for the existing CyBeer firmware and frontend.

## 1. Overview

Post-implementation review of the CyBeer ESP-IDF firmware identified 6 bugs (2 critical, 1 functional, 3 reliability) and 7 improvement opportunities. This spec defines the fixes and enhancements to bring the firmware to production-ready state for real championship use.

**Out of scope:** Flash wear protection (LittleFS handles wear-leveling internally), new hardware features, protocol changes to the existing WebSocket message format.

---

## 2. Phase 1 — Critical Bug Fixes

### 2.1 Use-After-Free in `PUT /api/settings`

**File:** `firmware/components/cybeer_web/cybeer_web.c`, function `h_put_settings`

**Problem:** `cJSON_Delete(root)` is called before accessing `jlc->valuedouble` and `jbr->valuedouble`, which are children of `root`. This is undefined behavior — likely crash on device.

**Fix:** Move `cJSON_Delete(root)` to after the values have been read into local `int` variables.

```c
int led = (int)jlc->valuedouble;
int br = (int)jbr->valuedouble;
cJSON_Delete(root);  // moved here
```

### 2.2 Claim by Name Cannot Create New Participants

**File:** `firmware/components/cybeer_storage/cybeer_storage.c`, function `cybeer_storage_claim_run`

**Problem:** When `by_participant_id == false`, `resolve_participant_by_name_locked()` searches `participants.json`. If the name doesn't exist, it returns `ESP_ERR_NOT_FOUND` and the claim fails. On a fresh device with no pre-registered participants, nobody can claim runs.

**Fix:** When name lookup returns `ESP_ERR_NOT_FOUND`, auto-create a new participant:

1. Generate UUID v4 for the new participant
2. Create JSON object `{ "id": uuid, "name": name, "createdAt": iso8601 }`
3. Append to `participants.json`
4. Use the new participant's ID to complete the claim

**New internal function:** `create_participant_by_name_locked(const char *name, char pid_out[37])` — creates participant in storage, returns its ID.

### 2.3 `/setup` Page — Wi-Fi Provisioning with Scan

**Problem:** `h_get_setup` returns plain text `"CyBeer Wi-Fi provisioning\n"`. Users connecting to the AP for first-time setup see nothing useful.

**Solution:** Full provisioning page with network scan.

**New API endpoint:** `GET /api/setup/scan`
- Triggers `esp_wifi_scan_start()` (blocking, passive scan, max 3s)
- Returns JSON array: `[{"ssid": "...", "rssi": -42, "auth": "WPA2"}, ...]`
- Auth field maps `wifi_auth_mode_t` to human-readable string

**New frontend file:** `frontend/setup.html`
- Self-contained (inline CSS/JS, no external deps — must work without LittleFS www assets since this is the captive portal before Wi-Fi is configured)
- UI: CyBeer logo/title, "Scan" button → dropdown of found networks, password field, "Connect" button
- On submit: POST to `/api/setup/wifi` with `{"ssid": "...", "password": "..."}`
- Success: show "Connected! Rebooting..." message

**Backend change:** `h_get_setup` serves the `setup.html` content (embedded as a string constant or from LittleFS if available, with fallback to embedded).

Since this page must work during captive portal (before LittleFS www is populated on first flash), the HTML should be **embedded in firmware** as a `const char[]` (or via `EMBED_FILES` in CMake).

---

## 3. Phase 2 — Reliability Fixes

### 3.1 WebSocket Initial State on Connect

**File:** `firmware/components/cybeer_web/cybeer_ws.c`, function `h_ws`

**Problem:** New WS clients receive battery info but not the current FSM state or running elapsed time. They must wait for a state change or poll `/api/status`.

**Fix:** After `clients_add(fd)` succeeds, immediately send:
1. `{"type": "state", "state": "<current>"}` — always
2. `{"type": "timer", "elapsedUs": N}` — only if state is RUNNING

### 3.2 Wi-Fi STA Reconnect on Disconnect

**File:** `firmware/components/cybeer_wifi/cybeer_wifi.c`, function `wifi_event`

**Problem:** On `WIFI_EVENT_STA_DISCONNECTED`, the flag is cleared but no reconnection attempt is made. The device loses LAN access permanently until reboot.

**Fix:** Add reconnect logic with exponential backoff:
- On disconnect: attempt `esp_wifi_connect()` immediately
- If fail: retry after 2s, 4s, 8s, 16s, then cap at 30s intervals
- Track retry count; reset on successful IP acquisition
- Max retries: unlimited (device should always try to reconnect)

Implementation: use a FreeRTOS timer or simple counter in the event handler. The `wifi_event` handler calls `esp_wifi_connect()` directly (which is safe from the event task context per ESP-IDF docs).

### 3.3 Missing `cJSON` Forward Declaration in Header

**File:** `firmware/components/cybeer_storage/include/cybeer_storage.h`

**Problem:** Header uses `cJSON *` in function signatures (lines 76-81) without including or forward-declaring the type.

**Fix:** Add before the function declarations:

```c
struct cJSON;
typedef struct cJSON cJSON;
```

This avoids pulling in the full `cJSON.h` into every consumer while satisfying the compiler for pointer types.

---

## 4. Phase 3 — Improvements

### 4.1 SNTP Time Synchronization

**File:** `firmware/components/cybeer_wifi/cybeer_wifi.c` (or new init in `app_main.c`)

**Purpose:** Without SNTP, `time(NULL)` returns epoch-relative values (1970-...) making `finished_at` timestamps meaningless.

**Implementation:**
- After STA gets IP (`ip_event` handler), call `esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL)` + `esp_sntp_setservername(0, "pool.ntp.org")` + `esp_sntp_init()`
- Guard against re-init if already started
- No callback needed; `time()` will automatically return correct UTC after sync

**Dependency:** Add `esp_sntp` or `lwip` (already present) to component requires.

### 4.2 Leaderboard Sort Endpoint

**File:** `firmware/components/cybeer_web/cybeer_web.c`

**New endpoint:** `GET /api/leaderboard?limit=N` (default N=20, max 50)

**Response:** JSON array of claimed runs sorted by `duration_us` ascending (fastest first), enriched with participant name:

```json
[
  {
    "rank": 1,
    "participantId": "uuid",
    "participantName": "Vasya",
    "durationUs": 3456789,
    "finishedAt": "2026-05-21T20:15:00Z"
  }
]
```

**Implementation:** Parse runs JSON, filter `claimed == true`, sort by `duration_us`, take first N, look up participant names. All in-memory (runs fit in 16KB buffer).

### 4.3 WebSocket Ping for Dead Connection Detection

**File:** `firmware/components/cybeer_web/cybeer_ws.c`

**Problem:** Dead TCP connections (phone screen off, network switch) are never detected. `s_fds[]` fills up with dead entries, blocking new clients.

**Fix:** In `cybeer_ws_timer_tick()` (called every 20ms from display_task), add a periodic check every 15 seconds:
- Send a PING frame to each registered client
- On send failure → `clients_remove(fd)`

ESP-IDF `httpd_ws_send_frame_async` with `HTTPD_WS_TYPE_PING` handles this. Browsers auto-respond with PONG.

### 4.4 Frontend Build Pipeline Documentation

**File:** `README.md` (add section)

Document the existing CMake-based pipeline:
1. Source: `frontend/` directory (HTML, CSS, JS)
2. At build time: CMake `cybeer_sync_frontend` target copies files to `firmware/frontend_dist/www/`
3. `littlefs_create_partition_image` creates the flash image
4. `idf.py flash` writes it to the `littlefs` partition

Also document: how to update frontend-only (without re-flashing firmware) if LittleFS supports it, or clarify that full reflash is needed.

### 4.5 LED WIFI_SETUP Effect Activation

**File:** `firmware/components/cybeer_wifi/cybeer_wifi.c` or `firmware/main/app_main.c`

**Problem:** `CYBEER_LED_FX_WIFI_SETUP` is defined in the enum and has a render implementation (blue pulse) but is never activated.

**Fix:** In `cybeer_wifi_start()`, after detecting `!have_sta` (provisioning mode), call:

```c
cybeer_led_set_fx(CYBEER_LED_FX_WIFI_SETUP);
```

This requires `cybeer_wifi` to depend on `cybeer_led` (add to component CMakeLists.txt REQUIRES), or pass the activation via a callback/extern. Prefer the callback approach to avoid circular dependency:
- Add `cybeer_led.h` include in `cybeer_wifi.c`
- Add `cybeer_led` to `cybeer_wifi/CMakeLists.txt` REQUIRES

**Reset:** When STA connects successfully (got IP), transition back to `CYBEER_LED_FX_AMBIENT` (or let `app_main` display_task handle it via FSM state).

### 4.6 Thread-Safety for Static JSON Buffers

**File:** `firmware/components/cybeer_storage/cybeer_storage.c`

**Problem:** `cybeer_storage_runs_json()` returns a pointer to `s_runs_json_buf`. If two HTTP handlers call it concurrently (possible with multiple TCP connections), the second overwrites the first's data mid-read.

**Fix approach:** Change the API to accept a caller-owned buffer:

```c
esp_err_t cybeer_storage_runs_json_copy(char *buf, size_t buf_sz);
```

Or simpler (minimal change): hold the mutex for the duration of the HTTP response serialization. Since `h_get_runs` immediately parses the string into cJSON (making a copy), the window is short. Add a comment documenting that the returned pointer is only valid until the next call to any `cybeer_storage_*_json()` function.

**Chosen approach:** Document + ensure single-threaded access pattern. The HTTP server uses a single-threaded model by default (`cfg.max_open_sockets` with sequential handling per socket). As long as `httpd` config doesn't enable multi-threaded mode, the current pattern is safe. Add an assertion/comment to make this explicit.

### 4.7 Hybrid Claim UI

**File:** `frontend/claim.html`, `frontend/app.js`

**Current:** Single text field for name or participant ID.

**New UI:**
1. On page load: fetch `GET /api/participants` → populate a `<select>` dropdown with existing participant names
2. Dropdown has a default "— Выбрать участника —" option
3. Below dropdown: text input "Или введите новое имя"
4. Submit logic:
   - If dropdown has a selection (not default) → send `{ "participantId": selectedId }`
   - Else if text field has value → send `{ "name": textValue }`
   - Else → show validation error

The backend auto-creation (§2.2) handles the case where a new name is submitted.

---

## 5. File Change Summary

| Phase | Files Modified | Files Created |
|-------|---------------|---------------|
| 1 | `cybeer_web.c`, `cybeer_storage.c`, `cybeer_wifi.c` | `frontend/setup.html` (embedded) |
| 2 | `cybeer_ws.c`, `cybeer_wifi.c`, `cybeer_storage.h` | — |
| 3 | `cybeer_wifi.c`, `cybeer_web.c`, `cybeer_ws.c`, `cybeer_led.c` (maybe), `claim.html`, `app.js`, `README.md`, `cybeer_storage.c` | — |

---

## 6. Testing Strategy

- **Unit tests:** Extend `test_switch_filter.c` / `test_fsm.c` with claim-creates-participant scenario (mock storage)
- **Manual:** Flash to device → verify claim flow with new name, settings PUT, /setup page scan
- **Integration:** Test WS reconnect by killing/resuming TCP connections; verify leaderboard sort order
- **Regression:** All existing FSM transitions must still pass after changes

---

## 7. Decisions Log

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Claim with unknown name | Auto-create participant | Championship flow: people show up, drink, claim. No pre-registration needed |
| /setup embedding | Embed HTML in firmware const | Must work before LittleFS is populated (first boot) |
| Flash wear protection | Skip | LittleFS does wear-leveling; event frequency (tens of runs per session) is well within flash endurance |
| Thread-safety buffers | Document + rely on httpd single-thread | Minimal code change; httpd default is sequential |
| Wi-Fi scan | Blocking passive scan, 3s max | Simple; provisioning is not time-critical |
| STA reconnect | Immediate + exponential backoff to 30s | Must reconnect without reboot for tournament reliability |
