#include "spectrum/render.h"

#include <math.h>
#include <stddef.h>

#include "spectrum/palette.h"

uint16_t spectrum_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return ((uint16_t)(r & 0xF8) << 8) |
           ((uint16_t)(g & 0xFC) << 3) |
           ((uint16_t)(b >> 3));
}

static uint16_t blend_rgb565(uint16_t dst, uint16_t src, uint8_t alpha)
{
    uint8_t dr = (uint8_t)((dst >> 11) & 0x1F);
    uint8_t dg = (uint8_t)((dst >> 5) & 0x3F);
    uint8_t db = (uint8_t)(dst & 0x1F);

    uint8_t sr = (uint8_t)((src >> 11) & 0x1F);
    uint8_t sg = (uint8_t)((src >> 5) & 0x3F);
    uint8_t sb = (uint8_t)(src & 0x1F);

    uint16_t r = (uint16_t)(((uint16_t)dr * (255 - alpha) + (uint16_t)sr * alpha + 127) / 255);
    uint16_t g = (uint16_t)(((uint16_t)dg * (255 - alpha) + (uint16_t)sg * alpha + 127) / 255);
    uint16_t b = (uint16_t)(((uint16_t)db * (255 - alpha) + (uint16_t)sb * alpha + 127) / 255);

    return (uint16_t)((r << 11) | (g << 5) | b);
}

static int effective_bins(uint8_t view_mode, uint32_t fft_size)
{
    return view_mode == 1 ? (int)(fft_size / 2) : (int)fft_size;
}

static int bin_to_px(float bin, int disp_w, int nbins)
{
    int px = (int)(bin * (float)(disp_w - 1) / (float)(nbins - 1) + 0.5f);
    if (px < 0)
        px = 0;
    if (px >= disp_w)
        px = disp_w - 1;
    return px;
}

int16_t spectrum_db_to_q8(float db)
{
    if (!isfinite(db))
        return -32767;
    float q = db * SPECTRUM_DB_Q8;
    if (q <= -32767.0f)
        return -32767;
    if (q >= 32767.0f)
        return 32767;
    return (int16_t)lrintf(q);
}

/* Map one stored q8 level to a palette index. `span_q8` is the ceiling floor
 * difference in q8 units, `scale_q16` is (255 << 16) / span_q8. Integer only:
 * this runs for every waterfall pixel on every line. */
static inline uint8_t q8_to_palette_index(int16_t v, int32_t floor_q8, uint32_t span_q8,
                                          uint32_t scale_q16)
{
    int32_t d = (int32_t)v - floor_q8;
    if (d <= 0)
        return 0;
    if ((uint32_t)d >= span_q8)
        return 255;
    return (uint8_t)(((uint32_t)d * scale_q16) >> 16);
}

void spectrum_draw_spectrum_strip_db(uint16_t *canvas_buf,
                                     int disp_w,
                                     int spec_h,
                                     const float *db_vals,
                                     const float *peak_vals,
                                     float db_floor,
                                     float db_ceil,
                                     uint16_t bg,
                                     uint16_t fg,
                                     uint16_t peak_fg)
{
    if (!canvas_buf || !db_vals || disp_w <= 0 || spec_h <= 0)
        return;

    for (int i = 0; i < spec_h * disp_w; i++)
        canvas_buf[i] = bg;

    const float dyn_db = (db_ceil - db_floor) > 1.0f ? (db_ceil - db_floor) : 1.0f;
    const int span_y = spec_h - 1;

    int prev_y = -1;
    for (int px = 0; px < disp_w; px++)
    {
        float norm = (db_vals[px] - db_floor) / dyn_db;
        if (norm < 0.0f)
            norm = 0.0f;
        if (norm > 1.0f)
            norm = 1.0f;
        int y = span_y - (int)(norm * (float)span_y);

        if (prev_y != -1)
        {
            int y0 = prev_y < y ? prev_y : y;
            int y1 = prev_y < y ? y : prev_y;
            for (int yy = y0; yy <= y1; yy++)
                canvas_buf[yy * disp_w + px] = fg;
        }
        else
        {
            canvas_buf[y * disp_w + px] = fg;
        }
        prev_y = y;

        if (peak_vals)
        {
            float pnorm = (peak_vals[px] - db_floor) / dyn_db;
            if (pnorm < 0.0f)
                pnorm = 0.0f;
            if (pnorm > 1.0f)
                pnorm = 1.0f;
            int py = span_y - (int)(pnorm * (float)span_y);
            if (py < y)
                canvas_buf[py * disp_w + px] = peak_fg;
        }
    }
}

