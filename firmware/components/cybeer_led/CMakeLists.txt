idf_component_register(
    SRCS "cybeer_led.c"
    INCLUDE_DIRS "include"
    REQUIRES led_strip cybeer_config nvs_flash driver
)
