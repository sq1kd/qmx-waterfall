#pragma once

#include <stdint.h>

uint16_t spectrum_rgb565(uint8_t r, uint8_t g, uint8_t b);

void spectrum_draw_spectrum_strip(uint16_t *canvas_buf,
                                  int disp_w,
                                  int spec_h,
                                  const float *mag,
                                  int mag_len,
                                  float db_floor,
                                  float db_ceil,
                                  float level_offset_db,
                                  uint16_t bg,
                                  uint16_t fg);

void spectrum_draw_spectrum_strip_db(uint16_t *canvas_buf,
                                     int disp_w,
                                     int spec_h,
                                     const float *db_vals,
                                     int db_len,
                                     float db_floor,
                                     float db_ceil,
                                     uint16_t bg,
                                     uint16_t fg);

void spectrum_draw_waterfall(uint16_t *canvas_buf,
                             int disp_w,
                             int wf_h,
                             int wf_y0,
                             const float *wf_db,
                             uint32_t wf_write_idx,
                             float db_floor,
                             float db_ceil,
                             const uint8_t *gamma_lut,
                             uint8_t palette);

void spectrum_store_wf_row(float *wf_db,
                           uint32_t *wf_write_idx,
                           int wf_h,
                           int disp_w,
                           const float *mag,
                           int mag_len,
                           float level_offset_db);

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
