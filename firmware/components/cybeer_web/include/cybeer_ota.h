#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

esp_err_t cybeer_ota_register_handlers(httpd_handle_t server);
