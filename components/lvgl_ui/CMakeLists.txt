
# Find all .cpp files recursively
file(GLOB_RECURSE CPP_FILES "*.cpp")

# Find all .c files but exclude frame*.c files
file(GLOB_RECURSE ALL_C_FILES "*.c")
list(FILTER ALL_C_FILES EXCLUDE REGEX ".*/frame[0-9]+\\.c$")
set(C_FILES ${ALL_C_FILES})

set(SRC_FILES ${CPP_FILES} ${C_FILES})


idf_component_register(SRCS ${SRC_FILES}
                    INCLUDE_DIRS "." "${CMAKE_SOURCE_DIR}/main"
                    REQUIRES "lvgl" "XPowersLib" "sensorlib" "freertos" "spi_flash" "esp_psram" "driver" "esp_hw_support" "esp32-camera" "espressif__esp_lvgl_port" "esp_port" "main")
