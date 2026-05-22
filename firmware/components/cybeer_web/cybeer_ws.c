#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "cybeer_battery.h"
#include "cybeer_fsm.h"
#include "cybeer_timer.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "cJSON.h"

static const char *TAG = "cybeer_ws";

#define CYBEER_WS_MAX_CLIENTS 10

static httpd_handle_t s_hd;
static SemaphoreHandle_t s_mu;
static int s_fds[CYBEER_WS_MAX_CLIENTS];
static int64_t s_last_timer_send_us;
static int s_last_battery_pct_sent = -1000;
static int64_t s_last_ping_us;
#define WS_PING_INTERVAL_US (15 * 1000000LL)
static bool s_battery_listener_registered;

static const char *fsm_state_str(cybeer_state_t s)
{
    switch (s) {
    case CYBEER_STATE_PREP:
        return "PREP";
    case CYBEER_STATE_RUNNING:
        return "RUNNING";
    case CYBEER_STATE_FINISHED:
        return "FINISHED";
    case CYBEER_STATE_READY:
        return "READY";
    default:
        return "UNKNOWN";
    }
}

/** @return 1 if newly registered, 0 if already present or table full */
static int clients_add(int fd)
{
    if (fd < 0) {
        return 0;
    }
    xSemaphoreTake(s_mu, portMAX_DELAY);
    for (int i = 0; i < CYBEER_WS_MAX_CLIENTS; i++) {
        if (s_fds[i] == fd) {
            xSemaphoreGive(s_mu);
            return 0;
        }
    }
    for (int i = 0; i < CYBEER_WS_MAX_CLIENTS; i++) {
        if (s_fds[i] < 0) {
            s_fds[i] = fd;
            ESP_LOGI(TAG, "ws client fd=%d slot=%d", fd, i);
            xSemaphoreGive(s_mu);
            return 1;
        }
    }
    xSemaphoreGive(s_mu);
    ESP_LOGW(TAG, "ws client table full, fd=%d", fd);
    return 0;
}

static void clients_remove(int fd)
{
    if (fd < 0) {
        return;
    }
    xSemaphoreTake(s_mu, portMAX_DELAY);
    for (int i = 0; i < CYBEER_WS_MAX_CLIENTS; i++) {
        if (s_fds[i] == fd) {
            s_fds[i] = -1;
            ESP_LOGI(TAG, "ws client removed fd=%d", fd);
            break;
        }
    }
    xSemaphoreGive(s_mu);
}

static int clients_copy(int *out, int max)
{
    int n = 0;
    xSemaphoreTake(s_mu, portMAX_DELAY);
    for (int i = 0; i < CYBEER_WS_MAX_CLIENTS && n < max; i++) {
        if (s_fds[i] >= 0) {
            out[n++] = s_fds[i];
        }
    }
    xSemaphoreGive(s_mu);
    return n;
}

static void send_text_payload_to_fd(int fd, const char *text, size_t len)
{
    if (!s_hd || fd < 0 || !text || len == 0) {
        return;
    }
    httpd_ws_frame_t frame = { 0 };
    frame.type = HTTPD_WS_TYPE_TEXT;
    frame.payload = (uint8_t *)text;
    frame.len = len;
    if (httpd_ws_send_frame_async(s_hd, fd, &frame) != ESP_OK) {
        clients_remove(fd);
    }
}

static void broadcast_text(const char *text, size_t len)
{
    if (!s_hd || !text || len == 0) {
        return;
    }
    int fds[CYBEER_WS_MAX_CLIENTS];
    int n = clients_copy(fds, CYBEER_WS_MAX_CLIENTS);
    if (n == 0) {
        return;
    }

    httpd_ws_frame_t frame = { 0 };
    frame.type = HTTPD_WS_TYPE_TEXT;
    frame.payload = (uint8_t *)text;
    frame.len = len;

    for (int i = 0; i < n; i++) {
        if (httpd_ws_send_frame_async(s_hd, fds[i], &frame) != ESP_OK) {
            clients_remove(fds[i]);
        }
    }
}

static void broadcast_battery_percent(int percent)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return;
    }
    cJSON_AddStringToObject(root, "type", "battery");
    cJSON_AddNumberToObject(root, "percent", percent);
    char *printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!printed) {
        return;
    }
    broadcast_text(printed, strlen(printed));
    free(printed);
}

static void on_battery_percent_ws(int percent)
{
    if (!s_hd) {
        return;
    }
    if (percent == s_last_battery_pct_sent) {
        return;
    }
    s_last_battery_pct_sent = percent;
    broadcast_battery_percent(percent);
}

