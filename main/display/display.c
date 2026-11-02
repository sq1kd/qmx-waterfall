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

#include <stdio.h>
#include <string.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
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

#define DISP_STAGE_DETAIL_LEN 24
/* 2 rows x 3 columns of badges: 3*92 + 2*2 gaps + 2*2 margins = 284. */
#define DISP_STAGE_COLS 3
#define DISP_STAGE_BOX_W 92
#define DISP_STAGE_BOX_H 34
#define DISP_STAGE_GAP 2
#define DISP_STAGE_MARGIN 2

static lv_display_t *s_disp = NULL;
static lv_obj_t *s_title = NULL;
static lv_obj_t *s_detail = NULL;
static lv_obj_t *s_canvas = NULL;

/* Startup checklist: fixed set of rows updated in place. */
static lv_obj_t *s_progress = NULL;
static lv_obj_t *s_stage_box[DISP_STAGE_COUNT];
static lv_obj_t *s_stage_name[DISP_STAGE_COUNT];
static lv_obj_t *s_stage_marker[DISP_STAGE_COUNT];
static lv_obj_t *s_stage_detail[DISP_STAGE_COUNT];
static char s_stage_detail_buf[DISP_STAGE_COUNT][DISP_STAGE_DETAIL_LEN];
static disp_stage_state_t s_stage_state[DISP_STAGE_COUNT];
static volatile bool s_progress_visible = false;
static bool s_progress_finished = false;

static const char *const s_stage_label[DISP_STAGE_COUNT] = {
    "USB device",
    "Audio",
    "Control link",
    "Radio",
    "IQ mode",
    "Spectrum",
};

static void display_set_status_lines_locked(const char *title, const char *detail)
{
    const char *safe_title = (title && title[0]) ? title : DISP_MSG_DISCONNECTED;
    lv_label_set_text(s_title, safe_title);
    lv_obj_align(s_title, LV_ALIGN_CENTER, 0, -10);

    if (detail && detail[0])
    {
        lv_label_set_text(s_detail, detail);
        /* The checklist replaces the two-line view while it is up. */
        lv_obj_set_hidden(s_detail, s_progress_visible);
        lv_obj_align(s_detail, LV_ALIGN_CENTER, 0, 10);
    }
    else
    {
        lv_label_set_text(s_detail, "");
        lv_obj_set_hidden(s_detail, true);
    }
}

static void display_stage_apply_locked(disp_stage_t stage)
{
    if ((int)stage < 0 || stage >= (int)DISP_STAGE_COUNT || s_stage_box[stage] == NULL)
    {
        return;
    }

    const char *marker = ".";
    uint32_t border = 0x3A3F52; /* pending: dim */
    uint32_t bg = 0x101218;
    uint32_t fg = 0x6B7280;
    switch (s_stage_state[stage])
    {
    case DISP_STAGE_ACTIVE:
        marker = ">";
        border = 0x7AA2F7;
        bg = 0x1A2233;
        fg = 0xD7DEEA;
        break;
    case DISP_STAGE_DONE:
        marker = "+";
        border = 0x4ADE80;
        bg = 0x16261B;
        fg = 0x4ADE80;
        break;
    case DISP_STAGE_FAILED:
        marker = "!";
        border = 0xF87171;
        bg = 0x2A1717;
        fg = 0xF87171;
        break;
    default:
        break;
    }

    lv_obj_set_style_border_color(s_stage_box[stage], lv_color_hex(border), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_stage_box[stage], lv_color_hex(bg), LV_PART_MAIN);
    lv_obj_set_style_text_color(s_stage_name[stage], lv_color_hex(fg), LV_PART_MAIN);
    lv_obj_set_style_text_color(s_stage_marker[stage], lv_color_hex(fg), LV_PART_MAIN);
    lv_obj_set_style_text_color(s_stage_detail[stage], lv_color_hex(0x95A3B7), LV_PART_MAIN);
    lv_label_set_text(s_stage_marker[stage], marker);
    lv_label_set_text(s_stage_name[stage], s_stage_label[stage]);
    lv_label_set_text(s_stage_detail[stage], s_stage_detail_buf[stage]);
}

