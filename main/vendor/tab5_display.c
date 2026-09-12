#include "vendor/drivers/tab5_display.h"

#ifdef CONFIG_M5STACK_TAB5

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_idf_version.h"
#include "esp_lcd_ili9881c.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_st7121.h"
#include "esp_lcd_st7123.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl_i2c/i2c_manager.h"
#ifdef CONFIG_M5STACK_TAB5_LANDSCAPE
#include "driver/ppa.h"
#include "esp_heap_caps.h"
#endif

/* ---- Panel geometry: the panel is physically portrait 720x1280 ---- */
#define TAB5_H_RES 720
#define TAB5_V_RES 1280

#ifdef CONFIG_M5STACK_TAB5_LANDSCAPE
/* Landscape UI is rendered at 1280x720 and PPA-rotated 90 degrees onto the
 * portrait panel. Set TAB5_LANDSCAPE_ANGLE_90 to 0 to use 270 instead (flips
 * the image 180 degrees) if the panel mounts the other way. The touch transform
 * below is kept consistent with this choice. */
#define TAB5_LS_W 1280
#define TAB5_LS_H 720
#define TAB5_LANDSCAPE_ANGLE_90 1
#endif

/* ---- MIPI-DSI / DPI parameters, per panel variant (from the M5 Tab5 BSP) ----
 * The Tab5 ships two panel families, told apart at runtime:
 *   - ILI9881C + GT911 touch (early units): 730 Mbps, 60 MHz DPI, RGB565.
 *   - ST7123 / ST7121 + touch@0x55 (units from 2026-04-28): 965 Mbps, 70 MHz DPI.
 * Both keep an RGB565 framebuffer (the DSI upconverts to the ST712x 24bpp panel),
 * so GhostESP's direct-mode / PPA path is unchanged. */
#define TAB5_DSI_LANES 2
#define TAB5_ILI9881C_LANE_MBPS 730
#define TAB5_ILI9881C_DPI_MHZ 60
#define TAB5_ST712X_LANE_MBPS 965
#define TAB5_ST712X_DPI_MHZ 70

/* MIPI D-PHY power comes from on-chip LDO channel 3 @ 2.5V (same as CrowPanel). */
#define TAB5_MIPI_LDO_CHAN 3
#define TAB5_MIPI_LDO_MV 2500

/* ---- Backlight: GPIO22, LEDC PWM ---- */
#define TAB5_BACKLIGHT_GPIO 22
#define TAB5_BACKLIGHT_LEDC_MODE LEDC_LOW_SPEED_MODE
#define TAB5_BACKLIGHT_LEDC_TIMER LEDC_TIMER_0
#define TAB5_BACKLIGHT_LEDC_CHANNEL LEDC_CHANNEL_1
#define TAB5_BACKLIGHT_LEDC_RES LEDC_TIMER_12_BIT
#define TAB5_BACKLIGHT_LEDC_FREQ_HZ 5000
#define TAB5_BACKLIGHT_LEDC_MAX_DUTY ((1 << 12) - 1)

/* ---- Touch (GT911, ILI9881C units only) ---- */
#define TAB5_TOUCH_INT_GPIO 23 /* GT911 INT: drive low at reset -> 0x5D */

/* ---- PI4IOE5V6408 IO-expanders on the system I2C bus (CONFIG_LV_I2C_TOUCH_PORT) ---- */
#define TAB5_I2C_PORT CONFIG_LV_I2C_TOUCH_PORT
#define TAB5_EXP1_ADDR 0x43 /* ADDR low  */
#define TAB5_EXP2_ADDR 0x44 /* ADDR high */
#define PI4IOE_REG_CHIP_RESET 0x01
#define PI4IOE_REG_IO_DIR 0x03
#define PI4IOE_REG_OUT_SET 0x05
#define PI4IOE_REG_OUT_H_IM 0x07
#define PI4IOE_REG_IN_DEF_STA 0x09
#define PI4IOE_REG_PULL_EN 0x0B
#define PI4IOE_REG_PULL_SEL 0x0D
#define PI4IOE_REG_INT_MASK 0x11

/* EXP1 (0x43): P4=LCD_RST(active-low), P5=TP_RST(active-low). Base output word
 * from the BSP with P1,P2,P4,P5,P6 high (LCD_RST + TP_RST deasserted). */
#define TAB5_EXP1_OUT_BASE 0x76 /* 0b01110110 */
#define TAB5_EXP1_TP_RST_BIT (1 << 5)

/* Touch controller I2C addresses used for panel-variant detection. */
#define TAB5_GT911_ADDR_MAIN 0x5D
#define TAB5_GT911_ADDR_ALT 0x14
#define TAB5_ST712X_TOUCH_ADDR 0x55

typedef enum {
    TAB5_PANEL_ILI9881C = 0,
    TAB5_PANEL_ST7121,
    TAB5_PANEL_ST7123,
} tab5_panel_variant_t;

static const char *TAG = "tab5_display";

static tab5_panel_variant_t s_variant = TAB5_PANEL_ILI9881C;
static esp_lcd_dsi_bus_handle_t s_dsi_bus;
static esp_lcd_panel_io_handle_t s_dbi_io;
static esp_ldo_channel_handle_t s_ldo_mipi;
static esp_lcd_panel_handle_t s_panel;
static SemaphoreHandle_t s_refresh_done;
static bool s_dirty_rows_pending;
static int s_dirty_y1;
static int s_dirty_y2;
static bool s_backlight_ready;
static bool s_power_ready;
static int16_t s_last_touch_x;
static int16_t s_last_touch_y;
#ifdef CONFIG_M5STACK_TAB5_LANDSCAPE
static ppa_client_handle_t s_srm;  /* dedicated PPA scale-rotate-mirror client */
static void *s_panel_fb[2];        /* panel-owned 720x1280 framebuffers (rotate targets) */
static void *s_ls_buf[2];          /* landscape 1280x720 LVGL draw buffers */
static int s_panel_back;           /* index of the panel FB to rotate into next */
#endif

/* ST7123/ST7121 touch registers (16-bit addresses on I2C 0x55). */
#define ST712X_ADV_INFO_REG 0x0010
#define ST712X_MAX_TOUCHES_REG 0x0009
#define ST712X_REPORT_COORD_REG 0x0014
#define ST712X_REPORT_SIZE 7 /* bytes per touch report */
#define ST712X_MAX_REPORTS 10

/* ILI9881C vendor init sequence, copied verbatim from the M5Stack Tab5 BSP
 * (components/m5stack_tab5/include/bsp/ili9881_init_data.c). */
