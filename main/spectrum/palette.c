#include "spectrum/palette.h"

#include "spectrum/render.h"

#define PAL_COUNT 7

typedef struct
{
    uint8_t r;
    uint8_t g;
    uint8_t b;
} rgb8_t;

static const uint16_t turbo_lut[256] = {
    0x1811, 0x2011, 0x2011, 0x2012, 0x2012, 0x2812, 0x2812, 0x2813,
    0x2813, 0x2813, 0x2814, 0x3014, 0x3014, 0x3015, 0x3015, 0x3016,
    0x3016, 0x3016, 0x3017, 0x3017, 0x3818, 0x3018, 0x3819, 0x3819,
    0x381A, 0x381A, 0x381B, 0x381B, 0x381C, 0x381C, 0x381D, 0x381D,
    0x381E, 0x381E, 0x381F, 0x381F, 0x383F, 0x303F, 0x303F, 0x305F,
    0x305F, 0x305F, 0x287F, 0x287F, 0x287F, 0x289F, 0x289F, 0x209F,
    0x20BF, 0x20BF, 0x20BF, 0x18DF, 0x18DF, 0x18DF, 0x10FF, 0x10FF,
    0x10FF, 0x091F, 0x091F, 0x011F, 0x013F, 0x013F, 0x013F, 0x015F,
    0x015F, 0x017F, 0x017F, 0x019F, 0x019F, 0x01BF, 0x01BF, 0x01DF,
    0x01DF, 0x01FF, 0x01FF, 0x021F, 0x021F, 0x023F, 0x023F, 0x025F,
    0x027F, 0x027F, 0x029F, 0x029F, 0x02BF, 0x02DF, 0x02DF, 0x02FF,
    0x031F, 0x031F, 0x033F, 0x035F, 0x035F, 0x037F, 0x039F, 0x03BF,
    0x03BF, 0x03DF, 0x03FF, 0x041F, 0x043F, 0x043F, 0x045F, 0x047F,
    0x049F, 0x04BF, 0x04DF, 0x04FF, 0x051F, 0x053F, 0x055F, 0x057F,
    0x059F, 0x05BF, 0x05DF, 0x05FF, 0x061F, 0x063F, 0x065F, 0x067F,
    0x069F, 0x06BF, 0x06DF, 0x06FF, 0x071F, 0x0F1F, 0x0F3F, 0x173F,
    0x1F3F, 0x273F, 0x273F, 0x2F3F, 0x373F, 0x3F1F, 0x471F, 0x4F1F,
    0x571F, 0x5F1F, 0x671F, 0x6F1F, 0x771F, 0x7EFF, 0x86FF, 0x8EFF,
    0x96FF, 0x9EDF, 0xA6DF, 0xAEDF, 0xB6BF, 0xBEBF, 0xC6BF, 0xCE9F,
    0xD69F, 0xDE7F, 0xE67F, 0xEE5F, 0xF65F, 0xFE3F, 0xFE3F, 0xFE5F,
    0xFE5F, 0xFE7F, 0xFE7F, 0xFE9F, 0xFE9F, 0xFEBF, 0xFEBF, 0xFEDF,
    0xFEDF, 0xFEFF, 0xFEFF, 0xFF1F, 0xFF1F, 0xFF3F, 0xFF3F, 0xFF5F,
    0xFF5F, 0xFF7F, 0xFF7F, 0xFF9F, 0xFF9F, 0xFFBF, 0xFFBF, 0xFFBF,
    0xFFDF, 0xFFDF, 0xFFDF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
    0xFFFF, 0xFFFF, 0xFFFF, 0xFFDF, 0xFFDF, 0xFFBF, 0xFFBF, 0xFF9F,
    0xFF9F, 0xFF7F, 0xFF7F, 0xFF5F, 0xFF3F, 0xFF3F, 0xFF1F, 0xFEFF,
    0xFEDF, 0xFEDF, 0xFEBF, 0xFE9F, 0xFE7F, 0xFE5F, 0xFE3F, 0xFE1F,
    0xFDFF, 0xFDDF, 0xFDBF, 0xFD9F, 0xFD7F, 0xFD5F, 0xFD3F, 0xFD1F,
    0xFCFF, 0xFCDF, 0xFCBF, 0xFC9F, 0xFC7F, 0xFC5F, 0xFC3F, 0xFC1F,
    0xFBFF, 0xFBDF, 0xFBBF, 0xFB9F, 0xFB7F, 0xFB5F, 0xFB3F, 0xFB1F,
    0xFAFF, 0xFADF, 0xFABF, 0xFA9F, 0xFA7F, 0xFA5F, 0xFA3F, 0xFA1F,
    0xF9FF, 0xF9DF, 0xF1BF, 0xF1BF, 0xF19F, 0xE99F, 0xE97F, 0xE17F,
};

