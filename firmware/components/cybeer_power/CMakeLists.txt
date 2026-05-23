idf_component_register(SRCS "cybeer_power.c"
                    INCLUDE_DIRS "include"
                    REQUIRES cybeer_config cybeer_switch cybeer_display cybeer_led
                             driver esp_hw_support esp_timer esp_wifi nvs_flash)
