/*
 * spectrum.c — IQ spectrum FFT display with waterfall
 *
 * Pipeline (runs on core 1, pinned):
 *   UAC RX_DONE (core 0) ──TaskNotify──► spectrum_task
 *     → uac_host_device_read()
 *     → decode PCM-24 / PCM-16 stereo → complex float [I, Q]
 *     → accumulate FFT_SIZE samples
 *     → Hann window
 *     → dsps_fft2r_fc32  (in-place complex FFT)
 *     → magnitudes, fftshift
 *     → store dB row in waterfall ring buffer
 *     → draw spectrum line plot (top 25 px)
 *     → draw waterfall from ring buffer (bottom 51 px)
 *     → draw filter overlay and IF marker
 *     → lv_obj_invalidate(canvas)
 *
 * Logical display after 90° CW rotation: 284 wide × 76 tall.
 * Layout: spectrum line plot (top 25 px), waterfall (bottom 51 px).
 * Newest waterfall row at top of waterfall area, scrolling down.
 *
 * Two view modes:
 *   0 = full spectrum (all FFT bins, DC in centre after fftshift)
 *   1 = positive-only (upper half of fftshift output, 0..+Fs/2)
 */

#include "spectrum/spectrum.h"

#include <string.h>
#include <math.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
#include "esp_dsp.h"
#include "spectrum/render.h"
#include "spectrum/dsp.h"

/* Logical display dimensions after rotation */
#define DISP_W 284 /* landscape width  (was physical height) */
#define DISP_H 76  /* landscape height (was physical width)  */

/* Layout: spectrum line plot on top, waterfall below */
#define SPEC_H 25  /* spectrum strip height in pixels */
#define WF_H   51  /* waterfall height in pixels      */
#define WF_Y0  SPEC_H /* first waterfall row in canvas */

/* Read buffer: enough for ~20 ms of 24-bit stereo @ 48 kHz
 * 48000 * 0.020 * 2 ch * 3 bytes = 5760 bytes — round up to power of 2 */
#define READ_BUF_BYTES 6144

static const char *TAG = "spectrum";

/* -------------------------------------------------------------------------- */
/* Module state                                                                */
/* -------------------------------------------------------------------------- */

static struct
{
    lv_obj_t *canvas;
    uint16_t *canvas_buf; /* RGB565, DISP_W × DISP_H        */
    spectrum_cfg_t cfg;
    uac_host_device_handle_t uac_handle;
    TaskHandle_t task_handle;

    /* Audio stream parameters discovered from USB descriptor */
    uint8_t channels;
    uint8_t bit_resolution;
    uint8_t bytes_per_sample;
    uint32_t sample_freq;

    /* DSP buffers */
    float *fft_buf;    /* size: fft_size * 2 (interleaved Re/Im) */
    float *window;     /* Hann window, fft_size floats            */

    /* Accumulation state */
    uint32_t accum_idx; /* samples accumulated so far              */
    uint8_t read_raw[READ_BUF_BYTES];

    bool dsp_initialized; /* true after dsps_fft2r_init_fc32 succeeds */

    /* IF marker */
    int marker_px; /* pixel column for the IF line; -1 = disabled  */

    /* CAT-driven spectrum filter overlay */
    uint8_t mode_code;
    uint32_t filter_hz;

    float level_offset_db;

    /* Waterfall state */
    float *wf_db;           /* ring buffer: WF_H rows × DISP_W floats  */
    uint32_t wf_write_idx;  /* next row to write (0..WF_H-1)           */
    float wf_db_floor;      /* dB value mapping to LUT index 0         */
    float wf_db_ceil;       /* dB value mapping to LUT index 255       */
    uint8_t view_mode;      /* 0=full spectrum, 1=positive-only         */

} s;

#define SPEC_BG spectrum_rgb565(0x00, 0x00, 0x08)
#define SPEC_FG spectrum_rgb565(0x00, 0xFF, 0x00)
#define SPEC_FILTER_FILL spectrum_rgb565(0x52, 0x52, 0x52)
#define SPEC_FILTER_EDGE spectrum_rgb565(0x9A, 0x9A, 0x9A)

