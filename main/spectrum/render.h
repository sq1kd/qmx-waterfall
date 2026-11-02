#pragma once

#include <stdint.h>

/* Waterfall rows are stored as signed dB in 1/8 dB units (q8). */
#define SPECTRUM_DB_Q8 8.0f

uint16_t spectrum_rgb565(uint8_t r, uint8_t g, uint8_t b);

int16_t spectrum_db_to_q8(float db);

/* Spectrum trace on top of the canvas. trace_vals and peak_vals may each be
 * NULL to draw only the other. */
void spectrum_draw_spectrum_strip_db(uint16_t *canvas_buf,
                                     int disp_w,
                                     int spec_h,
                                     const float *trace_vals,
                                     const float *peak_vals,
                                     float db_floor,
                                     float db_ceil,
                                     uint16_t bg,
                                     uint16_t fg,
                                     uint16_t peak_fg);

/* One pixel row per stored row: no resampling, no interpolation. Row 0 is the
 * most recent line, so the image scrolls down by exactly one row per line. */
void spectrum_draw_waterfall(uint16_t *canvas_buf,
                             int disp_w,
                             int wf_disp_h,
                             int wf_y0,
                             const int16_t *wf_db_q8,
                             uint32_t wf_write_idx,
                             int wf_store_h,
                             float db_floor,
                             float db_ceil,
                             uint8_t palette);

void spectrum_store_wf_row(int16_t *wf_db_q8,
                           uint32_t *wf_write_idx,
                           int wf_store_h,
                           int disp_w,
                           const float *db_vals);

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
                                  uint16_t filter_edge);
