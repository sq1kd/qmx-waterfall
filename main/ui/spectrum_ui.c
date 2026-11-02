#include "ui/spectrum_ui.h"

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

typedef enum
{
    UI_SET_LEVEL_OFF = 0,
    UI_SET_VIEW_MODE,
    UI_SET_WF_FLOOR,
    UI_SET_WF_CEIL,
    UI_SET_CW_PITCH,
    UI_SET_WF_PALETTE,
    UI_SET_WF_SPEED,
    UI_SET_WF_PEAK,
    UI_SET_COUNT,
} ui_setting_id_t;

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
static ui_setting_id_t s_active_setting;

static lv_obj_t *s_panel;
static lv_obj_t *s_title;
static lv_obj_t *s_slider;

static inline int clamp_i(int v, int lo, int hi)
{
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

static inline float clamp_f(float v, float lo, float hi)
{
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

static int ui_setting_to_slider(const app_settings_t *cfg, ui_setting_id_t id)
{
    switch (id)
    {
    case UI_SET_LEVEL_OFF:
        return (int)(cfg->level_offset_db * 10.0f + (cfg->level_offset_db >= 0 ? 0.5f : -0.5f));
    case UI_SET_VIEW_MODE:
        return (int)cfg->view_mode;
    case UI_SET_WF_FLOOR:
        return (int)(cfg->wf_floor_db + (cfg->wf_floor_db >= 0 ? 0.5f : -0.5f));
    case UI_SET_WF_CEIL:
        return (int)(cfg->wf_ceil_db + (cfg->wf_ceil_db >= 0 ? 0.5f : -0.5f));
    case UI_SET_CW_PITCH:
        return (int)cfg->cw_pitch_hz;
    case UI_SET_WF_PALETTE:
        return (int)cfg->wf_palette;
    case UI_SET_WF_SPEED:
        return (int)(cfg->speed_lps * 100.0f + 0.5f);
    case UI_SET_WF_PEAK:
        return cfg->peak ? 1 : 0;
    default:
        return 0;
    }
}

static const char *ui_setting_name(ui_setting_id_t id)
{
    switch (id)
    {
    case UI_SET_LEVEL_OFF:
        return "Level off";
    case UI_SET_VIEW_MODE:
        return "View";
    case UI_SET_WF_FLOOR:
        return "WF floor";
    case UI_SET_WF_CEIL:
        return "WF ceil";
    case UI_SET_CW_PITCH:
        return "CW pitch";
    case UI_SET_WF_PALETTE:
        return "Palette";
    case UI_SET_WF_SPEED:
        return "Speed";
    case UI_SET_WF_PEAK:
        return "Peak";
    default:
        return "Setting";
    }
}

static void ui_setting_range(ui_setting_id_t id, int *min, int *max)
{
    switch (id)
    {
    case UI_SET_LEVEL_OFF:
        *min = -800;
        *max = 800;
        break;
    case UI_SET_VIEW_MODE:
        *min = 0;
        *max = 1;
        break;
    case UI_SET_WF_FLOOR:
        *min = -180;
        *max = 0;
        break;
    case UI_SET_WF_CEIL:
        *min = -80;
        *max = 40;
        break;
    case UI_SET_CW_PITCH:
        *min = 400;
        *max = 1000;
        break;
    case UI_SET_WF_PALETTE:
        *min = 0;
        *max = 6;
        break;
    case UI_SET_WF_SPEED:
        *min = 100;
        *max = 2500;
        break;
    case UI_SET_WF_PEAK:
        *min = 0;
        *max = 1;
        break;
    default:
        *min = 0;
        *max = 100;
        break;
    }
}

static void ui_setting_format_value(const app_settings_t *cfg, ui_setting_id_t id, char *buf, size_t n)
{
    switch (id)
    {
    case UI_SET_LEVEL_OFF:
        snprintf(buf, n, "%.1f dB", cfg->level_offset_db);
        break;
    case UI_SET_VIEW_MODE:
        snprintf(buf, n, "%s", cfg->view_mode == 0 ? "Full" : "Pos");
        break;
    case UI_SET_WF_FLOOR:
        snprintf(buf, n, "%.0f dB", cfg->wf_floor_db);
        break;
    case UI_SET_WF_CEIL:
        snprintf(buf, n, "%.0f dB", cfg->wf_ceil_db);
        break;
    case UI_SET_CW_PITCH:
        snprintf(buf, n, "%u Hz", (unsigned)cfg->cw_pitch_hz);
        break;
    case UI_SET_WF_PALETTE:
        switch (cfg->wf_palette)
        {
        case 0:
            snprintf(buf, n, "Turbo");
            break;
        case 1:
            snprintf(buf, n, "Gray");
            break;
        case 2:
            snprintf(buf, n, "Inferno");
            break;
        case 3:
            snprintf(buf, n, "Viridis");
            break;
        case 4:
            snprintf(buf, n, "Magma");
            break;
        case 5:
            snprintf(buf, n, "Plasma");
            break;
        case 6:
            snprintf(buf, n, "SDR BlueHot");
            break;
        default:
            snprintf(buf, n, "%u", (unsigned)cfg->wf_palette);
            break;
        }
        break;
    case UI_SET_WF_SPEED:
        snprintf(buf, n, "%.1f l/s", cfg->speed_lps);
        break;
    case UI_SET_WF_PEAK:
        snprintf(buf, n, "%s", cfg->peak ? "On" : "Off");
        break;
    default:
        snprintf(buf, n, "-");
        break;
    }
}

static void ui_apply_delta(ui_setting_id_t id, int dir)
{
    app_settings_t cfg;
    app_settings_get(&cfg);

    switch (id)
    {
    case UI_SET_LEVEL_OFF:
        (void)app_settings_set_level_offset_db(clamp_f(cfg.level_offset_db + 0.5f * (float)dir, -80.0f, 80.0f));
        break;
    case UI_SET_VIEW_MODE:
        (void)app_settings_set_view_mode((uint8_t)clamp_i((int)cfg.view_mode + dir, 0, 1));
        break;
    case UI_SET_WF_FLOOR:
    {
        float next = clamp_f(cfg.wf_floor_db + 1.0f * (float)dir, -180.0f, cfg.wf_ceil_db - 1.0f);
        (void)app_settings_set_wf_range(next, cfg.wf_ceil_db);
    }
    break;
    case UI_SET_WF_CEIL:
    {
        float next = clamp_f(cfg.wf_ceil_db + 1.0f * (float)dir, cfg.wf_floor_db + 1.0f, 40.0f);
        (void)app_settings_set_wf_range(cfg.wf_floor_db, next);
    }
    break;
    case UI_SET_CW_PITCH:
        (void)app_settings_set_cw_pitch_hz((uint16_t)clamp_i((int)cfg.cw_pitch_hz + 10 * dir, 400, 1000));
        break;
    case UI_SET_WF_PALETTE:
        (void)app_settings_set_wf_palette((uint8_t)clamp_i((int)cfg.wf_palette + dir, 0, 6));
        break;
    case UI_SET_WF_SPEED:
        (void)app_settings_set_speed(clamp_f(cfg.speed_lps + 0.5f * (float)dir, 1.0f, 25.0f));
        break;
    case UI_SET_WF_PEAK:
        (void)app_settings_set_peak(dir > 0);
        break;
    default:
        break;
    }
}

static void ui_overlay_destroy_locked(void)
{
    if (s_panel)
    {
        lv_obj_del(s_panel);
        s_panel = NULL;
        s_title = NULL;
        s_slider = NULL;
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

    s_panel = lv_obj_create(scr);
    lv_obj_remove_style_all(s_panel);
    lv_obj_set_size(s_panel, 284, 20);
    lv_obj_align(s_panel, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_panel, lv_color_hex(0x101218), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_panel, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_panel, lv_color_hex(0x3A3F52), LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_panel, 1, LV_PART_MAIN);

    s_title = lv_label_create(s_panel);
    lv_obj_set_style_text_color(s_title, lv_color_hex(0xD7DEEA), LV_PART_MAIN);
#if defined(CONFIG_LV_FONT_MONTSERRAT_10)
    lv_obj_set_style_text_font(s_title, &lv_font_montserrat_10, LV_PART_MAIN);
#endif
    lv_obj_align(s_title, LV_ALIGN_TOP_LEFT, 2, -1);

    s_slider = lv_slider_create(s_panel);
    lv_obj_set_size(s_slider, 278, 6);
    lv_obj_align(s_slider, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_slider_set_mode(s_slider, LV_SLIDER_MODE_NORMAL);
    lv_obj_set_style_radius(s_slider, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(s_slider, 1, LV_PART_INDICATOR);
    lv_obj_set_style_height(s_slider, 3, LV_PART_MAIN);
    lv_obj_set_style_height(s_slider, 3, LV_PART_INDICATOR);
    lv_obj_set_style_width(s_slider, 6, LV_PART_KNOB);
    lv_obj_set_style_height(s_slider, 6, LV_PART_KNOB);
}

static void ui_overlay_refresh_locked(void)
{
    if (!s_panel || !s_title || !s_slider)
        return;

    app_settings_t cfg;
    char value[24];
    char line[48];
    int min = 0, max = 100;
    int slider_val;

    app_settings_get(&cfg);
    ui_setting_range(s_active_setting, &min, &max);
    slider_val = ui_setting_to_slider(&cfg, s_active_setting);
    ui_setting_format_value(&cfg, s_active_setting, value, sizeof(value));

    (void)snprintf(line, sizeof(line), "%s: %s", ui_setting_name(s_active_setting), value);
    lv_label_set_text(s_title, line);
    lv_slider_set_range(s_slider, min, max);
    lv_slider_set_value(s_slider, clamp_i(slider_val, min, max), LV_ANIM_OFF);
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

static bool btn_tick(gpio_num_t pin, btn_state_t *st, bool *short_press, bool *long_press, bool *repeat_press)
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
        if (st->held_ticks >= repeat_start_ticks)
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

        (void)btn_tick((gpio_num_t)BTN_UP_GPIO, &up, &up_short, &up_long, &up_rep);
        (void)btn_tick((gpio_num_t)BTN_OK_GPIO, &ok, &ok_short, &ok_long, &ok_rep);
        (void)ok_rep;
        (void)btn_tick((gpio_num_t)BTN_DOWN_GPIO, &down, &down_short, &down_long, &down_rep);

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
                s_active_setting = (ui_setting_id_t)((s_active_setting + 1) % UI_SET_COUNT);
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

    s_active_setting = UI_SET_LEVEL_OFF;
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
