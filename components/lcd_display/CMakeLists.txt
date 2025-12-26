idf_component_register(
    SRCS "lcd_display.cpp"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_rom
    PRIV_REQUIRES soft_rtc temp_control LovyanGFX
)