static const ili9881c_lcd_init_cmd_t tab5_ili9881c_init_cmds[] = {
    {0xFF, (uint8_t[]){0x98, 0x81, 0x01}, 3, 0},
    {0xB7, (uint8_t[]){0x03}, 1, 0}, /* set 2 lane */
    {0xFF, (uint8_t[]){0x98, 0x81, 0x03}, 3, 0},
    {0x01, (uint8_t[]){0x00}, 1, 0}, {0x02, (uint8_t[]){0x00}, 1, 0},
    {0x03, (uint8_t[]){0x73}, 1, 0}, {0x04, (uint8_t[]){0x00}, 1, 0},
    {0x05, (uint8_t[]){0x00}, 1, 0}, {0x06, (uint8_t[]){0x08}, 1, 0},
    {0x07, (uint8_t[]){0x00}, 1, 0}, {0x08, (uint8_t[]){0x00}, 1, 0},
    {0x09, (uint8_t[]){0x1B}, 1, 0}, {0x0a, (uint8_t[]){0x01}, 1, 0},
    {0x0b, (uint8_t[]){0x01}, 1, 0}, {0x0c, (uint8_t[]){0x0D}, 1, 0},
    {0x0d, (uint8_t[]){0x01}, 1, 0}, {0x0e, (uint8_t[]){0x01}, 1, 0},
    {0x0f, (uint8_t[]){0x26}, 1, 0}, {0x10, (uint8_t[]){0x26}, 1, 0},
    {0x11, (uint8_t[]){0x00}, 1, 0}, {0x12, (uint8_t[]){0x00}, 1, 0},
    {0x13, (uint8_t[]){0x02}, 1, 0}, {0x14, (uint8_t[]){0x00}, 1, 0},
    {0x15, (uint8_t[]){0x00}, 1, 0}, {0x16, (uint8_t[]){0x00}, 1, 0},
    {0x17, (uint8_t[]){0x00}, 1, 0}, {0x18, (uint8_t[]){0x00}, 1, 0},
    {0x19, (uint8_t[]){0x00}, 1, 0}, {0x1a, (uint8_t[]){0x00}, 1, 0},
    {0x1b, (uint8_t[]){0x00}, 1, 0}, {0x1c, (uint8_t[]){0x00}, 1, 0},
    {0x1d, (uint8_t[]){0x00}, 1, 0}, {0x1e, (uint8_t[]){0x40}, 1, 0},
    {0x1f, (uint8_t[]){0x00}, 1, 0}, {0x20, (uint8_t[]){0x06}, 1, 0},
    {0x21, (uint8_t[]){0x01}, 1, 0}, {0x22, (uint8_t[]){0x00}, 1, 0},
    {0x23, (uint8_t[]){0x00}, 1, 0}, {0x24, (uint8_t[]){0x00}, 1, 0},
    {0x25, (uint8_t[]){0x00}, 1, 0}, {0x26, (uint8_t[]){0x00}, 1, 0},
    {0x27, (uint8_t[]){0x00}, 1, 0}, {0x28, (uint8_t[]){0x33}, 1, 0},
    {0x29, (uint8_t[]){0x03}, 1, 0}, {0x2a, (uint8_t[]){0x00}, 1, 0},
    {0x2b, (uint8_t[]){0x00}, 1, 0}, {0x2c, (uint8_t[]){0x00}, 1, 0},
    {0x2d, (uint8_t[]){0x00}, 1, 0}, {0x2e, (uint8_t[]){0x00}, 1, 0},
    {0x2f, (uint8_t[]){0x00}, 1, 0}, {0x30, (uint8_t[]){0x00}, 1, 0},
    {0x31, (uint8_t[]){0x00}, 1, 0}, {0x32, (uint8_t[]){0x00}, 1, 0},
    {0x33, (uint8_t[]){0x00}, 1, 0}, {0x34, (uint8_t[]){0x00}, 1, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0}, {0x36, (uint8_t[]){0x00}, 1, 0},
    {0x37, (uint8_t[]){0x00}, 1, 0}, {0x38, (uint8_t[]){0x00}, 1, 0},
    {0x39, (uint8_t[]){0x00}, 1, 0}, {0x3a, (uint8_t[]){0x00}, 1, 0},
    {0x3b, (uint8_t[]){0x00}, 1, 0}, {0x3c, (uint8_t[]){0x00}, 1, 0},
    {0x3d, (uint8_t[]){0x00}, 1, 0}, {0x3e, (uint8_t[]){0x00}, 1, 0},
    {0x3f, (uint8_t[]){0x00}, 1, 0}, {0x40, (uint8_t[]){0x00}, 1, 0},
    {0x41, (uint8_t[]){0x00}, 1, 0}, {0x42, (uint8_t[]){0x00}, 1, 0},
    {0x43, (uint8_t[]){0x00}, 1, 0}, {0x44, (uint8_t[]){0x00}, 1, 0},
    {0x50, (uint8_t[]){0x01}, 1, 0}, {0x51, (uint8_t[]){0x23}, 1, 0},
    {0x52, (uint8_t[]){0x45}, 1, 0}, {0x53, (uint8_t[]){0x67}, 1, 0},
    {0x54, (uint8_t[]){0x89}, 1, 0}, {0x55, (uint8_t[]){0xab}, 1, 0},
    {0x56, (uint8_t[]){0x01}, 1, 0}, {0x57, (uint8_t[]){0x23}, 1, 0},
    {0x58, (uint8_t[]){0x45}, 1, 0}, {0x59, (uint8_t[]){0x67}, 1, 0},
    {0x5a, (uint8_t[]){0x89}, 1, 0}, {0x5b, (uint8_t[]){0xab}, 1, 0},
    {0x5c, (uint8_t[]){0xcd}, 1, 0}, {0x5d, (uint8_t[]){0xef}, 1, 0},
    {0x5e, (uint8_t[]){0x11}, 1, 0}, {0x5f, (uint8_t[]){0x02}, 1, 0},
    {0x60, (uint8_t[]){0x00}, 1, 0}, {0x61, (uint8_t[]){0x07}, 1, 0},
    {0x62, (uint8_t[]){0x06}, 1, 0}, {0x63, (uint8_t[]){0x0E}, 1, 0},
    {0x64, (uint8_t[]){0x0F}, 1, 0}, {0x65, (uint8_t[]){0x0C}, 1, 0},
    {0x66, (uint8_t[]){0x0D}, 1, 0}, {0x67, (uint8_t[]){0x02}, 1, 0},
    {0x68, (uint8_t[]){0x02}, 1, 0}, {0x69, (uint8_t[]){0x02}, 1, 0},
    {0x6a, (uint8_t[]){0x02}, 1, 0}, {0x6b, (uint8_t[]){0x02}, 1, 0},
    {0x6c, (uint8_t[]){0x02}, 1, 0}, {0x6d, (uint8_t[]){0x02}, 1, 0},
    {0x6e, (uint8_t[]){0x02}, 1, 0}, {0x6f, (uint8_t[]){0x02}, 1, 0},
    {0x70, (uint8_t[]){0x02}, 1, 0}, {0x71, (uint8_t[]){0x02}, 1, 0},
    {0x72, (uint8_t[]){0x02}, 1, 0}, {0x73, (uint8_t[]){0x05}, 1, 0},
    {0x74, (uint8_t[]){0x01}, 1, 0}, {0x75, (uint8_t[]){0x02}, 1, 0},
    {0x76, (uint8_t[]){0x00}, 1, 0}, {0x77, (uint8_t[]){0x07}, 1, 0},
    {0x78, (uint8_t[]){0x06}, 1, 0}, {0x79, (uint8_t[]){0x0E}, 1, 0},
    {0x7a, (uint8_t[]){0x0F}, 1, 0}, {0x7b, (uint8_t[]){0x0C}, 1, 0},
    {0x7c, (uint8_t[]){0x0D}, 1, 0}, {0x7d, (uint8_t[]){0x02}, 1, 0},
    {0x7e, (uint8_t[]){0x02}, 1, 0}, {0x7f, (uint8_t[]){0x02}, 1, 0},
    {0x80, (uint8_t[]){0x02}, 1, 0}, {0x81, (uint8_t[]){0x02}, 1, 0},
    {0x82, (uint8_t[]){0x02}, 1, 0}, {0x83, (uint8_t[]){0x02}, 1, 0},
    {0x84, (uint8_t[]){0x02}, 1, 0}, {0x85, (uint8_t[]){0x02}, 1, 0},
    {0x86, (uint8_t[]){0x02}, 1, 0}, {0x87, (uint8_t[]){0x02}, 1, 0},
    {0x88, (uint8_t[]){0x02}, 1, 0}, {0x89, (uint8_t[]){0x05}, 1, 0},
    {0x8A, (uint8_t[]){0x01}, 1, 0},
    {0xFF, (uint8_t[]){0x98, 0x81, 0x04}, 3, 0},
    {0x38, (uint8_t[]){0x01}, 1, 0}, {0x39, (uint8_t[]){0x00}, 1, 0},
    {0x6C, (uint8_t[]){0x15}, 1, 0}, {0x6E, (uint8_t[]){0x1A}, 1, 0},
    {0x6F, (uint8_t[]){0x25}, 1, 0}, {0x3A, (uint8_t[]){0xA4}, 1, 0},
    {0x8D, (uint8_t[]){0x20}, 1, 0}, {0x87, (uint8_t[]){0xBA}, 1, 0},
    {0x3B, (uint8_t[]){0x98}, 1, 0},
    {0xFF, (uint8_t[]){0x98, 0x81, 0x01}, 3, 0},
    {0x22, (uint8_t[]){0x0A}, 1, 0}, {0x31, (uint8_t[]){0x00}, 1, 0},
    {0x50, (uint8_t[]){0x6B}, 1, 0}, {0x51, (uint8_t[]){0x66}, 1, 0},
    {0x53, (uint8_t[]){0x73}, 1, 0}, {0x55, (uint8_t[]){0x8B}, 1, 0},
    {0x60, (uint8_t[]){0x1B}, 1, 0}, {0x61, (uint8_t[]){0x01}, 1, 0},
    {0x62, (uint8_t[]){0x0C}, 1, 0}, {0x63, (uint8_t[]){0x00}, 1, 0},
    {0xA0, (uint8_t[]){0x00}, 1, 0}, {0xA1, (uint8_t[]){0x15}, 1, 0},
    {0xA2, (uint8_t[]){0x1F}, 1, 0}, {0xA3, (uint8_t[]){0x13}, 1, 0},
    {0xA4, (uint8_t[]){0x11}, 1, 0}, {0xA5, (uint8_t[]){0x21}, 1, 0},
    {0xA6, (uint8_t[]){0x17}, 1, 0}, {0xA7, (uint8_t[]){0x1B}, 1, 0},
    {0xA8, (uint8_t[]){0x6B}, 1, 0}, {0xA9, (uint8_t[]){0x1E}, 1, 0},
    {0xAA, (uint8_t[]){0x2B}, 1, 0}, {0xAB, (uint8_t[]){0x5D}, 1, 0},
    {0xAC, (uint8_t[]){0x19}, 1, 0}, {0xAD, (uint8_t[]){0x14}, 1, 0},
    {0xAE, (uint8_t[]){0x4B}, 1, 0}, {0xAF, (uint8_t[]){0x1D}, 1, 0},
    {0xB0, (uint8_t[]){0x27}, 1, 0}, {0xB1, (uint8_t[]){0x49}, 1, 0},
    {0xB2, (uint8_t[]){0x5D}, 1, 0}, {0xB3, (uint8_t[]){0x39}, 1, 0},
    {0xC0, (uint8_t[]){0x00}, 1, 0}, {0xC1, (uint8_t[]){0x01}, 1, 0},
    {0xC2, (uint8_t[]){0x0C}, 1, 0}, {0xC3, (uint8_t[]){0x11}, 1, 0},
    {0xC4, (uint8_t[]){0x15}, 1, 0}, {0xC5, (uint8_t[]){0x28}, 1, 0},
    {0xC6, (uint8_t[]){0x1B}, 1, 0}, {0xC7, (uint8_t[]){0x1C}, 1, 0},
    {0xC8, (uint8_t[]){0x62}, 1, 0}, {0xC9, (uint8_t[]){0x1C}, 1, 0},
    {0xCA, (uint8_t[]){0x29}, 1, 0}, {0xCB, (uint8_t[]){0x60}, 1, 0},
    {0xCC, (uint8_t[]){0x16}, 1, 0}, {0xCD, (uint8_t[]){0x17}, 1, 0},
    {0xCE, (uint8_t[]){0x4A}, 1, 0}, {0xCF, (uint8_t[]){0x23}, 1, 0},
    {0xD0, (uint8_t[]){0x24}, 1, 0}, {0xD1, (uint8_t[]){0x4F}, 1, 0},
    {0xD2, (uint8_t[]){0x5F}, 1, 0}, {0xD3, (uint8_t[]){0x39}, 1, 0},
    {0xFF, (uint8_t[]){0x98, 0x81, 0x00}, 3, 0},
    {0x35, (uint8_t[]){0x00}, 0, 0},
    {0x53, (uint8_t[]){0x24}, 1, 0},
    {0x51, (uint8_t[]){0xFF}, 1, 0},
    {0xFE, (uint8_t[]){0x00}, 0, 0},
    {0x29, (uint8_t[]){0x00}, 0, 0},
};

