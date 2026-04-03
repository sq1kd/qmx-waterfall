#include "spectrum/render.h"

#include <math.h>

#include "spectrum/palette.h"

uint16_t spectrum_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return ((uint16_t)(r & 0xF8) << 8) |
           ((uint16_t)(g & 0xFC) << 3) |
           ((uint16_t)(b >> 3));
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

static uint8_t db_to_color_idx_linear(float db, float db_floor, float db_ceil)
{
    float range = db_ceil - db_floor;
    if (range < 1.0f)
        range = 1.0f;
    float norm = (db - db_floor) / range;
    if (norm < 0.0f)
        norm = 0.0f;
    if (norm > 1.0f)
        norm = 1.0f;
    return (uint8_t)(norm * 255.0f + 0.5f);
}

void spectrum_draw_spectrum_strip(uint16_t *canvas_buf,
                                  int disp_w,
                                  int spec_h,
                                  const float *mag,
                                  int mag_len,
                                  float db_floor,
                                  float db_ceil,
                                  float level_offset_db,
                                  uint16_t bg,
                                  uint16_t fg)
{
    for (int i = 0; i < spec_h * disp_w; i++)
        canvas_buf[i] = bg;

    const float dyn_db = db_ceil - db_floor;
    const float bot_db = db_floor;

    int prev_y = -1;
    for (int px = 0; px < disp_w; px++)
    {
        float fbin = (float)px * (float)(mag_len - 1) / (float)(disp_w - 1);
        int bin = (int)fbin;
        if (bin >= mag_len - 1)
            bin = mag_len - 2;
        if (bin < 0)
            bin = 0;
        float frac = fbin - (float)bin;

        float m = mag[bin] * (1.0f - frac) + mag[bin + 1] * frac;
        float db = 10.0f * log10f(m + 1e-12f) + level_offset_db;
        float norm = (db - bot_db) / dyn_db;
        if (norm < 0.0f)
            norm = 0.0f;
        if (norm > 1.0f)
            norm = 1.0f;

        int y_top = (spec_h - 1) - (int)(norm * (spec_h - 1));

        if (prev_y != -1)
        {
            int y0 = prev_y;
            int y1 = y_top;
            if (y0 > y1)
            {
                int tmp = y0;
                y0 = y1;
                y1 = tmp;
            }
            for (int y = y0; y <= y1; y++)
            {
                canvas_buf[y * disp_w + px] = fg;
            }
        }
        else
        {
            canvas_buf[y_top * disp_w + px] = fg;
        }
        prev_y = y_top;
    }
}

void spectrum_draw_spectrum_strip_db(uint16_t *canvas_buf,
                                     int disp_w,
                                     int spec_h,
                                     const float *db_vals,
                                     int db_len,
                                     float db_floor,
                                     float db_ceil,
                                     uint16_t bg,
                                     uint16_t fg)
{
    for (int i = 0; i < spec_h * disp_w; i++)
        canvas_buf[i] = bg;

    const float dyn_db = db_ceil - db_floor;
    const float bot_db = db_floor;

    int prev_y = -1;
    for (int px = 0; px < disp_w; px++)
    {
        float fbin = (float)px * (float)(db_len - 1) / (float)(disp_w - 1);
        int bin = (int)fbin;
        if (bin >= db_len - 1)
            bin = db_len - 2;
        if (bin < 0)
            bin = 0;
        float frac = fbin - (float)bin;

        float db = db_vals[bin] * (1.0f - frac) + db_vals[bin + 1] * frac;
        float norm = (db - bot_db) / dyn_db;
        if (norm < 0.0f)
            norm = 0.0f;
        if (norm > 1.0f)
            norm = 1.0f;

        int y_top = (spec_h - 1) - (int)(norm * (spec_h - 1));

        if (prev_y != -1)
        {
            int y0 = prev_y;
            int y1 = y_top;
            if (y0 > y1)
            {
                int tmp = y0;
                y0 = y1;
                y1 = tmp;
            }
            for (int y = y0; y <= y1; y++)
            {
                canvas_buf[y * disp_w + px] = fg;
            }
        }
        else
        {
            canvas_buf[y_top * disp_w + px] = fg;
        }
        prev_y = y_top;
    }
}

void spectrum_draw_waterfall(uint16_t *canvas_buf,
                             int disp_w,
                             int wf_h,
                             int wf_y0,
                             const float *wf_db,
                             uint32_t wf_write_idx,
                             float db_floor,
                             float db_ceil,
                             const uint8_t *gamma_lut,
                             uint8_t palette)
{
    if (!wf_db)
        return;

    const uint16_t *pal_lut = spectrum_palette_get_lut(palette);

    for (int row = 0; row < wf_h; row++)
    {
        int ring_idx = (int)wf_write_idx - 1 - row;
        if (ring_idx < 0)
            ring_idx += wf_h;

        const float *db_row = &wf_db[ring_idx * disp_w];
        uint16_t *px_row = &canvas_buf[(wf_y0 + row) * disp_w];

        for (int x = 0; x < disp_w; x++)
        {
            uint8_t idx = db_to_color_idx_linear(db_row[x], db_floor, db_ceil);
            if (gamma_lut)
                idx = gamma_lut[idx];
            px_row[x] = pal_lut[idx];
        }
    }
}

void spectrum_store_wf_row(float *wf_db,
                           uint32_t *wf_write_idx,
                           int wf_h,
                           int disp_w,
                           const float *mag,
                           int mag_len,
                           float level_offset_db)
{
    if (!wf_db || !wf_write_idx)
        return;

    float *dst = &wf_db[(*wf_write_idx) * disp_w];

    for (int px = 0; px < disp_w; px++)
    {
        float fbin = (float)px * (float)(mag_len - 1) / (float)(disp_w - 1);
        int bin = (int)fbin;
        if (bin >= mag_len - 1)
            bin = mag_len - 2;
        if (bin < 0)
            bin = 0;
        float frac = fbin - (float)bin;

        float m = mag[bin] * (1.0f - frac) + mag[bin + 1] * frac;
        dst[px] = 10.0f * log10f(m + 1e-12f) + level_offset_db;
    }

    *wf_write_idx = (*wf_write_idx + 1) % wf_h;
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
    if (filter_hz == 0 || sample_freq == 0 || marker_px < 0)
        return;

    int nbins = effective_bins(view_mode, fft_size);
    float center_bin = ((float)marker_px * (float)(nbins - 1)) / (float)(disp_w - 1);
    float bw_bins = (float)filter_hz * (float)nbins / (float)sample_freq;
    if (view_mode == 0)
    {
        bw_bins = (float)filter_hz * (float)fft_size / (float)sample_freq;
    }

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

    for (int y = 0; y < spec_h; y++)
    {
        uint16_t *row = &canvas_buf[y * disp_w];
        for (int x = left_px; x <= right_px; x++)
            row[x] = filter_fill;
        if (left_px >= 0 && left_px < disp_w)
            row[left_px] = filter_edge;
        if (right_px >= 0 && right_px < disp_w)
            row[right_px] = filter_edge;
    }
}
