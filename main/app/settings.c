#include "app/settings.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "display/display.h"
#include "spectrum/spectrum.h"
#include "ui/i18n.h"

#define SETTINGS_NAMESPACE "app_settings"
/* A schema mismatch discards every stored value, so bump it when the meaning
 * of a key changes rather than the key name. */
#define SETTINGS_SCHEMA_VER 2U
#define SETTINGS_SAVE_DEBOUNCE_US (1500ULL * 1000ULL)

static const char *TAG = "settings";

typedef enum
{
    FIELD_F32 = 0,
    FIELD_U8,
    FIELD_U16,
} field_type_t;

typedef struct
{
    app_field_spec_t spec;
    size_t offset;
    field_type_t type;
} field_desc_t;

/* Single source of truth for ranges, steps and defaults. A negative max marks
 * a bound that only the spectrum runtime knows (waterfall speed levels). */
static const field_desc_t s_fields[APP_FIELD_COUNT] = {
    [APP_FIELD_LEVEL_OFF] = {{-80.0f, 80.0f, 0.5f, 0.0f},
                             offsetof(app_settings_t, level_offset_db), FIELD_F32},
    [APP_FIELD_VIEW_MODE] = {{0.0f, 1.0f, 1.0f, 0.0f},
                             offsetof(app_settings_t, view_mode), FIELD_U8},
    [APP_FIELD_WF_FLOOR] = {{-180.0f, 0.0f, 1.0f, -140.0f},
                            offsetof(app_settings_t, wf_floor_db), FIELD_F32},
    [APP_FIELD_WF_CEIL] = {{-80.0f, 40.0f, 1.0f, 10.0f},
                           offsetof(app_settings_t, wf_ceil_db), FIELD_F32},
    /* Fastest at 512/48k; the runtime clamps this to the real level count. */
    [APP_FIELD_SPEED] = {{1.0f, -1.0f, 1.0f, 5.0f},
                         offsetof(app_settings_t, speed_level), FIELD_U8},
    [APP_FIELD_PALETTE] = {{0.0f, 6.0f, 1.0f, 4.0f},
                           offsetof(app_settings_t, wf_palette), FIELD_U8},
    [APP_FIELD_SPEC_MODE] = {{0.0f, 3.0f, 1.0f, 1.0f},
                             offsetof(app_settings_t, spec_mode), FIELD_U8},
    [APP_FIELD_CW_PITCH] = {{400.0f, 1000.0f, 10.0f, 700.0f},
                            offsetof(app_settings_t, cw_pitch_hz), FIELD_U16},
    [APP_FIELD_LANGUAGE] = {{0.0f, (float)(UI_LANG_COUNT - 1), 1.0f, (float)UI_LANG_EN},
                            offsetof(app_settings_t, language), FIELD_U8},
};

typedef enum
{
    NVS_F32_X10 = 0,
    NVS_U8,
    NVS_U32_U16,
} nvs_field_type_t;

typedef struct
{
    const char *key;
    nvs_field_type_t type;
    size_t offset;
} nvs_field_t;

/* Walked by both load and save so a key is spelled only once. */
static const nvs_field_t s_nvs_fields[] = {
    {"sp_off_x10", NVS_F32_X10, offsetof(app_settings_t, level_offset_db)},
    {"sp_view", NVS_U8, offsetof(app_settings_t, view_mode)},
    {"sp_floor_x10", NVS_F32_X10, offsetof(app_settings_t, wf_floor_db)},
    {"sp_ceil_x10", NVS_F32_X10, offsetof(app_settings_t, wf_ceil_db)},
    {"sp_cwbfo", NVS_U32_U16, offsetof(app_settings_t, cw_pitch_hz)},
    {"sp_palette", NVS_U8, offsetof(app_settings_t, wf_palette)},
    {"sp_speed_lvl", NVS_U8, offsetof(app_settings_t, speed_level)},
    {"sp_mode", NVS_U8, offsetof(app_settings_t, spec_mode)},
    {"ui_lang", NVS_U8, offsetof(app_settings_t, language)},
};

#define NVS_FIELD_COUNT (sizeof(s_nvs_fields) / sizeof(s_nvs_fields[0]))

static app_settings_t s_cfg;
static bool s_initialized;
static bool s_dirty;
static uint32_t s_generation;
static esp_timer_handle_t s_save_timer;
static SemaphoreHandle_t s_lock;