/* ST7123 vendor init sequence, copied verbatim from the M5Stack Tab5 BSP
 * (m5stack_tab5.c st7123_vendor_specific_init_default). ST7121 uses its
 * driver's built-in sequence (init_cmds = NULL). */
static const st7123_lcd_init_cmd_t tab5_st7123_init_cmds[] = {
    {0x60, (uint8_t[]){0x71, 0x23, 0xa2}, 3, 0},
    {0x60, (uint8_t[]){0x71, 0x23, 0xa3}, 3, 0},
    {0x60, (uint8_t[]){0x71, 0x23, 0xa4}, 3, 0},
    {0xA4, (uint8_t[]){0x31}, 1, 0},
    {0xD7, (uint8_t[]){0x10, 0x0A, 0x10, 0x2A, 0x80, 0x80}, 6, 0},
    {0x90, (uint8_t[]){0x71, 0x23, 0x5A, 0x20, 0x24, 0x09, 0x09}, 7, 0},
    {0xA3, (uint8_t[]){0x80, 0x01, 0x88, 0x30, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46, 0x00, 0x00,
                       0x1E, 0x5C, 0x1E, 0x80, 0x00, 0x4F, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46,
                       0x00, 0x00, 0x1E, 0x5C, 0x1E, 0x80, 0x00, 0x6F, 0x58, 0x00, 0x00, 0x00, 0xFF},
     40, 0},
    {0xA6, (uint8_t[]){0x03, 0x00, 0x24, 0x55, 0x36, 0x00, 0x39, 0x00, 0x6E, 0x6E, 0x91, 0xFF, 0x00, 0x24,
                       0x55, 0x38, 0x00, 0x37, 0x00, 0x6E, 0x6E, 0x91, 0xFF, 0x00, 0x24, 0x11, 0x00, 0x00,
                       0x00, 0x00, 0x6E, 0x6E, 0x91, 0xFF, 0x00, 0xEC, 0x11, 0x00, 0x03, 0x00, 0x03, 0x6E,
                       0x6E, 0xFF, 0xFF, 0x00, 0x08, 0x80, 0x08, 0x80, 0x06, 0x00, 0x00, 0x00, 0x00},
     55, 0},
    {0xA7, (uint8_t[]){0x19, 0x19, 0x80, 0x64, 0x40, 0x07, 0x16, 0x40, 0x00, 0x44, 0x03, 0x6E, 0x6E, 0x91, 0xFF,
                       0x08, 0x80, 0x64, 0x40, 0x25, 0x34, 0x40, 0x00, 0x02, 0x01, 0x6E, 0x6E, 0x91, 0xFF, 0x08,
                       0x80, 0x64, 0x40, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x6E, 0x6E, 0x91, 0xFF, 0x08, 0x80,
                       0x64, 0x40, 0x00, 0x00, 0x00, 0x00, 0x20, 0x00, 0x6E, 0x6E, 0x84, 0xFF, 0x08, 0x80, 0x44},
     60, 0},
    {0xAC, (uint8_t[]){0x03, 0x19, 0x19, 0x18, 0x18, 0x06, 0x13, 0x13, 0x11, 0x11, 0x08, 0x08, 0x0A, 0x0A, 0x1C,
                       0x1C, 0x07, 0x07, 0x00, 0x00, 0x02, 0x02, 0x01, 0x19, 0x19, 0x18, 0x18, 0x06, 0x12, 0x12,
                       0x10, 0x10, 0x09, 0x09, 0x0B, 0x0B, 0x1C, 0x1C, 0x07, 0x07, 0x03, 0x03, 0x01, 0x01},
     44, 0},
    {0xAD, (uint8_t[]){0xF0, 0x00, 0x46, 0x00, 0x03, 0x50, 0x50, 0xFF, 0xFF, 0xF0, 0x40, 0x06, 0x01,
                       0x07, 0x42, 0x42, 0xFF, 0xFF, 0x01, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF},
     25, 0},
    {0xAE, (uint8_t[]){0xFE, 0x3F, 0x3F, 0xFE, 0x3F, 0x3F, 0x00}, 7, 0},
    {0xB2, (uint8_t[]){0x15, 0x19, 0x05, 0x23, 0x49, 0xAF, 0x03, 0x2E, 0x5C, 0xD2, 0xFF, 0x10, 0x20, 0xFD, 0x20, 0xC0, 0x00},
     17, 0},
    {0xE8, (uint8_t[]){0x20, 0x6F, 0x04, 0x97, 0x97, 0x3E, 0x04, 0xDC, 0xDC, 0x3E, 0x06, 0xFA, 0x26, 0x3E}, 15, 0},
    {0x75, (uint8_t[]){0x03, 0x04}, 2, 0},
    {0xE7, (uint8_t[]){0x3B, 0x00, 0x00, 0x7C, 0xA1, 0x8C, 0x20, 0x1A, 0xF0, 0xB1, 0x50, 0x00,
                       0x50, 0xB1, 0x50, 0xB1, 0x50, 0xD8, 0x00, 0x55, 0x00, 0xB1, 0x00, 0x45,
                       0xC9, 0x6A, 0xFF, 0x5A, 0xD8, 0x18, 0x88, 0x15, 0xB1, 0x01, 0x01, 0x77},
     36, 0},
    {0xEA, (uint8_t[]){0x13, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x2C}, 8, 0},
    {0xB0, (uint8_t[]){0x22, 0x43, 0x11, 0x61, 0x25, 0x43, 0x43}, 7, 0},
    {0xb7, (uint8_t[]){0x00, 0x00, 0x73, 0x73}, 4, 0},
    {0xBF, (uint8_t[]){0xA6, 0xAA}, 2, 0},
    {0xA9, (uint8_t[]){0x00, 0x00, 0x73, 0xFF, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03}, 10, 0},
    {0xC8, (uint8_t[]){0x00, 0x00, 0x10, 0x1F, 0x36, 0x00, 0x5D, 0x04, 0x9D, 0x05, 0x10, 0xF2, 0x06,
                       0x60, 0x03, 0x11, 0xAD, 0x00, 0xEF, 0x01, 0x22, 0x2E, 0x0E, 0x74, 0x08, 0x32,
                       0xDC, 0x09, 0x33, 0x0F, 0xF3, 0x77, 0x0D, 0xB0, 0xDC, 0x03, 0xFF},
     37, 0},
    {0xC9, (uint8_t[]){0x00, 0x00, 0x10, 0x1F, 0x36, 0x00, 0x5D, 0x04, 0x9D, 0x05, 0x10, 0xF2, 0x06,
                       0x60, 0x03, 0x11, 0xAD, 0x00, 0xEF, 0x01, 0x22, 0x2E, 0x0E, 0x74, 0x08, 0x32,
                       0xDC, 0x09, 0x33, 0x0F, 0xF3, 0x77, 0x0D, 0xB0, 0xDC, 0x03, 0xFF},
     37, 0},
    {0x36, (uint8_t[]){0x00}, 1, 0},
    {0x11, (uint8_t[]){0x00}, 1, 100},
    {0x29, (uint8_t[]){0x00}, 1, 0},
    {0x35, (uint8_t[]){0x00}, 1, 100},
};

