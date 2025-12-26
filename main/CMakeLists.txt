idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "." "include"
    REQUIRES 
        freertos
        nvs_flash
        esp_wifi
        esp_event
        esp_netif
        esp_timer
        driver
        wifi_manager
        http_server
        soft_rtc
        temp_control
        scheduler
        lcd_display
)
