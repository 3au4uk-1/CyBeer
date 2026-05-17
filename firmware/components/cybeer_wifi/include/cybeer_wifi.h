#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "esp_http_server.h"

esp_err_t cybeer_wifi_init(void);
esp_err_t cybeer_wifi_start(void);
bool cybeer_wifi_sta_connected(void);
void cybeer_wifi_get_sta_ip_str(char *buf, size_t len);

/**
 * Register provisioning/admin URI handlers on an existing HTTP server (e.g. cybeer_web).
 * Call after httpd_start(). If invoked before cybeer_wifi_start(), cybeer_wifi_start() will
 * not start an internal HTTP server for provisioning.
 */
void cybeer_wifi_register_setup_handlers(httpd_handle_t server);

/** Clear STA credentials from NVS and reboot the chip. */
void cybeer_wifi_forget_sta(void);