static esp_err_t tab5_exp_write(uint8_t addr, uint8_t reg, uint8_t value)
{
    return lvgl_i2c_write(TAB5_I2C_PORT, addr, reg, &value, 1);
}

esp_err_t tab5_board_power_init(void)
{
    if (s_power_ready) {
        return ESP_OK;
    }

    esp_err_t err = lvgl_i2c_init(TAB5_I2C_PORT);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "system I2C init failed: %s", esp_err_to_name(err));
        return err;
    }

    /* EXP1 @0x43: display / touch reset + speaker + antenna rails. */
    tab5_exp_write(TAB5_EXP1_ADDR, PI4IOE_REG_CHIP_RESET, 0xFF);
    vTaskDelay(pdMS_TO_TICKS(1));
    tab5_exp_write(TAB5_EXP1_ADDR, PI4IOE_REG_IO_DIR, 0x7F);   /* P0..P6 output */
    tab5_exp_write(TAB5_EXP1_ADDR, PI4IOE_REG_OUT_H_IM, 0x00);
    tab5_exp_write(TAB5_EXP1_ADDR, PI4IOE_REG_PULL_SEL, 0x7F);
    tab5_exp_write(TAB5_EXP1_ADDR, PI4IOE_REG_PULL_EN, 0x7F);
    /* P1,P2,P4,P5,P6 high -> LCD_RST (P4) and TP_RST (P5) deasserted. */
    err = tab5_exp_write(TAB5_EXP1_ADDR, PI4IOE_REG_OUT_SET, TAB5_EXP1_OUT_BASE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PI4IOE1 (0x43) init failed: %s -- panel rails not powered",
                 esp_err_to_name(err));
        return err;
    }

    /* EXP2 @0x44: P0 = WLAN_PWR_EN (onboard ESP32-C6 radio power). Must be high
     * before ESP-Hosted brings up the SDIO link. */
    tab5_exp_write(TAB5_EXP2_ADDR, PI4IOE_REG_CHIP_RESET, 0xFF);
    vTaskDelay(pdMS_TO_TICKS(1));
    tab5_exp_write(TAB5_EXP2_ADDR, PI4IOE_REG_IO_DIR, 0xB9);
    tab5_exp_write(TAB5_EXP2_ADDR, PI4IOE_REG_OUT_H_IM, 0x06);
    tab5_exp_write(TAB5_EXP2_ADDR, PI4IOE_REG_PULL_SEL, 0xB9);
    tab5_exp_write(TAB5_EXP2_ADDR, PI4IOE_REG_PULL_EN, 0xF9);
    tab5_exp_write(TAB5_EXP2_ADDR, PI4IOE_REG_IN_DEF_STA, 0x40);
    tab5_exp_write(TAB5_EXP2_ADDR, PI4IOE_REG_INT_MASK, 0xBF);
    err = tab5_exp_write(TAB5_EXP2_ADDR, PI4IOE_REG_OUT_SET, 0x09); /* P0=WLAN_PWR_EN, P3=USB5V_EN */
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PI4IOE2 (0x44) init failed: %s -- C6 radio not powered",
                 esp_err_to_name(err));
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(20));
    s_power_ready = true;
    ESP_LOGI(TAG, "Tab5 board power up: WLAN_PWR_EN + display rails asserted");
    return ESP_OK;
}

