idf_component_register(
    SRCS "cybeer_wifi.c"
    INCLUDE_DIRS "include" "."
    REQUIRES cybeer_storage esp_netif esp_wifi esp_event esp_http_server
             mdns nvs_flash json freertos lwip
)
