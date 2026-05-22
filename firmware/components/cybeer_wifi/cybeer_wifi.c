#include "cybeer_wifi.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cybeer_led.h"
#include "cybeer_setup_html.h"
#include "cybeer_storage.h"

#include "cJSON.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_sntp.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/def.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "mdns.h"

static const char *TAG = "cybeer_wifi";

#define CYBEER_AP_IP_ADDR PP_HTONL(LWIP_MAKEU32(192, 168, 4, 1))
#define STA_RECONNECT_BASE_MS 2000
#define STA_RECONNECT_MAX_DELAY_MS 30000

/** DHCP offer flag: include DNS server option (matches ESP-IDF softap_sta example). */
static const uint8_t k_dhcps_offer_dns = 0x02;

static esp_netif_t *s_ap_netif;
static esp_netif_t *s_sta_netif;
static httpd_handle_t s_local_httpd;
static bool s_skip_internal_httpd;
static bool s_init_done;
static bool s_wifi_started;
static bool s_sta_has_ip;
static bool s_mdns_started;
static bool s_sntp_started;
static char s_sta_ip_str[16];
static char s_ap_ssid[33];
static int s_sta_retry_count;
static esp_timer_handle_t s_sta_reconnect_timer;

static esp_event_handler_instance_t s_wifi_inst;
static esp_event_handler_instance_t s_ip_inst;

static void dns_server_task(void *arg);

static void sta_reconnect_timer_cb(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "STA reconnect (retry count %d)", s_sta_retry_count);
    (void)esp_wifi_connect();
}

static void schedule_sta_reconnect(void)
{
    uint32_t delay_ms = STA_RECONNECT_BASE_MS << s_sta_retry_count;
    if (delay_ms > STA_RECONNECT_MAX_DELAY_MS) {
        delay_ms = STA_RECONNECT_MAX_DELAY_MS;
    }
    s_sta_retry_count++;

    if (s_sta_reconnect_timer == NULL) {
        const esp_timer_create_args_t args = {
            .callback = &sta_reconnect_timer_cb,
            .name = "sta_reconn",
        };
        if (esp_timer_create(&args, &s_sta_reconnect_timer) != ESP_OK) {
            ESP_LOGE(TAG, "STA reconnect timer create failed");
            (void)esp_wifi_connect();
            return;
        }
    }

    (void)esp_timer_stop(s_sta_reconnect_timer);
    esp_err_t err = esp_timer_start_once(s_sta_reconnect_timer, (uint64_t)delay_ms * 1000ULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "STA reconnect timer start failed: %s", esp_err_to_name(err));
        (void)esp_wifi_connect();
        return;
    }
    ESP_LOGI(TAG, "STA reconnect scheduled in %lu ms", (unsigned long)delay_ms);
}

static bool host_is_ap_portal(const char *host)
{
    if (host == NULL || host[0] == '\0') {
        return false;
    }
    if (strncasecmp(host, "192.168.4.1", 11) == 0 && (host[11] == '\0' || host[11] == ':')) {
        return true;
    }
    if (strcasecmp(host, "cybeer.local") == 0) {
        return true;
    }
    if (strncasecmp(host, "cybeer.local", 12) == 0 && host[12] == ':') {
        return true;
    }
    return false;
}

static size_t dns_skip_name(const uint8_t *pkt, size_t len, size_t off)
{
    while (off < len) {
        uint8_t lab = pkt[off];
        if (lab == 0) {
            return off + 1;
        }
        if ((lab & 0xc0) == 0xc0) {
            return off + 2;
        }
        off += 1 + lab;
    }
    return 0;
}