void spectrum_draw_waterfall(uint16_t *canvas_buf,
                             int disp_w,
                             int wf_disp_h,
                             int wf_y0,
                             const int16_t *wf_db_q8,
                             uint32_t wf_write_idx,
                             int wf_store_h,
                             float db_floor,
                             float db_ceil,
                             uint8_t palette)
{
    if (!canvas_buf || !wf_db_q8 || disp_w <= 0 || wf_store_h <= 0)
        return;

    if (wf_disp_h > wf_store_h)
        wf_disp_h = wf_store_h;

    const uint16_t *pal_lut = spectrum_palette_get_lut(palette);

    int32_t floor_q8 = spectrum_db_to_q8(db_floor);
    int32_t ceil_q8 = spectrum_db_to_q8(db_ceil);
    uint32_t span_q8 = (ceil_q8 > floor_q8) ? (uint32_t)(ceil_q8 - floor_q8) : 1u;
    uint32_t scale_q16 = (255u << 16) / span_q8;

    for (int row = 0; row < wf_disp_h; row++)
    {
        uint32_t ring_idx = (wf_write_idx + (uint32_t)wf_store_h - 1u - (uint32_t)row) %
                            (uint32_t)wf_store_h;
        const int16_t *src = &wf_db_q8[(size_t)ring_idx * (size_t)disp_w];
        uint16_t *dst = &canvas_buf[(wf_y0 + row) * disp_w];

        for (int x = 0; x < disp_w; x++)
        {
            dst[x] = pal_lut[q8_to_palette_index(src[x], floor_q8, span_q8, scale_q16)];
        }
    }
}

void spectrum_store_wf_row(int16_t *wf_db_q8,
                           uint32_t *wf_write_idx,
                           int wf_store_h,
                           int disp_w,
                           const float *db_vals)
{
    if (!wf_db_q8 || !wf_write_idx || !db_vals || wf_store_h <= 0 || disp_w <= 0)
        return;

    int16_t *dst = &wf_db_q8[(size_t)(*wf_write_idx) * (size_t)disp_w];
    for (int x = 0; x < disp_w; x++)
        dst[x] = spectrum_db_to_q8(db_vals[x]);

    *wf_write_idx = (*wf_write_idx + 1u) % (uint32_t)wf_store_h;
}

void spectrum_draw_filter_overlay(uint16_t *canvas_buf,
                                  int disp_w,
                                  int spec_h,
                                  uint8_t view_mode,
                                  uint32_t fft_size,
                                  uint32_t sample_freq,
                                  uint32_t filter_hz,
                                  uint8_t mode_code,
                                  int marker_px,
                                  uint16_t filter_fill,
                                  uint16_t filter_edge)
{
    const uint8_t fill_alpha = 77; /* ~30% */

    if (filter_hz == 0 || sample_freq == 0 || marker_px < 0)
        return;

    int nbins = effective_bins(view_mode, fft_size);
    float center_bin = ((float)marker_px * (float)(nbins - 1)) / (float)(disp_w - 1);
    /* The zoomed view shows the upper half of the same bin grid, so the
     * Hz-per-bin scale is sample_freq/fft_size in both views. Scaling the
     * bandwidth by the half-grid drew every filter at half its real width. */
    float bw_bins = (float)filter_hz * (float)fft_size / (float)sample_freq;

    float left_bin = center_bin - bw_bins * 0.5f;
    float right_bin = center_bin + bw_bins * 0.5f;

    switch (mode_code)
    {
    case 1:
        left_bin = center_bin - bw_bins;
        right_bin = center_bin;
        break;
    case 2:
        left_bin = center_bin;
        right_bin = center_bin + bw_bins;
        break;
    default:
        break;
    }

    int left_px = bin_to_px(left_bin, disp_w, nbins);
    int right_px = bin_to_px(right_bin, disp_w, nbins);
    if (right_px < left_px)
    {
        int t = left_px;
        left_px = right_px;
        right_px = t;
    }

    /* Narrow filters (hundreds of Hz on a 284 px wide 48 kHz span) round to a
     * single pixel, which is invisible against the trace. Keep a readable
     * minimum width anchored on the side the filter occupies. */
    enum { FILTER_MIN_SPAN_PX = 2 };
    if (right_px - left_px < FILTER_MIN_SPAN_PX)
    {
        if (mode_code == 1)
        {
            left_px = marker_px - FILTER_MIN_SPAN_PX;
            right_px = marker_px;
        }
        else if (mode_code == 2)
        {
            left_px = marker_px;
            right_px = marker_px + FILTER_MIN_SPAN_PX;
        }
        else
        {
            left_px = marker_px - FILTER_MIN_SPAN_PX / 2;
            right_px = left_px + FILTER_MIN_SPAN_PX;
        }
        if (left_px < 0)
        {
            right_px -= left_px;
            left_px = 0;
        }
        if (right_px >= disp_w)
        {
            left_px -= right_px - (disp_w - 1);
            right_px = disp_w - 1;
        }
        if (left_px < 0)
        {
            left_px = 0;
        }
    }

    for (int y = 0; y < spec_h; y++)
    {
        uint16_t *row = &canvas_buf[y * disp_w];
        for (int x = left_px; x <= right_px; x++)
            row[x] = blend_rgb565(row[x], filter_fill, fill_alpha);
        if (left_px >= 0 && left_px < disp_w)
            row[left_px] = filter_edge;
        if (right_px >= 0 && right_px < disp_w)
            row[right_px] = filter_edge;
    }
}