/* Distinguish the panel families. ST7123 and ST7121 both answer on touch I2C
 * 0x55 and differ only by touch FW version at reg 0x0000: 1 = ST7121, 3 = ST7123
 * (M5GFX PR #202). Otherwise it is the early ILI9881C + GT911 panel. */
static tab5_panel_variant_t tab5_detect_panel(void)
{
    uint8_t ver = 0;
    if (lvgl_i2c_read(TAB5_I2C_PORT, TAB5_ST712X_TOUCH_ADDR, 0x0000 | I2C_REG_16, &ver, 1) == ESP_OK) {
        tab5_panel_variant_t v = (ver == 1) ? TAB5_PANEL_ST7121 : TAB5_PANEL_ST7123;
        ESP_LOGI(TAG, "ST712x touch @0x55 FW version %u -> panel %s", ver,
                 v == TAB5_PANEL_ST7121 ? "ST7121" : "ST7123");
        return v;
    }
    uint8_t b = 0;
    if (lvgl_i2c_read(TAB5_I2C_PORT, TAB5_GT911_ADDR_MAIN, 0x8140 | I2C_REG_16, &b, 1) == ESP_OK ||
        lvgl_i2c_read(TAB5_I2C_PORT, TAB5_GT911_ADDR_ALT, 0x8140 | I2C_REG_16, &b, 1) == ESP_OK) {
        ESP_LOGI(TAG, "GT911 touch detected -> panel ILI9881C");
    } else {
        ESP_LOGW(TAG, "No known touch controller ACKed; defaulting to ILI9881C");
    }
    return TAB5_PANEL_ILI9881C;
}

static IRAM_ATTR bool tab5_refresh_done_cb(esp_lcd_panel_handle_t panel,
                                           esp_lcd_dpi_panel_event_data_t *event_data,
                                           void *user_ctx)
{
    (void)panel;
    (void)event_data;
    BaseType_t task_woken = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)user_ctx, &task_woken);
    return task_woken == pdTRUE;
}

static esp_err_t tab5_backlight_init(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode = TAB5_BACKLIGHT_LEDC_MODE,
        .timer_num = TAB5_BACKLIGHT_LEDC_TIMER,
        .duty_resolution = TAB5_BACKLIGHT_LEDC_RES,
        .freq_hz = TAB5_BACKLIGHT_LEDC_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "backlight LEDC timer config failed: %s", esp_err_to_name(err));
        return err;
    }
    const ledc_channel_config_t channel = {
        .gpio_num = TAB5_BACKLIGHT_GPIO,
        .speed_mode = TAB5_BACKLIGHT_LEDC_MODE,
        .channel = TAB5_BACKLIGHT_LEDC_CHANNEL,
        .timer_sel = TAB5_BACKLIGHT_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    err = ledc_channel_config(&channel);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "backlight LEDC channel config failed: %s", esp_err_to_name(err));
        return err;
    }
    s_backlight_ready = true;
    return ESP_OK;
}

