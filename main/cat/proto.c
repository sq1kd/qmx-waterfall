#include "cat/proto.h"

#include <string.h>

static bool parse_ndigits(const char *p, int n, int *out)
{
    if (!p || !out || n <= 0)
    {
        return false;
    }
    int v = 0;
    for (int i = 0; i < n; i++)
    {
        char c = p[i];
        if (c < '0' || c > '9')
        {
            return false;
        }
        v = v * 10 + (c - '0');
    }
    *out = v;
    return true;
}

static bool parse_signed_rit_5(const char *p, int32_t *out)
{
    if (!p || !out)
    {
        return false;
    }
    char sign = p[0];
    if (sign != '+' && sign != '-')
    {
        return false;
    }

    int mag = 0;
    if (!parse_ndigits(p + 1, 4, &mag))
    {
        return false;
    }
    *out = (sign == '-') ? -(int32_t)mag : (int32_t)mag;
    return true;
}

bool cat_proto_parse_iq_value(const char *frame, bool *iq_on)
{
    if (!frame || !iq_on || strncmp(frame, "Q9", 2) != 0)
    {
        return false;
    }
    size_t n = strlen(frame);
    for (size_t i = n; i > 0; i--)
    {
        char c = frame[i - 1];
        if (c == '0' || c == '1')
        {
            *iq_on = (c == '1');
            return true;
        }
    }
    return false;
}

bool cat_proto_parse_mode(const char *frame, char *out, size_t out_len)
{
    if (!frame || !out || out_len == 0 || strncmp(frame, "MD", 2) != 0)
    {
        return false;
    }
    strlcpy(out, frame + 2, out_len);
    return out[0] != '\0';
}

uint8_t cat_proto_parse_mode_code(const char *mode)
{
    if (!mode)
    {
        return 0;
    }

    uint32_t v = 0;
    bool have_digits = false;
    for (const char *p = mode; *p >= '0' && *p <= '9'; p++)
    {
        have_digits = true;
        if (v <= 255u)
        {
            v = v * 10u + (uint32_t)(*p - '0');
            if (v > 255u)
            {
                v = 255u;
            }
        }
    }
    return have_digits ? (uint8_t)v : 0;
}

bool cat_proto_parse_filter(const char *frame, char *out, size_t out_len, uint32_t *hz)
{
    const char *p = NULL;
    if (strncmp(frame, "FW", 2) == 0 || strncmp(frame, "BW", 2) == 0)
    {
        p = frame + 2;
    }
    else
    {
        return false;
    }

    strlcpy(out, p, out_len);
    uint32_t v = 0;
    bool have_digits = false;
    while (*p)
    {
        if (*p >= '0' && *p <= '9')
        {
            uint32_t digit = (uint32_t)(*p - '0');
            have_digits = true;
            if (v > (UINT32_MAX - digit) / 10u)
            {
                v = UINT32_MAX;
            }
            else
            {
                v = v * 10u + digit;
            }
        }
        else if (have_digits)
        {
            break;
        }
        p++;
    }
    *hz = have_digits ? v : 0;
    return out[0] != '\0';
}

bool cat_proto_parse_version(const char *frame, char *out, size_t out_len)
{
    if (!frame || !out || out_len == 0 || strncmp(frame, "VN", 2) != 0)
    {
        return false;
    }

    const char *p = frame + 2;
    size_t n = strlen(p);
    while (n > 0 && p[n - 1] == ' ')
    {
        n--;
    }
    /* QMX appends its model id: "1_04_010QMX". */
    if (n > 3 && strncmp(p + n - 3, "QMX", 3) == 0)
    {
        n -= 3;
    }
    if (n == 0 || n >= out_len)
    {
        return false;
    }
    for (size_t i = 0; i < n; i++)
    {
        char c = p[i];
        if ((c < '0' || c > '9') && c != '_' && c != '.')
        {
            return false;
        }
    }
    memcpy(out, p, n);
    out[n] = '\0';
    return true;
}

enum
{
    IF_RIT_POS = 16,
    IF_RIT_STAT_POS = 21,
    IF_MODE_POS = 27,
    IF_MIN_LEN = 34,
};

bool cat_proto_parse_if(const char *frame, uint8_t *mode_code, int32_t *rit_hz, bool *rit_on)
{
    if (!frame || strncmp(frame, "IF", 2) != 0)
    {
        return false;
    }

    const char *p = frame + 2;
    size_t n = strlen(p);
    if (n < IF_MIN_LEN)
    {
        return false;
    }

    /* TS-480 style IF payload, offsets after "IF": RIT (+/-dddd) at +16,
     * RIT status at +21, mode at +27. The payload is at least IF_MIN_LEN. */
    int32_t rit = 0;
    if (!parse_signed_rit_5(p + IF_RIT_POS, &rit))
    {
        return false;
    }

    char rit_status = p[IF_RIT_STAT_POS];
    if (rit_status != '0' && rit_status != '1')
    {
        return false;
    }

    int mode = 0;
    if (!parse_ndigits(p + IF_MODE_POS, 1, &mode))
    {
        return false;
    }

    if (mode_code)
    {
        *mode_code = (uint8_t)mode;
    }
    if (rit_hz)
    {
        *rit_hz = rit;
    }
    if (rit_on)
    {
        *rit_on = (rit_status == '1');
    }
    return true;
}
