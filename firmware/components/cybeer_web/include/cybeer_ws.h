#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "esp_http_server.h"

esp_err_t cybeer_ws_register(httpd_handle_t hd);
void cybeer_ws_broadcast_state(void);
void cybeer_ws_broadcast_state_deferred(int64_t delay_us);
void cybeer_ws_timer_tick(int64_t now_us);
void cybeer_ws_on_run_finished(const char *run_id, int64_t duration_us);
void cybeer_ws_broadcast_leaderboard_update(void);
void cybeer_ws_broadcast_text(const char *text, size_t len);