#ifdef CONFIG_M5STACK_TAB5_LANDSCAPE
static esp_err_t tab5_srm_ensure(void)
{
    if (s_srm) {
        return ESP_OK;
    }
    const ppa_client_config_t cfg = {
        .oper_type = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1,
        .data_burst_length = PPA_DATA_BURST_LENGTH_128,
    };
    esp_err_t err = ppa_register_client(&cfg, &s_srm);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PPA SRM client register failed: %s", esp_err_to_name(err));
    }
    return err;
}

/* Rotate a 1280x720 landscape RGB565 frame 90/270 degrees into the 720x1280
 * portrait panel framebuffer using the P4 PPA hardware. Blocking; the PPA
 * driver performs all cache maintenance on both buffers. */
static esp_err_t tab5_rotate_present(const void *src, void *dst)
{
    if (tab5_srm_ensure() != ESP_OK) {
        return ESP_FAIL;
    }
    ppa_srm_oper_config_t c = {
        .in = {
            .buffer = src,
            .pic_w = TAB5_LS_W,
            .pic_h = TAB5_LS_H,
            .block_w = TAB5_LS_W,
            .block_h = TAB5_LS_H,
            .block_offset_x = 0,
            .block_offset_y = 0,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
        .out = {
            .buffer = dst,
            .buffer_size = (uint32_t)TAB5_H_RES * TAB5_V_RES * sizeof(uint16_t),
            .pic_w = TAB5_H_RES,
            .pic_h = TAB5_V_RES,
            .block_offset_x = 0,
            .block_offset_y = 0,
            .srm_cm = PPA_SRM_COLOR_MODE_RGB565,
        },
#if TAB5_LANDSCAPE_ANGLE_90
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_90,
#else
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_270,
#endif
        .scale_x = 1.0f,
        .scale_y = 1.0f,
        .mirror_x = false,
        .mirror_y = false,
        .rgb_swap = false,
        .byte_swap = false,
        .alpha_update_mode = PPA_ALPHA_NO_CHANGE,
        .mode = PPA_TRANS_MODE_BLOCKING,
    };
    return ppa_do_scale_rotate_mirror(s_srm, &c);
}
#endif /* CONFIG_M5STACK_TAB5_LANDSCAPE */

esp_err_t tab5_display_init(void)
{
    esp_err_t err = tab5_board_power_init();
    if (err != ESP_OK) {
        return err;
    }

    s_variant = tab5_detect_panel();
    const bool is_ili = (s_variant == TAB5_PANEL_ILI9881C);

    const esp_ldo_channel_config_t ldo_config = {
        .chan_id = TAB5_MIPI_LDO_CHAN,
        .voltage_mv = TAB5_MIPI_LDO_MV,
    };
    err = esp_ldo_acquire_channel(&ldo_config, &s_ldo_mipi);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to enable MIPI PHY LDO%d: %s", TAB5_MIPI_LDO_CHAN,
                 esp_err_to_name(err));
        return err;
    }

    const esp_lcd_dsi_bus_config_t bus_config = {
        .bus_id = 0,
        .num_data_lanes = TAB5_DSI_LANES,
        .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = is_ili ? TAB5_ILI9881C_LANE_MBPS : TAB5_ST712X_LANE_MBPS,
    };
    err = esp_lcd_new_dsi_bus(&bus_config, &s_dsi_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to create DSI bus: %s", esp_err_to_name(err));
        return err;
    }

    const esp_lcd_dbi_io_config_t dbi_config = {
        .virtual_channel = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    err = esp_lcd_new_panel_io_dbi(s_dsi_bus, &dbi_config, &s_dbi_io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to create DSI DBI I/O: %s", esp_err_to_name(err));
        goto fail;
    }

    esp_lcd_dpi_panel_config_t dpi_config = {
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = is_ili ? TAB5_ILI9881C_DPI_MHZ : TAB5_ST712X_DPI_MHZ,
        .virtual_channel = 0,
        .in_color_format = LCD_COLOR_FMT_RGB565,
        .out_color_format = LCD_COLOR_FMT_RGB565,
        .num_fbs = 2,
        .video_timing = {
            .h_size = TAB5_H_RES,
            .v_size = TAB5_V_RES,
        },
    };
    if (is_ili) {
        dpi_config.video_timing.hsync_back_porch = 140;
        dpi_config.video_timing.hsync_pulse_width = 40;
        dpi_config.video_timing.hsync_front_porch = 40;
        dpi_config.video_timing.vsync_back_porch = 20;
        dpi_config.video_timing.vsync_pulse_width = 4;
        dpi_config.video_timing.vsync_front_porch = 20;
    } else {
        /* ST7123 / ST7121 share hsync timing (M5GFX PR #202). */
        dpi_config.video_timing.hsync_pulse_width = 2;
        dpi_config.video_timing.hsync_back_porch = 40;
        dpi_config.video_timing.hsync_front_porch = 40;
        if (s_variant == TAB5_PANEL_ST7121) {
            dpi_config.video_timing.vsync_pulse_width = 20;
            dpi_config.video_timing.vsync_back_porch = 24;
            dpi_config.video_timing.vsync_front_porch = 200;
        } else {
            dpi_config.video_timing.vsync_pulse_width = 2;
            dpi_config.video_timing.vsync_back_porch = 8;
            dpi_config.video_timing.vsync_front_porch = 220;
        }
    }

    if (s_variant == TAB5_PANEL_ILI9881C) {
        ili9881c_vendor_config_t vendor_config = {
            .init_cmds = tab5_ili9881c_init_cmds,
            .init_cmds_size = sizeof(tab5_ili9881c_init_cmds) / sizeof(tab5_ili9881c_init_cmds[0]),
            .mipi_config = {
                .dsi_bus = s_dsi_bus,
                .dpi_config = &dpi_config,
                .lane_num = TAB5_DSI_LANES,
            },
        };
        const esp_lcd_panel_dev_config_t panel_config = {
            .reset_gpio_num = -1, /* reset via IO-expander 0x43 P4 */
            .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
            .bits_per_pixel = 16,
            .vendor_config = &vendor_config,
        };
        err = esp_lcd_new_panel_ili9881c(s_dbi_io, &panel_config, &s_panel);
    } else if (s_variant == TAB5_PANEL_ST7121) {
        st7121_vendor_config_t vendor_config = {
            .init_cmds = NULL, /* ST7121 driver uses its built-in sequence */
            .init_cmds_size = 0,
            .mipi_config = {
                .dsi_bus = s_dsi_bus,
                .dpi_config = &dpi_config,
                /* st7121_vendor_config_t has no lane_num field (unlike ST7123);
                 * the ST7121 driver derives lane count from the DSI bus. */
            },
        };
        const esp_lcd_panel_dev_config_t panel_config = {
            .reset_gpio_num = -1,
            .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
            .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,
            .bits_per_pixel = 24,
            .vendor_config = &vendor_config,
        };
        err = esp_lcd_new_panel_st7121(s_dbi_io, &panel_config, &s_panel);
    } else { /* TAB5_PANEL_ST7123 */
        st7123_vendor_config_t vendor_config = {
            .init_cmds = tab5_st7123_init_cmds,
            .init_cmds_size = sizeof(tab5_st7123_init_cmds) / sizeof(tab5_st7123_init_cmds[0]),
            .mipi_config = {
                .dsi_bus = s_dsi_bus,
                .dpi_config = &dpi_config,
                .lane_num = TAB5_DSI_LANES,
            },
        };
        const esp_lcd_panel_dev_config_t panel_config = {
            .reset_gpio_num = -1,
            .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
            .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,
            .bits_per_pixel = 24,
            .vendor_config = &vendor_config,
        };
        err = esp_lcd_new_panel_st7123(s_dbi_io, &panel_config, &s_panel);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to create panel: %s", esp_err_to_name(err));
        goto fail;
    }

    err = esp_lcd_panel_reset(s_panel);
    if (err == ESP_OK) {
        err = esp_lcd_panel_init(s_panel);
    }
    if (err == ESP_OK) {
        err = esp_lcd_panel_disp_on_off(s_panel, true);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to initialize panel: %s", esp_err_to_name(err));
        goto fail;
    }

    s_refresh_done = xSemaphoreCreateBinary();
    if (!s_refresh_done) {
        err = ESP_ERR_NO_MEM;
        ESP_LOGE(TAG, "failed to create display VSYNC semaphore");
        goto fail;
    }
    const esp_lcd_dpi_panel_event_callbacks_t dpi_callbacks = {
        .on_refresh_done = tab5_refresh_done_cb,
    };
    err = esp_lcd_dpi_panel_register_event_callbacks(s_panel, &dpi_callbacks, s_refresh_done);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to register display VSYNC callback: %s", esp_err_to_name(err));
        goto fail;
    }