static esp_err_t ap_netif_configure_dhcps(esp_netif_t *ap)
{
    esp_netif_ip_info_t ip_info;
    memset(&ip_info, 0, sizeof(ip_info));
    ip_info.ip.addr = CYBEER_AP_IP_ADDR;
    ip_info.gw.addr = CYBEER_AP_IP_ADDR;
    ip_info.netmask.addr = PP_HTONL(LWIP_MAKEU32(255, 255, 255, 0));

    ESP_RETURN_ON_ERROR(esp_netif_dhcps_stop(ap), TAG, "dhcps_stop");
    ESP_RETURN_ON_ERROR(esp_netif_set_ip_info(ap, &ip_info), TAG, "set_ip_info");

    ESP_RETURN_ON_ERROR(esp_netif_dhcps_option(ap, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER,
                                               (void *)&k_dhcps_offer_dns, sizeof(k_dhcps_offer_dns)),
                        TAG, "dhcps dns offer");

    esp_netif_dns_info_t dns;
    memset(&dns, 0, sizeof(dns));
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = ip_info.ip.addr;
    ESP_RETURN_ON_ERROR(esp_netif_set_dns_info(ap, ESP_NETIF_DNS_MAIN, &dns), TAG, "set_dns_info");

    ESP_RETURN_ON_ERROR(esp_netif_dhcps_start(ap), TAG, "dhcps_start");
    return ESP_OK;
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;

    if (id == WIFI_EVENT_AP_START) {
        ESP_LOGI(TAG, "SoftAP started, SSID \"%s\", open, 192.168.4.1", s_ap_ssid[0] ? s_ap_ssid : "?");
    } else if (id == WIFI_EVENT_STA_START) {
        (void)esp_wifi_connect();
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        s_sta_has_ip = false;
        s_sta_ip_str[0] = '\0';
        if (s_sta_netif != NULL) {
            schedule_sta_reconnect();
        }
    }
}

static void ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;

    if (id != IP_EVENT_STA_GOT_IP || data == NULL) {
        return;
    }

    ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
    s_sta_retry_count = 0;
    if (s_sta_reconnect_timer != NULL) {
        (void)esp_timer_stop(s_sta_reconnect_timer);
    }
    s_sta_has_ip = true;
    snprintf(s_sta_ip_str, sizeof(s_sta_ip_str), IPSTR, IP2STR(&ev->ip_info.ip));
    ESP_LOGI(TAG, "STA got IP: %s", s_sta_ip_str);

    if (!s_mdns_started) {
        esp_err_t err = mdns_init();
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "mdns_init failed: %s", esp_err_to_name(err));
        } else {
            err = mdns_hostname_set("cybeer");
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "mdns_hostname_set failed: %s", esp_err_to_name(err));
            }
            (void)mdns_instance_name_set("CyBeer");
            s_mdns_started = true;
        }
    }

    if (!s_sntp_started) {
        esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "pool.ntp.org");
        esp_sntp_setservername(1, "time.google.com");
        esp_sntp_init();
        s_sntp_started = true;
        ESP_LOGI(TAG, "SNTP started");
    }
}

static esp_err_t h_get_setup(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, CYBEER_SETUP_HTML, HTTPD_RESP_USE_STRLEN);
}

static const char *auth_mode_str(wifi_auth_mode_t mode)
{
    switch (mode) {
    case WIFI_AUTH_OPEN:
        return "Open";
    case WIFI_AUTH_WEP:
        return "WEP";
    case WIFI_AUTH_WPA_PSK:
        return "WPA";
    case WIFI_AUTH_WPA2_PSK:
        return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:
        return "WPA/WPA2";
    case WIFI_AUTH_WPA3_PSK:
        return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:
        return "WPA2/WPA3";
    default:
        return "Other";
    }
}

static esp_err_t h_get_scan(httpd_req_t *req)
{
    wifi_scan_config_t scan_cfg = {
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_PASSIVE,
        .scan_time.passive = 300,
    };
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_send(req, "[]", HTTPD_RESP_USE_STRLEN);
    }

    uint16_t ap_count = 0;
    esp_wifi_scan_get_ap_num(&ap_count);
    if (ap_count > 20) {
        ap_count = 20;
    }

    wifi_ap_record_t *ap_list = NULL;
    if (ap_count > 0) {
        ap_list = (wifi_ap_record_t *)malloc(ap_count * sizeof(wifi_ap_record_t));
        if (!ap_list) {
            esp_wifi_scan_get_ap_records(&ap_count, NULL);
            httpd_resp_set_type(req, "application/json");
            return httpd_resp_send(req, "[]", HTTPD_RESP_USE_STRLEN);
        }
        esp_wifi_scan_get_ap_records(&ap_count, ap_list);
    }

    cJSON *arr = cJSON_CreateArray();
    if (!arr) {
        free(ap_list);
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "[]", HTTPD_RESP_USE_STRLEN);
    }

    for (uint16_t i = 0; i < ap_count; i++) {
        cJSON *item = cJSON_CreateObject();
        if (!item) {
            continue;
        }
        cJSON_AddStringToObject(item, "ssid", (const char *)ap_list[i].ssid);
        cJSON_AddNumberToObject(item, "rssi", (double)ap_list[i].rssi);
        cJSON_AddStringToObject(item, "auth", auth_mode_str(ap_list[i].authmode));
        cJSON_AddItemToArray(arr, item);
    }
    free(ap_list);

    char *printed = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (!printed) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "[]", HTTPD_RESP_USE_STRLEN);
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t e = httpd_resp_send(req, printed, HTTPD_RESP_USE_STRLEN);
    free(printed);
    return e;
}

