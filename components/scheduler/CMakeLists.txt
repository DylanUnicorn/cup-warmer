idf_component_register(
    SRCS "scheduler.c"
    INCLUDE_DIRS "include"
    REQUIRES soft_rtc temp_control esp_timer
)
