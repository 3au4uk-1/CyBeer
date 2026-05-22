#pragma once

#include <stdint.h>

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
