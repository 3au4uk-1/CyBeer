idf_component_register(
    SRCS "cybeer_switch.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_gpio cybeer_config
)