/* -------------------------------------------------------------------------- */
/* Helpers                                                                     */
/* -------------------------------------------------------------------------- */

/* -------------------------------------------------------------------------- */
/* spectrum task (core 1)                                                     */
/* -------------------------------------------------------------------------- */

static void spectrum_task(void *arg)
{
    ESP_LOGI(TAG, "spectrum task started (fft=%u, %.1f lines/s, view=%u)",
             s.cfg.fft_size, s.cfg.lines_per_sec, s.view_mode);

    const uint32_t fft_size = s.cfg.fft_size;
    const uint32_t ms_per_line = (uint32_t)(1000.0f / s.cfg.lines_per_sec);
    TickType_t last_push = 0;
    while (true)
    {
        /* Wait for RX_DONE notification from core 0.
         * pdTRUE clears the notification count, but we drain the ring
         * buffer fully below so stacked notifications are harmless. */
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        if (s.uac_handle == NULL)
        {
            continue;
        }

        /* ---- Drain the UAC ring buffer completely ----
         * The USB host pushes data at 48 kHz × 6 B/frame = 288 KB/s.
         * We must consume it all to prevent ring buffer overflow and
         * the resulting byte-stream discontinuities.  Display rendering
         * is rate-limited separately below. */
        const uint8_t bytes_per_sample = s.bytes_per_sample;
        const uint8_t frame_bytes = bytes_per_sample * s.channels;

        while (true)
        {
            uint32_t bytes_read = 0;
            esp_err_t err = uac_host_device_read(s.uac_handle,
                                                 s.read_raw,
                                                 sizeof(s.read_raw),
                                                 &bytes_read,
                                                 0 /* non-blocking */);
            if (err != ESP_OK || bytes_read == 0)
            {
                break; /* ring buffer drained */
            }

            const uint32_t frames = bytes_read / frame_bytes;

            for (uint32_t f = 0; f < frames; f++)
            {
                const uint8_t *p = s.read_raw + f * frame_bytes;

                float i_samp = 0.0f;
                float q_samp = 0.0f;
                if (!spectrum_decode_iq_sample(p, s.bit_resolution, bytes_per_sample,
                                               frame_bytes, &i_samp, &q_samp))
                {
                    continue;
                }

                /* fft_buf layout: [Re0, Im0, Re1, Im1, ...] */
                uint32_t idx = s.accum_idx;
                s.fft_buf[idx * 2] = i_samp * s.window[idx];
                s.fft_buf[idx * 2 + 1] = q_samp * s.window[idx];
                s.accum_idx++;

                if (s.accum_idx >= fft_size)
                {
                    s.accum_idx = 0;

                    /* Run FFT — use ANSI C path to bypass AES3 SIMD */
                    dsps_fft2r_fc32_ansi(s.fft_buf, (int)fft_size);
                    dsps_bit_rev_fc32(s.fft_buf, (int)fft_size);

                    /* Compute magnitude² for each bin (unnormalized, matching Python
                     * convention: 10*log10(|X|²) so dB values are directly comparable
                     * with the pc/iq_live_view.py reference viewer). */
                    float mag[fft_size]; /* stack-allocated; fft_size ≤ 1024 */
                    for (uint32_t k = 0; k < fft_size; k++)
                    {
                        float re = s.fft_buf[k * 2];
                        float im = s.fft_buf[k * 2 + 1];
                        float pwr = re * re + im * im;
                        mag[k] = pwr;  /* store power; converted to dB later */
                    }

                    /* Shift so DC is in the centre (negative freq left) */
                    spectrum_fftshift_mag(mag, (int)fft_size);

                    /* Select sub-array for positive-only mode */
                    const float *render_mag = mag;
                    int render_bins = (int)fft_size;
                    if (s.view_mode == 1)
                    {
                        render_mag = &mag[fft_size / 2];
                        render_bins = (int)(fft_size / 2);
                    }

                    spectrum_store_wf_row(s.wf_db, &s.wf_write_idx, WF_H, DISP_W,
                                          render_mag, render_bins, s.level_offset_db);

                    /* Rate-limited display push: render + push only when
                     * enough time has elapsed since the last frame. */
                    TickType_t now = xTaskGetTickCount();
                    TickType_t elapsed_ms = (now - last_push) * portTICK_PERIOD_MS;
                    if (last_push == 0 || elapsed_ms >= ms_per_line)
                    {
                        last_push = now;

                        /* Spectrum strip (top SPEC_H rows) */
                        spectrum_draw_spectrum_strip(s.canvas_buf, DISP_W, SPEC_H,
                                                     render_mag, render_bins,
                                                     s.wf_db_floor, s.wf_db_ceil,
                                                     s.level_offset_db,
                                                     SPEC_BG, SPEC_FG);

                        /* Waterfall (bottom WF_H rows) — redrawn from ring buffer */
                        spectrum_draw_waterfall(s.canvas_buf, DISP_W, WF_H, WF_Y0,
                                                s.wf_db, s.wf_write_idx,
                                                s.wf_db_floor, s.wf_db_ceil);

                        spectrum_draw_filter_overlay(s.canvas_buf, DISP_W, SPEC_H,
                                                     s.view_mode, s.cfg.fft_size,
                                                     s.sample_freq, s.filter_hz,
                                                     s.mode_code, s.marker_px,
                                                     SPEC_FILTER_FILL,
                                                     SPEC_FILTER_EDGE);

                        /* IF marker: red vertical line through spectrum strip */
                        if (s.marker_px >= 0)
                            for (int y = 0; y < SPEC_H; y++)
                                s.canvas_buf[y * DISP_W + s.marker_px] = spectrum_rgb565(0xFF, 0x00, 0x00);

                        if (lvgl_port_lock(0))
                        {
                            lv_obj_invalidate(s.canvas);
                            lvgl_port_unlock();
                        }
                    }

                } /* end FFT frame complete */
            } /* end per-frame loop */
        } /* end ring buffer drain loop */
    } /* end main task loop */
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                  */
/* -------------------------------------------------------------------------- */

void spectrum_default_cfg(spectrum_cfg_t *cfg)
{
    cfg->fft_size = 512;
    cfg->lines_per_sec = 25.3f; /* 25 fps */
    cfg->if_freq_hz = 12000;    /* QMX IF offset */
}


void spectrum_set_filter_params(uint8_t mode_code, uint32_t filter_hz)
{
    if (mode_code > 9)
    {
        mode_code = 0;
    }
    s.mode_code = mode_code;
    s.filter_hz = filter_hz;
}

void spectrum_set_level_offset_db(float offset_db)
{
    if (offset_db < -80.0f)
        offset_db = -80.0f;
    if (offset_db > 80.0f)
        offset_db = 80.0f;
    s.level_offset_db = offset_db;
}

void spectrum_set_view_mode(uint8_t mode)
{
    if (mode > 1)
        mode = 0;
    s.view_mode = mode;

    /* Recompute IF marker pixel for the new bin mapping */
    if (s.cfg.if_freq_hz > 0 && s.sample_freq > 0)
    {
        int nbins = (mode == 1) ? (int)(s.cfg.fft_size / 2) : (int)s.cfg.fft_size;
        float if_bin;
        if (mode == 1)
        {
            /* positive-only: IF maps directly as fraction of Fs/2 */
            if_bin = (float)s.cfg.if_freq_hz * (float)(s.cfg.fft_size) / (float)s.sample_freq;
            /* Clamp to the positive-only bin range */
            if (if_bin >= (float)nbins) if_bin = (float)(nbins - 1);
        }
        else
        {
            /* full spectrum: IF is offset from DC at centre */
            float dc_bin = (float)(s.cfg.fft_size / 2);
            if_bin = dc_bin + (float)s.cfg.if_freq_hz * (float)(s.cfg.fft_size) / (float)s.sample_freq;
        }
        s.marker_px = (int)(if_bin * (float)(DISP_W - 1) / (float)(nbins - 1) + 0.5f);
        if (s.marker_px < 0) s.marker_px = 0;
        if (s.marker_px >= DISP_W) s.marker_px = DISP_W - 1;
    }

    ESP_LOGI(TAG, "[sp] view_mode=%u marker_px=%d", mode, s.marker_px);
}

void spectrum_set_wf_range(float floor_db, float ceil_db)
{
    if (floor_db >= ceil_db)
    {
        ESP_LOGW(TAG, "[sp] invalid range: floor=%.1f >= ceil=%.1f", floor_db, ceil_db);
        return;
    }
    s.wf_db_floor = floor_db;
    s.wf_db_ceil = ceil_db;
    ESP_LOGI(TAG, "[sp] wf range: %.1f .. %.1f dB", floor_db, ceil_db);
}

void spectrum_clear(void)
{
    if (!s.canvas_buf)
    {
        return;
    }

    memset(s.canvas_buf, 0, (size_t)DISP_W * DISP_H * sizeof(uint16_t));

    /* Clear waterfall ring buffer too */
    if (s.wf_db)
    {
        /* Fill with floor dB so waterfall shows darkest colour */
        for (int i = 0; i < WF_H * DISP_W; i++)
            s.wf_db[i] = s.wf_db_floor;
        s.wf_write_idx = 0;
    }

    if (s.canvas && lvgl_port_lock(0))
    {
        lv_obj_invalidate(s.canvas);
        lvgl_port_unlock();
    }
}



esp_err_t spectrum_init(lv_obj_t *canvas, const spectrum_cfg_t *cfg)
{
    memset(&s, 0, sizeof(s));
    s.canvas = canvas;
    s.cfg = *cfg;

    /* Defaults for stream params — overwritten by spectrum_set_stream_params */
    s.channels = 2;
    s.bit_resolution = 24;
    s.bytes_per_sample = 3;  /* QMX uses 3-byte containers (bSubframeSize=3) */
    s.sample_freq = 48000;

    /* Default waterfall dB range (matches Python viewer defaults) */
    s.wf_db_floor = -140.0f;
    s.wf_db_ceil = 10.0f;
    s.view_mode = 0;

    /* Canvas pixel buffer: logical 284×76 RGB565 */
    const size_t buf_bytes = (size_t)DISP_W * DISP_H * sizeof(uint16_t);
    s.canvas_buf = heap_caps_malloc(buf_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s.canvas_buf)
    {
        ESP_LOGE(TAG, "canvas_buf alloc failed (%u bytes)", (unsigned)buf_bytes);
        return ESP_ERR_NO_MEM;
    }
    memset(s.canvas_buf, 0, buf_bytes);

    /* Attach to LVGL canvas */
    if (lvgl_port_lock(0))
    {
        lv_canvas_set_buffer(canvas, s.canvas_buf,
                             DISP_W, DISP_H,
                             LV_COLOR_FORMAT_RGB565);
        lvgl_port_unlock();
    }

    /* FFT buffer: fft_size complex floats */
    const size_t fft_bytes = (size_t)cfg->fft_size * 2 * sizeof(float);
    s.fft_buf = heap_caps_malloc(fft_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s.fft_buf)
    {
        ESP_LOGE(TAG, "fft_buf alloc failed");
        free(s.canvas_buf);
        return ESP_ERR_NO_MEM;
    }
    memset(s.fft_buf, 0, fft_bytes);

    /* Hann window */
    s.window = heap_caps_malloc((size_t)cfg->fft_size * sizeof(float),
                                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s.window)
    {
        ESP_LOGE(TAG, "window alloc failed");
        free(s.fft_buf);
        free(s.canvas_buf);
        return ESP_ERR_NO_MEM;
    }
    spectrum_build_hann_window(s.window, (int)cfg->fft_size);

    /* Waterfall dB ring buffer: WF_H rows × DISP_W floats */
    const size_t wf_bytes = (size_t)WF_H * DISP_W * sizeof(float);
    s.wf_db = heap_caps_malloc(wf_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s.wf_db)
    {
        ESP_LOGE(TAG, "wf_db alloc failed (%u bytes)", (unsigned)wf_bytes);
        free(s.window);
        free(s.fft_buf);
        free(s.canvas_buf);
        return ESP_ERR_NO_MEM;
    }
    /* Initialize to floor so waterfall starts dark */
    for (int i = 0; i < WF_H * DISP_W; i++)
        s.wf_db[i] = s.wf_db_floor;
    s.wf_write_idx = 0;

    /* Initialise esp-dsp FFT tables */
    esp_err_t ret = dsps_fft2r_init_fc32(NULL, (int)cfg->fft_size);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "dsps_fft2r_init_fc32 failed: %s", esp_err_to_name(ret));
        free(s.wf_db);
        free(s.window);
        free(s.fft_buf);
        free(s.canvas_buf);
        return ret;
    }
    s.dsp_initialized = true;

    /* Compute IF marker pixel position for initial view mode */
    if (cfg->if_freq_hz > 0 && s.sample_freq > 0)
    {
        float dc_bin = (float)(cfg->fft_size / 2);
        float if_bin = dc_bin + (float)cfg->if_freq_hz * (float)(cfg->fft_size) / (float)s.sample_freq;
        s.marker_px = (int)(if_bin * (float)(DISP_W - 1) / (float)(cfg->fft_size - 1) + 0.5f);
        if (s.marker_px < 0) s.marker_px = 0;
        if (s.marker_px >= DISP_W) s.marker_px = DISP_W - 1;
    }
    else
    {
        s.marker_px = -1;
    }

    BaseType_t ok = xTaskCreatePinnedToCore(spectrum_task, "spectrum",
                                            8192, NULL, 3,
                                            &s.task_handle, 1);
    if (ok != pdTRUE)
    {
        ESP_LOGE(TAG, "Failed to create spectrum task");
        free(s.wf_db);
        free(s.window);
        free(s.fft_buf);
        free(s.canvas_buf);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Spectrum init OK — FFT %u, %.1f fps, wf %dx%d (%.1f KB)",
             cfg->fft_size, cfg->lines_per_sec, DISP_W, WF_H,
             (float)wf_bytes / 1024.0f);
    return ESP_OK;
}

