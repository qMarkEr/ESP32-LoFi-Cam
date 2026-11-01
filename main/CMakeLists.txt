idf_component_register(
    SRCS "hardware_modules.c" "main.c" "img_process.c" "consts.c" "hardware_modules.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp32-camera esp_lcd esp_lcd_st7789 esp_psram esp_timer
             fatfs esp_driver_sdmmc
)