static esp_err_t h_post_setup_wifi(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 512) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req, "{\"error\":\"bad body\"}", HTTPD_RESP_USE_STRLEN);
    }

    size_t n = (size_t)req->content_len;
    char body[520];
    int r = httpd_req_recv(req, body, n);
    if (r <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req, "{\"error\":\"recv\"}", HTTPD_RESP_USE_STRLEN);
    }
    body[r] = '\0';

    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req, "{\"error\":\"json\"}", HTTPD_RESP_USE_STRLEN);
    }

    const cJSON *jssid = cJSON_GetObjectItemCaseSensitive(root, "ssid");
    const cJSON *jpass = cJSON_GetObjectItemCaseSensitive(root, "password");
    if (!cJSON_IsString(jssid) || jssid->valuestring == NULL || jssid->valuestring[0] == '\0') {
        cJSON_Delete(root);
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req, "{\"error\":\"ssid\"}", HTTPD_RESP_USE_STRLEN);
    }

    const char *ssid = jssid->valuestring;
    const char *pass = "";
    if (cJSON_IsString(jpass) && jpass->valuestring != NULL) {
        pass = jpass->valuestring;
    }

    esp_err_t err = cybeer_nvs_set_wifi(ssid, pass);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_send(req, "{\"ok\":false}", HTTPD_RESP_USE_STRLEN);
    }

    (void)httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    vTaskDelay(pdMS_TO_TICKS(150));
    esp_restart();
    return ESP_OK;
}

static esp_err_t h_post_forget(httpd_req_t *req)
{
    (void)req;
    httpd_resp_set_type(req, "application/json");
    (void)httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
    vTaskDelay(pdMS_TO_TICKS(150));
    cybeer_wifi_forget_sta();
    return ESP_OK;
}

static esp_err_t h_err_404(httpd_req_t *req, httpd_err_code_t err)
{
    if (err != HTTPD_404_NOT_FOUND) {
        return ESP_FAIL;
    }

    httpd_method_t m = req->method;
    if (m != HTTP_GET && m != HTTP_HEAD) {
        httpd_resp_set_status(req, "404 Not Found");
        return httpd_resp_send(req, "Not Found", HTTPD_RESP_USE_STRLEN);
    }

    char host[64];
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) == ESP_OK && host_is_ap_portal(host)) {
        httpd_resp_set_status(req, "404 Not Found");
        return httpd_resp_send(req, "Not Found", HTTPD_RESP_USE_STRLEN);
    }

    char sta_ssid[CYBEER_WIFI_SSID_MAX];
    char sta_pass[CYBEER_WIFI_PASS_MAX];
    bool have_sta = (cybeer_nvs_get_wifi(sta_ssid, sizeof(sta_ssid), sta_pass, sizeof(sta_pass)) == ESP_OK);
    if (have_sta) {
        httpd_resp_set_status(req, "404 Not Found");
        return httpd_resp_send(req, "Not Found", HTTPD_RESP_USE_STRLEN);
    }

    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/setup");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t bind_setup_handlers(httpd_handle_t h)
{
    httpd_uri_t u_setup = { .uri = "/setup", .method = HTTP_GET, .handler = h_get_setup, .user_ctx = NULL };
    httpd_uri_t u_api_wifi = {
        .uri = "/api/setup/wifi", .method = HTTP_POST, .handler = h_post_setup_wifi, .user_ctx = NULL
    };
    httpd_uri_t u_scan = {
        .uri = "/api/setup/scan", .method = HTTP_GET, .handler = h_get_scan, .user_ctx = NULL
    };
    httpd_uri_t u_forget = {
        .uri = "/api/admin/wifi/forget", .method = HTTP_POST, .handler = h_post_forget, .user_ctx = NULL
    };

    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(h, &u_setup), TAG, "uri /setup");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(h, &u_api_wifi), TAG, "uri /api/setup/wifi");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(h, &u_scan), TAG, "uri /api/setup/scan");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(h, &u_forget), TAG, "uri forget");
    ESP_RETURN_ON_ERROR(httpd_register_err_handler(h, HTTPD_404_NOT_FOUND, h_err_404), TAG, "err 404");
    return ESP_OK;
}

