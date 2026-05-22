# OTA Bundle Update System — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Enable one-click OTA updates (firmware + frontend) for non-technical users via admin panel, with GitHub Releases as update source.

**Architecture:** Custom `.cyb` bundle format containing firmware + LittleFS image with SHA-256 verification. Device checks GitHub for new versions, downloads and flashes both partitions atomically. Progress shown via WebSocket + LED effects.

**Tech Stack:** ESP-IDF 6.x (C), vanilla HTML/JS frontend, Python build tooling, mbedtls SHA-256, esp_partition API, esp_https_ota-like streaming.

---

## File Structure

| File | Responsibility |
|------|----------------|
| `firmware/components/cybeer_config/include/cybeer_config.h` | Add `CYBEER_OTA_MANIFEST_URL` constant |
| `firmware/components/cybeer_web/include/cybeer_ota.h` | OTA public API (rewrite) |
| `firmware/components/cybeer_web/cybeer_ota.c` | Bundle OTA logic: header parsing, streaming flash, SHA-256 verification, version check (rewrite) |
| `firmware/components/cybeer_web/include/cybeer_ws.h` | Add `cybeer_ws_broadcast_json()` declaration |
| `firmware/components/cybeer_web/cybeer_ws.c` | Add public broadcast helper |
| `firmware/components/cybeer_led/include/cybeer_led.h` | Add OTA LED effect enums |
| `firmware/components/cybeer_led/cybeer_led.c` | Implement OTA LED effects |
| `frontend/admin.html` | Add "Обновление прошивки" accordion section |
| `frontend/admin.js` | OTA UI logic: check, update, upload, progress |
| `tools/build_bundle.py` | Python script to create `.cyb` bundle |
| `version.json` | Manifest file (repo root) |

---

## Task 1: Build Bundle Script (`tools/build_bundle.py`)

**Files:**
- Create: `tools/build_bundle.py`

- [ ] **Step 1: Create the bundle script**

```python
#!/usr/bin/env python3
"""Build CyBeer OTA bundle (.cyb) from firmware and LittleFS images."""

import hashlib
import re
import struct
import sys
from pathlib import Path

MAGIC = b"CYBR"
HEADER_VERSION = 1
HEADER_SIZE = 109  # 4 + 1 + 4 + 4 + 32 + 32 + 32
MAX_FW_SIZE = 0x140000
MAX_FS_SIZE = 0x30000


def get_version_from_cmake(firmware_dir: Path) -> str:
    cmake_file = firmware_dir / "CMakeLists.txt"
    text = cmake_file.read_text()
    m = re.search(r"project\(\s*cybeer\s+VERSION\s+([\d.]+)\s*\)", text)
    if not m:
        sys.exit("ERROR: Could not parse PROJECT_VER from CMakeLists.txt")
    return m.group(1)


def build_bundle(firmware_dir: Path) -> Path:
    version = get_version_from_cmake(firmware_dir)
    fw_path = firmware_dir / "build" / "cybeer.bin"
    fs_path = firmware_dir / "build" / "littlefs.bin"

    if not fw_path.exists():
        sys.exit(f"ERROR: {fw_path} not found. Run 'idf.py build' first.")
    if not fs_path.exists():
        sys.exit(f"ERROR: {fs_path} not found. Run 'idf.py build' first.")

    fw_data = fw_path.read_bytes()
    fs_data = fs_path.read_bytes()

    if len(fw_data) > MAX_FW_SIZE:
        sys.exit(f"ERROR: firmware too large ({len(fw_data)} > {MAX_FW_SIZE})")
    if len(fs_data) > MAX_FS_SIZE:
        sys.exit(f"ERROR: LittleFS image too large ({len(fs_data)} > {MAX_FS_SIZE})")

    fw_sha = hashlib.sha256(fw_data).digest()
    fs_sha = hashlib.sha256(fs_data).digest()

    version_bytes = version.encode("ascii")[:31].ljust(32, b"\x00")

    header = bytearray()
    header += MAGIC
    header += struct.pack("<B", HEADER_VERSION)
    header += struct.pack("<I", len(fw_data))
    header += struct.pack("<I", len(fs_data))
    header += fw_sha
    header += fs_sha
    header += version_bytes

    assert len(header) == HEADER_SIZE

    bundle = bytes(header) + fw_data + fs_data
    out_path = firmware_dir / "build" / f"cybeer-v{version}.cyb"
    out_path.write_bytes(bundle)

    bundle_sha = hashlib.sha256(bundle).hexdigest()
    print(f"Bundle: {out_path}")
    print(f"  Version:  {version}")
    print(f"  Firmware: {len(fw_data)} bytes")
    print(f"  LittleFS: {len(fs_data)} bytes")
    print(f"  Total:    {len(bundle)} bytes")
    print(f"  SHA-256:  {bundle_sha}")
    return out_path


if __name__ == "__main__":
    firmware_dir = Path(__file__).resolve().parent.parent / "firmware"
    if len(sys.argv) > 1:
        firmware_dir = Path(sys.argv[1])
    build_bundle(firmware_dir)
```

- [ ] **Step 2: Verify the script runs (dry run sanity check)**

