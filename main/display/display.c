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
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "lvgl.h"
#include "ui/fonts.h"
#include "ui/i18n.h"

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

/* Instrument palette: graphite surfaces with one warm accent. States stay
 * muted so the status view does not compete with the waterfall colours. */
#define UI_SURFACE 0x101418
#define UI_BORDER 0x2B333C
#define UI_TEXT 0xE4E7EB
#define UI_MUTED 0x8A94A0
#define UI_ACCENT 0xC8965A
#define UI_SUCCESS 0x6FA97C
#define UI_ERROR 0xB4544F

/* Status layout on the 284×76 logical screen. A single 4 px rhythm is used
 * for the screen edge, the gap under the header rule and the rail bottom. */
#define SCREEN_PAD 4
#define TITLE_LINE_H 17 /* lv_font_qmx_12 line height */
#define SMALL_LINE_H 13 /* lv_font_qmx_10 line height */

#define HDR_X SCREEN_PAD
#define HDR_Y 0
#define RULE_Y 17
#define RULE_H 1
#define HEADER_GAP 4
#define BODY_Y (RULE_Y + RULE_H + HEADER_GAP)
#define ICON_X 5
#define ICON_Y (BODY_Y + 1)
#define ICON_SIZE 22
#define STATUS_X 33
#define TITLE_Y BODY_Y
#define DETAIL_Y (BODY_Y + 15)
#define RAIL_H 21
#define RAIL_Y (DISP_LOGICAL_H - RAIL_H - SCREEN_PAD)
#define CHIP_W 66
#define CHIP_H 17
#define CHIP_GAP 3
#define CHIP_DOT 5
#define CHIP_DOT_X 6
#define CHIP_LABEL_X 12
#define CHIP_LABEL_W (CHIP_W - CHIP_LABEL_X - 3)
/* Scroll far enough that the active chip and the next two are already on
 * screen. Three chips (204 px) fit the 284 px rail, so the last stage is
 * visible well before it becomes active. */
#define RAIL_LOOKAHEAD 2

_Static_assert(RULE_Y + RULE_H <= BODY_Y, "header rule overlaps the status body");
_Static_assert(TITLE_Y + TITLE_LINE_H <= RAIL_Y, "title overlaps the stage rail");
_Static_assert(DETAIL_Y + SMALL_LINE_H <= RAIL_Y, "detail overlaps the stage rail");
_Static_assert(ICON_Y + ICON_SIZE <= RAIL_Y, "status icon overlaps the stage rail");
_Static_assert(RAIL_Y + RAIL_H <= DISP_LOGICAL_H, "stage rail leaves the screen");
_Static_assert(CHIP_H + 4 <= RAIL_H, "stage chip does not fit the rail");

#define DISP_STAGE_DETAIL_LEN 24

typedef enum
{
    STATUS_HIDDEN = 0,
    STATUS_BOOT,
    STATUS_PROGRESS,
    STATUS_DISCONNECTED,
} status_mode_t;

typedef enum
{
    ICON_WAIT = 0,
    ICON_CONNECT,
    ICON_ERROR,
} icon_state_t;

static lv_display_t *s_disp = NULL;
static lv_obj_t *s_brand = NULL;
static lv_obj_t *s_credit = NULL;
static lv_obj_t *s_rule = NULL;
static lv_obj_t *s_icon = NULL;
static lv_obj_t *s_icon_symbol = NULL;
static lv_obj_t *s_icon_badge = NULL;
static lv_obj_t *s_title = NULL;
static lv_obj_t *s_detail = NULL;
static lv_obj_t *s_canvas = NULL;

/* One horizontal, auto-scrolling rail of stage chips. */
static lv_obj_t *s_rail = NULL;
static lv_obj_t *s_chip[DISP_STAGE_COUNT];
static lv_obj_t *s_chip_dot[DISP_STAGE_COUNT];
static lv_obj_t *s_chip_label[DISP_STAGE_COUNT];
static char s_stage_detail_buf[DISP_STAGE_COUNT][DISP_STAGE_DETAIL_LEN];
static disp_stage_state_t s_stage_state[DISP_STAGE_COUNT];
static status_mode_t s_status_mode = STATUS_HIDDEN;
static volatile bool s_progress_visible = false;
static bool s_progress_finished = false;

