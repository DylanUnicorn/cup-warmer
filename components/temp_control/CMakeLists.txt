idf_component_register(
    SRCS "temp_control.c" "pid.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_adc esp_timer
)
