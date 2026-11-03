#include "ui/i18n.h"

#include <ctype.h>
#include <stddef.h>

/* The outer dimension is inferred so a language added to ui_lang_t without a
 * row here fails the build instead of silently returning empty strings. */
static const char *const s_text[][TXT_COUNT] = {
    [UI_LANG_EN] = {
        [TXT_BRAND] = "QMX-waterfall",
        [TXT_CREDIT] = "by SQ1KD",
        [TXT_WAITING] = "Waiting for device",
        [TXT_PLUG_IN] = "Plug in USB audio + CAT",
        [TXT_DISCONNECTED] = "Device disconnected",
        [TXT_RECONNECT] = "Reconnect USB to continue",
        [TXT_STOPPED] = "Stopped",
        [TXT_STOPPED_DETAIL] = "Power cycle to restart",
        [TXT_CONNECTING] = "Connecting",
        [TXT_CONNECT_DETAIL] = "USB audio + CAT",
        [TXT_PROBLEM] = "Connection problem",

        [TXT_DETAIL_SYNCED] = "synced",
        [TXT_DETAIL_ON] = "on",
        [TXT_DETAIL_STARTING] = "starting",
        [TXT_DETAIL_ENABLING] = "enabling",
        [TXT_DETAIL_REENABLING] = "re-enabling",
        [TXT_DETAIL_NO_REPLIES] = "no replies",
        [TXT_DETAIL_OPENING] = "opening",
        [TXT_DETAIL_SYNCING] = "syncing",
        [TXT_DETAIL_READING] = "reading",
        [TXT_DETAIL_STREAMING] = "streaming",
        [TXT_DETAIL_FOUND] = "found",
        [TXT_DETAIL_CONFIGURING] = "configuring",
        [TXT_DETAIL_RUNNING] = "running",

        [TXT_STAGE_USB] = "USB",
        [TXT_STAGE_AUDIO] = "Audio",
        [TXT_STAGE_CONTROL] = "CAT",
        [TXT_STAGE_RADIO] = "Radio",
        [TXT_STAGE_IQ] = "IQ",
        [TXT_STAGE_SPECTRUM] = "Spectrum",

        [TXT_SET_LEVEL] = "Level",
        [TXT_SET_FLOOR] = "Low",
        [TXT_SET_CEIL] = "High",
        [TXT_SET_GAMMA] = "Gamma",
        [TXT_SET_SPAN] = "Span",
        [TXT_SET_SPEED] = "Speed",
        [TXT_SET_PALETTE] = "Palette",
        [TXT_SET_DISPLAY] = "Display",
        [TXT_SET_CW_PITCH] = "CW pitch",
        [TXT_SET_LANGUAGE] = "Language",

        [TXT_VAL_FULL] = "Full",
        [TXT_VAL_IF_CENTRED] = "IF centred",
        [TXT_VAL_TRACE] = "Trace",
        [TXT_VAL_TRACE_PEAK] = "Trace+peak",
        [TXT_VAL_PEAK] = "Peak",
        [TXT_VAL_OFF] = "Off",

        [TXT_LANG_EN] = "English",
        [TXT_LANG_PL] = "Polski",
    },
    [UI_LANG_PL] = {
        [TXT_BRAND] = "QMX-waterfall",
        [TXT_CREDIT] = "by SQ1KD",
        [TXT_WAITING] = "Czekam na urządzenie",
        [TXT_PLUG_IN] = "Podłącz USB audio + CAT",
        [TXT_DISCONNECTED] = "Urządzenie odłączone",
        [TXT_RECONNECT] = "Podłącz USB ponownie",
        [TXT_STOPPED] = "Zatrzymano",
        [TXT_STOPPED_DETAIL] = "Wyłącz i włącz zasilanie",
        [TXT_CONNECTING] = "Łączenie",
        [TXT_CONNECT_DETAIL] = "USB audio + CAT",
        [TXT_PROBLEM] = "Problem z połączeniem",

        [TXT_DETAIL_SYNCED] = "zsynchronizowano",
        [TXT_DETAIL_ON] = "wł.",
        [TXT_DETAIL_STARTING] = "uruchamianie",
        [TXT_DETAIL_ENABLING] = "włączanie",
        [TXT_DETAIL_REENABLING] = "ponowne włączanie",
        [TXT_DETAIL_NO_REPLIES] = "brak odpowiedzi",
        [TXT_DETAIL_OPENING] = "otwieranie",
        [TXT_DETAIL_SYNCING] = "synchronizacja",
        [TXT_DETAIL_READING] = "odczyt",
        [TXT_DETAIL_STREAMING] = "strumień",
        [TXT_DETAIL_FOUND] = "znaleziono",
        [TXT_DETAIL_CONFIGURING] = "konfiguracja",
        [TXT_DETAIL_RUNNING] = "działa",

        [TXT_STAGE_USB] = "USB",
        [TXT_STAGE_AUDIO] = "Audio",
        [TXT_STAGE_CONTROL] = "CAT",
        [TXT_STAGE_RADIO] = "Radio",
        [TXT_STAGE_IQ] = "IQ",
        [TXT_STAGE_SPECTRUM] = "Widmo",

        [TXT_SET_LEVEL] = "Poziom",
        [TXT_SET_FLOOR] = "Dół",
        [TXT_SET_CEIL] = "Góra",
        [TXT_SET_GAMMA] = "Gamma",
        [TXT_SET_SPAN] = "Zakres",
        [TXT_SET_SPEED] = "Szybkość",
        [TXT_SET_PALETTE] = "Paleta",
        [TXT_SET_DISPLAY] = "Widok",
        [TXT_SET_CW_PITCH] = "Ton CW",
        [TXT_SET_LANGUAGE] = "Język",

        [TXT_VAL_FULL] = "Cały",
        [TXT_VAL_IF_CENTRED] = "Wokół pośredniej",
        [TXT_VAL_TRACE] = "Ślad",
        [TXT_VAL_TRACE_PEAK] = "Ślad+szczyt",
        [TXT_VAL_PEAK] = "Szczyt",
        [TXT_VAL_OFF] = "Wył.",

        [TXT_LANG_EN] = "English",
        [TXT_LANG_PL] = "Polski",
    },
};

