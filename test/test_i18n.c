/* Host test for the UI string table.
 *
 * The firmware falls back to English for a missing string, which would ship a
 * half-translated screen silently. Pin that every key exists in every language
 * and that the language selector parses exactly the advertised codes.
 */
#include <stdio.h>
#include <string.h>

#include "ui/i18n.h"

static int s_failures;

#define CHECK(cond, ...)                                \
    do                                                  \
    {                                                   \
        if (!(cond))                                    \
        {                                               \
            s_failures++;                               \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                        \
            printf("\n");                               \
        }                                               \
    } while (0)

int main(void)
{
    for (int lang = 0; lang < UI_LANG_COUNT; lang++)
    {
        for (int key = 0; key < TXT_COUNT; key++)
        {
            const char *text = ui_i18n_text((ui_lang_t)lang, (ui_text_t)key);
            CHECK(text != NULL && text[0] != '\0',
                  "missing string lang=%d key=%d", lang, key);
        }
    }

    uint8_t lang = 0xFF;
    CHECK(ui_lang_parse("en", &lang) && lang == UI_LANG_EN, "parse en");
    CHECK(ui_lang_parse("PL", &lang) && lang == UI_LANG_PL, "parse PL");
    CHECK(!ui_lang_parse("de", &lang), "parse de must fail");
    CHECK(!ui_lang_parse("", &lang), "parse empty must fail");
    CHECK(strcmp(ui_lang_code(UI_LANG_EN), "en") == 0, "code en");
    CHECK(strcmp(ui_lang_code(UI_LANG_PL), "pl") == 0, "code pl");

    ui_i18n_set_lang(UI_LANG_PL);
    CHECK(strcmp(ui_tr(TXT_DISCONNECTED), "Urządzenie odłączone") == 0,
          "active language is not Polish");

    if (s_failures == 0)
        printf("i18n: ok\n");
    return s_failures ? 1 : 0;
}
