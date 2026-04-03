#include "spectrum/spectrum.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_dsp.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"

#include "spectrum/dsp.h"
#include "spectrum/freq_map.h"
#include "spectrum/spectrum_priv.h"

static const char *TAG = "spectrum";

spectrum_state_t s;

void spectrum_update_marker_px(void)
{
    s.marker_px = spectrum_compute_marker_px(s.cfg.if_freq_hz,
                                             s.mode_code,
                                             s.cw_pitch_hz,
                                             s.rit_hz,
                                             s.rit_on ? 1 : 0,
                                             s.view_mode,
                                             s.cfg.fft_size,
                                             s.sample_freq,
                                             DISP_W);
}

void spectrum_rebuild_gamma_lut(void)
{
    float g = s.wf_gamma;
    if (g < 0.5f)
        g = 0.5f;
    if (g > 2.5f)
        g = 2.5f;
    s.wf_gamma = g;

    for (int i = 0; i < 256; i++)
    {
        float x = (float)i / 255.0f;
        float y = powf(x, g);
        int v = (int)(y * 255.0f + 0.5f);
        if (v < 0)
            v = 0;
        if (v > 255)
            v = 255;
        s.wf_gamma_lut[i] = (uint8_t)v;
    }
}

void spectrum_default_cfg(spectrum_cfg_t *cfg)
{
    cfg->fft_size = 512;
    cfg->lines_per_sec = 25.3f;
    cfg->if_freq_hz = 12000;
}

void spectrum_set_filter_params(uint8_t mode_code, uint32_t filter_hz)
{
    if (mode_code > 9)
        mode_code = 0;
    s.mode_code = mode_code;
    s.filter_hz = filter_hz;
    spectrum_update_marker_px();
}

void spectrum_set_cat_tuning(uint8_t mode_code, int32_t rit_hz, bool rit_on)
{
    if (mode_code > 9)
        mode_code = 0;
    if (rit_hz < -9999)
        rit_hz = -9999;
    if (rit_hz > 9999)
        rit_hz = 9999;

    s.mode_code = mode_code;
    s.rit_hz = rit_hz;
    s.rit_on = rit_on;
    spectrum_update_marker_px();
}