static void display_canvas_destroy_locked(void);

static void display_stage_apply_locked(disp_stage_t stage);
static void chip_apply_all_locked(void);

static void status_set_visible_locked(bool visible)
{
    if (s_brand)
        lv_obj_set_hidden(s_brand, !visible);
    if (s_credit)
        lv_obj_set_hidden(s_credit, !visible);
    if (s_rule)
        lv_obj_set_hidden(s_rule, !visible);
    if (s_icon)
        lv_obj_set_hidden(s_icon, !visible);
    if (s_title)
        lv_obj_set_hidden(s_title, !visible);
    if (s_detail)
        lv_obj_set_hidden(s_detail, !visible);
}

static void icon_set_state_locked(icon_state_t state)
{
    if (!s_icon || !s_icon_symbol || !s_icon_badge)
        return;

    uint32_t color = UI_MUTED;
    if (state == ICON_CONNECT)
        color = UI_ACCENT;
    else if (state == ICON_ERROR)
        color = UI_ERROR;

    lv_obj_set_style_border_color(s_icon, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_text_color(s_icon_symbol, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_hidden(s_icon_badge, state != ICON_ERROR);
}

static void chip_apply_locked(disp_stage_t stage)
{
    if ((int)stage < 0 || stage >= (int)DISP_STAGE_COUNT || s_chip[stage] == NULL)
        return;

    uint32_t border = 0x242C35;
    uint32_t bg = 0x0C1015;
    uint32_t fg = 0x6D7784;
    uint32_t dot = 0x3A434E;

    switch (s_stage_state[stage])
    {
    case DISP_STAGE_ACTIVE:
        border = UI_ACCENT;
        bg = 0x1B1813;
        fg = UI_TEXT;
        dot = UI_ACCENT;
        break;
    case DISP_STAGE_DONE:
        border = 0x35513F;
        bg = 0x101A14;
        fg = 0x9DB8A5;
        dot = UI_SUCCESS;
        break;
    case DISP_STAGE_FAILED:
        border = 0x5A2F2D;
        bg = 0x1C1212;
        fg = 0xD08B86;
        dot = UI_ERROR;
        break;
    default:
        break;
    }

    lv_obj_set_style_border_color(s_chip[stage], lv_color_hex(border), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_chip[stage], lv_color_hex(bg), LV_PART_MAIN);
    lv_obj_set_style_text_color(s_chip_label[stage], lv_color_hex(fg), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_chip_dot[stage], lv_color_hex(dot), LV_PART_MAIN);
}

static int stage_active_latest(void)
{
    for (int i = (int)DISP_STAGE_COUNT - 1; i >= 0; i--)
    {
        if (s_stage_state[i] == DISP_STAGE_ACTIVE)
            return i;
    }
    return -1;
}

static int stage_failed_first(void)
{
    for (int i = 0; i < (int)DISP_STAGE_COUNT; i++)
    {
        if (s_stage_state[i] == DISP_STAGE_FAILED)
            return i;
    }
    return -1;
}

static ui_text_t stage_text_key(disp_stage_t stage)
{
    switch (stage)
    {
    case DISP_STAGE_USB:
        return TXT_STAGE_USB;
    case DISP_STAGE_AUDIO:
        return TXT_STAGE_AUDIO;
    case DISP_STAGE_CAT:
        return TXT_STAGE_CONTROL;
    case DISP_STAGE_RADIO:
        return TXT_STAGE_RADIO;
    case DISP_STAGE_IQ:
        return TXT_STAGE_IQ;
    default:
        return TXT_STAGE_SPECTRUM;
    }
}

/* "<stage> · <technical detail>" for the status line. */
static void compose_stage_detail(int stage, char *buf, size_t n)
{
    if (stage < 0 || stage >= (int)DISP_STAGE_COUNT)
    {
        (void)snprintf(buf, n, "%s", ui_tr(TXT_CONNECT_DETAIL));
        return;
    }

    const char *stage_name = ui_tr(stage_text_key((disp_stage_t)stage));
    const char *detail = s_stage_detail_buf[stage];
    if (detail[0] != '\0')
        (void)snprintf(buf, n, "%s · %s", stage_name, detail);
    else
        (void)snprintf(buf, n, "%s", stage_name);
}

static void refresh_status_text_locked(void)
{
    if (!s_title || !s_detail || s_status_mode == STATUS_HIDDEN)
        return;

    char detail[64];

    switch (s_status_mode)
    {
    case STATUS_BOOT:
        lv_label_set_text(s_title, ui_tr(TXT_WAITING));
        lv_label_set_text(s_detail, ui_tr(TXT_PLUG_IN));
        icon_set_state_locked(ICON_WAIT);
        break;

    case STATUS_PROGRESS:
    {
        const int failed = stage_failed_first();
        if (failed >= 0)
        {
            lv_label_set_text(s_title, ui_tr(TXT_PROBLEM));
            compose_stage_detail(failed, detail, sizeof(detail));
            icon_set_state_locked(ICON_ERROR);
        }
        else
        {
            lv_label_set_text(s_title, ui_tr(TXT_CONNECTING));
            compose_stage_detail(stage_active_latest(), detail, sizeof(detail));
            icon_set_state_locked(ICON_CONNECT);
        }
        lv_label_set_text(s_detail, detail);
        break;
    }

    case STATUS_DISCONNECTED:
        lv_label_set_text(s_title, ui_tr(TXT_DISCONNECTED));
        lv_label_set_text(s_detail, ui_tr(TXT_RECONNECT));
        icon_set_state_locked(ICON_ERROR);
        break;

    default:
        break;
    }
}

static void refresh_chip_labels_locked(void)
{
    for (int i = 0; i < (int)DISP_STAGE_COUNT; i++)
    {
        if (s_chip_label[i])
            lv_label_set_text(s_chip_label[i], ui_tr(stage_text_key((disp_stage_t)i)));
    }
}

static void rail_reset_locked(void)
{
    for (int i = 0; i < (int)DISP_STAGE_COUNT; i++)
    {
        s_stage_detail_buf[i][0] = '\0';
        s_stage_state[i] = DISP_STAGE_PENDING;
    }
    chip_apply_all_locked();
    if (s_rail)
    {
        lv_obj_set_style_opa(s_rail, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_scroll_to_x(s_rail, 0, LV_ANIM_OFF);
    }
}

/* Keep the active chip and the two after it on screen. The last stages arrive
 * in a burst, so reveal them while an earlier stage is still active instead of
 * chasing each chip as it flips. */
static void rail_reveal_stage_locked(int stage)
{
    if (!s_rail || stage < 0 || stage >= (int)DISP_STAGE_COUNT)
        return;

    int last = stage + RAIL_LOOKAHEAD;
    if (last >= (int)DISP_STAGE_COUNT)
        last = (int)DISP_STAGE_COUNT - 1;

    lv_obj_update_layout(s_rail);

    lv_area_t first_a;
    lv_area_t last_a;
    lv_area_t rail_a;
    lv_obj_get_coords(s_chip[stage], &first_a);
    lv_obj_get_coords(s_chip[last], &last_a);
    lv_obj_get_coords(s_rail, &rail_a);

    const int32_t margin = 4;
    int32_t scroll_x = lv_obj_get_scroll_x(s_rail);

    if (last_a.x2 > rail_a.x2 - margin)
        scroll_x += last_a.x2 - (rail_a.x2 - margin);
    if (first_a.x1 < rail_a.x1 + margin)
        scroll_x -= (rail_a.x1 + margin) - first_a.x1;

    lv_obj_scroll_to_x(s_rail, scroll_x, LV_ANIM_ON);
}

static void chip_apply_all_locked(void)
{
    for (int i = 0; i < (int)DISP_STAGE_COUNT; i++)
        chip_apply_locked((disp_stage_t)i);
}

static void display_stage_apply_locked(disp_stage_t stage)
{
    chip_apply_locked(stage);
    refresh_status_text_locked();
}

static void status_create_locked(lv_obj_t *scr)
{
    s_brand = lv_label_create(scr);
    lv_label_set_text(s_brand, ui_tr(TXT_BRAND));
    lv_obj_set_style_text_color(s_brand, lv_color_hex(UI_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_brand, &lv_font_qmx_12, LV_PART_MAIN);
    lv_obj_set_pos(s_brand, HDR_X, HDR_Y);

    s_credit = lv_label_create(scr);
    lv_label_set_text(s_credit, ui_tr(TXT_CREDIT));
    lv_obj_set_style_text_color(s_credit, lv_color_hex(UI_MUTED), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_credit, &lv_font_qmx_10, LV_PART_MAIN);
    lv_obj_align(s_credit, LV_ALIGN_TOP_RIGHT, -SCREEN_PAD, 2);

    s_rule = lv_obj_create(scr);
    lv_obj_remove_style_all(s_rule);
    lv_obj_set_size(s_rule, DISP_LOGICAL_W - (2 * SCREEN_PAD), RULE_H);
    lv_obj_set_pos(s_rule, SCREEN_PAD, RULE_Y);
    lv_obj_set_style_bg_color(s_rule, lv_color_hex(UI_BORDER), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_rule, LV_OPA_COVER, LV_PART_MAIN);

    s_icon = lv_obj_create(scr);
    lv_obj_remove_style_all(s_icon);
    lv_obj_set_size(s_icon, ICON_SIZE, ICON_SIZE);
    lv_obj_set_pos(s_icon, ICON_X, ICON_Y);
    lv_obj_set_style_radius(s_icon, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_icon, lv_color_hex(UI_SURFACE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_icon, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_icon, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_icon, 0, LV_PART_MAIN);

    s_icon_symbol = lv_label_create(s_icon);
    lv_label_set_text(s_icon_symbol, LV_SYMBOL_USB);
    lv_obj_set_style_text_font(s_icon_symbol, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_center(s_icon_symbol);

    s_icon_badge = lv_obj_create(s_icon);
    lv_obj_remove_style_all(s_icon_badge);
    lv_obj_set_size(s_icon_badge, 8, 8);
    lv_obj_align(s_icon_badge, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_radius(s_icon_badge, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_icon_badge, lv_color_hex(UI_ERROR), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_icon_badge, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_icon_badge, 0, LV_PART_MAIN);

    s_title = lv_label_create(scr);
    lv_obj_set_pos(s_title, STATUS_X, TITLE_Y);
    lv_obj_set_width(s_title, DISP_LOGICAL_W - STATUS_X - 4);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(s_title, lv_color_hex(UI_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_title, &lv_font_qmx_12, LV_PART_MAIN);

    s_detail = lv_label_create(scr);
    lv_obj_set_pos(s_detail, STATUS_X, DETAIL_Y);
    lv_obj_set_width(s_detail, DISP_LOGICAL_W - STATUS_X - 4);
    lv_label_set_long_mode(s_detail, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(s_detail, lv_color_hex(UI_MUTED), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_detail, &lv_font_qmx_10, LV_PART_MAIN);
}

static void rail_create_locked(lv_obj_t *scr)
{
    s_rail = lv_obj_create(scr);
    lv_obj_remove_style_all(s_rail);
    lv_obj_set_size(s_rail, DISP_LOGICAL_W, RAIL_H);
    lv_obj_set_pos(s_rail, 0, RAIL_Y);
    lv_obj_set_style_bg_opa(s_rail, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_left(s_rail, SCREEN_PAD, LV_PART_MAIN);
    lv_obj_set_style_pad_right(s_rail, SCREEN_PAD, LV_PART_MAIN);
    lv_obj_set_style_pad_top(s_rail, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(s_rail, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_column(s_rail, CHIP_GAP, LV_PART_MAIN);
    lv_obj_set_scroll_dir(s_rail, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(s_rail, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(s_rail, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_rail, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    for (int i = 0; i < (int)DISP_STAGE_COUNT; i++)
    {
        lv_obj_t *chip = lv_obj_create(s_rail);
        lv_obj_remove_style_all(chip);
        lv_obj_set_size(chip, CHIP_W, CHIP_H);
        lv_obj_set_style_radius(chip, 3, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(chip, 1, LV_PART_MAIN);
        lv_obj_set_style_pad_all(chip, 0, LV_PART_MAIN);
        s_chip[i] = chip;

        s_chip_dot[i] = lv_obj_create(chip);
        lv_obj_remove_style_all(s_chip_dot[i]);
        lv_obj_set_size(s_chip_dot[i], CHIP_DOT, CHIP_DOT);
        lv_obj_set_pos(s_chip_dot[i], CHIP_DOT_X, (CHIP_H - CHIP_DOT) / 2);
        lv_obj_set_style_radius(s_chip_dot[i], LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_chip_dot[i], LV_OPA_COVER, LV_PART_MAIN);

        s_chip_label[i] = lv_label_create(chip);
        lv_obj_set_pos(s_chip_label[i], CHIP_LABEL_X, 1);
        lv_obj_set_width(s_chip_label[i], CHIP_LABEL_W);
        lv_label_set_long_mode(s_chip_label[i], LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_font(s_chip_label[i], &lv_font_qmx_10, LV_PART_MAIN);
        lv_obj_set_style_text_color(s_chip_label[i], lv_color_hex(0x6D7784), LV_PART_MAIN);
    }

    lv_obj_set_style_opa(s_rail, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_hidden(s_rail, true);
    rail_reset_locked();
}

static void display_init_panel(void)
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
}

void display_init(void)
{
    display_init_panel();
    if (s_disp == NULL)
        return;

    if (lvgl_port_lock(0))
    {
        lv_obj_t *scr = lv_display_get_screen_active(s_disp);
        lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

        status_create_locked(scr);
        rail_create_locked(scr);

        s_status_mode = STATUS_BOOT;
        status_set_visible_locked(true);
        /* Keep the checklist on the waiting screen too: the first class
         * driver callbacks arrive after enumeration, and without this the
         * rail would pop in with several stages already done. */
        lv_obj_set_hidden(s_rail, false);
        refresh_chip_labels_locked();
        refresh_status_text_locked();

        lvgl_port_unlock();
    }

    ESP_LOGI(TAG, "Display initialised (%d×%d)", DISP_H_RES, DISP_V_RES);
}

void display_language_changed(void)
{
    if (!s_disp || !s_brand || !s_credit)
        return;

    if (!lvgl_port_lock(100))
        return;

    lv_label_set_text(s_brand, ui_tr(TXT_BRAND));
    lv_label_set_text(s_credit, ui_tr(TXT_CREDIT));
    refresh_chip_labels_locked();
    refresh_status_text_locked();

    lvgl_port_unlock();
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

    /* A disconnect can be reported while the canvas is still on screen; now
     * that it is gone, uncover the disconnected view and its reset rail. */
    if (s_status_mode == STATUS_DISCONNECTED)
    {
        status_set_visible_locked(true);
        if (s_rail != NULL)
        {
            lv_obj_set_hidden(s_rail, false);
            lv_obj_move_foreground(s_rail);
        }
    }

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

        status_set_visible_locked(false);
        s_status_mode = STATUS_HIDDEN;

        /* Black background */
        lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);

        /* Full-screen canvas — buffer is attached by spectrum_init() */
        canvas = lv_canvas_create(scr);
        lv_obj_set_size(canvas, DISP_LOGICAL_W, DISP_LOGICAL_H);
        lv_obj_align(canvas, LV_ALIGN_TOP_LEFT, 0, 0);
        s_canvas = canvas;

        /* Keep the startup rail above the canvas until the first waterfall
         * frame is on screen. If progress is no longer active (a stale
         * disconnect/finish raced the new connection), hide whatever rail is
         * left; otherwise it stays over the canvas forever because nothing
         * calls display_progress_finish() anymore. */
        if (s_rail != NULL)
        {
            if (s_progress_visible)
            {
                lv_obj_set_hidden(s_rail, false);
                lv_obj_move_foreground(s_rail);
            }
            else
            {
                lv_obj_set_hidden(s_rail, true);
            }
        }

        lvgl_port_unlock();
    }
    return canvas;
}

void display_show_disconnected(void)
{
    if (!s_disp || !s_title || !s_detail || !s_rail)
        return;

    if (lvgl_port_lock(100))
    {
        /* Do not delete the canvas here: the CDC disconnect path calls this
         * while the spectrum task may still be rendering. display_spectrum_detach()
         * deletes it once the pipeline is stopped, which uncovers this view. */
        s_progress_visible = false;
        s_progress_finished = false;
        s_status_mode = STATUS_DISCONNECTED;

        status_set_visible_locked(true);
        refresh_status_text_locked();

        /* A finished connection leaves no residual stage state. Show the
         * pending rail only when no canvas is still on screen; a late
         * disconnect that races a new spectrum start must not leave chips
         * floating above the running waterfall. */
        rail_reset_locked();
        lv_obj_set_hidden(s_rail, s_canvas != NULL);

        lvgl_port_unlock();
    }
}

void display_show_progress(void)
{
    if (s_progress_finished)
        return;
    if (!s_disp || !s_title || !s_detail || !s_rail)
        return;

    if (!lvgl_port_lock(100))
        return;

    if (!s_progress_visible)
        rail_reset_locked();

    s_status_mode = STATUS_PROGRESS;
    s_progress_visible = true;

    status_set_visible_locked(true);
    lv_obj_set_hidden(s_rail, false);
    lv_obj_move_foreground(s_rail);
    refresh_chip_labels_locked();
    refresh_status_text_locked();

    lvgl_port_unlock();
}

void display_stage(disp_stage_t stage, disp_stage_state_t state, const char *detail)
{
    if ((int)stage < 0 || stage >= (int)DISP_STAGE_COUNT)
        return;
    if (!lvgl_port_lock(100))
        return;

    if (s_rail != NULL && s_progress_visible)
    {
        s_stage_state[stage] = state;
        if (detail != NULL)
        {
            strlcpy(s_stage_detail_buf[stage], detail, DISP_STAGE_DETAIL_LEN);
        }
        display_stage_apply_locked(stage);

        if (state == DISP_STAGE_ACTIVE || state == DISP_STAGE_FAILED)
        {
            rail_reveal_stage_locked((int)stage);
        }
    }

    lvgl_port_unlock();
}

bool display_progress_finish(void)
{
    if (!lvgl_port_lock(100))
    {
        return false;
    }

    /* A disconnect can win the lock while the first frame is in flight.
     * Finishing then would hide the disconnected view and leave the screen
     * blank. STATUS_HIDDEN is the normal state in the spectrum view (the rail
     * is still above the canvas until this call), so only skip disconnects. */
    if (s_status_mode == STATUS_DISCONNECTED)
    {
        /* The disconnect view owns the screen; never leave a startup rail
         * floating over it or over a canvas started by the next connection. */
        if (s_rail != NULL)
            lv_obj_set_hidden(s_rail, true);
        s_progress_visible = false;
        lvgl_port_unlock();
        return true;
    }

    if (s_rail != NULL)
    {
        if (s_stage_state[DISP_STAGE_SPECTRUM] != DISP_STAGE_FAILED)
        {
            s_stage_state[DISP_STAGE_SPECTRUM] = DISP_STAGE_DONE;
            strlcpy(s_stage_detail_buf[DISP_STAGE_SPECTRUM], ui_tr(TXT_DETAIL_RUNNING),
                    DISP_STAGE_DETAIL_LEN);
            display_stage_apply_locked(DISP_STAGE_SPECTRUM);
        }
        lv_obj_set_hidden(s_rail, true);
    }
    status_set_visible_locked(false);
    s_status_mode = STATUS_HIDDEN;
    s_progress_visible = false;
    s_progress_finished = true;

    lvgl_port_unlock();
    return true;
}

bool display_progress_active(void)
{
    return s_progress_visible;
}
