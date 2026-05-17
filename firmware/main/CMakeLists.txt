idf_component_register(SRCS "app_main.c"
                    INCLUDE_DIRS "."
                    REQUIRES nvs_flash cybeer_storage cybeer_config cybeer_wifi cybeer_web cybeer_switch cybeer_fsm cybeer_timer cybeer_display cybeer_led esp_timer driver json)