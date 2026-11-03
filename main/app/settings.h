#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "ui/i18n.h"

typedef struct
{
    float level_offset_db; /* dB added to the spectrum/waterfall level       */
    uint8_t view_mode;     /* 0 = full band, 1 = IF-centred view              */
    float wf_floor_db;     /* palette floor                                  */
    float wf_ceil_db;      /* palette ceiling                                */
    float wf_gamma;        /* waterfall colour curve; 1.0 = linear           */
    uint16_t cw_pitch_hz;  /* CW beat note used for the marker offset        */
    uint8_t wf_palette;    /* palette index                                  */
    uint8_t speed_level;   /* 1..N waterfall speed level (no unit in UI)     */
    uint8_t spec_mode;     /* 0 trace, 1 trace+peak, 2 peak only, 3 off      */
    uint8_t language;      /* ui_lang_t: 0 English, 1 Polish                 */
} app_settings_t;

/* Every settable value is described once here; the UI, the CLI and the
 * validator all read the same ranges, steps and defaults. Units are field
 * units: dB for LEVEL_OFF/FLOOR/CEIL, index for the enums, level number for
 * SPEED, Hz for CW_PITCH. */
typedef enum
{
    APP_FIELD_LEVEL_OFF = 0,
    APP_FIELD_VIEW_MODE,
    APP_FIELD_WF_FLOOR,
    APP_FIELD_WF_CEIL,
    APP_FIELD_GAMMA,
    APP_FIELD_SPEED,
    APP_FIELD_PALETTE,
    APP_FIELD_SPEC_MODE,
    APP_FIELD_CW_PITCH,
    APP_FIELD_LANGUAGE,
    APP_FIELD_COUNT,
} app_field_id_t;

typedef struct
{
    float min;
    float max; /* < 0 means "dynamic from the spectrum API" */
    float step;
    float def;
} app_field_spec_t;

void app_settings_defaults(app_settings_t *cfg);
esp_err_t app_settings_init(void);
void app_settings_get(app_settings_t *out);
void app_settings_apply_to_runtime(void);

const app_field_spec_t *app_settings_field_spec(app_field_id_t id);
float app_settings_field_get(const app_settings_t *cfg, app_field_id_t id);
esp_err_t app_settings_field_set(app_field_id_t id, float value);
float app_settings_field_clamp(app_field_id_t id, float value);

esp_err_t app_settings_set_level_offset_db(float offset_db);
esp_err_t app_settings_set_view_mode(uint8_t mode);
esp_err_t app_settings_set_wf_range(float floor_db, float ceil_db);
esp_err_t app_settings_set_cw_pitch_hz(uint16_t hz);
esp_err_t app_settings_set_wf_palette(uint8_t palette);
esp_err_t app_settings_set_speed_level(uint8_t level);
esp_err_t app_settings_set_spec_mode(uint8_t mode);
esp_err_t app_settings_set_language(uint8_t lang);

esp_err_t app_settings_flush_now(void);
esp_err_t app_settings_reset_to_defaults(bool persist_now);