Run: `python tools/build_bundle.py`
Expected: Error message "firmware.bin not found" (since we haven't built yet). This confirms the script loads and parses args correctly.

- [ ] **Step 3: Commit**

```bash
git add tools/build_bundle.py
git commit -m "feat(ota): add bundle build script (.cyb format)"
```

---

## Task 2: WebSocket Broadcast Helper

**Files:**
- Modify: `firmware/components/cybeer_web/include/cybeer_ws.h`
- Modify: `firmware/components/cybeer_web/cybeer_ws.c`

- [ ] **Step 1: Add declaration to `cybeer_ws.h`**

Add after existing declarations in `firmware/components/cybeer_web/include/cybeer_ws.h`:

```c
#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

esp_err_t cybeer_ws_register(httpd_handle_t hd);
void cybeer_ws_broadcast_state(void);
void cybeer_ws_broadcast_state_deferred(int64_t delay_us);
void cybeer_ws_timer_tick(int64_t now_us);
void cybeer_ws_on_run_finished(const char *run_id, int64_t duration_us);
void cybeer_ws_broadcast_leaderboard_update(void);
void cybeer_ws_broadcast_text(const char *text, size_t len);
```

- [ ] **Step 2: Expose `broadcast_text` as public in `cybeer_ws.c`**

Add a public wrapper at the end of `cybeer_ws.c` (before the closing of the file):

```c
void cybeer_ws_broadcast_text(const char *text, size_t len)
{
    broadcast_text(text, len);
}
```

- [ ] **Step 3: Build to verify compilation**

Run: `cd firmware && idf.py build`
Expected: BUILD SUCCESS

- [ ] **Step 4: Commit**

```bash
git add firmware/components/cybeer_web/include/cybeer_ws.h firmware/components/cybeer_web/cybeer_ws.c
git commit -m "feat(ws): expose broadcast_text as public API for OTA progress"
```

---

## Task 3: OTA LED Effects

**Files:**
- Modify: `firmware/components/cybeer_led/include/cybeer_led.h`
- Modify: `firmware/components/cybeer_led/cybeer_led.c`

- [ ] **Step 1: Add OTA enums to `cybeer_led.h`**

Replace the enum in `firmware/components/cybeer_led/include/cybeer_led.h`:

```c
typedef enum {
    CYBEER_LED_FX_AMBIENT,
    CYBEER_LED_FX_ARMED,
    CYBEER_LED_FX_RUNNING,
    CYBEER_LED_FX_FINISHED,
    CYBEER_LED_FX_CLAIM_PENDING,
    CYBEER_LED_FX_PODIUM,
    CYBEER_LED_FX_WIFI_SETUP,
    CYBEER_LED_FX_OTA_DOWNLOAD,
    CYBEER_LED_FX_OTA_WRITE,
    CYBEER_LED_FX_OTA_OK,
    CYBEER_LED_FX_OTA_FAIL,
} cybeer_led_fx_t;
```

- [ ] **Step 2: Implement OTA effects in `cybeer_led.c`**

Add cases in the `cybeer_led_task_tick` function's switch/effect rendering section. The effects are:

- `CYBEER_LED_FX_OTA_DOWNLOAD`: slow blue breathing (tri_wave at 1500ms half-period, color 0,0,blue)
- `CYBEER_LED_FX_OTA_WRITE`: blue chase (rotating single bright pixel at ~100ms step, color 0,40,255)
- `CYBEER_LED_FX_OTA_OK`: solid green for 3 seconds, then return to AMBIENT
- `CYBEER_LED_FX_OTA_FAIL`: red triple flash (150ms on, 150ms off, 3 times), then return to AMBIENT

Implementation requires reading the existing effect pattern in `cybeer_led.c` (how other effects like `CYBEER_LED_FX_FINISHED` use timers and state) and following the same style. Key points:
- Use `s_fx_requested` for state
- Use timestamp tracking similar to `s_finished_enter_us` for timed effects
- OTA_OK and OTA_FAIL are transient — after their animation completes, set `s_fx_requested = CYBEER_LED_FX_AMBIENT`

- [ ] **Step 3: Build to verify compilation**

Run: `cd firmware && idf.py build`
Expected: BUILD SUCCESS

- [ ] **Step 4: Commit**

```bash
git add firmware/components/cybeer_led/
git commit -m "feat(led): add OTA progress LED effects (download, write, ok, fail)"
```

---

## Task 4: OTA Config Constant

**Files:**
- Modify: `firmware/components/cybeer_config/include/cybeer_config.h`

- [ ] **Step 1: Add manifest URL to config**

Add at the end of `firmware/components/cybeer_config/include/cybeer_config.h`:

```c
#define CYBEER_OTA_MANIFEST_URL "https://raw.githubusercontent.com/zau/CyBeer/main/version.json"
#define CYBEER_OTA_CHECK_CACHE_SEC 300
```

- [ ] **Step 2: Build to verify**

Run: `cd firmware && idf.py build`
Expected: BUILD SUCCESS

- [ ] **Step 3: Commit**

```bash
git add firmware/components/cybeer_config/
git commit -m "feat(config): add OTA manifest URL constant"
```

---

## Task 5: Rewrite OTA Core Logic

**Files:**
- Modify: `firmware/components/cybeer_web/include/cybeer_ota.h`
- Modify: `firmware/components/cybeer_web/cybeer_ota.c`

This is the largest task. The existing `cybeer_ota.c` (460 lines) is entirely replaced with new bundle-aware logic.

- [ ] **Step 1: Rewrite `cybeer_ota.h`**

```c
#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

/** Bundle header: "CYBR" + version + sizes + SHA-256s + version string */
#define CYBEER_OTA_HEADER_SIZE 109
#define CYBEER_OTA_MAGIC "CYBR"

typedef struct {
    uint8_t header_version;
    uint32_t firmware_size;
    uint32_t littlefs_size;
    uint8_t firmware_sha256[32];
    uint8_t littlefs_sha256[32];
    char version[32];
} cybeer_ota_header_t;

esp_err_t cybeer_ota_register_handlers(httpd_handle_t server);
```

- [ ] **Step 2: Implement new `cybeer_ota.c` — header parsing**

The file starts with includes, static state, and the header parser:

```c
#include "cybeer_ota.h"

#include <string.h>
#include <stdlib.h>

#include "cybeer_config.h"
#include "cybeer_led.h"
#include "cybeer_storage.h"
#include "cybeer_ws.h"

#include "cJSON.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"

static const char *TAG = "cybeer_ota";

#define OTA_BUF_SIZE 4096
#define OTA_STACK_SIZE (1024 * 12)

static SemaphoreHandle_t s_ota_mx;

typedef struct {
    bool active;
    int percent;
    char stage[16];
    char error[64];
} ota_status_t;

static ota_status_t s_status;

static void ota_set_status(const char *stage, int percent, const char *error)
{
    s_status.active = (error == NULL && strcmp(stage, "done") != 0);
    s_status.percent = percent;
    strncpy(s_status.stage, stage, sizeof(s_status.stage) - 1);
    if (error) {
        strncpy(s_status.error, error, sizeof(s_status.error) - 1);
    } else {
        s_status.error[0] = '\0';
    }
}

static void ota_broadcast_progress(const char *stage, int percent)
{
    ota_set_status(stage, percent, NULL);
    char buf[96];
    int n = snprintf(buf, sizeof(buf),
                     "{\"type\":\"ota_progress\",\"percent\":%d,\"stage\":\"%s\"}",
                     percent, stage);
    cybeer_ws_broadcast_text(buf, (size_t)n);
}

static void ota_broadcast_done(void)
{
    ota_set_status("done", 100, NULL);
    const char *msg = "{\"type\":\"ota_done\"}";
    cybeer_ws_broadcast_text(msg, strlen(msg));
}

static void ota_broadcast_error(const char *message)
{
    ota_set_status("error", 0, message);
    char buf[128];
    int n = snprintf(buf, sizeof(buf),
                     "{\"type\":\"ota_error\",\"message\":\"%s\"}", message);
    cybeer_ws_broadcast_text(buf, (size_t)n);
}

static bool parse_header(const uint8_t *data, cybeer_ota_header_t *hdr)
{
    if (memcmp(data, CYBEER_OTA_MAGIC, 4) != 0) return false;
    hdr->header_version = data[4];
    if (hdr->header_version != 1) return false;
    memcpy(&hdr->firmware_size, data + 5, 4);
    memcpy(&hdr->littlefs_size, data + 9, 4);
    memcpy(hdr->firmware_sha256, data + 13, 32);
    memcpy(hdr->littlefs_sha256, data + 45, 32);
    memcpy(hdr->version, data + 77, 32);
    hdr->version[31] = '\0';
    if (hdr->firmware_size > 0x140000) return false;
    if (hdr->littlefs_size > 0x30000) return false;
    return true;
}
```

- [ ] **Step 3: Implement bundle flash logic (streaming)**

Add the core flash function that processes a stream of data (used by both upload and download paths):

```c
typedef struct {
    cybeer_ota_header_t hdr;
    bool header_parsed;
    uint8_t header_buf[CYBEER_OTA_HEADER_SIZE];
    size_t header_received;

    esp_ota_handle_t ota_handle;
    const esp_partition_t *ota_part;
    const esp_partition_t *fs_part;
    bool ota_begun;

    mbedtls_sha256_context fw_sha_ctx;
    mbedtls_sha256_context fs_sha_ctx;
    size_t fw_written;
    size_t fs_written;
    size_t total_size;
} ota_stream_ctx_t;

static esp_err_t ota_stream_init(ota_stream_ctx_t *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    mbedtls_sha256_init(&ctx->fw_sha_ctx);
    mbedtls_sha256_init(&ctx->fs_sha_ctx);
    mbedtls_sha256_starts(&ctx->fw_sha_ctx, 0);
    mbedtls_sha256_starts(&ctx->fs_sha_ctx, 0);
    return ESP_OK;
}

static esp_err_t ota_stream_feed(ota_stream_ctx_t *ctx, const uint8_t *data, size_t len)
{
    size_t offset = 0;

    /* Phase 1: accumulate header */
    if (!ctx->header_parsed) {
        size_t need = CYBEER_OTA_HEADER_SIZE - ctx->header_received;
        size_t take = (len < need) ? len : need;
        memcpy(ctx->header_buf + ctx->header_received, data, take);
        ctx->header_received += take;
        offset += take;

        if (ctx->header_received < CYBEER_OTA_HEADER_SIZE) {
            return ESP_OK; /* need more data */
        }

        if (!parse_header(ctx->header_buf, &ctx->hdr)) {
            return ESP_ERR_INVALID_ARG;
        }
        ctx->header_parsed = true;
        ctx->total_size = ctx->hdr.firmware_size + ctx->hdr.littlefs_size;

        /* Begin OTA */
        ctx->ota_part = esp_ota_get_next_update_partition(NULL);
        if (!ctx->ota_part) return ESP_ERR_NOT_FOUND;

        esp_err_t err = esp_ota_begin(ctx->ota_part, ctx->hdr.firmware_size, &ctx->ota_handle);
        if (err != ESP_OK) return err;
        ctx->ota_begun = true;

        /* Find LittleFS partition */
        ctx->fs_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                 ESP_PARTITION_SUBTYPE_ANY, "littlefs");
        if (!ctx->fs_part) {
            esp_ota_abort(ctx->ota_handle);
            return ESP_ERR_NOT_FOUND;
        }

        /* Erase LittleFS partition upfront */
        err = esp_partition_erase_range(ctx->fs_part, 0, ctx->fs_part->size);
        if (err != ESP_OK) {
            esp_ota_abort(ctx->ota_handle);
            return err;
        }

        cybeer_led_set_fx(CYBEER_LED_FX_OTA_WRITE);
    }

    /* Phase 2: write firmware */
    while (offset < len && ctx->fw_written < ctx->hdr.firmware_size) {
        size_t remaining_fw = ctx->hdr.firmware_size - ctx->fw_written;
        size_t chunk = len - offset;
        if (chunk > remaining_fw) chunk = remaining_fw;

        esp_err_t err = esp_ota_write(ctx->ota_handle, data + offset, chunk);
        if (err != ESP_OK) return err;

        mbedtls_sha256_update(&ctx->fw_sha_ctx, data + offset, chunk);
        ctx->fw_written += chunk;
        offset += chunk;

        int pct = (int)((ctx->fw_written * 80ULL) / ctx->total_size);
        ota_broadcast_progress("firmware", pct);
    }

    /* Phase 3: write LittleFS */
    while (offset < len && ctx->fs_written < ctx->hdr.littlefs_size) {
        size_t remaining_fs = ctx->hdr.littlefs_size - ctx->fs_written;
        size_t chunk = len - offset;
        if (chunk > remaining_fs) chunk = remaining_fs;

        esp_err_t err = esp_partition_write(ctx->fs_part, ctx->fs_written, data + offset, chunk);
        if (err != ESP_OK) return err;

        mbedtls_sha256_update(&ctx->fs_sha_ctx, data + offset, chunk);
        ctx->fs_written += chunk;
        offset += chunk;

        int pct = 80 + (int)((ctx->fs_written * 20ULL) / ctx->hdr.littlefs_size);
        ota_broadcast_progress("littlefs", pct);
    }

    return ESP_OK;
}

static esp_err_t ota_stream_finish(ota_stream_ctx_t *ctx)
{
    if (!ctx->header_parsed || !ctx->ota_begun) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Verify firmware SHA-256 */
    uint8_t digest[32];
    mbedtls_sha256_finish(&ctx->fw_sha_ctx, digest);
    if (memcmp(digest, ctx->hdr.firmware_sha256, 32) != 0) {
        esp_ota_abort(ctx->ota_handle);
        return ESP_ERR_INVALID_CRC;
    }

    /* Verify LittleFS SHA-256 */
    mbedtls_sha256_finish(&ctx->fs_sha_ctx, digest);
    if (memcmp(digest, ctx->hdr.littlefs_sha256, 32) != 0) {
        esp_ota_abort(ctx->ota_handle);
        return ESP_ERR_INVALID_CRC;
    }

    /* Finalize OTA */
    esp_err_t err = esp_ota_end(ctx->ota_handle);
    if (err != ESP_OK) return err;

    err = esp_ota_set_boot_partition(ctx->ota_part);
    return err;
}

static void ota_stream_cleanup(ota_stream_ctx_t *ctx)
{
    mbedtls_sha256_free(&ctx->fw_sha_ctx);
    mbedtls_sha256_free(&ctx->fs_sha_ctx);
}
```

- [ ] **Step 4: Implement admin PIN helper + JSON error helper**

Reuse the same pattern from the old code:

```c
static bool admin_pin_ok(httpd_req_t *req)
{
    char hdr[CYBEER_ADMIN_PIN_MAX_LEN + 8];
    if (httpd_req_get_hdr_value_str(req, "X-Admin-Pin", hdr, sizeof(hdr)) != ESP_OK) {
        return false;
    }
    size_t n = strlen(hdr);
    while (n > 0 && (hdr[n - 1] == ' ' || hdr[n - 1] == '\t')) {
        hdr[--n] = '\0';
    }
    char *p = hdr;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '\0') return false;
    return cybeer_admin_verify_pin(p) == ESP_OK;
}

static esp_err_t send_json_err(httpd_req_t *req, const char *status, const char *json)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t require_admin(httpd_req_t *req)
{
    if (!cybeer_nvs_admin_pin_is_configured()) {
        return send_json_err(req, "403 Forbidden",
                             "{\"error\":\"admin pin not configured\"}");
    }
    if (!admin_pin_ok(req)) {
        return send_json_err(req, "401 Unauthorized",
                             "{\"error\":\"invalid pin\"}");
    }
    return ESP_OK;
}
```

- [ ] **Step 5: Implement `/api/admin/ota/upload` handler (bundle upload)**

```c
static esp_err_t h_post_ota_upload(httpd_req_t *req)
{
    if (require_admin(req) != ESP_OK) return ESP_OK;

    size_t content_len = (size_t)req->content_len;
    if (content_len <= CYBEER_OTA_HEADER_SIZE || content_len > (0x140000 + 0x30000 + CYBEER_OTA_HEADER_SIZE)) {
        return send_json_err(req, "400 Bad Request", "{\"error\":\"invalid size\"}");
    }

    if (xSemaphoreTake(s_ota_mx, 0) != pdTRUE) {
        return send_json_err(req, "409 Conflict", "{\"error\":\"ota busy\"}");
    }

    cybeer_led_set_fx(CYBEER_LED_FX_OTA_DOWNLOAD);
    ota_broadcast_progress("receiving", 0);

    ota_stream_ctx_t ctx;
    ota_stream_init(&ctx);

    uint8_t buf[OTA_BUF_SIZE];
    size_t received = 0;
    esp_err_t err = ESP_OK;

    while (received < content_len) {
        size_t want = content_len - received;
        if (want > OTA_BUF_SIZE) want = OTA_BUF_SIZE;
        int r = httpd_req_recv(req, (char *)buf, want);
        if (r <= 0) {
            err = ESP_FAIL;
            break;
        }
        received += (size_t)r;
        err = ota_stream_feed(&ctx, buf, (size_t)r);
        if (err != ESP_OK) break;
    }

    if (err == ESP_OK) {
        err = ota_stream_finish(&ctx);
    }

    ota_stream_cleanup(&ctx);

    if (err != ESP_OK) {
        const char *msg = (err == ESP_ERR_INVALID_ARG) ? "invalid bundle format"
                        : (err == ESP_ERR_INVALID_CRC) ? "SHA-256 mismatch"
                        : "flash write error";
        cybeer_led_set_fx(CYBEER_LED_FX_OTA_FAIL);
        ota_broadcast_error(msg);
        xSemaphoreGive(s_ota_mx);
        return send_json_err(req, "400 Bad Request", "{\"error\":\"ota failed\"}");
    }

    cybeer_led_set_fx(CYBEER_LED_FX_OTA_OK);
    ota_broadcast_done();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);

    vTaskDelay(pdMS_TO_TICKS(3000));
    esp_restart();
    return ESP_OK;
}
```

- [ ] **Step 6: Implement version check handler (`/api/admin/ota/check`)**

```c
static bool version_is_newer(const char *remote, const char *local)
{
    int rm, rn, rp, lm, ln, lp;
    if (sscanf(remote, "%d.%d.%d", &rm, &rn, &rp) != 3) return false;
    if (sscanf(local, "%d.%d.%d", &lm, &ln, &lp) != 3) return false;
    if (rm != lm) return rm > lm;
    if (rn != ln) return rn > ln;
    return rp > lp;
}

static esp_err_t h_get_ota_check(httpd_req_t *req)
{
    if (require_admin(req) != ESP_OK) return ESP_OK;

    esp_http_client_config_t cfg = {
        .url = CYBEER_OTA_MANIFEST_URL,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return send_json_err(req, "500 Internal Server Error", "{\"error\":\"http init\"}");
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return send_json_err(req, "502 Bad Gateway", "{\"error\":\"cannot reach update server\"}");
    }

    int content_length = esp_http_client_fetch_headers(client);
    if (content_length <= 0 || content_length > 1024) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return send_json_err(req, "502 Bad Gateway", "{\"error\":\"bad manifest\"}");
    }

    char *manifest = calloc(1, (size_t)content_length + 1);
    if (!manifest) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return send_json_err(req, "500 Internal Server Error", "{\"error\":\"alloc\"}");
    }

    int read_len = esp_http_client_read(client, manifest, content_length);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (read_len <= 0) {
        free(manifest);
        return send_json_err(req, "502 Bad Gateway", "{\"error\":\"read manifest\"}");
    }
    manifest[read_len] = '\0';

    cJSON *root = cJSON_Parse(manifest);
    free(manifest);
    if (!root) {
        return send_json_err(req, "502 Bad Gateway", "{\"error\":\"parse manifest\"}");
    }

    const char *ver = cJSON_GetStringValue(cJSON_GetObjectItem(root, "version"));
    const char *changelog = cJSON_GetStringValue(cJSON_GetObjectItem(root, "changelog"));
    cJSON *size_item = cJSON_GetObjectItem(root, "size");

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "currentVersion", PROJECT_VER);
    cJSON_AddStringToObject(resp, "remoteVersion", ver ? ver : "");
    cJSON_AddBoolToObject(resp, "available", ver && version_is_newer(ver, PROJECT_VER));
    cJSON_AddStringToObject(resp, "changelog", changelog ? changelog : "");
    if (cJSON_IsNumber(size_item)) {
        cJSON_AddNumberToObject(resp, "size", size_item->valuedouble);
    }

    cJSON_Delete(root);

    char *printed = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, printed, HTTPD_RESP_USE_STRLEN);
    free(printed);
    return ESP_OK;
}
```

- [ ] **Step 7: Implement `/api/admin/ota/start` (download from manifest URL)**

```c
typedef struct {
    char url[512];
    size_t expected_size;
} ota_download_args_t;

static void ota_download_task(void *arg)
{
    ota_download_args_t *args = (ota_download_args_t *)arg;

    cybeer_led_set_fx(CYBEER_LED_FX_OTA_DOWNLOAD);
    ota_broadcast_progress("downloading", 0);

    esp_http_client_config_t cfg = {
        .url = args->url,
        .timeout_ms = 60000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = OTA_BUF_SIZE,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        ota_broadcast_error("http init failed");
        cybeer_led_set_fx(CYBEER_LED_FX_OTA_FAIL);
        free(args);
        xSemaphoreGive(s_ota_mx);
        vTaskDelete(NULL);
        return;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ota_broadcast_error("cannot connect to server");
        cybeer_led_set_fx(CYBEER_LED_FX_OTA_FAIL);
        esp_http_client_cleanup(client);
        free(args);
        xSemaphoreGive(s_ota_mx);
        vTaskDelete(NULL);
        return;
    }

    esp_http_client_fetch_headers(client);

    ota_stream_ctx_t ctx;
    ota_stream_init(&ctx);

    uint8_t buf[OTA_BUF_SIZE];
    err = ESP_OK;

    while (1) {
        int r = esp_http_client_read(client, (char *)buf, OTA_BUF_SIZE);
        if (r < 0) {
            err = ESP_FAIL;
            break;
        }
        if (r == 0) break;
        err = ota_stream_feed(&ctx, buf, (size_t)r);
        if (err != ESP_OK) break;
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (err == ESP_OK) {
        err = ota_stream_finish(&ctx);
    }
    ota_stream_cleanup(&ctx);
    free(args);

    if (err != ESP_OK) {
        const char *msg = (err == ESP_ERR_INVALID_ARG) ? "invalid bundle format"
                        : (err == ESP_ERR_INVALID_CRC) ? "SHA-256 mismatch"
                        : "download/flash error";
        ota_broadcast_error(msg);
        cybeer_led_set_fx(CYBEER_LED_FX_OTA_FAIL);
        xSemaphoreGive(s_ota_mx);
        vTaskDelete(NULL);
        return;
    }

    cybeer_led_set_fx(CYBEER_LED_FX_OTA_OK);
    ota_broadcast_done();
    vTaskDelay(pdMS_TO_TICKS(3000));
    esp_restart();
}

static esp_err_t h_post_ota_start(httpd_req_t *req)
{
    if (require_admin(req) != ESP_OK) return ESP_OK;

    if (xSemaphoreTake(s_ota_mx, 0) != pdTRUE) {
        return send_json_err(req, "409 Conflict", "{\"error\":\"ota busy\"}");
    }

    /* Fetch manifest to get URL */
    esp_http_client_config_t cfg = {
        .url = CYBEER_OTA_MANIFEST_URL,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        xSemaphoreGive(s_ota_mx);
        return send_json_err(req, "500 Internal Server Error", "{\"error\":\"http init\"}");
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        xSemaphoreGive(s_ota_mx);
        return send_json_err(req, "502 Bad Gateway", "{\"error\":\"manifest fetch\"}");
    }

    int clen = esp_http_client_fetch_headers(client);
    if (clen <= 0 || clen > 1024) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        xSemaphoreGive(s_ota_mx);
        return send_json_err(req, "502 Bad Gateway", "{\"error\":\"manifest fetch\"}");
    }

    char *manifest = calloc(1, (size_t)clen + 1);
    int rd = esp_http_client_read(client, manifest, clen);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (rd <= 0) {
        free(manifest);
        xSemaphoreGive(s_ota_mx);
        return send_json_err(req, "502 Bad Gateway", "{\"error\":\"manifest read\"}");
    }
    manifest[rd] = '\0';

    cJSON *root = cJSON_Parse(manifest);
    free(manifest);
    if (!root) {
        xSemaphoreGive(s_ota_mx);
        return send_json_err(req, "502 Bad Gateway", "{\"error\":\"manifest parse\"}");
    }

    const char *url = cJSON_GetStringValue(cJSON_GetObjectItem(root, "url"));
    if (!url || strlen(url) > 500 || strncmp(url, "https://", 8) != 0) {
        cJSON_Delete(root);
        xSemaphoreGive(s_ota_mx);
        return send_json_err(req, "502 Bad Gateway", "{\"error\":\"bad url in manifest\"}");
    }

    ota_download_args_t *args = calloc(1, sizeof(*args));
    strncpy(args->url, url, sizeof(args->url) - 1);
    cJSON_Delete(root);

    BaseType_t ok = xTaskCreate(ota_download_task, "ota_dl", OTA_STACK_SIZE, args,
                                tskIDLE_PRIORITY + 5, NULL);
    if (ok != pdPASS) {
        free(args);
        xSemaphoreGive(s_ota_mx);
        return send_json_err(req, "500 Internal Server Error", "{\"error\":\"task create\"}");
    }

    httpd_resp_set_status(req, "202 Accepted");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true,\"status\":\"started\"}", HTTPD_RESP_USE_STRLEN);
}
```

- [ ] **Step 8: Implement `/api/admin/ota/status` and register all handlers**

```c
static esp_err_t h_get_ota_status(httpd_req_t *req)
{
    if (require_admin(req) != ESP_OK) return ESP_OK;

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "active", s_status.active);
    cJSON_AddNumberToObject(resp, "percent", s_status.percent);
    cJSON_AddStringToObject(resp, "stage", s_status.stage);
    if (s_status.error[0]) {
        cJSON_AddStringToObject(resp, "error", s_status.error);
    }
    char *printed = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, printed, HTTPD_RESP_USE_STRLEN);
    free(printed);
    return ESP_OK;
}

esp_err_t cybeer_ota_register_handlers(httpd_handle_t server)
{
    if (!s_ota_mx) {
        s_ota_mx = xSemaphoreCreateMutex();
        if (!s_ota_mx) return ESP_ERR_NO_MEM;
        xSemaphoreGive(s_ota_mx);
    }
    memset(&s_status, 0, sizeof(s_status));

    const httpd_uri_t handlers[] = {
        { .uri = "/api/admin/ota/check",  .method = HTTP_GET,  .handler = h_get_ota_check },
        { .uri = "/api/admin/ota/start",  .method = HTTP_POST, .handler = h_post_ota_start },
        { .uri = "/api/admin/ota/upload", .method = HTTP_POST, .handler = h_post_ota_upload },
        { .uri = "/api/admin/ota/status", .method = HTTP_GET,  .handler = h_get_ota_status },
    };

    for (int i = 0; i < 4; i++) {
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &handlers[i]), TAG, "reg %s", handlers[i].uri);
    }

    ESP_LOGI(TAG, "OTA bundle endpoints registered");
    return ESP_OK;
}
```

- [ ] **Step 9: Build to verify full compilation**

Run: `cd firmware && idf.py build`
Expected: BUILD SUCCESS

- [ ] **Step 10: Commit**

```bash
git add firmware/components/cybeer_web/cybeer_ota.c firmware/components/cybeer_web/include/cybeer_ota.h
git commit -m "feat(ota): rewrite OTA for .cyb bundle format with SHA-256 verification"
```

---

## Task 6: Admin UI — Update Section

**Files:**
- Modify: `frontend/admin.html`
- Modify: `frontend/admin.js`

- [ ] **Step 1: Add OTA section to `admin.html`**

Add before the closing `</section>` tag (after the Tournament details block, line 149):

```html
      <details id="otaSection">
        <summary>Обновление прошивки</summary>
        <div id="otaCurrentVer"></div>
        <div id="otaAvailable" style="display:none">
          <p><strong id="otaNewVer"></strong></p>
          <p class="hint" id="otaChangelog"></p>
          <button type="button" id="otaStartBtn" class="btn-primary">Обновить</button>
        </div>
        <div id="otaUpToDate" style="display:none">
          <p style="color:var(--green)">Актуальная версия</p>
        </div>
        <div id="otaProgress" style="display:none">
          <progress id="otaBar" max="100" value="0" style="width:100%"></progress>
          <p id="otaStage"></p>
          <p class="hint" style="color:var(--amber)">Не закрывайте страницу!</p>
        </div>
        <div id="otaError" style="display:none">
          <p class="err" id="otaErrMsg"></p>
          <button type="button" id="otaRetryBtn">Повторить</button>
        </div>
        <hr />
        <p class="hint">Или загрузите файл вручную:</p>
        <input type="file" id="otaFileInput" accept=".cyb" />
        <button type="button" id="otaUploadBtn" disabled>Загрузить</button>
        <button type="button" id="otaCheckBtn" class="btn-scan">Проверить обновления</button>
      </details>
```

- [ ] **Step 2: Add OTA logic to `admin.js`**

Append to the end of `frontend/admin.js`:

```javascript
/* --- OTA Update --- */

async function otaCheck() {
  try {
    const res = await fetch("/api/admin/ota/check", { headers: pinHeaders() });
    if (!res.ok) throw new Error("status " + res.status);
    const data = await res.json();

    document.getElementById("otaCurrentVer").textContent = "Версия: " + data.currentVersion;

    if (data.available) {
      document.getElementById("otaAvailable").style.display = "";
      document.getElementById("otaUpToDate").style.display = "none";
      document.getElementById("otaNewVer").textContent = "Доступна v" + data.remoteVersion;
      document.getElementById("otaChangelog").textContent = data.changelog || "";
    } else {
      document.getElementById("otaAvailable").style.display = "none";
      document.getElementById("otaUpToDate").style.display = "";
    }
  } catch (e) {
    document.getElementById("otaCurrentVer").textContent = "Не удалось проверить обновления";
  }
}

function otaShowProgress() {
  document.getElementById("otaProgress").style.display = "";
  document.getElementById("otaAvailable").style.display = "none";
  document.getElementById("otaUpToDate").style.display = "none";
  document.getElementById("otaError").style.display = "none";
  document.querySelectorAll("#adminPanel details:not(#otaSection) button, #adminPanel details:not(#otaSection) input, #adminPanel details:not(#otaSection) select").forEach(el => el.disabled = true);
}

function otaShowError(msg) {
  document.getElementById("otaProgress").style.display = "none";
  document.getElementById("otaError").style.display = "";
  document.getElementById("otaErrMsg").textContent = msg;
  document.querySelectorAll("#adminPanel button, #adminPanel input, #adminPanel select").forEach(el => el.disabled = false);
}

async function otaStart() {
  otaShowProgress();
  try {
    const res = await fetch("/api/admin/ota/start", {
      method: "POST",
      headers: pinHeaders(),
    });
    if (!res.ok) {
      const d = await res.json().catch(() => ({}));
      throw new Error(d.error || "Ошибка запуска");
    }
  } catch (e) {
    otaShowError(e.message);
  }
}

async function otaUpload() {
  const fileInput = document.getElementById("otaFileInput");
  const file = fileInput.files[0];
  if (!file) return;

  otaShowProgress();
  try {
    const res = await fetch("/api/admin/ota/upload", {
      method: "POST",
      headers: { "X-Admin-Pin": getAdminPin(), "Content-Type": "application/octet-stream" },
      body: file,
    });
    if (!res.ok) {
      const d = await res.json().catch(() => ({}));
      throw new Error(d.error || "Ошибка загрузки");
    }
  } catch (e) {
    otaShowError(e.message);
  }
}

function otaHandleWsMessage(data) {
  if (data.type === "ota_progress") {
    document.getElementById("otaBar").value = data.percent;
    const stageNames = { downloading: "Скачивание...", receiving: "Получение...", firmware: "Запись прошивки...", littlefs: "Запись интерфейса..." };
    document.getElementById("otaStage").textContent = stageNames[data.stage] || data.stage;
  } else if (data.type === "ota_done") {
    document.getElementById("otaStage").textContent = "Обновлено! Перезагрузка...";
    document.getElementById("otaBar").value = 100;
    setTimeout(() => location.reload(), 7000);
  } else if (data.type === "ota_error") {
    otaShowError(data.message);
  }
}

(function initOta() {
  document.getElementById("otaStartBtn")?.addEventListener("click", otaStart);
  document.getElementById("otaUploadBtn")?.addEventListener("click", otaUpload);
  document.getElementById("otaRetryBtn")?.addEventListener("click", otaCheck);
  document.getElementById("otaCheckBtn")?.addEventListener("click", otaCheck);
  document.getElementById("otaFileInput")?.addEventListener("change", function () {
    document.getElementById("otaUploadBtn").disabled = !this.files.length;
  });

  otaCheck();
})();
```

- [ ] **Step 3: Hook OTA WebSocket messages into existing WS handler**

In `frontend/app.js`, find the WebSocket `onmessage` handler and add a dispatch to `otaHandleWsMessage`. Look for the `ws.onmessage` callback and add:

```javascript
if (typeof otaHandleWsMessage === "function" && (msg.type === "ota_progress" || msg.type === "ota_done" || msg.type === "ota_error")) {
    otaHandleWsMessage(msg);
}
```

- [ ] **Step 4: Test in browser**

Open `http://cybeer.local/admin.html` and verify:
- OTA section appears as accordion
- "Проверить обновления" button triggers check (may fail if no Wi-Fi to GitHub — that's OK)
- File input accepts `.cyb` files
- Upload button enables when file selected

- [ ] **Step 5: Commit**

```bash
git add frontend/admin.html frontend/admin.js frontend/app.js
git commit -m "feat(ui): add OTA update section to admin panel"
```

---

## Task 7: Version Manifest File

**Files:**
- Create: `version.json` (repo root)

- [ ] **Step 1: Create initial `version.json`**

```json
{
  "version": "1.1.0",
  "url": "https://github.com/zau/CyBeer/releases/download/v1.1.0/cybeer-v1.1.0.cyb",
  "size": 0,
  "sha256": "",
  "changelog": "Начальная версия с OTA"
}
```

This is a placeholder — the URL/size/sha256 will be filled when the first real release is created. The version matches `PROJECT_VER` so the device won't see a false "update available".

- [ ] **Step 2: Commit**

```bash
git add version.json
git commit -m "feat(ota): add version.json manifest for auto-update checks"
```

---

## Task 8: Integration Test

**Files:** None (manual testing)

- [ ] **Step 1: Build full project**

```bash
cd firmware
idf.py build
```

Expected: BUILD SUCCESS with no errors or warnings related to OTA.

- [ ] **Step 2: Create test bundle**

```bash
python tools/build_bundle.py
```

Expected: Output like:
```
Bundle: firmware/build/cybeer-v1.1.0.cyb
  Version:  1.1.0
  Firmware: NNNNNN bytes
  LittleFS: NNNNNN bytes
  Total:    NNNNNN bytes
  SHA-256:  abcdef...
```

- [ ] **Step 3: Flash and verify OTA endpoints respond**

```bash
cd firmware
idf.py -p COM7 flash monitor
```

Then from another terminal:
```bash
curl -H "X-Admin-Pin: 1111" http://cybeer.local/api/admin/ota/status
```

Expected: `{"active":false,"percent":0,"stage":""}`

- [ ] **Step 4: Test bundle upload via curl**

Create a dummy version bump (change `PROJECT_VER` to 1.1.1 temporarily), rebuild, create bundle, then upload to device running 1.1.0:

```bash
curl -X POST -H "X-Admin-Pin: 1111" -H "Content-Type: application/octet-stream" --data-binary @firmware/build/cybeer-v1.1.1.cyb http://cybeer.local/api/admin/ota/upload
```

Expected: Device accepts bundle, flashes, reboots. After reboot, `/api/status` shows `firmwareVersion: "1.1.1"`.

- [ ] **Step 5: Commit version bump (if keeping)**

```bash
git add firmware/CMakeLists.txt
git commit -m "chore: bump version to 1.1.1"
```

---

## Summary of Commit Sequence

1. `feat(ota): add bundle build script (.cyb format)`
2. `feat(ws): expose broadcast_text as public API for OTA progress`
3. `feat(led): add OTA progress LED effects`
4. `feat(config): add OTA manifest URL constant`
5. `feat(ota): rewrite OTA for .cyb bundle format with SHA-256 verification`
6. `feat(ui): add OTA update section to admin panel`
7. `feat(ota): add version.json manifest for auto-update checks`
8. `chore: bump version to 1.1.1` (integration test)