#ifdef CONFIG_M5STACK_TAB5_LANDSCAPE
    /* Cache the panel-owned framebuffers as PPA rotate targets and bring up the
     * SRM client (LVGL renders into separate 1280x720 landscape buffers). */
    esp_lcd_dpi_panel_get_frame_buffer(s_panel, 2, &s_panel_fb[0], &s_panel_fb[1]);
    tab5_srm_ensure();
#endif

    if (tab5_backlight_init() == ESP_OK) {
        tab5_display_set_backlight(100);
    }

    ESP_LOGI(TAG, "M5Stack Tab5 display initialized: %s %dx%d",
             s_variant == TAB5_PANEL_ILI9881C ? "ILI9881C"
             : s_variant == TAB5_PANEL_ST7121 ? "ST7121" : "ST7123",
             TAB5_H_RES, TAB5_V_RES);
    return ESP_OK;

fail:
    if (s_panel) {
        esp_lcd_panel_del(s_panel);
        s_panel = NULL;
    }
    if (s_refresh_done) {
        vSemaphoreDelete(s_refresh_done);
        s_refresh_done = NULL;
    }
    if (s_dbi_io) {
        esp_lcd_panel_io_del(s_dbi_io);
        s_dbi_io = NULL;
    }
    if (s_dsi_bus) {
        esp_lcd_del_dsi_bus(s_dsi_bus);
        s_dsi_bus = NULL;
    }
    return err;
}

esp_err_t tab5_display_touch_reset(void)
{
    /* Only the ILI9881C units carry a GT911 whose I2C address latches from INT
     * at TP_RST release (INT low -> 0x5D). The ST712x units use a touch@0x55
     * with no such strap, so skip the GT911-specific dance for them. */
    if (s_variant != TAB5_PANEL_ILI9881C) {
        return ESP_OK;
    }
    const gpio_config_t int_config = {
        .pin_bit_mask = 1ULL << TAB5_TOUCH_INT_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&int_config);
    if (err != ESP_OK) {
        return err;
    }
    gpio_set_level(TAB5_TOUCH_INT_GPIO, 0);

    err = tab5_exp_write(TAB5_EXP1_ADDR, PI4IOE_REG_OUT_SET,
                         TAB5_EXP1_OUT_BASE & ~TAB5_EXP1_TP_RST_BIT);
    vTaskDelay(pdMS_TO_TICKS(10));
    if (err == ESP_OK) {
        err = tab5_exp_write(TAB5_EXP1_ADDR, PI4IOE_REG_OUT_SET, TAB5_EXP1_OUT_BASE);
    }
    vTaskDelay(pdMS_TO_TICKS(100));
    if (err == ESP_OK) {
        err = gpio_set_direction(TAB5_TOUCH_INT_GPIO, GPIO_MODE_INPUT);
    }
    return err;
}

bool tab5_display_is_st712x(void)
{
    return s_variant != TAB5_PANEL_ILI9881C;
}

/* Poll the ST7123/ST7121 touch controller over the shared system I2C bus
 * (reimplemented from esp_lcd_touch_st7123's read_data so it routes through the
 * lvgl_i2c manager rather than opening a second I2C master on port 0). */
