# WiFi Fallback + Admin WiFi Panel + Mobile Admin UX

**Date:** 2026-05-22
**Status:** Approved

## Problem

1. SoftAP is always on (APSTA mode) even when STA is connected — wastes resources and clutters the radio.
2. No way to configure WiFi from the admin panel — only possible through captive portal on first setup.
3. Admin panel forms are poorly adapted for mobile phones — small inputs, no structure, endless scroll.

## Requirements

- When STA credentials exist, start in STA-only mode (no AP).
- After 5 failed STA connection attempts (~20–30s with exponential backoff), activate fallback AP with captive DNS and LED indication.
- Admin panel must include a full WiFi management section: status, scan, connect, forget.
- Admin panel must be usable on mobile phones — collapsible sections, touch-friendly form controls.
- WiFi changes trigger a device reboot (no hot-switch).

---

## 1. WiFi Fallback: STA-only with AP Fallback

### Current behavior

On boot with STA credentials in NVS, the device enters APSTA mode — SoftAP is always active alongside STA. Reconnect uses exponential backoff (2s → 30s) indefinitely.

### New behavior

**Boot with STA credentials:**
- Start in `WIFI_MODE_STA` (no AP, no captive DNS).
- Attempt STA connection with existing exponential backoff (2s base, 30s max).

**After 5 failed STA attempts:**
- Call new function `activate_fallback_ap()`.
- Switch to `WIFI_MODE_APSTA`.
- Configure and start SoftAP (`CyBeer-XXXXXX`, open, 192.168.4.1).
- Start captive DNS task.
- Set LED effect `CYBEER_LED_FX_WIFI_SETUP` (blue pulse).
- Continue STA reconnect attempts in background (in case the router comes back).

**Boot without STA credentials:**
- Unchanged — immediate AP mode with captive DNS and LED indication.

**After successful provisioning (from setup page or admin panel):**
- Save credentials to NVS, reboot.

### Changes

| File | Change |
|------|--------|
| `cybeer_wifi.c` — `cybeer_wifi_start()` | If `have_sta`: use `WIFI_MODE_STA` instead of `WIFI_MODE_APSTA`. Skip AP netif creation, captive DNS, and AP config. |
| `cybeer_wifi.c` — `schedule_sta_reconnect()` | Check `s_sta_retry_count >= 5`. If threshold reached, call `activate_fallback_ap()`. |
| `cybeer_wifi.c` — new `activate_fallback_ap()` | Create AP netif if not exists, configure DHCP, switch to `WIFI_MODE_APSTA`, set AP config, start captive DNS task, set LED effect. Guard against double activation with a static bool. |
| `cybeer_wifi.c` — `ip_event()` | On `IP_EVENT_STA_GOT_IP`: reset retry counter. Fallback AP stays active until next reboot (simplicity; no runtime teardown of AP netif). |

### Threshold rationale

5 retries with exponential backoff (2→4→8→16→30s) = ~60s total wait before fallback. Fast enough for "brought to a new venue" scenario, long enough to survive a brief router hiccup.

---

## 2. WiFi Section in Admin Panel

### Existing API (reused as-is)

| Endpoint | Method | Auth | Purpose |
|----------|--------|------|---------|
| `/api/setup/scan` | GET | None | Scan visible WiFi networks, return `[{ssid, rssi, auth}]` |
| `/api/setup/wifi` | POST | None | Save `{ssid, password}` to NVS, reboot |
| `/api/admin/wifi/forget` | POST | PIN | Clear STA credentials from NVS, reboot |
| `/api/status` | GET | None | Device status including `wifi: {sta, ap, staIp}` |

### API addition

**`/api/status` — add fields:**

```json
{
  "wifi": {
    "sta": true,
    "ap": false,
    "staIp": "192.168.1.42",
    "ssid": "HomeNetwork",
    "rssi": -54
  }
}
```

Implementation: call `esp_wifi_sta_get_ap_info()` in the status handler to get current SSID and RSSI. ~5 lines in `cybeer_web.c`.

### Admin UI

New section **"WiFi"** at the top of `admin.html` (first section, most important when relocating device).

