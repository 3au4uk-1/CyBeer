# OTA Bundle Update System — Design Spec

## Overview

Единая система OTA-обновлений для CyBeer, позволяющая нетехническим организаторам обновлять прошивку и веб-интерфейс устройства одним действием через admin-панель.

## Requirements

- Обновление firmware + frontend (LittleFS) одним файлом за одно действие
- Автоматическая проверка новых версий на GitHub Releases
- Ручная загрузка `.cyb` файла как fallback
- Прогресс-бар и WebSocket-нотификации в UI
- LED-индикация этапов обновления
- Защита от порчи устройства при сбоях
- Admin PIN авторизация на всех OTA endpoints

## Bundle Format

Единый файл `.cyb` с бинарным хедером:

```
Offset  Size    Field
0x00    4       Magic: "CYBR" (ASCII)
0x04    1       Header version: 1
0x05    4       Firmware size (uint32_t, little-endian)
0x09    4       LittleFS image size (uint32_t, little-endian)
0x0D    32      Firmware SHA-256
0x2D    32      LittleFS SHA-256
0x4D    32      Version string (null-terminated, zero-padded)
────────────────────────────────────────────
0x6D    N       Firmware binary
0x6D+N  M       LittleFS image
```

Header size: **109 bytes** (0x6D).

Constraints:
- Firmware size ≤ 0x140000 (1,310,720 bytes)
- LittleFS size ≤ 0x30000 (196,608 bytes)
- Magic must be exactly "CYBR"
- Header version must be 1 (future-proof for format changes)

## Build Pipeline

### Python script: `tools/build_bundle.py`

Input:
- `firmware/build/cybeer.bin` (app binary)
- `firmware/build/littlefs.bin` (LittleFS image)
- Version from `firmware/CMakeLists.txt` (`PROJECT_VER`)

Output:
- `firmware/build/cybeer-v{VERSION}.cyb`

The script:
1. Reads both binaries
2. Computes SHA-256 for each
3. Constructs header with magic, sizes, hashes, version
4. Concatenates header + firmware + littlefs
5. Writes output file

### Integration with build

Optional CMake custom target `bundle`:
```
idf.py build && python tools/build_bundle.py
```

### GitHub Actions (optional, future)

On tag push `v*`:
1. Build firmware with `idf.py build`
2. Run `build_bundle.py`
3. Create GitHub Release with `.cyb` file attached
4. Update `version.json` in `main` branch

## Server: GitHub Releases + Manifest

### `version.json` in repository root (branch `main`)

```json
{
  "version": "1.2.0",
  "url": "https://github.com/zau/CyBeer/releases/download/v1.2.0/cybeer-v1.2.0.cyb",
  "size": 1540096,
  "sha256": "ab12cd34ef56...",
  "changelog": "Исправлен таймер, новый эффект LED"
}
```

### Manifest URL configuration

The manifest URL is a compile-time constant defined in `cybeer_config.h`:
```c
#define CYBEER_OTA_MANIFEST_URL "https://raw.githubusercontent.com/zau/CyBeer/main/version.json"
```

This avoids runtime configuration complexity. To change the URL, rebuild firmware.

### How the device checks for updates

1. `GET` the configured manifest URL
2. Parse JSON, extract `version`
3. Compare with current `PROJECT_VER` (semver: major.minor.patch numeric comparison)
4. If remote version is newer → report available update to UI
5. Cache result in RAM for 5 minutes (avoid repeated requests)

### Why `raw.githubusercontent.com`

- HTTPS with valid certificate (ESP32 CA bundle covers it)
- No API tokens needed for public repositories
- Tiny response (~200 bytes)
- No rate limiting concerns for single-device polling

## Firmware: OTA Update Logic

### Entry points

Two paths converge into a single processing function:

1. **Auto-check + download**: device fetches `.cyb` from URL in `version.json`
2. **Manual upload**: user uploads `.cyb` file through admin panel

### Update algorithm

```
1. Receive data (HTTP chunked download OR multipart upload)
2. Read header (first 109 bytes)
   - Validate magic == "CYBR"
   - Validate header_version == 1
   - Validate firmware_size ≤ 0x140000
   - Validate littlefs_size ≤ 0x30000
3. Stream firmware to inactive OTA slot (chunk by chunk, 4-8 KB)
   - Compute SHA-256 incrementally (mbedtls_sha256)
   - On completion: verify SHA-256 matches header
4. Stream LittleFS image to LittleFS partition (esp_partition_erase + write)
   - Compute SHA-256 incrementally
   - On completion: verify SHA-256 matches header
5. If BOTH parts verified:
   - esp_ota_set_boot_partition() → new OTA slot
   - Send WebSocket "ota_done"
   - Reboot after 3 seconds
6. On ANY error:
   - Do NOT change boot partition
   - Report error via WebSocket + HTTP response
   - Device remains on current working version
```

### Memory constraints

ESP32-C3 has ~320 KB RAM. Bundle is ~1.5 MB — cannot fit in memory.

- Stream processing: read 4-8 KB chunks, write to flash immediately
- Header parsed from first chunk
- SHA-256 computed incrementally (mbedtls_sha256_update per chunk)
- No full-file buffering at any stage

### Progress reporting