_Static_assert(sizeof(s_text) / sizeof(s_text[0]) == UI_LANG_COUNT,
               "string table must have one row per language");

static volatile uint8_t s_lang = UI_LANG_EN;

void ui_i18n_set_lang(uint8_t lang)
{
    if (lang >= UI_LANG_COUNT)
        lang = UI_LANG_EN;
    s_lang = lang;
}

uint8_t ui_i18n_get_lang(void)
{
    return s_lang;
}

const char *ui_i18n_text(ui_lang_t lang, ui_text_t key)
{
    if (lang >= UI_LANG_COUNT || key >= TXT_COUNT)
        return "";
    const char *text = s_text[lang][key];
    return text ? text : "";
}

const char *ui_i18n_resolve(const char *translated, const char *fallback)
{
    if (translated != NULL && translated[0] != '\0')
        return translated;
    return fallback != NULL ? fallback : "";
}

const char *ui_tr(ui_text_t key)
{
    return ui_i18n_resolve(ui_i18n_text((ui_lang_t)s_lang, key),
                           ui_i18n_text(UI_LANG_EN, key));
}

bool ui_lang_parse(const char *code, uint8_t *out)
{
    if (!code || !out || code[0] == '\0')
        return false;

    const char c0 = (char)tolower((unsigned char)code[0]);
    const char c1 = (char)tolower((unsigned char)code[1]);

    if (c0 == 'e' && c1 == 'n' && code[2] == '\0')
    {
        *out = UI_LANG_EN;
        return true;
    }
    if (c0 == 'p' && c1 == 'l' && code[2] == '\0')
    {
        *out = UI_LANG_PL;
        return true;
    }
    return false;
}

const char *ui_lang_code(ui_lang_t lang)
{
    return lang == UI_LANG_PL ? "pl" : "en";
}
