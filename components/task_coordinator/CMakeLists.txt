idf_component_register(
    SRCS "task_coordinator.cpp"
    INCLUDE_DIRS "."
    REQUIRES freertos esp_common esp_timer main
)