static void start_captive_dns_task(void)
{
    BaseType_t ok = xTaskCreate(dns_server_task, "cybeer_dns", 4096, NULL, 5, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "DNS task create failed");
    }
}

static void dns_server_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "dns socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(53);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "dns bind: errno %d", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    uint8_t rx[512];
    uint8_t tx[512];

    for (;;) {
        struct sockaddr_in src;
        socklen_t srclen = sizeof(src);
        ssize_t n = recvfrom(sock, rx, sizeof(rx), 0, (struct sockaddr *)&src, &srclen);
        if (n < 12) {
            continue;
        }

        size_t qend = dns_skip_name(rx, (size_t)n, 12);
        if (qend == 0 || qend + 4 > (size_t)n) {
            continue;
        }

        memcpy(tx, rx, qend + 4);
        tx[2] = 0x81;
        tx[3] = 0x80;
        tx[4] = rx[4];
        tx[5] = rx[5];
        tx[6] = 0;
        tx[7] = 1;
        tx[8] = 0;
        tx[9] = 0;
        tx[10] = 0;
        tx[11] = 0;

        size_t pos = qend + 4;
        if (pos + 12 > sizeof(tx)) {
            continue;
        }

        tx[pos++] = 0xc0;
        tx[pos++] = 0x0c;
        tx[pos++] = 0;
        tx[pos++] = 1;
        tx[pos++] = 0;
        tx[pos++] = 1;
        tx[pos++] = 0;
        tx[pos++] = 0;
        tx[pos++] = 0;
        tx[pos++] = 60;
        tx[pos++] = 0;
        tx[pos++] = 4;
        uint32_t ip = CYBEER_AP_IP_ADDR;
        memcpy(&tx[pos], &ip, 4);
        pos += 4;

        (void)sendto(sock, tx, pos, 0, (struct sockaddr *)&src, sizeof(src));
    }
}

void cybeer_wifi_register_setup_handlers(httpd_handle_t server)
{
    if (!server) {
        return;
    }
    esp_err_t err = bind_setup_handlers(server);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register_setup_handlers failed: %s", esp_err_to_name(err));
        return;
    }
    s_skip_internal_httpd = true;
    ESP_LOGI(TAG, "Setup handlers registered on external HTTP server");
}

esp_err_t cybeer_wifi_init(void)
{
    if (s_init_done) {
        return ESP_OK;
    }

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL,
                                                            &s_wifi_inst),
                        TAG, "wifi evt");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, ip_event, NULL, &s_ip_inst), TAG,
        "ip evt");

    wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&wcfg), TAG, "wifi_init");
    /* CyBeer stores STA creds in namespace "cybeer"; avoid ESP-IDF Wi-Fi NVS overriding AP mode. */
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "wifi_storage_ram");

    s_init_done = true;
    return ESP_OK;
}

static void build_ap_ssid(char *out, size_t out_len)
{
    uint8_t mac[6];
    (void)esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(out, out_len, "CyBeer-%02X%02X%02X", mac[3], mac[4], mac[5]);
}

