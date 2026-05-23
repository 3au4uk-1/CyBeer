# WebUI UX Improvements — Design Spec

**Date:** 2026-05-23
**Version:** v1.1.13 baseline

## Scope

1. Fix participant name display bug (UUID shown instead of entered name)
2. Admin power control buttons (Reboot, Eco Mode, Sleep)
3. Participant self-rename from player page

---

## 1. Fix: Participant Name Display Bug

### Symptom

After claiming a run with a manually entered name, the leaderboard and player page show the participant's UUID prefix (`58961787…`) instead of the actual name. The player page also shows "Ошибка загрузки статистики" and "Ошибка" in run history.

### Root Cause Investigation

The claim flow: frontend sends `POST /api/claim` with `{ "runId": "...", "name": "Alice" }`. Backend calls `cybeer_storage_claim_run` which either resolves an existing participant by name or creates a new one via `create_participant_by_name_locked`. The leaderboard handler joins `participant_id` → `participants.json` to get names.

Suspected causes (to verify during implementation):
- **Persistence timing:** `create_participant_by_name_locked` persists to flash, but if the leaderboard is read before fsync completes, `participantName` comes back empty.
- **Name field not written:** The `create_participant_by_name_locked` function may receive an empty or corrupted string.
- **Player page endpoints broken:** `/api/participants/:id/stats` and `/api/participants/:id/runs` return errors (visible on screenshot). Likely a URI wildcard parsing issue similar to other `/*` endpoints.

### Fix Strategy

1. Add ESP_LOG tracing in `create_participant_by_name_locked` to confirm `name` parameter value at write time.
2. Verify `persist_json_locked` completes synchronously before the claim response is sent.
3. Inspect and fix `/api/participants/:id/stats` and `/api/participants/:id/runs` handlers for URI parsing correctness.
4. After claim response, ensure WebSocket `leaderboardUpdate` fires only after persistence is confirmed.

---

## 2. Admin Power Control Buttons

### New API Endpoints

All require `X-Admin-Pin` header.

| Method | Path | Behavior |
|--------|------|----------|
| `POST` | `/api/admin/reboot` | Sends 200 OK, then calls `esp_restart()` |
| `POST` | `/api/admin/power/eco` | Toggles eco mode. Eco ON: blanks TM1637, turns off LED strip, sets `s_eco_mode = true`. Eco OFF: restores display/LED to current FSM state. Returns `{ "eco": true/false }` |
| `POST` | `/api/admin/power/sleep` | Sends 200 OK, then after 500ms delay calls `esp_light_sleep_start()`. Device becomes unreachable until triple-tap on GPIO9. |

### Eco Mode Details

- Global flag `s_eco_mode` in `cybeer_power.c` (same component that handles idle sleep).
- When eco is active, FSM display/LED updates are suppressed (check flag before writing to TM1637/WS2812).
- Physical button press (single tap on GPIO9) exits eco mode.
- Status reflected in `GET /api/status` response: `"powerMode": "normal"` or `"eco"`.

### Sleep Behavior

- Same as existing idle-timeout sleep logic in `cybeer_power.c`, but triggered immediately via HTTP.
- 500ms delay allows the HTTP response to be fully sent before Wi-Fi goes down.
- Wake mechanism unchanged: triple-tap GPIO9 within 3 seconds.

### Frontend (admin.html)

New section "Управление устройством" below existing admin sections:

```
┌──────────────────────────────────────────┐
│  Управление устройством                  │
│                                          │
│  [🔄 Перезагрузить]  [💡 Энергосбережение]  [😴 Сон]  │
│                                          │
│  Статус: Нормальный режим                │
└──────────────────────────────────────────┘
```

- "Перезагрузить" — red accent button, single click fires `POST /api/admin/reboot`.
- "Энергосбережение" — yellow/amber toggle button. Active state shows "Вкл" label. Fires `POST /api/admin/power/eco`.
- "Сон" — muted/blue button. Fires `POST /api/admin/power/sleep`. After click, shows message "Устройство уснуло. Для пробуждения нажмите кнопку 3 раза."

---

## 3. Participant Self-Rename

### New API Endpoint

Public (no PIN required).

| Method | Path | Body | Response |
|--------|------|------|----------|
| `PATCH` | `/api/participants/:id` | `{ "name": "New Name" }` | 200 `{ "ok": true }` |

### Validation Rules

- `name` must be a non-empty string after trim, 1–32 chars.
- If another participant already has this name (case-sensitive match) → 409 Conflict `{ "error": "name_taken" }`.
- If participant ID not found → 404 `{ "error": "not_found" }`.

### Backend Implementation

New function in `cybeer_storage.c`:

```c
esp_err_t cybeer_storage_rename_participant(const char *participant_id, const char *new_name);
```

Logic:
1. Take mutex.
2. Parse `participants.json`.
3. Check for name uniqueness (skip self).
4. Find participant by ID, update `name` field.
5. Persist and release mutex.

The web handler (`h_patch_participant`) calls this function and, on success, broadcasts WebSocket `{ "type": "leaderboardUpdate" }` via `cybeer_ws_broadcast()` — same pattern as the claim handler.

### URI Handler Registration

New wildcard handler: `PATCH /api/participants/*`. Extract ID from URI path after `/api/participants/`.

### Frontend (player.html)

Current state: page header shows participant name as static text.

New behavior:
- Next to name: a small "edit" button (pencil icon or text link "Изменить").
- Click → name becomes an `<input>` pre-filled with current name + "Сохранить" / "Отмена" buttons.
- On save: `PATCH /api/participants/:id` with new name.
- Success → update displayed name, hide edit controls.
- Error 409 → show "Это имя уже занято" under the input.
- Error 404 → show "Участник не найден".

---

## Files to Modify

| File | Changes |
|------|---------|
| `firmware/components/cybeer_web/cybeer_web.c` | Add 4 new URI handlers (reboot, eco, sleep, rename). Fix participant stats/runs handlers if broken. |
| `firmware/components/cybeer_storage/cybeer_storage.c` | Add `cybeer_storage_rename_participant()`. Debug claim persistence. |
| `firmware/components/cybeer_storage/include/cybeer_storage.h` | Declare new function. |
| `firmware/components/cybeer_power/cybeer_power.c` | Add eco mode flag + toggle. Export sleep-trigger function. |
| `firmware/components/cybeer_power/include/cybeer_power.h` | Declare eco/sleep API. |
| `frontend/admin.html` | Add "Управление устройством" section with 3 buttons. |
| `frontend/admin.js` | Add click handlers for power buttons. |
| `frontend/player.html` | Add inline rename UI. |
| `frontend/app.js` | Add rename fetch logic (or inline in player.html script). |

---

## Out of Scope

- Deep sleep (ESP32-C3 GPIO9 can't wake from deep sleep)
- Admin-only rename
- Participant avatars / colors / QR codes
- Confirmation modal for reboot
- Rate limiting on rename endpoint
