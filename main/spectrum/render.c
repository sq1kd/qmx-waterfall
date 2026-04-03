#include "spectrum/render.h"

#include <math.h>

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

static uint8_t db_to_color_idx(float db, float db_floor, float db_ceil)
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

void spectrum_draw_waterfall(uint16_t *canvas_buf,
                             int disp_w,
                             int wf_h,
                             int wf_y0,
                             const float *wf_db,
                             uint32_t wf_write_idx,
                             float db_floor,
                             float db_ceil)
{
    if (!wf_db)
        return;

    for (int row = 0; row < wf_h; row++)
    {
        int ring_idx = (int)wf_write_idx - 1 - row;
        if (ring_idx < 0)
            ring_idx += wf_h;

        const float *db_row = &wf_db[ring_idx * disp_w];
        uint16_t *px_row = &canvas_buf[(wf_y0 + row) * disp_w];

        for (int x = 0; x < disp_w; x++)
        {
            px_row[x] = turbo_lut[db_to_color_idx(db_row[x], db_floor, db_ceil)];
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
        {
            uint16_t cur = row[x];
            row[x] = ((cur >> 1) & 0x7BEF) + ((filter_fill >> 1) & 0x7BEF);
        }
        if (left_px >= 0 && left_px < disp_w)
            row[left_px] = filter_edge;
        if (right_px >= 0 && right_px < disp_w)
            row[right_px] = filter_edge;
    }
}