static bool settings_lock(void)
{
    return s_lock && xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE;
}

static void settings_unlock(void)
{
    xSemaphoreGive(s_lock);
}

static float clampf(float v, float lo, float hi)
{
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

static float field_max(const field_desc_t *desc)
{
    if (desc->spec.max >= 0.0f)
        return desc->spec.max;

    float max = (float)spectrum_speed_level_count();
    return max > desc->spec.min ? max : desc->spec.min;
}

static float field_read(const app_settings_t *cfg, const field_desc_t *desc)
{
    const char *base = (const char *)cfg + desc->offset;

    switch (desc->type)
    {
    case FIELD_F32:
        return *(const float *)base;
    case FIELD_U8:
        return (float)*(const uint8_t *)base;
    case FIELD_U16:
        return (float)*(const uint16_t *)base;
    }
    return 0.0f;
}

static void field_write(app_settings_t *cfg, const field_desc_t *desc, float value)
{
    char *base = (char *)cfg + desc->offset;

    switch (desc->type)
    {
    case FIELD_F32:
        *(float *)base = value;
        break;
    case FIELD_U8:
        *(uint8_t *)base = (uint8_t)lrintf(value);
        break;
    case FIELD_U16:
        *(uint16_t *)base = (uint16_t)lrintf(value);
        break;
    }
}

const app_field_spec_t *app_settings_field_spec(app_field_id_t id)
{
    if ((unsigned)id >= (unsigned)APP_FIELD_COUNT)
        return NULL;
    return &s_fields[id].spec;
}

float app_settings_field_get(const app_settings_t *cfg, app_field_id_t id)
{
    if (!cfg || (unsigned)id >= (unsigned)APP_FIELD_COUNT)
        return 0.0f;
    return field_read(cfg, &s_fields[id]);
}

float app_settings_field_clamp(app_field_id_t id, float value)
{
    if ((unsigned)id >= (unsigned)APP_FIELD_COUNT)
        return value;

    const field_desc_t *desc = &s_fields[id];
    return clampf(value, desc->spec.min, field_max(desc));
}

void app_settings_defaults(app_settings_t *cfg)
{
    if (!cfg)
        return;

    /* Padding is compared by memcmp, so clear it before filling the fields. */
    memset(cfg, 0, sizeof(*cfg));
    for (unsigned i = 0; i < (unsigned)APP_FIELD_COUNT; i++)
        field_write(cfg, &s_fields[i], s_fields[i].spec.def);
}

static bool app_settings_validate_and_repair(app_settings_t *cfg)
{
    app_settings_t def;
    bool repaired = false;

    if (!cfg)
        return false;

    app_settings_defaults(&def);

    for (unsigned i = 0; i < (unsigned)APP_FIELD_COUNT; i++)
    {
        const field_desc_t *desc = &s_fields[i];
        float value = field_read(cfg, desc);

        if (!isfinite(value) || value < desc->spec.min || value > field_max(desc))
        {
            field_write(cfg, desc, field_read(&def, desc));
            repaired = true;
        }
    }

    if (cfg->wf_floor_db >= cfg->wf_ceil_db)
    {
        cfg->wf_floor_db = def.wf_floor_db;
        cfg->wf_ceil_db = def.wf_ceil_db;
        repaired = true;
    }

    return repaired;
}

static esp_err_t app_settings_open_rw(nvs_handle_t *out)
{
    return nvs_open(SETTINGS_NAMESPACE, NVS_READWRITE, out);
}

static esp_err_t app_settings_nvs_read(nvs_handle_t h, const nvs_field_t *field, app_settings_t *cfg)
{
    char *base = (char *)cfg + field->offset;

    switch (field->type)
    {
    case NVS_F32_X10:
    {
        int32_t value = 0;
        esp_err_t err = nvs_get_i32(h, field->key, &value);
        if (err == ESP_OK)
            *(float *)base = (float)value / 10.0f;
        return err;
    }
    case NVS_U8:
    {
        uint8_t value = 0;
        esp_err_t err = nvs_get_u8(h, field->key, &value);
        if (err == ESP_OK)
            *(uint8_t *)base = value;
        return err;
    }
    case NVS_U32_U16:
    {
        uint32_t value = 0;
        esp_err_t err = nvs_get_u32(h, field->key, &value);
        if (err == ESP_OK)
            *(uint16_t *)base = (uint16_t)value;
        return err;
    }
    }
    return ESP_ERR_INVALID_ARG;
}

static esp_err_t app_settings_nvs_write(nvs_handle_t h, const nvs_field_t *field, const app_settings_t *cfg)
{
    const char *base = (const char *)cfg + field->offset;

    switch (field->type)
    {
    case NVS_F32_X10:
        return nvs_set_i32(h, field->key, (int32_t)lrintf(*(const float *)base * 10.0f));
    case NVS_U8:
        return nvs_set_u8(h, field->key, *(const uint8_t *)base);
    case NVS_U32_U16:
        return nvs_set_u32(h, field->key, (uint32_t)*(const uint16_t *)base);
    }
    return ESP_ERR_INVALID_ARG;
}

static esp_err_t app_settings_load(void)
{
    nvs_handle_t h = 0;
    uint32_t schema = 0;
    esp_err_t err = app_settings_open_rw(&h);
    if (err != ESP_OK)
    {
        return err;
    }

    app_settings_defaults(&s_cfg);

    err = nvs_get_u32(h, "schema", &schema);
    if (err == ESP_ERR_NVS_NOT_FOUND)
    {
        nvs_close(h);
        return ESP_OK;
    }
    if (err != ESP_OK)
    {
        nvs_close(h);
        return err;
    }

    if (schema != SETTINGS_SCHEMA_VER)
    {
        ESP_LOGW(TAG, "settings schema %lu unsupported, using defaults", (unsigned long)schema);
        s_dirty = true;
        nvs_close(h);
        return ESP_OK;
    }

    bool speed_loaded = false;
    for (size_t i = 0; i < NVS_FIELD_COUNT; i++)
    {
        if (app_settings_nvs_read(h, &s_nvs_fields[i], &s_cfg) != ESP_OK)
            continue;
        if (s_nvs_fields[i].offset == offsetof(app_settings_t, speed_level))
            speed_loaded = true;
    }

    if (!speed_loaded)
    {
        uint32_t speed_x100 = 0;
        if (nvs_get_u32(h, "sp_speed_x100", &speed_x100) == ESP_OK)
        {
            /* The old "lines/s" setting was not the displayed scroll rate (the
             * old code pushed 2-4 ring rows per frame), so map it to the nearest
             * level of the rate that looked like it. */
            s_cfg.speed_level = spectrum_speed_level_from_lps((float)speed_x100 / 100.0f * 2.0f);
        }
    }

    nvs_close(h);

    if (app_settings_validate_and_repair(&s_cfg))
    {
        ESP_LOGW(TAG, "settings repaired to defaults for invalid fields");
        s_dirty = true;
    }

    return ESP_OK;
}

static esp_err_t app_settings_save(const app_settings_t *cfg)
{
    nvs_handle_t h = 0;
    esp_err_t err = app_settings_open_rw(&h);
    if (err != ESP_OK)
    {
        return err;
    }

    err = nvs_set_u32(h, "schema", SETTINGS_SCHEMA_VER);
    for (size_t i = 0; i < NVS_FIELD_COUNT && err == ESP_OK; i++)
        err = app_settings_nvs_write(h, &s_nvs_fields[i], cfg);

    if (err == ESP_OK)
        err = nvs_commit(h);

    /* Schema 1 keys that no longer have a runtime meaning. */
    if (err == ESP_OK)
    {
        static const char *const legacy_keys[] = {
            "sp_gamma_x100", "sp_alpha_x1000", "sp_cond_x100", "sp_beta_x1000", "sp_bilin",
            "sp_peak", "sp_speed_x100",
        };
        for (size_t i = 0; i < sizeof(legacy_keys) / sizeof(legacy_keys[0]); i++)
        {
            (void)nvs_erase_key(h, legacy_keys[i]);
        }
        err = nvs_commit(h);
    }

    nvs_close(h);
    return err;
}

static void app_settings_save_timer_cb(void *arg)
{
    (void)arg;

    app_settings_t snapshot;
    uint32_t generation;

    if (!settings_lock())
        return;
    if (!s_initialized || !s_dirty)
    {
        settings_unlock();
        return;
    }
    snapshot = s_cfg;
    generation = s_generation;
    settings_unlock();

    esp_err_t err = app_settings_save(&snapshot);
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "debounced save failed: %s", esp_err_to_name(err));
        return;
    }

    if (settings_lock())
    {
        if (generation == s_generation)
            s_dirty = false;
        settings_unlock();
    }
}