Via WebSocket on existing connection:
```json
{"type": "ota_progress", "percent": 42, "stage": "firmware"}
{"type": "ota_progress", "percent": 85, "stage": "littlefs"}
{"type": "ota_done"}
{"type": "ota_error", "message": "SHA-256 mismatch"}
```

Percent calculated from bytes_written / total_expected_size (firmware + littlefs combined).

Stages: `downloading` → `firmware` → `littlefs` → `rebooting` | `error`

### Write order rationale

Firmware is written first (to inactive OTA slot — safe, current firmware unaffected).
LittleFS is written second (in-place overwrite — no dual partition for FS).

If LittleFS write fails:
- Boot partition is NOT changed
- Device keeps running current firmware + current (possibly corrupted) LittleFS
- Mitigation: firmware includes a minimal embedded "OTA recovery" HTML page served if LittleFS mount fails after reboot

## API Endpoints

All OTA endpoints require `X-Admin-Pin` header.

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/api/admin/ota/check` | GET | Check `version.json`, return `{available, version, changelog, size, currentVersion}` |
| `/api/admin/ota/start` | POST | Begin download of bundle from manifest URL. Returns 202. |
| `/api/admin/ota/upload` | POST | Upload `.cyb` file (multipart/form-data or raw binary). Returns 202. |
| `/api/admin/ota/status` | GET | Current OTA status `{active, percent, stage, error}` (for reconnection) |

Existing mutex prevents concurrent OTA (409 if busy).

### Replacing old endpoints

Old endpoints `/api/admin/ota/url` and `/api/admin/ota/upload` (raw .bin) are replaced:
- `/api/admin/ota/url` → `/api/admin/ota/start` (now uses manifest internally)
- `/api/admin/ota/upload` → same path, but now expects `.cyb` format

Legacy `.bin` upload support can be dropped (USB flashing covers that case).

## Admin UI

New accordion section "Обновление прошивки" in `admin.html`.

### States

1. **Up to date**: "Версия 1.1.0 — актуальна" (green checkmark)
2. **Update available**: "Доступна v1.2.0" + changelog text + "Обновить" button
3. **Updating**: progress bar with stage labels, all other admin controls disabled, "Не закрывайте страницу" warning
4. **Error**: error message + "Повторить" button
5. **Success**: "Обновлено! Перезагрузка..." → auto-reload page after reboot

### Elements

- Current version display (from `/api/status` → `firmwareVersion`)
- "Проверить обновления" button (manual check trigger)
- Changelog block (when update available)
- "Обновить" primary button
- Separator
- "Загрузить файл вручную" section: file input (accept=`.cyb`) + "Загрузить" button
- Progress bar + stage text (driven by WebSocket messages)

### Behavior

- On admin panel open → auto-check via `GET /api/admin/ota/check`
- During OTA → all other admin sections disabled
- After success → page auto-reloads after device reboots (~5-7 seconds with spinner)

## Error Handling

| Situation | Response |
|-----------|----------|
| No Wi-Fi / server unreachable | "Не удалось проверить обновления" |
| Invalid file (bad magic / not .cyb) | "Неверный формат файла" |
| Firmware/LittleFS too large for partition | "Файл слишком большой для этого устройства" |
| SHA-256 mismatch (firmware) | Do not set boot partition. "Ошибка целостности прошивки" |
| SHA-256 mismatch (LittleFS) | Do not set boot partition. "Ошибка целостности, обновление отменено" |
| Connection lost during download | "Загрузка прервана. Попробуйте снова" |
| OTA already in progress | 409 "Обновление уже выполняется" |

**Core safety principle:** boot partition changes ONLY when both firmware and LittleFS are written and SHA-256 verified. Any failure → device stays on current working version.

**ESP-IDF rollback:** if new firmware crashes on boot (before calling `esp_ota_mark_app_valid_cancel_rollback()`), bootloader automatically reverts to previous slot. Transparent to user.

## LED Indication

New effects in `cybeer_led`:

| Stage | Effect |
|-------|--------|
| Downloading / receiving | Slow blue breathing |
| Writing firmware | Blue chase (running) |
| Writing LittleFS | Cyan chase |
| Success | Green flash (3 seconds) → reboot |
| Error | Red triple flash → return to normal |

New enum values:
- `CYBEER_LED_FX_OTA_DOWNLOAD`
- `CYBEER_LED_FX_OTA_WRITE`
- `CYBEER_LED_FX_OTA_OK`
- `CYBEER_LED_FX_OTA_FAIL`

## Version Comparison

Simple semver numeric comparison (no pre-release suffixes):
- Parse "X.Y.Z" into three integers
- Compare major, then minor, then patch
- Remote > local → update available

## Component Changes Summary

| Component | Changes |
|-----------|---------|
| `cybeer_web` / `cybeer_ota` | Rewrite OTA logic for bundle format, add `/check`, `/start`, `/status` endpoints, WebSocket progress |
| `cybeer_led` | Add OTA LED effects |
| `frontend/admin.html` + `admin.js` | New "Обновление" section |
| `tools/build_bundle.py` | New script |
| `version.json` | New file in repo root |
| `firmware/CMakeLists.txt` | Optional `bundle` custom target |
