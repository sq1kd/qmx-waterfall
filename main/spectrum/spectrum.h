#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "freertos/FreeRTOS.h"
#include "lvgl.h"
#include "usb/uac_host.h"

/* Number of dB columns the spectrum is rendered into (display width). */
#define SPECTRUM_IQ_COLUMNS 284

/*
 * spectrum display configuration.
 * All fields have defaults applied by spectrum_default_cfg().
 */
typedef struct
{
    /* FFT */
    uint16_t fft_size; /* power of 2 (256/512/1024). default: 512 */

    /* Initial waterfall speed; overwritten by the stored setting. */
    float lines_per_sec;

    /* IF marker */
    uint32_t if_freq_hz; /* IF centre offset from DC, Hz.  default: 12000 */
                         /* Set to 0 to disable the marker.            */
} spectrum_cfg_t;

/* Fill *cfg with safe defaults. */
void spectrum_default_cfg(spectrum_cfg_t *cfg);

/*
 * Called once before spectrum_init() to pass the actual stream parameters
 * discovered from the USB descriptor.  Safe defaults (24-bit, stereo, 48kHz)
 * are applied in spectrum_init() if this is not called.
 */
void spectrum_set_stream_params(uint8_t channels, uint8_t bit_resolution,
                                uint32_t sample_freq);

/* Update CAT-derived mode/filter for spectrum filter overlay rendering. */
void spectrum_set_filter_params(uint8_t mode_code, uint32_t filter_hz);

/* Update CAT-driven mode/RIT used for marker and filter centering. */
void spectrum_set_cat_tuning(uint8_t mode_code, int32_t rit_hz, bool rit_on);

/*
 * Erase waterfall history and the spectrum trace on the next frame.
 * Used after IQ mode is restored: rows captured while the radio streamed
 * audio instead of I/Q are not spectrum data.
 */
void spectrum_request_history_reset(void);

/* Set CW beat frequency used for CW/CWR center offset. */
void spectrum_set_cw_pitch_hz(uint16_t hz);

/* Display settings. */
void spectrum_set_level_offset_db(float offset_db);
void spectrum_set_view_mode(uint8_t mode);
void spectrum_set_wf_range(float floor_db, float ceil_db);
void spectrum_set_wf_palette(uint8_t palette);
void spectrum_set_speed(float lines_per_sec); /* 1..25 waterfall lines/s */
void spectrum_set_peak(bool enabled);

/* FFT implementation selection (SPECTRUM_FFT_ANSI / SPECTRUM_FFT_AES3). */
uint8_t spectrum_get_fft_impl(void);
esp_err_t spectrum_set_fft_impl(uint8_t impl);

/* Compare the ANSI and AES3 FFTs on one deterministic block: gain, offset,
 * per-bin error and timing. Logs the result. */
esp_err_t spectrum_fft_selftest(void);

/* Diagnostics. Counters are updated from the pipeline, so they are available
 * only while it runs. */
typedef struct
{
    uint32_t fft_frames;
    uint32_t lines;
    uint32_t draws;
    uint32_t draws_skipped;
    uint32_t read_calls;
    uint64_t bytes_read;
    uint64_t audio_us; /* audio time contained in bytes_read */
    uint64_t run_us;   /* wall time since the first read */
    uint32_t sample_freq;
    uint32_t frame_bytes;
    uint32_t draw_us_last;
    uint32_t draw_us_avg;
    uint32_t draw_us_max;
    uint32_t lock_wait_us_max;
    uint32_t stack_free_words;
    uint32_t heap_internal_free;
    uint32_t heap_psram_free;
} spectrum_stats_t;

void spectrum_get_stats(spectrum_stats_t *out);
void spectrum_reset_stats(void);

/* Capture one raw I/Q block (512 frames) plus the dB vector the firmware
 * derives from it, so a PC reference FFT can be compared against it. */
bool spectrum_capture_iq_block(void);

typedef struct
{
    uint32_t frames;
    uint32_t frame_bytes;
    uint16_t fft_size;
    uint8_t view_mode;
} spectrum_iq_block_info_t;

/* Returns the number of frames copied, or 0 when the capture is not ready. */
uint32_t spectrum_get_iq_block(spectrum_iq_block_info_t *info,
                               uint8_t *raw,
                               uint32_t raw_cap,
                               int16_t *db_q8,
                               uint32_t db_cap);

esp_err_t spectrum_init(lv_obj_t *canvas, const spectrum_cfg_t *cfg);
void spectrum_notify_rx(uac_host_device_handle_t handle);
void spectrum_stop(void);
bool spectrum_is_running(void);
