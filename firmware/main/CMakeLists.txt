idf_component_register(SRCS "app_main.c"
                    INCLUDE_DIRS "."
                    REQUIRES nvs_flash app_update cybeer_storage cybeer_config cybeer_battery cybeer_wifi cybeer_web cybeer_tournament cybeer_switch cybeer_fsm cybeer_timer cybeer_display cybeer_led cybeer_power esp_timer driver cjson)