static void app_settings_mark_dirty_locked(void)
{
    if (!s_initialized)
        return;

    s_dirty = true;
    if (s_save_timer)
    {
        (void)esp_timer_stop(s_save_timer);
        (void)esp_timer_start_once(s_save_timer, SETTINGS_SAVE_DEBOUNCE_US);
    }
}

static bool app_settings_assign_if_changed_locked(const app_settings_t *next)
{
    if (!next || memcmp(&s_cfg, next, sizeof(s_cfg)) == 0)
        return false;
    s_cfg = *next;
    s_generation++;
    return true;
}

static void app_settings_apply_snapshot(const app_settings_t *cfg)
{
    /* Language changes must reach the status screen even before the
     * spectrum exists (boot, connect and disconnect screens). Only touch the
     * display here when the language actually changed: this function runs on
     * every level/speed tick from the UI task. */
    uint8_t old_lang = ui_i18n_get_lang();
    ui_i18n_set_lang(cfg->language);
    if (old_lang != cfg->language)
        display_language_changed();

    if (!spectrum_is_running())
        return;

    spectrum_set_level_offset_db(cfg->level_offset_db);
    spectrum_set_view_mode(cfg->view_mode);
    spectrum_set_wf_range(cfg->wf_floor_db, cfg->wf_ceil_db);
    spectrum_set_cw_pitch_hz(cfg->cw_pitch_hz);
    spectrum_set_wf_palette(cfg->wf_palette);
    uint8_t level = spectrum_set_speed_level(cfg->speed_level);
    if (level != cfg->speed_level && settings_lock())
    {
        /* A concurrent change may have landed first; only fold back the clamp
         * into the value this snapshot was taken from. */
        if (s_cfg.speed_level == cfg->speed_level)
        {
            s_cfg.speed_level = level;
            s_generation++;
            app_settings_mark_dirty_locked();
        }
        settings_unlock();
    }
    spectrum_set_spec_mode(cfg->spec_mode);
}

