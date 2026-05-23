cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(cybeer VERSION 1.1.9)

add_custom_target(
    cybeer_sync_frontend
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_SOURCE_DIR}/frontend_dist/www
    COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/../frontend
            ${CMAKE_SOURCE_DIR}/frontend_dist/www
    COMMENT "Sync ../frontend -> frontend_dist/www"
    VERBATIM)

littlefs_create_partition_image(littlefs ${CMAKE_SOURCE_DIR}/frontend_dist FLASH_IN_PROJECT
                                DEPENDS cybeer_sync_frontend)