void spectrum_set_stream_params(uint8_t channels, uint8_t bit_resolution,
                                 uint32_t sample_freq)
{
    s.channels = channels;
    s.bit_resolution = bit_resolution;
    s.sample_freq = sample_freq;
    s.bytes_per_sample = (uint8_t)((bit_resolution + 7) / 8);
}

void spectrum_notify_rx(uac_host_device_handle_t handle)
{
    /* Called from the USB host client task (not an ISR), so use the
     * task-context notify API which does not need a yield hint. */
    s.uac_handle = handle;
    if (s.task_handle)
    {
        xTaskNotifyGive(s.task_handle);
    }
}

void spectrum_stop(void)
{
    s.uac_handle = NULL;
    if (s.task_handle)
    {
        vTaskDelete(s.task_handle);
        s.task_handle = NULL;
    }
    if (s.dsp_initialized)
    {
        dsps_fft2r_deinit_fc32();
    }
    free(s.window);
    free(s.fft_buf);
    free(s.wf_db);
    /* Detach canvas before freeing its backing buffer so the LVGL render
     * task never reads freed memory. */
    if (s.canvas_buf && s.canvas)
    {
        if (lvgl_port_lock(0))
        {
            lv_canvas_set_buffer(s.canvas, NULL, 0, 0, LV_COLOR_FORMAT_RGB565);
            lvgl_port_unlock();
        }
    }
    free(s.canvas_buf);
    memset(&s, 0, sizeof(s));
}