static esp_err_t h_ws(httpd_req_t *req)
{
    const int fd = httpd_req_to_sockfd(req);
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(ws_pkt));

    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        clients_remove(fd);
        return ret;
    }

    if (ws_pkt.len > 1024) {
        clients_remove(fd);
        return ESP_FAIL;
    }

    uint8_t *payload_buf = NULL;
    if (ws_pkt.len > 0) {
        payload_buf = (uint8_t *)malloc(ws_pkt.len);
        if (!payload_buf) {
            clients_remove(fd);
            return ESP_ERR_NO_MEM;
        }
        ws_pkt.payload = payload_buf;
        ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
        free(payload_buf);
        if (ret != ESP_OK) {
            clients_remove(fd);
            return ret;
        }
    }

    if (ws_pkt.type == HTTPD_WS_TYPE_CLOSE) {
        clients_remove(fd);
        return ESP_OK;
    }

    if (clients_add(fd)) {
        const cybeer_fsm_snapshot_t snap = cybeer_fsm_snapshot();

        cJSON *state_msg = cJSON_CreateObject();
        if (state_msg) {
            cJSON_AddStringToObject(state_msg, "type", "state");
            cJSON_AddStringToObject(state_msg, "state", fsm_state_str(snap.state));
            char *printed = cJSON_PrintUnformatted(state_msg);
            cJSON_Delete(state_msg);
            if (printed) {
                send_text_payload_to_fd(fd, printed, strlen(printed));
                free(printed);
            }
        }

        if (snap.state == CYBEER_STATE_RUNNING) {
            const int64_t now_us = esp_timer_get_time();
            const int64_t elapsed = cybeer_timer_elapsed_us(now_us);
            cJSON *timer_msg = cJSON_CreateObject();
            if (timer_msg) {
                cJSON_AddStringToObject(timer_msg, "type", "timer");
                cJSON_AddNumberToObject(timer_msg, "elapsedUs", (double)elapsed);
                char *printed = cJSON_PrintUnformatted(timer_msg);
                cJSON_Delete(timer_msg);
                if (printed) {
                    send_text_payload_to_fd(fd, printed, strlen(printed));
                    free(printed);
                }
            }
        }

        const int pct = cybeer_battery_get_percent();
        cJSON *root = cJSON_CreateObject();
        if (root) {
            cJSON_AddStringToObject(root, "type", "battery");
            cJSON_AddNumberToObject(root, "percent", pct);
            char *printed = cJSON_PrintUnformatted(root);
            cJSON_Delete(root);
            if (printed) {
                send_text_payload_to_fd(fd, printed, strlen(printed));
                free(printed);
            }
        }
        s_last_battery_pct_sent = pct;
    }
    return ESP_OK;
}

esp_err_t cybeer_ws_register(httpd_handle_t hd)
{
    s_hd = hd;
    if (s_mu == NULL) {
        s_mu = xSemaphoreCreateMutex();
    }
    if (!s_mu) {
        ESP_LOGE(TAG, "mutex create failed");
        return ESP_FAIL;
    }

    for (int i = 0; i < CYBEER_WS_MAX_CLIENTS; i++) {
        s_fds[i] = -1;
    }
    s_last_timer_send_us = 0;
    s_last_ping_us = 0;
    s_last_battery_pct_sent = -1000;

    if (!s_battery_listener_registered) {
        cybeer_battery_register_listener(on_battery_percent_ws);
        s_battery_listener_registered = true;
    }

    httpd_uri_t u_ws = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = h_ws,
        .user_ctx = NULL,
        .is_websocket = true,
    };

    esp_err_t err = httpd_register_uri_handler(hd, &u_ws);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register /ws failed");
        return err;
    }
    return ESP_OK;
}

static esp_timer_handle_t s_state_broadcast_timer;

void cybeer_ws_broadcast_state(void)
{
    if (!s_hd) {
        return;
    }
    cybeer_fsm_snapshot_t snap = cybeer_fsm_snapshot();
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return;
    }
    cJSON_AddStringToObject(root, "type", "state");
    cJSON_AddStringToObject(root, "state", fsm_state_str(snap.state));
    char *printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!printed) {
        return;
    }
    broadcast_text(printed, strlen(printed));
    free(printed);
}

static void state_broadcast_timer_cb(void *arg)
{
    (void)arg;
    cybeer_ws_broadcast_state();
}

void cybeer_ws_broadcast_state_deferred(int64_t delay_us)
{
    if (!s_hd || delay_us <= 0) {
        cybeer_ws_broadcast_state();
        return;
    }
    if (s_state_broadcast_timer == NULL) {
        const esp_timer_create_args_t args = {
            .callback = &state_broadcast_timer_cb,
            .name = "ws_state_defer",
        };
        if (esp_timer_create(&args, &s_state_broadcast_timer) != ESP_OK) {
            cybeer_ws_broadcast_state();
            return;
        }
    }
    (void)esp_timer_stop(s_state_broadcast_timer);
    (void)esp_timer_start_once(s_state_broadcast_timer, (uint64_t)delay_us);
}

void cybeer_ws_timer_tick(int64_t now_us)
{
    if (!s_hd) {
        return;
    }

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

    if (cybeer_fsm_snapshot().state != CYBEER_STATE_RUNNING) {
        s_last_timer_send_us = 0;
        return;
    }
    const int64_t period_us = 100000; /* 10 Hz — lower httpd load while RUNNING */
    if (s_last_timer_send_us != 0 && (now_us - s_last_timer_send_us) < period_us) {
        return;
    }
    s_last_timer_send_us = now_us;

    const int64_t elapsed = cybeer_timer_elapsed_us(now_us);
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return;
    }
    cJSON_AddStringToObject(root, "type", "timer");
    cJSON_AddNumberToObject(root, "elapsedUs", (double)elapsed);
    char *printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!printed) {
        return;
    }
    broadcast_text(printed, strlen(printed));
    free(printed);
}

void cybeer_ws_broadcast_leaderboard_update(void)
{
    if (!s_hd) {
        return;
    }
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return;
    }
    cJSON_AddStringToObject(root, "type", "leaderboardUpdate");
    char *printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!printed) {
        return;
    }
    broadcast_text(printed, strlen(printed));
    free(printed);
}

void cybeer_ws_on_run_finished(const char *run_id, int64_t duration_us)
{
    if (!s_hd || !run_id) {
        return;
    }
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return;
    }
    cJSON_AddStringToObject(root, "type", "runFinished");
    cJSON_AddStringToObject(root, "runId", run_id);
    cJSON_AddNumberToObject(root, "durationUs", (double)duration_us);
    char *printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!printed) {
        return;
    }
    broadcast_text(printed, strlen(printed));
    free(printed);
}