static uint16_t s_palette_lut[PAL_COUNT][256];
static int s_palette_lut_init;

static uint16_t gray_lut_color(uint8_t idx)
{
    return spectrum_rgb565(idx, idx, idx);
}

static rgb8_t lerp_rgb(rgb8_t a, rgb8_t b, float t)
{
    rgb8_t o;
    o.r = (uint8_t)(a.r + (b.r - a.r) * t + 0.5f);
    o.g = (uint8_t)(a.g + (b.g - a.g) * t + 0.5f);
    o.b = (uint8_t)(a.b + (b.b - a.b) * t + 0.5f);
    return o;
}

static void build_palette_from_stops(uint16_t *dst,
                                     const rgb8_t *stops,
                                     int stop_count)
{
    if (!dst || !stops || stop_count < 2)
        return;

    for (int i = 0; i < 256; i++)
    {
        float p = (float)i / 255.0f;
        float segf = p * (float)(stop_count - 1);
        int seg = (int)segf;
        if (seg >= stop_count - 1)
            seg = stop_count - 2;
        float t = segf - (float)seg;
        rgb8_t c = lerp_rgb(stops[seg], stops[seg + 1], t);
        dst[i] = spectrum_rgb565(c.r, c.g, c.b);
    }
}

static void ensure_palette_luts(void)
{
    if (s_palette_lut_init)
        return;

    for (int i = 0; i < 256; i++)
    {
        s_palette_lut[0][i] = turbo_lut[i];
        s_palette_lut[1][i] = gray_lut_color((uint8_t)i);
    }

    static const rgb8_t inferno_stops[] = {
        {0, 0, 4}, {31, 12, 72}, {85, 15, 109}, {136, 34, 106},
        {186, 54, 85}, {227, 89, 51}, {249, 140, 10}, {252, 195, 65}, {252, 255, 164}};
    static const rgb8_t viridis_stops[] = {
        {68, 1, 84}, {71, 44, 122}, {59, 81, 139}, {44, 113, 142},
        {33, 144, 141}, {39, 173, 129}, {92, 200, 99}, {170, 220, 50}, {253, 231, 37}};
    static const rgb8_t magma_stops[] = {
        {0, 0, 4}, {27, 16, 90}, {75, 18, 123}, {122, 28, 126},
        {170, 47, 109}, {214, 81, 82}, {243, 135, 72}, {253, 194, 104}, {252, 253, 191}};
    static const rgb8_t plasma_stops[] = {
        {13, 8, 135}, {75, 3, 161}, {125, 3, 168}, {168, 34, 150},
        {203, 70, 121}, {229, 107, 93}, {248, 148, 65}, {253, 195, 40}, {240, 249, 33}};
    static const rgb8_t sdr_bluehot_stops[] = {
        {0, 2, 18}, {0, 8, 58}, {0, 30, 126}, {0, 78, 210}, {0, 150, 248},
        {28, 214, 250}, {180, 236, 92}, {252, 210, 8}, {255, 124, 0}, {250, 36, 0}};

    build_palette_from_stops(s_palette_lut[2], inferno_stops, (int)(sizeof(inferno_stops) / sizeof(inferno_stops[0])));
    build_palette_from_stops(s_palette_lut[3], viridis_stops, (int)(sizeof(viridis_stops) / sizeof(viridis_stops[0])));
    build_palette_from_stops(s_palette_lut[4], magma_stops, (int)(sizeof(magma_stops) / sizeof(magma_stops[0])));
    build_palette_from_stops(s_palette_lut[5], plasma_stops, (int)(sizeof(plasma_stops) / sizeof(plasma_stops[0])));
    build_palette_from_stops(s_palette_lut[6], sdr_bluehot_stops, (int)(sizeof(sdr_bluehot_stops) / sizeof(sdr_bluehot_stops[0])));

    s_palette_lut_init = 1;
}

const uint16_t *spectrum_palette_get_lut(uint8_t palette)
{
    ensure_palette_luts();
    if (palette >= PAL_COUNT)
        palette = 0;
    return s_palette_lut[palette];
}
