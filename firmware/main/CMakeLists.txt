idf_component_register(SRCS "app_main.c"
                    INCLUDE_DIRS "."
                    REQUIRES nvs_flash cybeer_config cybeer_switch cybeer_fsm cybeer_timer cybeer_display esp_timer driver)
