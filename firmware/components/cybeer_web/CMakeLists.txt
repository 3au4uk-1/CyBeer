idf_component_register(
    SRCS "cybeer_web.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server cybeer_wifi cybeer_fsm cybeer_storage json vfs
)
