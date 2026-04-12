#include "app/settings.h"

#include <math.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "spectrum/spectrum.h"

#define SETTINGS_NAMESPACE "app_settings"
#define SETTINGS_SCHEMA_VER 1U
#define SETTINGS_SAVE_DEBOUNCE_US (1500ULL * 1000ULL)

static const char *TAG = "settings";

static app_settings_t s_cfg;
static bool s_initialized;
static bool s_dirty;
static esp_timer_handle_t s_save_timer;

static float clampf(float v, float lo, float hi)
{
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

void app_settings_defaults(app_settings_t *cfg)
{
    if (!cfg)
        return;

    cfg->level_offset_db = 0.0f;
    cfg->view_mode = 0;
    cfg->wf_floor_db = -140.0f;
    cfg->wf_ceil_db = 10.0f;
    cfg->cw_pitch_hz = 700;
    cfg->wf_gamma = 1.0f;
    cfg->wf_palette = 4;
    cfg->spec_alpha = 0.25f;
    cfg->wf_condense = 2.0f;
    cfg->wf_temporal_beta = 0.0f;
    cfg->wf_bilinear = true;
}

static bool app_settings_validate_and_repair(app_settings_t *cfg)
{
    app_settings_t def;
    bool repaired = false;

    if (!cfg)
        return false;

    app_settings_defaults(&def);

    if (!isfinite(cfg->level_offset_db) || cfg->level_offset_db < -80.0f || cfg->level_offset_db > 80.0f)
    {
        cfg->level_offset_db = def.level_offset_db;
        repaired = true;
    }
    if (cfg->view_mode > 1)
    {
        cfg->view_mode = def.view_mode;
        repaired = true;
    }
    if (!isfinite(cfg->wf_floor_db) || !isfinite(cfg->wf_ceil_db) || cfg->wf_floor_db >= cfg->wf_ceil_db)
    {
        cfg->wf_floor_db = def.wf_floor_db;
        cfg->wf_ceil_db = def.wf_ceil_db;
        repaired = true;
    }
    if (cfg->cw_pitch_hz < 400 || cfg->cw_pitch_hz > 1000)
    {
        cfg->cw_pitch_hz = def.cw_pitch_hz;
        repaired = true;
    }
    if (!isfinite(cfg->wf_gamma) || cfg->wf_gamma < 0.5f || cfg->wf_gamma > 2.5f)
    {
        cfg->wf_gamma = def.wf_gamma;
        repaired = true;
    }
    if (cfg->wf_palette > 5)
    {
        cfg->wf_palette = def.wf_palette;
        repaired = true;
    }
    if (!isfinite(cfg->spec_alpha) || cfg->spec_alpha <= 0.0f || cfg->spec_alpha > 1.0f)
    {
        cfg->spec_alpha = def.spec_alpha;
        repaired = true;
    }
    if (!isfinite(cfg->wf_condense) || cfg->wf_condense < 1.0f || cfg->wf_condense > 3.0f)
    {
        cfg->wf_condense = def.wf_condense;
        repaired = true;
    }
    if (!isfinite(cfg->wf_temporal_beta) || cfg->wf_temporal_beta < 0.0f || cfg->wf_temporal_beta > 0.5f)
    {
        cfg->wf_temporal_beta = def.wf_temporal_beta;
        repaired = true;
    }

    return repaired;
}

static esp_err_t app_settings_open_rw(nvs_handle_t *out)
{
    return nvs_open(SETTINGS_NAMESPACE, NVS_READWRITE, out);
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
        nvs_close(h);
        return ESP_OK;
    }

    int32_t i32 = 0;
    uint32_t u32 = 0;
    uint8_t u8 = 0;

    if (nvs_get_i32(h, "sp_off_x10", &i32) == ESP_OK)
        s_cfg.level_offset_db = (float)i32 / 10.0f;
    if (nvs_get_u8(h, "sp_view", &u8) == ESP_OK)
        s_cfg.view_mode = u8;
    if (nvs_get_i32(h, "sp_floor_x10", &i32) == ESP_OK)
        s_cfg.wf_floor_db = (float)i32 / 10.0f;
    if (nvs_get_i32(h, "sp_ceil_x10", &i32) == ESP_OK)
        s_cfg.wf_ceil_db = (float)i32 / 10.0f;
    if (nvs_get_u32(h, "sp_cwbfo", &u32) == ESP_OK)
        s_cfg.cw_pitch_hz = (uint16_t)u32;
    if (nvs_get_u32(h, "sp_gamma_x100", &u32) == ESP_OK)
        s_cfg.wf_gamma = (float)u32 / 100.0f;
    if (nvs_get_u8(h, "sp_palette", &u8) == ESP_OK)
        s_cfg.wf_palette = u8;
    if (nvs_get_u32(h, "sp_alpha_x1000", &u32) == ESP_OK)
        s_cfg.spec_alpha = (float)u32 / 1000.0f;
    if (nvs_get_u32(h, "sp_cond_x100", &u32) == ESP_OK)
        s_cfg.wf_condense = (float)u32 / 100.0f;
    if (nvs_get_u32(h, "sp_beta_x1000", &u32) == ESP_OK)
        s_cfg.wf_temporal_beta = (float)u32 / 1000.0f;
    if (nvs_get_u8(h, "sp_bilin", &u8) == ESP_OK)
        s_cfg.wf_bilinear = (u8 != 0);

    nvs_close(h);

    if (app_settings_validate_and_repair(&s_cfg))
    {
        ESP_LOGW(TAG, "settings repaired to defaults for invalid fields");
        s_dirty = true;
    }

    return ESP_OK;
}

