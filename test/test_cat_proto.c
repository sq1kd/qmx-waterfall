/* Host test for the CAT frame parsers.
 *
 * service.c strips the ';' delimiter before dispatch, so the string parsers
 * see bare frames; the numeric parsers also accept a trailing delimiter when
 * they scan for digits. The IF payload uses the TS-480 positions.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cat/proto.h"

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

static void test_iq_value(void)
{
    bool on = false;

    CHECK(cat_proto_parse_iq_value("Q90", &on) && !on, "Q90 must report IQ off");
    CHECK(cat_proto_parse_iq_value("Q91", &on) && on, "Q91 must report IQ on");
    CHECK(cat_proto_parse_iq_value("Q90;", &on) && !on, "Q90; must report IQ off");
    CHECK(cat_proto_parse_iq_value("Q91;", &on) && on, "Q91; must report IQ on");
    CHECK(!cat_proto_parse_iq_value("Q9", &on), "Q9 without a value must be rejected");
    CHECK(!cat_proto_parse_iq_value("X91", &on), "a non-Q9 prefix must be rejected");
    CHECK(!cat_proto_parse_iq_value(NULL, &on), "NULL frame must be rejected");
    CHECK(!cat_proto_parse_iq_value("Q91", NULL), "NULL output must be rejected");
}

static void test_mode(void)
{
    char out[16];

    memset(out, 0, sizeof(out));
    CHECK(cat_proto_parse_mode("MD3", out, sizeof(out)) && strcmp(out, "3") == 0,
          "MD3 must yield mode \"3\"");
    CHECK(cat_proto_parse_mode("MD7", out, sizeof(out)) && strcmp(out, "7") == 0,
          "MD7 must yield mode \"7\"");
    CHECK(!cat_proto_parse_mode("MD", out, sizeof(out)), "an empty MD frame must be rejected");
    CHECK(!cat_proto_parse_mode("XX3", out, sizeof(out)), "a non-MD prefix must be rejected");
    CHECK(!cat_proto_parse_mode(NULL, out, sizeof(out)), "NULL frame must be rejected");
    CHECK(!cat_proto_parse_mode("MD3", NULL, sizeof(out)), "NULL output must be rejected");
    CHECK(!cat_proto_parse_mode("MD3", out, 0), "a zero-length output must be rejected");
}

static void test_mode_code(void)
{
    CHECK(cat_proto_parse_mode_code("0") == 0, "mode 0");
    CHECK(cat_proto_parse_mode_code("1") == 1, "mode 1");
    CHECK(cat_proto_parse_mode_code("3") == 3, "mode 3");
    CHECK(cat_proto_parse_mode_code("7") == 7, "mode 7");
    CHECK(cat_proto_parse_mode_code("") == 0, "empty mode string");
    CHECK(cat_proto_parse_mode_code(NULL) == 0, "NULL mode string");
    CHECK(cat_proto_parse_mode_code("USB") == 0, "a non-numeric mode string");
    CHECK(cat_proto_parse_mode_code("-1") == 0, "negative mode string");
    CHECK(cat_proto_parse_mode_code("999") == 255, "mode codes clamp to 255");
    CHECK(cat_proto_parse_mode_code("99999999999999999999999999999999") == 255,
          "absurdly long mode codes saturate instead of overflowing");
}

static void test_filter(void)
{
    char out[24];
    uint32_t hz = 0;

    memset(out, 0, sizeof(out));
    CHECK(cat_proto_parse_filter("FW500", out, sizeof(out), &hz),
          "FW500 must parse");
    CHECK(hz == 500, "FW500 must yield 500 Hz, got %u", (unsigned)hz);
    CHECK(strcmp(out, "500") == 0, "FW500 must expose filter \"500\", got \"%s\"", out);

    CHECK(cat_proto_parse_filter("BW2400", out, sizeof(out), &hz),
          "BW2400 must parse");
    CHECK(hz == 2400, "BW2400 must yield 2400 Hz, got %u", (unsigned)hz);
    CHECK(strcmp(out, "2400") == 0, "BW2400 must expose filter \"2400\", got \"%s\"", out);

    CHECK(cat_proto_parse_filter("FW500;", out, sizeof(out), &hz),
          "FW500; must parse");
    CHECK(hz == 500, "FW500; must yield 500 Hz, got %u", (unsigned)hz);

    hz = 7;
    CHECK(!cat_proto_parse_filter("FW", out, sizeof(out), &hz),
          "FW without digits must be rejected");
    CHECK(hz == 0, "FW without digits must yield 0, got %u", (unsigned)hz);
    hz = 7;
    CHECK(!cat_proto_parse_filter("BW", out, sizeof(out), &hz),
          "BW without digits must be rejected");
    CHECK(hz == 0, "BW without digits must yield 0, got %u", (unsigned)hz);

    CHECK(cat_proto_parse_filter("FWabc", out, sizeof(out), &hz),
          "FWabc keeps the current acceptance rule");
    CHECK(hz == 0, "FWabc must yield 0, got %u", (unsigned)hz);

    CHECK(cat_proto_parse_filter("FW99999999999999999999", out, sizeof(out), &hz),
          "an overlong filter must still parse");
    CHECK(hz == UINT32_MAX, "an overlong filter must saturate, got %u", (unsigned)hz);
    CHECK(cat_proto_parse_filter("FW99999999999999999999;", out, sizeof(out), &hz),
          "an overlong terminated filter must still parse");
    CHECK(hz == UINT32_MAX, "an overlong terminated filter must saturate, got %u",
          (unsigned)hz);
}

static void test_version(void)
{
    char out[16];

    memset(out, 0, sizeof(out));
    CHECK(cat_proto_parse_version("VN1_04_010QMX", out, sizeof(out)),
          "VN with the QMX model suffix must parse");
    CHECK(strcmp(out, "1_04_010") == 0, "model suffix must be stripped, got \"%s\"", out);

    CHECK(cat_proto_parse_version("VN1_04_010", out, sizeof(out)) &&
              strcmp(out, "1_04_010") == 0,
          "VN without a suffix must parse");
    CHECK(cat_proto_parse_version("VN1_04_010QMX ", out, sizeof(out)) &&
              strcmp(out, "1_04_010") == 0,
          "trailing spaces must be trimmed before the suffix");
    CHECK(!cat_proto_parse_version("VN", out, sizeof(out)), "an empty VN must be rejected");
    CHECK(!cat_proto_parse_version("VN1_04_01x", out, sizeof(out)),
          "a non-digit version character must be rejected");
    CHECK(!cat_proto_parse_version("VN1_04_010QM", out, sizeof(out)),
          "a partial model suffix must be rejected");
    CHECK(!cat_proto_parse_version("VN1_04_010QMX", out, 5),
          "a version longer than the output must be rejected");
    CHECK(!cat_proto_parse_version(NULL, out, sizeof(out)), "NULL frame must be rejected");
    CHECK(!cat_proto_parse_version("VN1_04_010QMX", NULL, sizeof(out)),
          "NULL output must be rejected");
}

static void test_if(void)
{
    const char *neg_rit = "IF0000000000000000-01231aaaaa3bbbbbb";
    const char *pos_rit = "IF0000000000000000+04560aaaaa7bbbbbb";
    uint8_t mode_code = 0xFF;
    int32_t rit_hz = 999;
    bool rit_on = true;

    CHECK(cat_proto_parse_if(neg_rit, &mode_code, &rit_hz, &rit_on),
          "a full IF payload must parse");
    CHECK(mode_code == 3, "IF mode at offset 27, got %u", (unsigned)mode_code);
    CHECK(rit_hz == -123, "IF RIT must be -123, got %ld", (long)rit_hz);
    CHECK(rit_on, "IF RIT status at offset 21 must be on");

    CHECK(cat_proto_parse_if(pos_rit, &mode_code, &rit_hz, &rit_on),
          "a positive RIT payload must parse");
    CHECK(mode_code == 7, "IF mode 7, got %u", (unsigned)mode_code);
    CHECK(rit_hz == 456, "IF RIT must be +456, got %ld", (long)rit_hz);
    CHECK(!rit_on, "IF RIT status 0 must be off");

    CHECK(cat_proto_parse_if(neg_rit, NULL, NULL, NULL),
          "IF outputs are optional");

    CHECK(!cat_proto_parse_if("IF", &mode_code, &rit_hz, &rit_on),
          "a short IF payload must be rejected");
    CHECK(!cat_proto_parse_if("IF0000000000000000-01231aaaaa3bbbbb", &mode_code, &rit_hz,
                              &rit_on),
          "a 33-character IF payload must be rejected");
    CHECK(!cat_proto_parse_if("IF0000000000000000x01231aaaaa3bbbbbb", &mode_code, &rit_hz,
                              &rit_on),
          "a bad RIT sign must be rejected");
    CHECK(!cat_proto_parse_if("IF0000000000000000-01232aaaaa3bbbbbb", &mode_code, &rit_hz,
                              &rit_on),
          "a RIT status other than 0/1 must be rejected");
    CHECK(!cat_proto_parse_if("IF0000000000000000-01231aaaaaxbbbbbb", &mode_code, &rit_hz,
                              &rit_on),
          "a non-digit IF mode must be rejected");
    CHECK(!cat_proto_parse_if(NULL, &mode_code, &rit_hz, &rit_on),
          "NULL frame must be rejected");
}

int main(void)
{
    test_iq_value();
    test_mode();
    test_mode_code();
    test_filter();
    test_version();
    test_if();

    if (s_failures)
    {
        printf("%d check(s) failed\n", s_failures);
        return 1;
    }
    printf("cat proto tests passed\n");
    return 0;
}
