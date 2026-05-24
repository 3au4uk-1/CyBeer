idf_component_register(
    SRCS "cybeer_web.c" "cybeer_ws.c" "cybeer_ota.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server esp_https_ota esp_http_client app_update mbedtls esp_netif cybeer_wifi cybeer_fsm cybeer_storage cybeer_tournament cybeer_battery cybeer_timer cybeer_config cybeer_led cybeer_power cybeer_sync cjson vfs
)
