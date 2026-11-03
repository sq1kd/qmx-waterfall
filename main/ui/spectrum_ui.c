#include "ui/spectrum_ui.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>

#include "sdkconfig.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "app/settings.h"
#include "spectrum/spectrum.h"
#include "ui/fonts.h"
#include "ui/geometry.h"
#include "ui/i18n.h"
#include "ui/theme.h"
#include "lvgl.h"

#define BTN_UP_GPIO 4
#define BTN_OK_GPIO 5
#define BTN_DOWN_GPIO 6

#define BTN_POLL_MS 20
#define BTN_DEBOUNCE_MS 40
#define BTN_LONG_MS 700
#define BTN_REPEAT_START_MS 400
#define BTN_REPEAT_MS 120

static const char *TAG = "spectrum_ui";

/* Menu order is deliberate: the two knobs people touch while operating sit
 * first, then the visual preferences, then the rarely changed CW pitch. */
typedef struct
{
    int stable_level;
    int sampled_level;
    int debounce_ticks;
    int held_ticks;
    int repeat_ticks;
    bool long_sent;
} btn_state_t;

static TaskHandle_t s_ui_task;
static volatile bool s_ui_enabled;
static bool s_menu_visible;
static app_field_id_t s_active_setting;

static lv_obj_t *s_panel;
static lv_obj_t *s_title;
static lv_obj_t *s_value;
static lv_obj_t *s_bar;

