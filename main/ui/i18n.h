#pragma once

#include <stdbool.h>
#include <stdint.h>

/* The UI is intentionally a flat key table: the firmware has one screen and
 * a short menu, so gettext-style catalogs would cost more than they save.
 * Every key MUST exist in every language: the build checks the row count and
 * test/test_i18n.c checks each entry. */
typedef enum
{
    UI_LANG_EN = 0,
    UI_LANG_PL,
    UI_LANG_COUNT,
} ui_lang_t;

typedef enum
{
    TXT_BRAND = 0,
    TXT_CREDIT,
    TXT_WAITING,
    TXT_PLUG_IN,
    TXT_DISCONNECTED,
    TXT_RECONNECT,
    TXT_STOPPED,
    TXT_STOPPED_DETAIL,
    TXT_CONNECTING,
    TXT_CONNECT_DETAIL,
    TXT_PROBLEM,

    TXT_DETAIL_SYNCED,
    TXT_DETAIL_ON,
    TXT_DETAIL_STARTING,
    TXT_DETAIL_ENABLING,
    TXT_DETAIL_REENABLING,
    TXT_DETAIL_NO_REPLIES,
    TXT_DETAIL_OPENING,
    TXT_DETAIL_SYNCING,
    TXT_DETAIL_READING,
    TXT_DETAIL_STREAMING,
    TXT_DETAIL_FOUND,
    TXT_DETAIL_CONFIGURING,
    TXT_DETAIL_RUNNING,

    TXT_STAGE_USB,
    TXT_STAGE_AUDIO,
    TXT_STAGE_CONTROL,
    TXT_STAGE_RADIO,
    TXT_STAGE_IQ,
    TXT_STAGE_SPECTRUM,

    TXT_SET_LEVEL,
    TXT_SET_FLOOR,
    TXT_SET_CEIL,
    TXT_SET_GAMMA,
    TXT_SET_SPAN,
    TXT_SET_SPEED,
    TXT_SET_PALETTE,
    TXT_SET_DISPLAY,
    TXT_SET_CW_PITCH,
    TXT_SET_LANGUAGE,

    TXT_VAL_FULL,
    TXT_VAL_IF_CENTRED,
    TXT_VAL_TRACE,
    TXT_VAL_TRACE_PEAK,
    TXT_VAL_PEAK,
    TXT_VAL_OFF,

    TXT_LANG_EN,
    TXT_LANG_PL,

    TXT_COUNT,
} ui_text_t;

/* Active language, used by every screen. */
void ui_i18n_set_lang(uint8_t lang);
uint8_t ui_i18n_get_lang(void);

/* String lookup for the active language; falls back to English. */
const char *ui_tr(ui_text_t key);

/* Direct lookup, for tests and language pickers. */
const char *ui_i18n_text(ui_lang_t lang, ui_text_t key);

/* Pick the first non-empty string. Exposed so the host test can exercise the
 * English fallback without a deliberately half-translated table. */
const char *ui_i18n_resolve(const char *translated, const char *fallback);

/* Accepts "en"/"pl" (case-insensitive). Returns false for unknown codes. */
bool ui_lang_parse(const char *code, uint8_t *out);
const char *ui_lang_code(ui_lang_t lang);
