# ST7121 / ST7123 MIPI-DSI panel drivers for the M5Stack Tab5 (newer panel
# variants). Vendored from the M5Stack Tab5 BSP (Espressif-authored, Apache-2.0)
# because they are not published on the component registry; the matching touch
# controller (esp_lcd_touch_st7123) is pulled as a managed dependency instead.
idf_component_register(
    SRCS "esp_lcd_st7121.c" "esp_lcd_st7123.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_lcd
)
