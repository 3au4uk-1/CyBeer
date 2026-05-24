idf_component_register(
    SRCS "cybeer_sync.c"
    INCLUDE_DIRS "include"
    REQUIRES cybeer_config cybeer_storage cybeer_wifi esp_http_client esp-tls cjson
)
