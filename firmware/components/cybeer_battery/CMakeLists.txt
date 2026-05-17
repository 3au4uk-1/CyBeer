idf_component_register(
    SRCS "cybeer_battery.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_adc driver freertos cybeer_config
)