**Status block:**
- Current SSID and IP (or "Не подключена" if no STA).
- Signal strength as text (e.g. "Сигнал: -54 dBm").
- Mode indicator: "STA" / "Точка доступа" / "Fallback AP".

**Scan + Connect form:**
- Button "Сканировать сети" → calls `/api/setup/scan`.
- Dropdown with results: `SSID (RSSI dBm, auth_type)`.
- Password field (placeholder: "пусто для открытой сети").
- Button "Подключить" → calls `/api/setup/wifi`, shows "Перезагрузка...".

**Forget button:**
- Red danger button "Забыть сеть".
- `confirm()` dialog before calling `/api/admin/wifi/forget`.
- Requires admin PIN (sent via `X-Admin-Pin` header).

### JS changes (`admin.js`)

New functions:
- `loadWifiStatus()` — called on page load, populates status block from `/api/status`.
- `scanWifi()` — scan button handler, populates network dropdown.
- `connectWifi(ssid, password)` — connect button handler.
- `forgetWifi()` — forget button handler with confirmation.

Logic mirrors `setup.html` script but adapted to admin panel conventions (uses `pinHeaders()`, `showMsg()`, admin CSS classes).

---

## 3. Mobile Admin UX

### 3a. Collapsible sections (accordions)

Replace flat `<h2>` headings in `admin.html` with `<details>`/`<summary>` elements. Native HTML — no JS required, accessible, works everywhere.

**Sections:**

| Section | Default state |
|---------|---------------|
| WiFi | Open if no STA connection, closed otherwise |
| PIN админки | Closed |
| Ручной заезд | Closed |
| Редактировать / удалить заезд | Closed |
| Данные и экспорт | Closed (merge current "Данные" + "Экспорт") |
| LED-лента | Closed |
| Турнир | Closed |

**Styling:**
- `<summary>` styled like current `<h2>` — gold color, same font size.
- Disclosure arrow (CSS `::marker` or `::before` triangle) rotates on open.
- Smooth content reveal via CSS (`details[open] > :not(summary)` with max-height transition or similar).

### 3b. Mobile media queries

Add `@media (max-width: 600px)` block to `app.css`:

| Element | Mobile style | Reason |
|---------|-------------|--------|
| `input, select, textarea` | `width: 100%; padding: 0.6rem; font-size: 1rem` | Prevent iOS zoom on focus, full-width for fat fingers |
| `button` | `min-height: 44px; width: 100%` | Apple HIG minimum touch target |
| `textarea` | Remove `cols` attribute (HTML change), `width: 100%` | Prevent horizontal overflow |
| `label` | `font-size: 0.95rem; margin-top: 0.75rem` | Readable on small screens |
| `.panel` | `padding: 0.75rem` | Reduce side padding to give content more room |
| `main` | `padding: 0.75rem` | Same |

### Files changed

| File | Change |
|------|--------|
| `frontend/admin.html` | Add WiFi section, wrap sections in `<details>/<summary>`, remove `cols="72"` from textarea |
| `frontend/app.css` | Style `<details>/<summary>`, add `@media (max-width: 600px)` block |
| `frontend/admin.js` | Add WiFi functions: `loadWifiStatus()`, `scanWifi()`, `connectWifi()`, `forgetWifi()`. Wire WiFi section default open/closed based on status. |

---

## Out of Scope

- Mobile adaptation for non-admin pages (index, player, tournament, spectator) — currently acceptable.
- Hamburger menu / navigation changes.
- PWA / service worker.
- Hot-switch WiFi without reboot.
- Disabling fallback AP after STA reconnects (can be added later; for now AP stays up once activated until next reboot).

## Risk Notes

- `esp_wifi_set_mode()` at runtime (STA → APSTA) is supported by ESP-IDF but should be tested for stability, especially if a timer run is in progress.
- Fallback AP activation creates AP netif mid-lifecycle — must guard against double-init.
- WiFi scan from admin panel temporarily interrupts STA connection on ESP32-C3 (single radio) — acceptable, scan takes ~2 seconds.