esp_err_t app_settings_init(void)
{
    if (s_initialized)
        return ESP_OK;

    s_lock = xSemaphoreCreateMutex();
    if (!s_lock)
        return ESP_ERR_NO_MEM;

    const esp_timer_create_args_t args = {
        .callback = app_settings_save_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "settings_save",
        .skip_unhandled_events = true,
    };

    esp_err_t err = esp_timer_create(&args, &s_save_timer);
    if (err != ESP_OK)
        return err;

    err = app_settings_load();
    if (err != ESP_OK)
        return err;

    /* display_init() reads the strings, so the language must be active
     * before it runs. */
    ui_i18n_set_lang(s_cfg.language);

    if (settings_lock())
    {
        s_initialized = true;
        if (s_dirty)
            app_settings_mark_dirty_locked();
        settings_unlock();
    }
    return ESP_OK;
}

void app_settings_get(app_settings_t *out)
{
    if (!out)
        return;

    if (!settings_lock())
    {
        memset(out, 0, sizeof(*out));
        return;
    }
    *out = s_cfg;
    settings_unlock();
}

void app_settings_apply_to_runtime(void)
{
    if (!s_initialized)
        return;

    /* The runtime calls happen outside the settings lock (they take the LVGL
     * and spectrum locks), so another task can commit while this snapshot is
     * being applied. Re-apply when that happened instead of leaving the
     * runtime older than the stored settings. */
    for (int attempt = 0; attempt < 3; attempt++)
    {
        app_settings_t cfg;
        uint32_t generation;

        if (!settings_lock())
            return;
        cfg = s_cfg;
        generation = s_generation;
        settings_unlock();

        app_settings_apply_snapshot(&cfg);

        if (!settings_lock())
            return;
        bool current = generation == s_generation;
        settings_unlock();
        if (current)
            return;
    }
}

/* Lock held by the caller; runtime and NVS work happen after it is dropped. */
static bool app_settings_commit_locked(app_settings_t candidate)
{
    (void)app_settings_validate_and_repair(&candidate);
    if (!app_settings_assign_if_changed_locked(&candidate))
        return false;

    app_settings_mark_dirty_locked();
    return true;
}

