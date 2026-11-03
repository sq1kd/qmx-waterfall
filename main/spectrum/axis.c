#include "spectrum/axis.h"

#include "spectrum/layout.h"

#include <stdio.h>

static const uint8_t glyph_3x5[][5] = {
    {0x7, 0x5, 0x5, 0x5, 0x7},
    {0x2, 0x6, 0x2, 0x2, 0x7},
    {0x7, 0x1, 0x7, 0x4, 0x7},
    {0x7, 0x1, 0x7, 0x1, 0x7},
    {0x5, 0x5, 0x7, 0x1, 0x1},
    {0x7, 0x4, 0x7, 0x1, 0x7},
    {0x7, 0x4, 0x7, 0x5, 0x7},
    {0x7, 0x1, 0x1, 0x1, 0x1},
    {0x7, 0x5, 0x7, 0x5, 0x7},
    {0x7, 0x5, 0x7, 0x1, 0x7},
    {0x0, 0x0, 0x7, 0x0, 0x0},
    {0x0, 0x2, 0x7, 0x2, 0x0},
    {0x0, 0x0, 0x0, 0x0, 0x0},
};

static int glyph_index(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c == '-')
        return 10;
    if (c == '+')
        return 11;
    return 12;
}

static void draw_char_3x5(uint16_t *canvas_buf, int disp_w, int disp_h,
                          int x0, int y0, char c, uint16_t color)
{
    int gi = glyph_index(c);
    for (int y = 0; y < 5; y++)
    {
        int yy = y0 + y;
        if (yy < 0 || yy >= disp_h)
            continue;
        uint8_t bits = glyph_3x5[gi][y];
        for (int x = 0; x < 3; x++)
        {
            if (((bits >> (2 - x)) & 0x1) == 0)
                continue;
            int xx = x0 + x;
            if (xx < 0 || xx >= disp_w)
                continue;
            canvas_buf[yy * disp_w + xx] = color;
        }
    }
}

static void draw_text_3x5(uint16_t *canvas_buf, int disp_w, int disp_h,
                          int x0, int y0, const char *txt, uint16_t color)
{
    int x = x0;
    for (const char *p = txt; *p; p++)
    {
        draw_char_3x5(canvas_buf, disp_w, disp_h, x, y0, *p, color);
        x += 4;
    }
}

static int text_px_width_3x5(const char *txt)
{
    int n = 0;
    for (const char *p = txt; *p; p++)
        n++;
    if (n == 0)
        return 0;
    return n * 4 - 1;
}

static int round_div_i32(int v, int d)
{
    if (d <= 0)
        return 0;
    if (v >= 0)
        return (v + d / 2) / d;
    return -((-v + d / 2) / d);
}

static void format_khz_label(int khz, bool always_sign, char *out, int out_sz)
{
    if (out_sz <= 0)
        return;
    if (always_sign)
        (void)snprintf(out, (size_t)out_sz, "%+d", khz);
    else
        (void)snprintf(out, (size_t)out_sz, "%d", khz);
}

/* Labels are relative to the IF passband, not to DC: the waterfall is always
 * tuned to the IF, so "0" must sit where the radio's passband centre is. */
void spectrum_draw_axis_overlay(uint16_t *canvas_buf,
                                int disp_w,
                                int spec_h,
                                uint32_t if_freq_hz,
                                uint8_t view_mode,
                                uint32_t fft_size,
                                uint32_t sample_freq,
                                bool axis_enabled,
                                uint16_t axis_color,
                                uint16_t label_color)
{
    if (!canvas_buf || !axis_enabled || disp_w < 2 || spec_h < 8)
        return;

    int nbins = spectrum_view_bins(view_mode, fft_size);
    if (nbins < 2)
        return;

    int y_base = spec_h - 1;
    uint16_t *row = &canvas_buf[y_base * disp_w];
    for (int x = 0; x < disp_w; x++)
        row[x] = axis_color;

    int fs_khz = round_div_i32((int)sample_freq, 1000);
    int if_khz = round_div_i32((int)if_freq_hz, 1000);

    int y_lbl = spec_h - 8;

    int khz_vals[5];
    int x_anchor[5];
    for (int i = 0; i < 5; i++)
    {
        x_anchor[i] = (i * (disp_w - 1) + 2) / 4;
        /* Full span covers [-fs/2 .. +fs/2); the IF view covers [0 .. fs/2]. */
        int abs_khz = (view_mode == SPECTRUM_VIEW_IF) ? (fs_khz * i) / 8
                                                      : (fs_khz * (i - 2)) / 4;
        khz_vals[i] = abs_khz - if_khz;
    }

    int prev_right = -1000;
    for (int i = 0; i < 5; i++)
    {
        int khz = khz_vals[i];
        char txt[8];
        format_khz_label(khz, khz != 0, txt, (int)sizeof(txt));
        int w = text_px_width_3x5(txt);
        if (w >= disp_w)
            continue;
        int x = x_anchor[i] - w / 2;
        if (x < 0)
            x = 0;
        if (x + w >= disp_w)
            x = disp_w - w;
        if (x <= prev_right)
            x = prev_right + 1;
        if (x + w >= disp_w)
            x = disp_w - w;
        prev_right = x + w;
        draw_text_3x5(canvas_buf, disp_w, spec_h, x, y_lbl, txt, label_color);
    }
}
