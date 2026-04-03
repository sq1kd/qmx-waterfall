#pragma once

#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "lvgl.h"
#include "usb/uac_host.h"

/*
 * spectrum display configuration.
 * All fields have defaults applied by spectrum_default_cfg().
 */
typedef struct
{
    /* FFT */
    uint16_t fft_size; /* power of 2 (256/512/1024). default: 512     */

    /* Speed */
    float lines_per_sec; /* new rows per second.  default: 25.3         */

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

/* Apply display level offset before mapping to colors. */
void spectrum_set_level_offset_db(float offset_db);

/* View mode: 0=full spectrum (neg+pos), 1=positive-only (0..+Fs/2). */
void spectrum_set_view_mode(uint8_t mode);

/* Set waterfall dB floor and ceiling for colour mapping. */
void spectrum_set_wf_range(float floor_db, float ceil_db);

/* Clear spectrum and spectrum canvas immediately. */
void spectrum_clear(void);

/*
 * Initialise spectrum and create the DSP task on core 1.
 * canvas  — full-screen lv_obj_t created by display_show_spectrum().
 * cfg     — settings (use spectrum_default_cfg() then override as needed).
 * Returns ESP_OK on success.
 */
esp_err_t spectrum_init(lv_obj_t *canvas, const spectrum_cfg_t *cfg);

/*
 * Called from the UAC RX_DONE device callback (core 0) to wake the spectrum
 * task. Called from task context.
 */
void spectrum_notify_rx(uac_host_device_handle_t handle);

/*
 * Tear down the spectrum task and free all resources.
 * Call before uac_host_device_stop().
 */
void spectrum_stop(void);
