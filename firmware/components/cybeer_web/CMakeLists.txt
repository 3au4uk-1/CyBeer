idf_component_register(
    SRCS "cybeer_web.c" "cybeer_ws.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server cybeer_wifi cybeer_fsm cybeer_storage cybeer_battery cybeer_timer json vfs
)
