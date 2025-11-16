
# Find all .cpp and .c files recursively
file(GLOB_RECURSE CPP_FILES "*.cpp")
file(GLOB_RECURSE C_FILES "*.c")
set(SRC_FILES ${CPP_FILES} ${C_FILES})


idf_component_register(SRCS ${SRC_FILES}
                    INCLUDE_DIRS "."
                    REQUIRES "lvgl" "XPowersLib" "sensorlib" "freertos" "spi_flash" "esp_psram" "driver" "esp_hw_support" "esp32-camera" "espressif__esp_lvgl_port" "esp_port")