esp_err_t cybeer_wifi_start(void)
{
    ESP_RETURN_ON_FALSE(s_init_done, ESP_ERR_INVALID_STATE, TAG, "init first");
    if (s_wifi_started) {
        return ESP_OK;
    }

    char sta_ssid[CYBEER_WIFI_SSID_MAX];
    char sta_pass[CYBEER_WIFI_PASS_MAX];
    bool have_sta = (cybeer_nvs_get_wifi(sta_ssid, sizeof(sta_ssid), sta_pass, sizeof(sta_pass)) == ESP_OK);

    build_ap_ssid(s_ap_ssid, sizeof(s_ap_ssid));

    s_ap_netif = esp_netif_create_default_wifi_ap();
    ESP_RETURN_ON_FALSE(s_ap_netif != NULL, ESP_FAIL, TAG, "ap netif");

    ESP_RETURN_ON_ERROR(ap_netif_configure_dhcps(s_ap_netif), TAG, "ap ip/dhcp");

    wifi_config_t ap_cfg = { 0 };
    strlcpy((char *)ap_cfg.ap.ssid, s_ap_ssid, sizeof(ap_cfg.ap.ssid));
    ap_cfg.ap.ssid_len = (uint8_t)strlen((char *)ap_cfg.ap.ssid);
    ap_cfg.ap.channel = 1;
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    ap_cfg.ap.password[0] = '\0';

    if (have_sta) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
        ESP_RETURN_ON_FALSE(s_sta_netif != NULL, ESP_FAIL, TAG, "sta netif");

        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "mode apsta");

        wifi_config_t sta_cfg = { 0 };
        strlcpy((char *)sta_cfg.sta.ssid, sta_ssid, sizeof(sta_cfg.sta.ssid));
        strlcpy((char *)sta_cfg.sta.password, sta_pass, sizeof(sta_cfg.sta.password));
        sta_cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
        if (sta_pass[0] == '\0') {
            sta_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
        } else {
            sta_cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        }

        ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg), TAG, "cfg ap");
        ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg), TAG, "cfg sta");
    } else {
        s_sta_netif = NULL;
        ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "mode ap");
        ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg), TAG, "cfg ap");
    }

    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG, "wifi_ps_none");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi_start");

    ESP_LOGI(TAG, "SoftAP SSID:%s IP 192.168.4.1/24 (open)", s_ap_ssid);
    if (have_sta) {
        ESP_LOGI(TAG, "STA joining: %s", sta_ssid);
    } else {
        ESP_LOGI(TAG, "Provisioning mode (no STA credentials)");
        cybeer_led_set_fx(CYBEER_LED_FX_WIFI_SETUP);
    }

    if (!have_sta) {
        start_captive_dns_task();
    }

    if (!s_skip_internal_httpd) {
        httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
        cfg.server_port = 80;
        cfg.lru_purge_enable = true;
        esp_err_t herr = httpd_start(&s_local_httpd, &cfg);
        if (herr != ESP_OK) {
            ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(herr));
            return herr;
        }
        herr = bind_setup_handlers(s_local_httpd);
        if (herr != ESP_OK) {
            return herr;
        }
        ESP_LOGI(TAG, "Internal HTTP server on :80");
    }

    s_wifi_started = true;
    return ESP_OK;
}

bool cybeer_wifi_sta_connected(void)
{
    return s_sta_has_ip;
}

bool cybeer_wifi_is_started(void)
{
    return s_wifi_started;
}

bool cybeer_wifi_sta_credentials_configured(void)
{
    char sta_ssid[CYBEER_WIFI_SSID_MAX];
    char sta_pass[CYBEER_WIFI_PASS_MAX];
    return cybeer_nvs_get_wifi(sta_ssid, sizeof(sta_ssid), sta_pass, sizeof(sta_pass)) == ESP_OK;
}

void cybeer_wifi_get_ap_ssid_str(char *buf, size_t len)
{
    if (!buf || len == 0) {
        return;
    }
    if (!s_ap_ssid[0]) {
        buf[0] = '\0';
        return;
    }
    snprintf(buf, len, "%s", s_ap_ssid);
}

void cybeer_wifi_get_sta_ip_str(char *buf, size_t len)
{
    if (!buf || len == 0) {
        return;
    }
    if (!s_sta_has_ip || s_sta_ip_str[0] == '\0') {
        buf[0] = '\0';
        return;
    }
    snprintf(buf, len, "%s", s_sta_ip_str);
}

void cybeer_wifi_forget_sta(void)
{
    esp_err_t err = cybeer_nvs_clear_wifi();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS clear wifi failed: %s", esp_err_to_name(err));
    }
    esp_restart();
}
