/*
 * display.c — ST7789P3 (76×284) via SPI + LVGL status screen
 *
 * Pin assignment (SPI2 native IOMUX):
 *   SCL / CLK  → GPIO 12
 *   SDA / MOSI → GPIO 11
 *   CS         → GPIO 10
 *   DC         → GPIO  9
 *   RST        → GPIO  8
 *   BL         → GPIO  7   (on/off, active-low: 0 = backlight on)
 */

#include "display/display.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "lvgl.h"

#define DISP_SCL_GPIO 12
#define DISP_SDA_GPIO 11
#define DISP_CS_GPIO 10
#define DISP_DC_GPIO 9
#define DISP_RST_GPIO 8
#define DISP_BL_GPIO 7

/* Physical panel dimensions */
#define DISP_H_RES 76
#define DISP_V_RES 284

/*
 * ST7789 internal frame buffer is 240×320.
 * This 76×284 panel is typically mounted with the active window offset:
 *   x_gap = 82  (centres 76 px in the 240-wide frame)
 *   y_gap = 18  (shifts  284 px in the 320-tall  frame)
 *
 * If text/graphics appear shifted, adjust these values to match your
 * specific panel module.
 */
#define DISP_X_GAP 82
#define DISP_Y_GAP 18

/* SPI clock: 40 MHz is safe for most ST7789 modules at short wire lengths */
#define DISP_SPI_CLK_HZ (40 * 1000 * 1000)

static const char *TAG = "display";

static lv_display_t *s_disp = NULL;
static lv_obj_t *s_title = NULL;
static lv_obj_t *s_detail = NULL;
static lv_obj_t *s_canvas = NULL;

static void display_set_status_lines_locked(const char *title, const char *detail)
{
    const char *safe_title = (title && title[0]) ? title : DISP_MSG_DISCONNECTED;
    lv_label_set_text(s_title, safe_title);
    lv_obj_align(s_title, LV_ALIGN_CENTER, 0, -10);

    if (detail && detail[0])
    {
        lv_label_set_text(s_detail, detail);
        lv_obj_set_hidden(s_detail, false);
        lv_obj_align(s_detail, LV_ALIGN_CENTER, 0, 10);
    }
    else
    {
        lv_label_set_text(s_detail, "");
        lv_obj_set_hidden(s_detail, true);
    }
}

void display_init(void)
{
    /* ------------------------------------------------------------------ */
    /* 1. Backlight — drive low to turn backlight on (active-low)         */
    /* ------------------------------------------------------------------ */
    const gpio_config_t bl_cfg = {
        .pin_bit_mask = BIT64(DISP_BL_GPIO),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&bl_cfg));
    gpio_set_level(DISP_BL_GPIO, 0);

    /* ------------------------------------------------------------------ */
    /* 2. SPI bus                                                          */
    /* ------------------------------------------------------------------ */
    const spi_bus_config_t bus_cfg = {
        .mosi_io_num = DISP_SDA_GPIO,
        .miso_io_num = -1, /* display-only, no MISO needed */
        .sclk_io_num = DISP_SCL_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = DISP_H_RES * DISP_V_RES * 2, /* RGB565 */
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    /* ------------------------------------------------------------------ */
    /* 3. LCD panel IO (SPI → esp_lcd abstraction)                        */
    /* ------------------------------------------------------------------ */
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

    /* ------------------------------------------------------------------ */
    /* 4. ST7789 panel driver                                              */
    /* ------------------------------------------------------------------ */
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

    /* ------------------------------------------------------------------ */
    /* 5. LVGL port init                                                   */
    /* ------------------------------------------------------------------ */
    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    /* Physical portrait dimensions — rotation is applied by lvgl_port on every
     * flush via lv_display_set_rotation so the hardware is always consistent. */
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io,
        .panel_handle = panel,
        .buffer_size = DISP_V_RES * 40,
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
        },
    };
    s_disp = lvgl_port_add_disp(&disp_cfg);
    if (s_disp == NULL)
    {
        ESP_LOGE(TAG, "lvgl_port_add_disp failed");
        return;
    }

    /* Tell the port to rotate 90° CW.  With rotation.swap_xy=false in the
     * config the port computes ROTATION_90 as: swap_xy=true, mirror_y=true
     * (see lvgl_port_disp_rotation_update) — identical to the ESPHome config
     * that works.  LVGL's logical resolution becomes 284×76 (landscape). */
    if (lvgl_port_lock(0))
    {
        lv_display_set_rotation(s_disp, LV_DISPLAY_ROTATION_90);
        lvgl_port_unlock();
    }

    /* ------------------------------------------------------------------ */
    /* 6. Initial UI: black screen with centered status label             */
    /* ------------------------------------------------------------------ */
    if (lvgl_port_lock(0))
    {
        lv_obj_t *scr = lv_display_get_screen_active(s_disp);
        lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

        s_title = lv_label_create(scr);
        lv_label_set_long_mode(s_title, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(s_title, DISP_V_RES - 8);
        lv_obj_set_style_text_color(s_title, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_text_align(s_title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_align(s_title, LV_ALIGN_CENTER, 0, -10);
        lv_label_set_text(s_title, DISP_MSG_CONNECT_USB);

        s_detail = lv_label_create(scr);
        lv_label_set_long_mode(s_detail, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(s_detail, DISP_V_RES - 8);
        lv_obj_set_style_text_color(s_detail, lv_color_hex(0x95A3B7), LV_PART_MAIN);
        lv_obj_set_style_text_align(s_detail, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_align(s_detail, LV_ALIGN_CENTER, 0, 10);
        lv_label_set_text(s_detail, "Plug in USB audio + CAT");

        lvgl_port_unlock();
    }

    ESP_LOGI(TAG, "Display initialised (%d×%d)", DISP_H_RES, DISP_V_RES);
}

void display_set_status(const char *msg)
{
    display_set_status_lines(msg, NULL);
}

void display_set_status_lines(const char *title, const char *detail)
{
    if (s_title == NULL || s_detail == NULL)
        return;

    if (lvgl_port_lock(0))
    {
        display_set_status_lines_locked(title, detail);

        lvgl_port_unlock();
    }
}

lv_obj_t *display_show_spectrum(void)
{
    lv_obj_t *canvas = NULL;
    if (lvgl_port_lock(0))
    {
        lv_obj_t *scr = lv_display_get_screen_active(s_disp);

        /* Hide the status label */
        if (s_title != NULL)
        {
            lv_obj_set_hidden(s_title, true);
        }
        if (s_detail != NULL)
        {
            lv_obj_set_hidden(s_detail, true);
        }

        /* Black background */
        lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);

        /* Full-screen canvas — buffer is attached by spectrum_init() */
        canvas = lv_canvas_create(scr);
        lv_obj_set_size(canvas, DISP_LOGICAL_W, DISP_LOGICAL_H);
        lv_obj_align(canvas, LV_ALIGN_TOP_LEFT, 0, 0);
        s_canvas = canvas;

        lvgl_port_unlock();
    }
    return canvas;
}

void display_show_status(const char *msg)
{
    if (!s_disp || !s_title || !s_detail)
        return;

    if (lvgl_port_lock(0))
    {
        lv_obj_t *scr = lv_display_get_screen_active(s_disp);

        if (s_canvas)
        {
            lv_obj_del(s_canvas);
            s_canvas = NULL;
        }

        lv_obj_set_hidden(s_title, false);
        lv_obj_set_hidden(s_detail, false);
        lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);
        display_set_status_lines_locked(msg ? msg : DISP_MSG_DISCONNECTED, "");

        lvgl_port_unlock();
    }
}
