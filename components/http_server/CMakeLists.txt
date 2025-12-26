idf_component_register(
    SRCS "http_server.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server cjson
    PRIV_REQUIRES soft_rtc temp_control scheduler esp_timer
)
