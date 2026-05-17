idf_component_register(
    SRCS "cybeer_nvs.c" "cybeer_storage.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash vfs json esp_littlefs
)