static void app_settings_hold_range_pair(app_settings_t *cfg, app_field_id_t id)
{
    const float gap = 1.0f;

    if (id == APP_FIELD_WF_FLOOR && cfg->wf_ceil_db - cfg->wf_floor_db < gap)
        cfg->wf_floor_db = cfg->wf_ceil_db - gap;
    else if (id == APP_FIELD_WF_CEIL && cfg->wf_ceil_db - cfg->wf_floor_db < gap)
        cfg->wf_ceil_db = cfg->wf_floor_db + gap;
}

esp_err_t app_settings_field_set(app_field_id_t id, float value)
{
    if ((unsigned)id >= (unsigned)APP_FIELD_COUNT)
        return ESP_ERR_INVALID_ARG;
    if (!s_initialized)
        return ESP_ERR_INVALID_STATE;
    if (!settings_lock())
        return ESP_ERR_INVALID_STATE;

    app_settings_t next = s_cfg;
    field_write(&next, &s_fields[id], app_settings_field_clamp(id, value));
    app_settings_hold_range_pair(&next, id);
    bool changed = app_settings_commit_locked(next);

    settings_unlock();

    if (changed)
        app_settings_apply_to_runtime();
    return ESP_OK;
}

esp_err_t app_settings_set_level_offset_db(float offset_db)
{
    return app_settings_field_set(APP_FIELD_LEVEL_OFF, offset_db);
}

esp_err_t app_settings_set_view_mode(uint8_t mode)
{
    return app_settings_field_set(APP_FIELD_VIEW_MODE, (float)mode);
}

esp_err_t app_settings_set_wf_range(float floor_db, float ceil_db)
{
    if (!s_initialized)
        return ESP_ERR_INVALID_STATE;
    if (!settings_lock())
        return ESP_ERR_INVALID_STATE;

    app_settings_t next = s_cfg;
    next.wf_floor_db = app_settings_field_clamp(APP_FIELD_WF_FLOOR, floor_db);
    next.wf_ceil_db = app_settings_field_clamp(APP_FIELD_WF_CEIL, ceil_db);
    app_settings_hold_range_pair(&next, APP_FIELD_WF_FLOOR);
    bool changed = app_settings_commit_locked(next);

    settings_unlock();

    if (changed)
        app_settings_apply_to_runtime();
    return ESP_OK;
}

esp_err_t app_settings_set_cw_pitch_hz(uint16_t hz)
{
    return app_settings_field_set(APP_FIELD_CW_PITCH, (float)hz);
}

esp_err_t app_settings_set_speed_level(uint8_t level)
{
    return app_settings_field_set(APP_FIELD_SPEED, (float)level);
}

esp_err_t app_settings_set_spec_mode(uint8_t mode)
{
    return app_settings_field_set(APP_FIELD_SPEC_MODE, (float)mode);
}

esp_err_t app_settings_set_wf_palette(uint8_t palette)
{
    return app_settings_field_set(APP_FIELD_PALETTE, (float)palette);
}

esp_err_t app_settings_set_language(uint8_t lang)
{
    return app_settings_field_set(APP_FIELD_LANGUAGE, (float)lang);
}

esp_err_t app_settings_flush_now(void)
{
    app_settings_t snapshot;
    uint32_t generation;

    if (!s_initialized)
        return ESP_ERR_INVALID_STATE;
    if (!settings_lock())
        return ESP_ERR_INVALID_STATE;

    if (s_save_timer)
        (void)esp_timer_stop(s_save_timer);
    if (!s_dirty)
    {
        settings_unlock();
        return ESP_OK;
    }
    snapshot = s_cfg;
    generation = s_generation;
    settings_unlock();

    esp_err_t err = app_settings_save(&snapshot);
    if (err != ESP_OK)
        return err;

    if (settings_lock())
    {
        if (generation == s_generation)
            s_dirty = false;
        settings_unlock();
    }
    return ESP_OK;
}

esp_err_t app_settings_reset_to_defaults(bool persist_now)
{
    if (!s_initialized)
        return ESP_ERR_INVALID_STATE;
    if (!settings_lock())
        return ESP_ERR_INVALID_STATE;

    app_settings_defaults(&s_cfg);
    s_generation++;
    s_dirty = true;
    if (!persist_now)
        app_settings_mark_dirty_locked();
    settings_unlock();

    app_settings_apply_to_runtime();

    if (persist_now)
        return app_settings_flush_now();
    return ESP_OK;
}