static esp_err_t app_settings_save_locked(void)
{
    nvs_handle_t h = 0;
    esp_err_t err = app_settings_open_rw(&h);
    if (err != ESP_OK)
    {
        return err;
    }

    int32_t sp_off_x10 = (int32_t)lrintf(clampf(s_cfg.level_offset_db, -80.0f, 80.0f) * 10.0f);
    int32_t sp_floor_x10 = (int32_t)lrintf(s_cfg.wf_floor_db * 10.0f);
    int32_t sp_ceil_x10 = (int32_t)lrintf(s_cfg.wf_ceil_db * 10.0f);
    uint32_t sp_gamma_x100 = (uint32_t)lrintf(clampf(s_cfg.wf_gamma, 0.5f, 2.5f) * 100.0f);
    uint32_t sp_alpha_x1000 = (uint32_t)lrintf(clampf(s_cfg.spec_alpha, 0.01f, 1.0f) * 1000.0f);
    uint32_t sp_cond_x100 = (uint32_t)lrintf(clampf(s_cfg.wf_condense, 1.0f, 3.0f) * 100.0f);
    uint32_t sp_beta_x1000 = (uint32_t)lrintf(clampf(s_cfg.wf_temporal_beta, 0.0f, 0.5f) * 1000.0f);

    err = nvs_set_u32(h, "schema", SETTINGS_SCHEMA_VER);
    if (err == ESP_OK)
        err = nvs_set_i32(h, "sp_off_x10", sp_off_x10);
    if (err == ESP_OK)
        err = nvs_set_u8(h, "sp_view", s_cfg.view_mode);
    if (err == ESP_OK)
        err = nvs_set_i32(h, "sp_floor_x10", sp_floor_x10);
    if (err == ESP_OK)
        err = nvs_set_i32(h, "sp_ceil_x10", sp_ceil_x10);
    if (err == ESP_OK)
        err = nvs_set_u32(h, "sp_cwbfo", (uint32_t)s_cfg.cw_pitch_hz);
    if (err == ESP_OK)
        err = nvs_set_u32(h, "sp_gamma_x100", sp_gamma_x100);
    if (err == ESP_OK)
        err = nvs_set_u8(h, "sp_palette", s_cfg.wf_palette);
    if (err == ESP_OK)
        err = nvs_set_u32(h, "sp_alpha_x1000", sp_alpha_x1000);
    if (err == ESP_OK)
        err = nvs_set_u32(h, "sp_cond_x100", sp_cond_x100);
    if (err == ESP_OK)
        err = nvs_set_u32(h, "sp_beta_x1000", sp_beta_x1000);
    if (err == ESP_OK)
        err = nvs_set_u8(h, "sp_bilin", s_cfg.wf_bilinear ? 1 : 0);
    if (err == ESP_OK)
        err = nvs_commit(h);

    nvs_close(h);
    return err;
}

static void app_settings_save_timer_cb(void *arg)
{
    (void)arg;
    if (!s_initialized || !s_dirty)
        return;

    esp_err_t err = app_settings_save_locked();
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "debounced save failed: %s", esp_err_to_name(err));
        return;
    }
    s_dirty = false;
}

static void app_settings_mark_dirty(void)
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

static bool app_settings_assign_if_changed(const app_settings_t *next)
{
    if (!next)
        return false;
    if (memcmp(&s_cfg, next, sizeof(s_cfg)) == 0)
        return false;
    s_cfg = *next;
    return true;
}

