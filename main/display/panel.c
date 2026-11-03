/*
 * panel.c — ST7789P3 (76×284) via SPI, plus the LVGL port
 *
 * The pin map and the frame-buffer window offsets come from the board
 * profile (boards/<id>/sdkconfig.defaults, Kconfig menu "Display"); the
 * defaults there match the reference board:
 *
 *   SCL / CLK  → GPIO 12
 *   SDA / MOSI → GPIO 11
 *   CS         → GPIO 10
 *   DC         → GPIO  9
 *   RST        → GPIO  8
 *   BL         → GPIO  7   (on/off, active-low: 0 = backlight on)
 *
 * The panel size itself is fixed in code: the layout below (geometry.h,
 * status.c) is calibrated for a 76×284 panel.
 */

#include "display/display_priv.h"

#include "sdkconfig.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"

#define DISP_SCL_GPIO CONFIG_QMX_DISP_SCL_GPIO
#define DISP_SDA_GPIO CONFIG_QMX_DISP_SDA_GPIO
#define DISP_CS_GPIO CONFIG_QMX_DISP_CS_GPIO
#define DISP_DC_GPIO CONFIG_QMX_DISP_DC_GPIO
#define DISP_RST_GPIO CONFIG_QMX_DISP_RST_GPIO
#define DISP_BL_GPIO CONFIG_QMX_DISP_BL_GPIO

/* Physical panel dimensions, in portrait orientation. */
#define DISP_H_RES 76
#define DISP_V_RES 284

/*
 * ST7789 internal frame buffer is 240×320.
 * The active 76×284 window sits at:
 *   x_gap (columns, 82) — centres 76 px in the 240-wide frame
 *   y_gap (rows, 18)    — shifts  284 px in the 320-tall  frame
 *
 * Adjust the board profile if graphics appear shifted.
 */
#define DISP_X_GAP CONFIG_QMX_DISP_X_GAP
#define DISP_Y_GAP CONFIG_QMX_DISP_Y_GAP

/* Board profile: 40 MHz is safe for ST7789 modules at short wire lengths. */
#define DISP_SPI_CLK_HZ CONFIG_QMX_DISP_SPI_CLK_HZ

_Static_assert(DISP_H_RES == DISP_LOGICAL_H && DISP_V_RES == DISP_LOGICAL_W,
               "panel geometry does not match the rotated logical screen");

static const char *TAG = "display";

lv_display_t *display_panel_init(void)
{
#if CONFIG_QMX_DISP_BL_GPIO >= 0
    const gpio_config_t bl_cfg = {
        .pin_bit_mask = BIT64(DISP_BL_GPIO),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&bl_cfg));
    gpio_set_level(DISP_BL_GPIO, 0); /* active-low: 0 = on */
#endif

    const spi_bus_config_t bus_cfg = {
        .mosi_io_num = DISP_SDA_GPIO,
        .miso_io_num = -1, /* display-only, no MISO needed */
        .sclk_io_num = DISP_SCL_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = DISP_H_RES * DISP_V_RES * 2, /* RGB565 */
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io;
    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = DISP_DC_GPIO,
        .cs_gpio_num = DISP_CS_GPIO,
        .pclk_hz = DISP_SPI_CLK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST,
                                             &io_cfg, &io));

    esp_lcd_panel_handle_t panel;
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = DISP_RST_GPIO,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &panel_cfg, &panel));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));

    /* This panel path already maps colors correctly without inversion. */
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, false));

    /* Set gap for the rotated state (swap_xy+mirror_y = 90° CW).
     * After swap_xy: CASET addresses physical rows (offset = row gap = 18),
     *                RASET addresses physical columns (offset = col gap = 82). */
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel, DISP_Y_GAP, DISP_X_GAP));

    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));

    /* Core 1: the spectrum pipeline draws under the port lock on core 1, and
     * core 0 belongs to the USB host library and the UAC task (priority 3).
     * A full-frame render+flush on core 0 preempts the isochronous IN
     * resubmission and costs audio packets; keep the two apart. */
    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_cfg.task_affinity = 1;
    lvgl_cfg.task_priority = 3;
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    /* A whole-frame draw buffer in PSRAM flushes the canvas as one SPI burst
     * instead of two 40-row chunks (at most one panel seam per frame). PSRAM
     * is DMA capable on the S3, but it is optional hardware: without it, fall
     * back to the partial internal buffer so the display still comes up. */
    const size_t frame_px = (size_t)DISP_H_RES * DISP_V_RES;
    const size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    const bool use_psram = psram_total >= frame_px * sizeof(uint16_t) + (256 * 1024);
    ESP_LOGI(TAG, "draw buffer: %s (%u px, psram %u KB)",
             use_psram ? "whole frame in PSRAM" : "partial in internal RAM",
             (unsigned)(use_psram ? frame_px : (size_t)DISP_V_RES * 40),
             (unsigned)(psram_total / 1024));

    lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io,
        .panel_handle = panel,
        .buffer_size = use_psram ? frame_px : (size_t)DISP_V_RES * 40,
        .double_buffer = false,
        .hres = DISP_H_RES, /* 76  — physical portrait width  */
        .vres = DISP_V_RES, /* 284 — physical portrait height */
        .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .rotation = {
            .swap_xy = false,
            .mirror_x = true,
            .mirror_y = true,
        },
        .flags = {
            .swap_bytes = true,
            .buff_dma = true,
            .buff_spiram = use_psram,
            .full_refresh = use_psram,
        },
    };
    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);
    if (disp == NULL)
    {
        ESP_LOGE(TAG, "lvgl_port_add_disp failed");
        return NULL;
    }

    /* Tell the port to rotate 90° CW.  With rotation.swap_xy=false in the
     * config the port computes ROTATION_90 as: swap_xy=true, mirror_y=true
     * (see lvgl_port_disp_rotation_update) — identical to the ESPHome config
     * that works.  LVGL's logical resolution becomes 284×76 (landscape). */
    if (lvgl_port_lock(0))
    {
        lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_90);
        lvgl_port_unlock();
    }

    return disp;
}
