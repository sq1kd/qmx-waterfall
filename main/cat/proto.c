#include "cat/proto.h"

#include <stdlib.h>
#include <string.h>

bool cat_proto_parse_iq_value(const char *frame, bool *iq_on)
{
    if (strncmp(frame, "Q9", 2) != 0)
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
    if (strncmp(frame, "MD", 2) != 0)
    {
        return false;
    }
    strlcpy(out, frame + 2, out_len);
    return out[0] != '\0';
}

uint8_t cat_proto_parse_mode_code(const char *mode)
{
    if (!mode || mode[0] == '\0')
    {
        return 0;
    }
    int v = atoi(mode);
    if (v < 0)
    {
        return 0;
    }
    if (v > 255)
    {
        v = 255;
    }
    return (uint8_t)v;
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
            have_digits = true;
            v = v * 10u + (uint32_t)(*p - '0');
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