esp_err_t app_settings_init(void)
{
    if (s_initialized)
        return ESP_OK;

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

    s_initialized = true;
    if (s_dirty)
        app_settings_mark_dirty();
    return ESP_OK;
}

void app_settings_get(app_settings_t *out)
{
    if (!out)
        return;
    *out = s_cfg;
}

void app_settings_apply_to_runtime(void)
{
    if (!spectrum_is_running())
        return;

    spectrum_set_level_offset_db(s_cfg.level_offset_db);
    spectrum_set_view_mode(s_cfg.view_mode);
    spectrum_set_wf_range(s_cfg.wf_floor_db, s_cfg.wf_ceil_db);
    spectrum_set_cw_pitch_hz(s_cfg.cw_pitch_hz);
    spectrum_set_wf_gamma(s_cfg.wf_gamma);
    spectrum_set_wf_palette(s_cfg.wf_palette);
    spectrum_set_spec_alpha(s_cfg.spec_alpha);
    spectrum_set_wf_condense(s_cfg.wf_condense);
    spectrum_set_wf_temporal_beta(s_cfg.wf_temporal_beta);
    spectrum_set_wf_bilinear(s_cfg.wf_bilinear);
}

static esp_err_t app_settings_set_all(const app_settings_t *next)
{
    app_settings_t candidate;

    if (!s_initialized)
        return ESP_ERR_INVALID_STATE;

    if (!next)
        return ESP_ERR_INVALID_ARG;

    candidate = *next;
    (void)app_settings_validate_and_repair(&candidate);
    if (!app_settings_assign_if_changed(&candidate))
        return ESP_OK;

    app_settings_apply_to_runtime();
    app_settings_mark_dirty();
    return ESP_OK;
}

esp_err_t app_settings_set_level_offset_db(float offset_db)
{
    app_settings_t next = s_cfg;
    next.level_offset_db = offset_db;
    return app_settings_set_all(&next);
}

esp_err_t app_settings_set_view_mode(uint8_t mode)
{
    app_settings_t next = s_cfg;
    next.view_mode = mode;
    return app_settings_set_all(&next);
}

esp_err_t app_settings_set_wf_range(float floor_db, float ceil_db)
{
    app_settings_t next = s_cfg;
    next.wf_floor_db = floor_db;
    next.wf_ceil_db = ceil_db;
    return app_settings_set_all(&next);
}

esp_err_t app_settings_set_cw_pitch_hz(uint16_t hz)
{
    app_settings_t next = s_cfg;
    next.cw_pitch_hz = hz;
    return app_settings_set_all(&next);
}

esp_err_t app_settings_set_wf_gamma(float gamma)
{
    app_settings_t next = s_cfg;
    next.wf_gamma = gamma;
    return app_settings_set_all(&next);
}

esp_err_t app_settings_set_wf_palette(uint8_t palette)
{
    app_settings_t next = s_cfg;
    next.wf_palette = palette;
    return app_settings_set_all(&next);
}

esp_err_t app_settings_set_spec_alpha(float alpha)
{
    app_settings_t next = s_cfg;
    next.spec_alpha = alpha;
    return app_settings_set_all(&next);
}

esp_err_t app_settings_set_wf_condense(float factor)
{
    app_settings_t next = s_cfg;
    next.wf_condense = factor;
    return app_settings_set_all(&next);
}

esp_err_t app_settings_set_wf_temporal_beta(float beta)
{
    app_settings_t next = s_cfg;
    next.wf_temporal_beta = beta;
    return app_settings_set_all(&next);
}

esp_err_t app_settings_set_wf_bilinear(bool enabled)
{
    app_settings_t next = s_cfg;
    next.wf_bilinear = enabled;
    return app_settings_set_all(&next);
}

esp_err_t app_settings_flush_now(void)
{
    if (!s_initialized)
        return ESP_ERR_INVALID_STATE;

    if (s_save_timer)
        (void)esp_timer_stop(s_save_timer);
    if (!s_dirty)
        return ESP_OK;

    esp_err_t err = app_settings_save_locked();
    if (err == ESP_OK)
        s_dirty = false;
    return err;
}

esp_err_t app_settings_reset_to_defaults(bool persist_now)
{
    if (!s_initialized)
        return ESP_ERR_INVALID_STATE;

    app_settings_t def;
    app_settings_defaults(&def);
    s_cfg = def;
    app_settings_apply_to_runtime();
    s_dirty = true;
    if (persist_now)
        return app_settings_flush_now();
    app_settings_mark_dirty();
    return ESP_OK;
}