void spectrum_set_cw_pitch_hz(uint16_t hz)
{
    if (hz < 400)
        hz = 400;
    if (hz > 1000)
        hz = 1000;
    s.cw_pitch_hz = hz;
    spectrum_update_marker_px();
    ESP_LOGI(TAG, "[sp] cw_pitch_hz=%u", (unsigned)hz);
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
    spectrum_update_marker_px();
    if (s.spec_ema_db)
    {
        for (int i = 0; i < DISP_W; i++)
            s.spec_ema_db[i] = s.wf_db_floor;
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

void spectrum_set_wf_gamma(float gamma)
{
    if (gamma < 0.5f)
        gamma = 0.5f;
    if (gamma > 2.5f)
        gamma = 2.5f;
    s.wf_gamma = gamma;
    spectrum_rebuild_gamma_lut();
    ESP_LOGI(TAG, "[sp] gamma=%.2f", gamma);
}

void spectrum_set_wf_palette(uint8_t palette)
{
    if (palette > 5)
        palette = 5;
    s.wf_palette = palette;
    ESP_LOGI(TAG, "[sp] palette=%u", (unsigned)palette);
}

void spectrum_set_spec_alpha(float alpha)
{
    if (alpha <= 0.0f)
        alpha = 0.01f;
    if (alpha > 1.0f)
        alpha = 1.0f;
    s.spec_alpha = alpha;
    ESP_LOGI(TAG, "[sp] alpha=%.2f", alpha);
}

void spectrum_get_runtime(float *level_offset_db,
                          uint8_t *view_mode,
                          float *wf_floor_db,
                          float *wf_ceil_db,
                          uint16_t *cw_pitch_hz,
                          float *wf_gamma,
                          uint8_t *wf_palette,
                          float *spec_alpha)
{
    if (level_offset_db)
        *level_offset_db = s.level_offset_db;
    if (view_mode)
        *view_mode = s.view_mode;
    if (wf_floor_db)
        *wf_floor_db = s.wf_db_floor;
    if (wf_ceil_db)
        *wf_ceil_db = s.wf_db_ceil;
    if (cw_pitch_hz)
        *cw_pitch_hz = s.cw_pitch_hz;
    if (wf_gamma)
        *wf_gamma = s.wf_gamma;
    if (wf_palette)
        *wf_palette = s.wf_palette;
    if (spec_alpha)
        *spec_alpha = s.spec_alpha;
}

esp_err_t spectrum_init(lv_obj_t *canvas, const spectrum_cfg_t *cfg)
{
    memset(&s, 0, sizeof(s));
    s.canvas = canvas;
    s.cfg = *cfg;

    s.channels = 2;
    s.bit_resolution = 24;
    s.bytes_per_sample = 3;
    s.sample_freq = 48000;

    s.wf_db_floor = -140.0f;
    s.wf_db_ceil = 10.0f;
    s.wf_gamma = 1.0f;
    s.wf_palette = 0;
    s.spec_alpha = 0.25f;
    s.view_mode = 0;
    s.cw_pitch_hz = 700;
    spectrum_rebuild_gamma_lut();

    const size_t buf_bytes = (size_t)DISP_W * DISP_H * sizeof(uint16_t);
    s.canvas_buf = heap_caps_malloc(buf_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s.canvas_buf)
    {
        ESP_LOGE(TAG, "canvas_buf alloc failed (%u bytes)", (unsigned)buf_bytes);
        return ESP_ERR_NO_MEM;
    }
    memset(s.canvas_buf, 0, buf_bytes);

    if (lvgl_port_lock(0))
    {
        lv_canvas_set_buffer(canvas, s.canvas_buf, DISP_W, DISP_H, LV_COLOR_FORMAT_RGB565);
        lvgl_port_unlock();
    }

    const size_t fft_bytes = (size_t)cfg->fft_size * 2 * sizeof(float);
    s.fft_buf = heap_caps_malloc(fft_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s.fft_buf)
    {
        ESP_LOGE(TAG, "fft_buf alloc failed");
        free(s.canvas_buf);
        return ESP_ERR_NO_MEM;
    }
    memset(s.fft_buf, 0, fft_bytes);

    s.window = heap_caps_malloc((size_t)cfg->fft_size * sizeof(float), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s.window)
    {
        ESP_LOGE(TAG, "window alloc failed");
        free(s.fft_buf);
        free(s.canvas_buf);
        return ESP_ERR_NO_MEM;
    }
    spectrum_build_hann_window(s.window, (int)cfg->fft_size);

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
    for (int i = 0; i < WF_H * DISP_W; i++)
        s.wf_db[i] = s.wf_db_floor;
    s.wf_write_idx = 0;

    s.spec_ema_db = heap_caps_malloc((size_t)DISP_W * sizeof(float), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s.spec_ema_db)
    {
        ESP_LOGE(TAG, "spec_ema_db alloc failed");
        free(s.wf_db);
        free(s.window);
        free(s.fft_buf);
        free(s.canvas_buf);
        return ESP_ERR_NO_MEM;
    }
    for (int i = 0; i < DISP_W; i++)
        s.spec_ema_db[i] = s.wf_db_floor;

    esp_err_t ret = dsps_fft2r_init_fc32(NULL, (int)cfg->fft_size);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "dsps_fft2r_init_fc32 failed: %s", esp_err_to_name(ret));
        free(s.spec_ema_db);
        free(s.wf_db);
        free(s.window);
        free(s.fft_buf);
        free(s.canvas_buf);
        return ret;
    }
    s.dsp_initialized = true;

    spectrum_update_marker_px();

    BaseType_t ok = xTaskCreatePinnedToCore(spectrum_pipeline_task, "spectrum", 8192, NULL, 3, &s.task_handle, 1);
    if (ok != pdTRUE)
    {
        ESP_LOGE(TAG, "Failed to create spectrum task");
        free(s.spec_ema_db);
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

void spectrum_set_stream_params(uint8_t channels, uint8_t bit_resolution, uint32_t sample_freq)
{
    s.channels = channels;
    s.bit_resolution = bit_resolution;
    s.sample_freq = sample_freq;
    s.bytes_per_sample = (uint8_t)((bit_resolution + 7) / 8);
    spectrum_update_marker_px();
}

void spectrum_notify_rx(uac_host_device_handle_t handle)
{
    s.uac_handle = handle;
    if (s.task_handle)
        xTaskNotifyGive(s.task_handle);
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
    free(s.spec_ema_db);
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
