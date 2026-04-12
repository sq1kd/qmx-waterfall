#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct
{
    float level_offset_db;
    uint8_t view_mode;
    float wf_floor_db;
    float wf_ceil_db;
    uint16_t cw_pitch_hz;
    float wf_gamma;
    uint8_t wf_palette;
    float spec_alpha;
    float wf_condense;
    float wf_temporal_beta;
    bool wf_bilinear;
} app_settings_t;

void app_settings_defaults(app_settings_t *cfg);
esp_err_t app_settings_init(void);
void app_settings_get(app_settings_t *out);
void app_settings_apply_to_runtime(void);

esp_err_t app_settings_set_level_offset_db(float offset_db);
esp_err_t app_settings_set_view_mode(uint8_t mode);
esp_err_t app_settings_set_wf_range(float floor_db, float ceil_db);
esp_err_t app_settings_set_cw_pitch_hz(uint16_t hz);
esp_err_t app_settings_set_wf_gamma(float gamma);
esp_err_t app_settings_set_wf_palette(uint8_t palette);
esp_err_t app_settings_set_spec_alpha(float alpha);
esp_err_t app_settings_set_wf_condense(float factor);
esp_err_t app_settings_set_wf_temporal_beta(float beta);
esp_err_t app_settings_set_wf_bilinear(bool enabled);

esp_err_t app_settings_flush_now(void);
esp_err_t app_settings_reset_to_defaults(bool persist_now);