void tab5_touch_read(lv_indev_data_t *data)
{
    data->point.x = s_last_touch_x;
    data->point.y = s_last_touch_y;
    data->state = LV_INDEV_STATE_REL;

    uint8_t adv = 0;
    if (lvgl_i2c_read(TAB5_I2C_PORT, TAB5_ST712X_TOUCH_ADDR, ST712X_ADV_INFO_REG | I2C_REG_16,
                      &adv, 1) != ESP_OK) {
        return;
    }
    if (!((adv >> 3) & 0x01)) { /* with_coord bit clear -> no new touch data */
        return;
    }
    uint8_t max_touches = 0;
    if (lvgl_i2c_read(TAB5_I2C_PORT, TAB5_ST712X_TOUCH_ADDR, ST712X_MAX_TOUCHES_REG | I2C_REG_16,
                      &max_touches, 1) != ESP_OK ||
        max_touches == 0) {
        return;
    }
    if (max_touches > ST712X_MAX_REPORTS) {
        max_touches = ST712X_MAX_REPORTS;
    }
    uint8_t buf[ST712X_MAX_REPORTS * ST712X_REPORT_SIZE];
    if (lvgl_i2c_read(TAB5_I2C_PORT, TAB5_ST712X_TOUCH_ADDR, ST712X_REPORT_COORD_REG | I2C_REG_16,
                      buf, (uint16_t)(max_touches * ST712X_REPORT_SIZE)) != ESP_OK) {
        return;
    }
    for (int i = 0; i < max_touches; i++) {
        const uint8_t *r = &buf[i * ST712X_REPORT_SIZE];
        if (!(r[0] & 0x80)) { /* valid bit (bit7 of byte0) */
            continue;
        }
        uint16_t x = ((uint16_t)(r[0] & 0x3F) << 8) | r[1];
        uint16_t y = ((uint16_t)r[2] << 8) | r[3];
        if (x >= TAB5_H_RES) x = TAB5_H_RES - 1;
        if (y >= TAB5_V_RES) y = TAB5_V_RES - 1;
        s_last_touch_x = (int16_t)x;
        s_last_touch_y = (int16_t)y;
        data->point.x = (int16_t)x;
        data->point.y = (int16_t)y;
        data->state = LV_INDEV_STATE_PR;
        return;
    }
}

esp_err_t tab5_display_set_backlight(uint8_t percentage)
{
    if (!s_backlight_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    if (percentage > 100) {
        percentage = 100;
    }
    uint32_t duty = ((uint32_t)TAB5_BACKLIGHT_LEDC_MAX_DUTY * percentage) / 100;
    esp_err_t err = ledc_set_duty(TAB5_BACKLIGHT_LEDC_MODE, TAB5_BACKLIGHT_LEDC_CHANNEL, duty);
    if (err != ESP_OK) {
        return err;
    }
    return ledc_update_duty(TAB5_BACKLIGHT_LEDC_MODE, TAB5_BACKLIGHT_LEDC_CHANNEL);
}

esp_err_t tab5_display_get_frame_buffers(void **fb0, void **fb1)
{
    if (!s_panel || !fb0 || !fb1) {
        return ESP_ERR_INVALID_ARG;
    }
#ifdef CONFIG_M5STACK_TAB5_LANDSCAPE
    /* LVGL draws landscape 1280x720; hand it two PSRAM buffers. The 720x1280
     * panel FBs stay internal as the PPA rotate targets. */
    for (int i = 0; i < 2; i++) {
        if (!s_ls_buf[i]) {
            s_ls_buf[i] = heap_caps_aligned_alloc(64, (size_t)TAB5_LS_W * TAB5_LS_H * sizeof(uint16_t),
                                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA);
            if (!s_ls_buf[i]) {
                ESP_LOGE(TAG, "failed to allocate landscape draw buffer %d", i);
                return ESP_ERR_NO_MEM;
            }
        }
    }
    *fb0 = s_ls_buf[0];
    *fb1 = s_ls_buf[1];
    return ESP_OK;
#else
    return esp_lcd_dpi_panel_get_frame_buffer(s_panel, 2, fb0, fb1);
#endif
}

void tab5_display_mark_dirty_rows(int y1, int y2)
{
    if (y1 < 0) y1 = 0;
    if (y2 >= TAB5_V_RES) y2 = TAB5_V_RES - 1;
    if (y1 > y2) return;

    if (!s_dirty_rows_pending) {
        s_dirty_y1 = y1;
        s_dirty_y2 = y2;
        s_dirty_rows_pending = true;
    } else {
        if (y1 < s_dirty_y1) s_dirty_y1 = y1;
        if (y2 > s_dirty_y2) s_dirty_y2 = y2;
    }
}

void tab5_display_transform_touch(lv_indev_data_t *data)
{
#ifdef CONFIG_M5STACK_TAB5_LANDSCAPE
    /* Touch reports native portrait coords (x in [0,720), y in [0,1280)).
     * Map them into the landscape 1280x720 space, matching the PPA rotation. */
    int16_t tx = data->point.x;
    int16_t ty = data->point.y;
#if TAB5_LANDSCAPE_ANGLE_90
    data->point.x = (int16_t)((TAB5_V_RES - 1) - ty); /* lx = 1279 - ty */
    data->point.y = tx;                               /* ly = tx        */
#else
    data->point.x = ty;                               /* lx = ty        */
    data->point.y = (int16_t)((TAB5_H_RES - 1) - tx); /* ly = 719 - tx  */
#endif
#else
    (void)data;
#endif
}

void tab5_display_flush_cb(lv_disp_drv_t *drv,
                           const lv_area_t *area,
                           lv_color_t *color_p)
{
#ifdef CONFIG_M5STACK_TAB5_LANDSCAPE
    /* Landscape: color_p is the full 1280x720 frame (direct mode). Rotate the
     * whole frame into the back panel FB via PPA and present it. */
    (void)area;
    if (!lv_disp_flush_is_last(drv)) {
        lv_disp_flush_ready(drv);
        return;
    }
    void *dst = s_panel_fb[s_panel_back];
    esp_err_t err = tab5_rotate_present(color_p, dst);
    if (err == ESP_OK) {
        err = esp_lcd_panel_draw_bitmap(s_panel, 0, 0, TAB5_H_RES, TAB5_V_RES, dst);
        s_panel_back ^= 1;
    }
    if (err == ESP_OK && s_refresh_done) {
        xSemaphoreTake(s_refresh_done, 0);
        if (xSemaphoreTake(s_refresh_done, pdMS_TO_TICKS(50)) != pdTRUE) {
            ESP_LOGW(TAG, "display frame completion timeout");
        }
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "landscape present failed: %s", esp_err_to_name(err));
    }
    lv_disp_flush_ready(drv);
#else
    tab5_display_mark_dirty_rows(area->y1, area->y2);

    if (!lv_disp_flush_is_last(drv)) {
        lv_disp_flush_ready(drv);
        return;
    }

    int draw_y1 = s_dirty_rows_pending ? s_dirty_y1 : 0;
    int draw_y2 = s_dirty_rows_pending ? s_dirty_y2 + 1 : TAB5_V_RES;
    s_dirty_rows_pending = false;
    esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, 0, draw_y1, TAB5_H_RES, draw_y2, color_p);
    if (err == ESP_OK && s_refresh_done) {
        xSemaphoreTake(s_refresh_done, 0);
        if (xSemaphoreTake(s_refresh_done, pdMS_TO_TICKS(50)) != pdTRUE) {
            ESP_LOGW(TAG, "display frame completion timeout");
        }
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "display flush failed: %s", esp_err_to_name(err));
    }
    lv_disp_flush_ready(drv);
#endif
}

#endif /* CONFIG_M5STACK_TAB5 */