static inline int clamp_i(int v, int lo, int hi)
{
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

static const char *ui_palette_name(uint8_t palette)
{
    switch (palette)
    {
    case 0:
        return "Turbo";
    case 1:
        return "Gray";
    case 2:
        return "Inferno";
    case 3:
        return "Viridis";
    case 4:
        return "Magma";
    case 5:
        return "Plasma";
    case 6:
        return "Blue Hot";
    default:
        return "?";
    }
}

static int ui_setting_to_slider(const app_settings_t *cfg, app_field_id_t id)
{
    return (int)lrintf(app_settings_field_get(cfg, id));
}

static const char *ui_setting_name(app_field_id_t id)
{
    switch (id)
    {
    case APP_FIELD_LEVEL_OFF:
        return ui_tr(TXT_SET_LEVEL);
    case APP_FIELD_VIEW_MODE:
        return ui_tr(TXT_SET_SPAN);
    case APP_FIELD_WF_FLOOR:
        return ui_tr(TXT_SET_FLOOR);
    case APP_FIELD_WF_CEIL:
        return ui_tr(TXT_SET_CEIL);
    case APP_FIELD_SPEED:
        return ui_tr(TXT_SET_SPEED);
    case APP_FIELD_PALETTE:
        return ui_tr(TXT_SET_PALETTE);
    case APP_FIELD_SPEC_MODE:
        return ui_tr(TXT_SET_DISPLAY);
    case APP_FIELD_CW_PITCH:
        return ui_tr(TXT_SET_CW_PITCH);
    case APP_FIELD_LANGUAGE:
        return ui_tr(TXT_SET_LANGUAGE);
    default:
        return "-";
    }
}

static void ui_setting_range(app_field_id_t id, int *min, int *max)
{
    /* Clamping a value far outside the range yields the bound itself, so the
     * dynamic speed limit stays in one place (settings.c). */
    *min = (int)lrintf(app_settings_field_clamp(id, -1.0e9f));
    *max = (int)lrintf(app_settings_field_clamp(id, 1.0e9f));
}

static void ui_setting_format_value(const app_settings_t *cfg, app_field_id_t id, char *buf, size_t n)
{
    switch (id)
    {
    case APP_FIELD_LEVEL_OFF:
        snprintf(buf, n, "%.1f dB", app_settings_field_get(cfg, id));
        break;
    case APP_FIELD_VIEW_MODE:
        snprintf(buf, n, "%s", cfg->view_mode == 0 ? ui_tr(TXT_VAL_FULL)
                                                   : ui_tr(TXT_VAL_IF_CENTRED));
        break;
    case APP_FIELD_WF_FLOOR:
        snprintf(buf, n, "%.0f dB", app_settings_field_get(cfg, id));
        break;
    case APP_FIELD_WF_CEIL:
        snprintf(buf, n, "%.0f dB", app_settings_field_get(cfg, id));
        break;
    case APP_FIELD_SPEED:
        snprintf(buf, n, "%u/%u", (unsigned)cfg->speed_level,
                 (unsigned)spectrum_speed_level_count());
        break;
    case APP_FIELD_PALETTE:
        snprintf(buf, n, "%s", ui_palette_name(cfg->wf_palette));
        break;
    case APP_FIELD_SPEC_MODE:
        switch (cfg->spec_mode)
        {
        case 0:
            snprintf(buf, n, "%s", ui_tr(TXT_VAL_TRACE));
            break;
        case 1:
            snprintf(buf, n, "%s", ui_tr(TXT_VAL_TRACE_PEAK));
            break;
        case 2:
            snprintf(buf, n, "%s", ui_tr(TXT_VAL_PEAK));
            break;
        default:
            snprintf(buf, n, "%s", ui_tr(TXT_VAL_OFF));
            break;
        }
        break;
    case APP_FIELD_CW_PITCH:
        snprintf(buf, n, "%u Hz", (unsigned)cfg->cw_pitch_hz);
        break;
    case APP_FIELD_LANGUAGE:
        snprintf(buf, n, "%s", ui_tr(cfg->language == UI_LANG_PL ? TXT_LANG_PL : TXT_LANG_EN));
        break;
    default:
        snprintf(buf, n, "-");
        break;
    }
}

static void ui_apply_delta(app_field_id_t id, int dir)
{
    app_settings_t cfg;
    const app_field_spec_t *spec = app_settings_field_spec(id);

    if (!spec)
        return;

    app_settings_get(&cfg);
    (void)app_settings_field_set(id, app_settings_field_get(&cfg, id) + spec->step * (float)dir);
}

static void ui_overlay_destroy_locked(void)
{
    if (s_panel)
    {
        lv_obj_del(s_panel);
        s_panel = NULL;
        s_title = NULL;
        s_value = NULL;
        s_bar = NULL;
    }
}

static void ui_overlay_create_locked(void)
{
    lv_display_t *disp = lv_display_get_default();
    if (!disp)
        return;

    lv_obj_t *scr = lv_display_get_screen_active(disp);
    if (!scr)
        return;

    ui_overlay_destroy_locked();

    /* A floating card, not a strip glued to the panel edge: 4 px of waterfall
     * stay visible below it and the rounded corners read as an overlay. */
    const int panel_w = QMX_SCREEN_W - 8;

    s_panel = lv_obj_create(scr);
    lv_obj_remove_style_all(s_panel);
    lv_obj_set_size(s_panel, panel_w, 22);
    lv_obj_align(s_panel, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_style_radius(s_panel, 4, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_panel, lv_color_hex(UI_SURFACE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_panel, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_panel, lv_color_hex(UI_BORDER), LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_panel, 0, LV_PART_MAIN);

    s_title = lv_label_create(s_panel);
    lv_obj_set_style_text_color(s_title, lv_color_hex(UI_MUTED), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_title, &lv_font_qmx_10, LV_PART_MAIN);
    lv_obj_set_pos(s_title, 6, 1);

    s_value = lv_label_create(s_panel);
    lv_obj_set_style_text_color(s_value, lv_color_hex(UI_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_value, &lv_font_qmx_10, LV_PART_MAIN);
    lv_obj_align(s_value, LV_ALIGN_TOP_RIGHT, -6, 1);

    s_bar = lv_bar_create(s_panel);
    lv_obj_set_size(s_bar, panel_w - 12, 3);
    lv_obj_align(s_bar, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_obj_set_style_radius(s_bar, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(s_bar, 2, LV_PART_INDICATOR);
    lv_obj_set_style_border_width(s_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_bar, 0, LV_PART_INDICATOR);
    lv_obj_set_style_pad_all(s_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_bar, 0, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(UI_BORDER), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(UI_ACCENT), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, LV_PART_INDICATOR);
}

static void ui_overlay_refresh_locked(void)
{
    if (!s_panel || !s_title || !s_value || !s_bar)
        return;

    app_settings_t cfg;
    char value[24];
    int min = 0, max = 100;
    int slider_val;

    app_settings_get(&cfg);
    ui_setting_range(s_active_setting, &min, &max);
    slider_val = ui_setting_to_slider(&cfg, s_active_setting);
    ui_setting_format_value(&cfg, s_active_setting, value, sizeof(value));

    lv_label_set_text(s_title, ui_setting_name(s_active_setting));
    lv_label_set_text(s_value, value);
    lv_bar_set_range(s_bar, min, max);
    lv_bar_set_value(s_bar, clamp_i(slider_val, min, max), LV_ANIM_OFF);
}

static void ui_menu_show_locked(void)
{
    if (!s_panel)
    {
        ui_overlay_create_locked();
    }
    ui_overlay_refresh_locked();
    if (s_panel)
    {
        lv_obj_set_hidden(s_panel, false);
    }
    s_menu_visible = true;
}

static void ui_menu_hide_locked(void)
{
    if (s_panel)
    {
        lv_obj_set_hidden(s_panel, true);
    }
    s_menu_visible = false;
}

static bool btn_is_pressed(gpio_num_t pin)
{
    return gpio_get_level(pin) == 0;
}

static bool btn_tick(gpio_num_t pin, btn_state_t *st, bool allow_repeat,
                     bool *short_press, bool *long_press, bool *repeat_press)
{
    int sample = btn_is_pressed(pin) ? 1 : 0;
    int debounce_ticks = BTN_DEBOUNCE_MS / BTN_POLL_MS;
    int long_ticks = BTN_LONG_MS / BTN_POLL_MS;
    int repeat_start_ticks = BTN_REPEAT_START_MS / BTN_POLL_MS;
    int repeat_ticks = BTN_REPEAT_MS / BTN_POLL_MS;

    *short_press = false;
    *long_press = false;
    *repeat_press = false;

    if (sample != st->sampled_level)
    {
        st->sampled_level = sample;
        st->debounce_ticks = 0;
    }
    else if (st->debounce_ticks < debounce_ticks)
    {
        st->debounce_ticks++;
        if (st->debounce_ticks >= debounce_ticks && st->stable_level != sample)
        {
            st->stable_level = sample;
            if (sample)
            {
                st->held_ticks = 0;
                st->repeat_ticks = 0;
                st->long_sent = false;
            }
            else
            {
                if (!st->long_sent)
                {
                    *short_press = true;
                }
            }
        }
    }

    if (st->stable_level)
    {
        st->held_ticks++;
        if (!st->long_sent && st->held_ticks >= long_ticks)
        {
            st->long_sent = true;
            *long_press = true;
        }
        if (allow_repeat && st->held_ticks >= repeat_start_ticks)
        {
            st->repeat_ticks++;
            if (st->repeat_ticks >= repeat_ticks)
            {
                st->repeat_ticks = 0;
                *repeat_press = true;
            }
        }
    }

    return *short_press || *long_press || *repeat_press;
}

static void spectrum_ui_task(void *arg)
{
    (void)arg;

    btn_state_t up = {0};
    btn_state_t ok = {0};
    btn_state_t down = {0};

    while (true)
    {
        bool up_short = false, up_long = false, up_rep = false;
        bool ok_short = false, ok_long = false, ok_rep = false;
        bool down_short = false, down_long = false, down_rep = false;
        bool changed = false;

        (void)btn_tick((gpio_num_t)BTN_UP_GPIO, &up, true, &up_short, &up_long, &up_rep);
        (void)btn_tick((gpio_num_t)BTN_OK_GPIO, &ok, false, &ok_short, &ok_long, &ok_rep);
        (void)btn_tick((gpio_num_t)BTN_DOWN_GPIO, &down, true, &down_short, &down_long, &down_rep);

        if (s_ui_enabled && spectrum_is_running())
        {
            bool any_press = up_short || up_long || up_rep || ok_short || ok_long || down_short || down_long || down_rep;

            if (!s_menu_visible && any_press)
            {
                if (lvgl_port_lock(0))
                {
                    ui_menu_show_locked();
                    lvgl_port_unlock();
                }
                vTaskDelay(pdMS_TO_TICKS(BTN_POLL_MS));
                continue;
            }

            if (ok_long)
            {
                if (lvgl_port_lock(0))
                {
                    ui_menu_hide_locked();
                    lvgl_port_unlock();
                }
                vTaskDelay(pdMS_TO_TICKS(BTN_POLL_MS));
                continue;
            }

            if (ok_short)
            {
                s_active_setting = (app_field_id_t)((s_active_setting + 1) % APP_FIELD_COUNT);
                changed = true;
            }

            if (up_short || up_rep)
            {
                ui_apply_delta(s_active_setting, +1);
                changed = true;
            }
            if (down_short || down_rep)
            {
                ui_apply_delta(s_active_setting, -1);
                changed = true;
            }

            if (changed && lvgl_port_lock(0))
            {
                if (!s_panel)
                {
                    ui_overlay_create_locked();
                }
                ui_overlay_refresh_locked();
                lvgl_port_unlock();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(BTN_POLL_MS));
    }
}

void spectrum_ui_init(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = BIT64(BTN_UP_GPIO) | BIT64(BTN_OK_GPIO) | BIT64(BTN_DOWN_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));

    if (s_ui_task == NULL)
    {
        BaseType_t ok = xTaskCreate(spectrum_ui_task, "spec_ui", 4096, NULL, 2, &s_ui_task);
        if (ok != pdTRUE)
        {
            ESP_LOGE(TAG, "failed to create UI task");
        }
    }

    s_active_setting = APP_FIELD_LEVEL_OFF;
    s_ui_enabled = false;
    s_menu_visible = false;
}

void spectrum_ui_start(void)
{
    s_ui_enabled = true;
    if (lvgl_port_lock(0))
    {
        ui_overlay_create_locked();
        ui_menu_hide_locked();
        lvgl_port_unlock();
    }
}

void spectrum_ui_stop(void)
{
    s_ui_enabled = false;
    s_menu_visible = false;
    if (lvgl_port_lock(0))
    {
        ui_overlay_destroy_locked();
        lvgl_port_unlock();
    }
}