static void display_progress_create_locked(void)
{
    if (s_progress != NULL)
    {
        return;
    }
    lv_display_t *disp = lv_display_get_default();
    if (disp == NULL)
    {
        return;
    }
    lv_obj_t *scr = lv_display_get_screen_active(disp);
    if (scr == NULL)
    {
        return;
    }

    s_progress = lv_obj_create(scr);
    lv_obj_remove_style_all(s_progress);
    lv_obj_set_size(s_progress, DISP_LOGICAL_W, DISP_LOGICAL_H);
    lv_obj_set_pos(s_progress, 0, 0);
    lv_obj_set_style_bg_opa(s_progress, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_progress, 0, LV_PART_MAIN);

    for (int i = 0; i < (int)DISP_STAGE_COUNT; i++)
    {
        int col = i % DISP_STAGE_COLS;
        int row = i / DISP_STAGE_COLS;
        int x = DISP_STAGE_MARGIN + col * (DISP_STAGE_BOX_W + DISP_STAGE_GAP);
        int y = DISP_STAGE_MARGIN + row * (DISP_STAGE_BOX_H + DISP_STAGE_GAP);

        s_stage_detail_buf[i][0] = '\0';
        s_stage_state[i] = DISP_STAGE_PENDING;

        lv_obj_t *box = lv_obj_create(s_progress);
        lv_obj_remove_style_all(box);
        lv_obj_set_size(box, DISP_STAGE_BOX_W, DISP_STAGE_BOX_H);
        lv_obj_set_pos(box, x, y);
        lv_obj_set_style_radius(box, 3, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(box, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(box, 1, LV_PART_MAIN);
        lv_obj_set_style_pad_all(box, 0, LV_PART_MAIN);
        lv_obj_set_style_clip_corner(box, true, LV_PART_MAIN);
        s_stage_box[i] = box;

        s_stage_name[i] = lv_label_create(box);
        lv_obj_set_pos(s_stage_name[i], 4, 1);

        s_stage_marker[i] = lv_label_create(box);
        lv_obj_align(s_stage_marker[i], LV_ALIGN_TOP_RIGHT, -4, 1);

        s_stage_detail[i] = lv_label_create(box);
        lv_obj_set_pos(s_stage_detail[i], 4, 17);
        lv_obj_set_width(s_stage_detail[i], DISP_STAGE_BOX_W - 8);

#if defined(CONFIG_LV_FONT_MONTSERRAT_10)
        lv_obj_set_style_text_font(s_stage_name[i], &lv_font_montserrat_10, LV_PART_MAIN);
        lv_obj_set_style_text_font(s_stage_marker[i], &lv_font_montserrat_10, LV_PART_MAIN);
        lv_obj_set_style_text_font(s_stage_detail[i], &lv_font_montserrat_10, LV_PART_MAIN);
#endif
        display_stage_apply_locked((disp_stage_t)i);
    }
    lv_obj_set_hidden(s_progress, true);
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
    /* Core 1: the spectrum pipeline draws under the port lock on core 1, and
     * core 0 belongs to the USB host library and the UAC task (priority 3).
     * A full-frame render+flush on core 0 preempts the isochronous IN
     * resubmission and costs audio packets; keep the two apart. */
    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_cfg.task_affinity = 1;
    lvgl_cfg.task_priority = 3;
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    /* Physical portrait dimensions — rotation is applied by lvgl_port on every
     * flush via lv_display_set_rotation so the hardware is always consistent. */
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io,
        .panel_handle = panel,
        /* One whole frame per draw buffer, in PSRAM (S3 PSRAM is DMA
         * capable): the canvas is flushed in a single SPI burst instead of
         * two 40-row chunks, so the panel shows at most one seam per frame
         * instead of two with a render gap in between. */
        .buffer_size = DISP_H_RES * DISP_V_RES,
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
            .buff_spiram = true,
            .full_refresh = true,
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

static void display_canvas_destroy_locked(void)
{
    if (s_canvas)
    {
        lv_obj_del(s_canvas);
        s_canvas = NULL;
    }
}

bool display_spectrum_detach(void)
{
    if (!s_disp)
        return true;

    /* The pipeline is stopped by the caller, so the lock is normally free;
     * the timeout only guards against a wedged LVGL task, where handing the
     * caller a "still attached" answer beats deadlocking the teardown. */
    /* lvgl_port_lock() takes milliseconds (0 would mean "wait forever"). */
    if (!lvgl_port_lock(100))
        return false;

    display_canvas_destroy_locked();
    lvgl_port_unlock();
    return true;
}

lv_obj_t *display_show_spectrum(void)
{
    lv_obj_t *canvas = NULL;
    if (lvgl_port_lock(0))
    {
        lv_obj_t *scr = lv_display_get_screen_active(s_disp);

        /* A retried start must not stack canvases (the old one would keep a
         * pointer to a freed buffer). */
        display_canvas_destroy_locked();

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

        /* Keep the startup checklist above the canvas until the first
         * waterfall frame is on screen. */
        if (s_progress != NULL && s_progress_visible)
        {
            lv_obj_move_foreground(s_progress);
        }

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

        display_canvas_destroy_locked();

        if (s_progress != NULL)
        {
            lv_obj_set_hidden(s_progress, true);
        }
        s_progress_visible = false;
        s_progress_finished = false;

        lv_obj_set_hidden(s_title, false);
        lv_obj_set_hidden(s_detail, false);
        lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);
        display_set_status_lines_locked(msg ? msg : DISP_MSG_DISCONNECTED, "");

        lvgl_port_unlock();
    }
}

void display_show_progress(void)
{
    if (!lvgl_port_lock(100))
    {
        return;
    }

    display_progress_create_locked();
    if (s_progress != NULL && !s_progress_visible && !s_progress_finished)
    {
        for (int i = 0; i < (int)DISP_STAGE_COUNT; i++)
        {
            s_stage_detail_buf[i][0] = '\0';
            s_stage_state[i] = DISP_STAGE_PENDING;
            display_stage_apply_locked((disp_stage_t)i);
        }
        if (s_title != NULL)
        {
            lv_obj_set_hidden(s_title, true);
        }
        if (s_detail != NULL)
        {
            lv_obj_set_hidden(s_detail, true);
        }
        lv_obj_set_hidden(s_progress, false);
        lv_obj_move_foreground(s_progress);
        s_progress_visible = true;
    }

    lvgl_port_unlock();
}

void display_stage(disp_stage_t stage, disp_stage_state_t state, const char *detail)
{
    if ((int)stage < 0 || stage >= (int)DISP_STAGE_COUNT)
    {
        return;
    }
    if (!lvgl_port_lock(100))
    {
        return;
    }

    if (s_progress != NULL && s_progress_visible)
    {
        s_stage_state[stage] = state;
        if (detail != NULL)
        {
            strlcpy(s_stage_detail_buf[stage], detail, DISP_STAGE_DETAIL_LEN);
        }
        display_stage_apply_locked(stage);
    }

    lvgl_port_unlock();
}

bool display_progress_finish(void)
{
    if (!lvgl_port_lock(100))
    {
        return false;
    }

    if (s_progress != NULL)
    {
        if (s_stage_state[DISP_STAGE_SPECTRUM] != DISP_STAGE_FAILED)
        {
            s_stage_state[DISP_STAGE_SPECTRUM] = DISP_STAGE_DONE;
            strlcpy(s_stage_detail_buf[DISP_STAGE_SPECTRUM], "running", DISP_STAGE_DETAIL_LEN);
            display_stage_apply_locked(DISP_STAGE_SPECTRUM);
        }
        lv_obj_set_hidden(s_progress, true);
    }
    s_progress_visible = false;
    s_progress_finished = true;

    lvgl_port_unlock();
    return true;
}

bool display_progress_active(void)
{
    return s_progress_visible;
}
