#ifndef M5STACK_TAB5_DISPLAY_H
#define M5STACK_TAB5_DISPLAY_H

#include <stdint.h>
#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * M5Stack Tab5 (ESP32-P4 + onboard ESP32-C6) display + board bring-up.
 *
 * The Tab5 gates the display power rails AND the onboard C6 radio rail through
 * two PI4IOE5V6408 I2C IO-expanders (0x43 / 0x44). tab5_board_power_init() must
 * run early in app_main -- BEFORE esp_hosted_init() -- so WLAN_PWR_EN is
 * asserted and the C6 is alive when the SDIO transport comes up. The remaining
 * functions mirror the crowpanel_p4_display.* contract used by display_manager.
 */

/* I2C + IO-expander bring-up: asserts WLAN_PWR_EN (C6 radio power) and the
 * panel/backlight rails, deasserts LCD_RST/TP_RST. Idempotent. Call before
 * ESP-Hosted and before tab5_display_init(). */
esp_err_t tab5_board_power_init(void);

esp_err_t tab5_display_init(void);

/* True when the detected panel is an ST7123/ST7121 (touch @0x55) rather than
 * the ILI9881C (GT911). Valid only after tab5_display_init(). */
bool tab5_display_is_st712x(void);

/* Read the ST7123/ST7121 touch controller (I2C 0x55) into an LVGL indev data
 * struct. Used in place of the GT911 path on ST712x panels. */
void tab5_touch_read(lv_indev_data_t *data);

/* Map native-portrait touch coordinates into the active orientation. In
 * landscape it rotates them to the 1280x720 space; in portrait it is a no-op.
 * Call after reading touch (works for both GT911 and ST712x). */
void tab5_display_transform_touch(lv_indev_data_t *data);

esp_err_t tab5_display_touch_reset(void);
esp_err_t tab5_display_set_backlight(uint8_t percentage);
esp_err_t tab5_display_get_frame_buffers(void **fb0, void **fb1);
void tab5_display_mark_dirty_rows(int y1, int y2);
void tab5_display_flush_cb(lv_disp_drv_t *drv,
                           const lv_area_t *area,
                           lv_color_t *color_p);

#ifdef __cplusplus
}
#endif

#endif
