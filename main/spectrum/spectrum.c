#include "spectrum/spectrum.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_dsp.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "freertos/semphr.h"

#include "display/display.h"
#include "spectrum/dsp.h"
#include "spectrum/freq_map.h"
#include "spectrum/palette.h"
#include "spectrum/render.h"
#include "spectrum/spectrum_priv.h"

static const char *TAG = "spectrum";

spectrum_state_t s;

typedef enum
{
    SPEC_ST_STOPPED = 0,
    SPEC_ST_STARTING,
    SPEC_ST_RUNNING,
    SPEC_ST_STOPPING,
} spectrum_lifecycle_t;

/* The pipeline draws under the LVGL port lock on core 1; core 0 belongs to the
 * USB host library and the UAC task, and a full-frame render+flush there
 * preempts isochronous IN resubmission. The priority matches the LVGL task. */
enum
{
    SPECTRUM_TASK_STACK = 8192,
    SPECTRUM_TASK_PRIORITY = 3,
    SPECTRUM_TASK_CORE = 1,
    SPECTRUM_TASK_STOP_WAIT_MS = 500,
};

static StaticSemaphore_t s_lifecycle_mutex_buf;
static StaticSemaphore_t s_task_exit_buf;
static SemaphoreHandle_t s_lifecycle_mutex;
static SemaphoreHandle_t s_task_exit;
static portMUX_TYPE s_lock_init_spin = portMUX_INITIALIZER_UNLOCKED;
static spectrum_lifecycle_t s_lifecycle = SPEC_ST_STOPPED;
static uint32_t s_init_failures;
/* ANSI vs AES3 was pinned on hardware with `sp ffttest`: gain 1.000000,
 * bias -0.000016 dB, max bin error 0.008 dB, AES3 ~1.6x faster. */
/* Default for the next spectrum_init(); the live value lives in s.fft_impl. */
static uint8_t s_fft_impl_default = SPECTRUM_FFT_AES3_AVAILABLE ? SPECTRUM_FFT_AES3 : SPECTRUM_FFT_ANSI;

/* Static, so first use neither allocates nor races: the CLI task and the UAC
 * callback can both reach this module before spectrum_init(). */
static void spectrum_lock_init(void)
{
    if (s_lifecycle_mutex != NULL && s_task_exit != NULL)
        return;

    taskENTER_CRITICAL(&s_lock_init_spin);
    if (s_lifecycle_mutex == NULL)
        s_lifecycle_mutex = xSemaphoreCreateMutexStatic(&s_lifecycle_mutex_buf);
    if (s_task_exit == NULL)
        s_task_exit = xSemaphoreCreateBinaryStatic(&s_task_exit_buf);
    taskEXIT_CRITICAL(&s_lock_init_spin);
}

static bool spectrum_lock(TickType_t timeout)
{
    spectrum_lock_init();
    if (s_lifecycle_mutex == NULL)
        return false;
    return xSemaphoreTake(s_lifecycle_mutex, timeout) == pdTRUE;
}

static void spectrum_unlock(void)
{
    if (s_lifecycle_mutex)
        xSemaphoreGive(s_lifecycle_mutex);
}

static void spectrum_cleanup_state(spectrum_state_t *st)
{
    if (st->dsp_initialized)
    {
        dsps_fft2r_deinit_fc32();
        st->dsp_initialized = false;
    }

    /* LVGL renders the canvas through a pointer to canvas_buf: delete the
     * widget first or it keeps reading freed memory after the pipeline stops
     * (garbage frames, or a hang in the render task). */
    bool keep_canvas_buf = false;
    if (st->canvas_buf != NULL && !display_spectrum_detach())
    {
        /* LVGL is wedged and still references the buffer. Leaking it beats
         * rendering freed memory; the device is being torn down anyway. */
        ESP_LOGW(TAG, "canvas still attached, leaking %u-byte canvas buffer",
                 (unsigned)((size_t)DISP_W * DISP_H * sizeof(uint16_t)));
        keep_canvas_buf = true;
    }

    free(st->window);
    free(st->fft_buf);
    free(st->mag);
    free(st->wf_db);
    if (!keep_canvas_buf)
    {
        free(st->canvas_buf);
    }

    memset(st, 0, sizeof(*st));
}

static void spectrum_mark_init_failed(void)
{
    if (spectrum_lock(portMAX_DELAY))
    {
        s_lifecycle = SPEC_ST_STOPPED;
        s_init_failures++;
        spectrum_unlock();
    }
}

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

void spectrum_default_cfg(spectrum_cfg_t *cfg)
{
    cfg->fft_size = 512;
    cfg->lines_per_sec = 25.0f;
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

void spectrum_request_history_reset(void)
{
    s.history_reset_pending = true;
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
    /* Rows are stored without the offset: it is folded into the palette
     * floor/ceiling at draw time, so history follows the setting. */
    s.level_offset_db = offset_db;
}

void spectrum_set_view_mode(uint8_t mode)
{
    if (mode > SPECTRUM_VIEW_IF)
        mode = SPECTRUM_VIEW_FULL;
    s.view_mode = mode;
    spectrum_update_marker_px();
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

static void spectrum_rebuild_wf_lut(uint8_t palette, float gamma)
{
    uint8_t next = (uint8_t)(s.wf_lut_active ^ 1u);
    spectrum_palette_gamma_lut(s.wf_lut[next], spectrum_palette_get_lut(palette), gamma);
    s.wf_lut_active = next;
}

void spectrum_set_wf_colors(uint8_t palette, float gamma)
{
    if (palette > 6)
        palette = 6;
    if (!isfinite(gamma))
        gamma = 1.0f;
    if (gamma < SPECTRUM_GAMMA_MIN)
        gamma = SPECTRUM_GAMMA_MIN;
    if (gamma > SPECTRUM_GAMMA_MAX)
        gamma = SPECTRUM_GAMMA_MAX;

    if (palette == s.wf_palette && gamma == s.wf_gamma)
        return;

    s.wf_palette = palette;
    s.wf_gamma = gamma;
    spectrum_rebuild_wf_lut(palette, gamma);
    ESP_LOGI(TAG, "[sp] palette=%u gamma=%.2f", (unsigned)palette, gamma);
}

void spectrum_set_spec_mode(uint8_t mode)
{
    if (mode > SPECTRUM_MODE_OFF)
        mode = SPECTRUM_MODE_TRACE_PEAK;
    s.spec_mode = mode;
    if (!mode_has_peak(mode))
    {
        for (int i = 0; i < DISP_W; i++)
            s.peak_db[i] = s.wf_db_floor;
    }
    ESP_LOGI(TAG, "[sp] mode=%u (0 trace, 1 trace+peak, 2 peak, 3 off)", mode);
}

uint8_t spectrum_get_fft_impl(void)
{
    return s.fft_impl;
}

esp_err_t spectrum_set_fft_impl(uint8_t impl)
{
    if (impl == SPECTRUM_FFT_AES3)
    {
        if (!SPECTRUM_FFT_AES3_AVAILABLE)
            return ESP_ERR_NOT_SUPPORTED;
        s_fft_impl_default = SPECTRUM_FFT_AES3;
    }
    else
    {
        s_fft_impl_default = SPECTRUM_FFT_ANSI;
    }
    s.fft_impl = s_fft_impl_default;
    ESP_LOGI(TAG, "[sp] fft=%s", s.fft_impl ? "aes3" : "ansi");
    return ESP_OK;
}

/* Stream parameters arrive from the UAC descriptor before spectrum_init()
 * runs, so they are staged and applied after the state reset. */
static struct
{
    bool valid;
    uint8_t channels;
    uint8_t bit_resolution;
    uint32_t sample_freq;
} s_pending_stream;

enum
{
    DEFAULT_CHANNELS = 2,
    DEFAULT_BIT_RESOLUTION = 24,
    DEFAULT_FFT_SIZE = 512,
    MIN_FFT_SIZE = 256,
};

static const uint32_t k_default_sample_freq = 48000;

static uint16_t normalize_fft_size(uint16_t requested)
{
    if (requested >= MIN_FFT_SIZE && requested <= SPECTRUM_FFT_MAX && (requested & (requested - 1)) == 0)
        return requested;
    ESP_LOGW(TAG, "unsupported fft_size %u, using %u", (unsigned)requested, (unsigned)DEFAULT_FFT_SIZE);
    return DEFAULT_FFT_SIZE;
}

static void spectrum_apply_stream_params(void)
{
    uint8_t channels = s_pending_stream.channels ? s_pending_stream.channels : DEFAULT_CHANNELS;
    uint8_t bits = s_pending_stream.bit_resolution ? s_pending_stream.bit_resolution : DEFAULT_BIT_RESOLUTION;
    uint32_t freq = s_pending_stream.sample_freq ? s_pending_stream.sample_freq : k_default_sample_freq;

    s.channels = channels;
    s.bit_resolution = bits;
    s.bytes_per_sample = (uint8_t)((bits + 7) / 8);
    s.sample_freq = freq;
    spectrum_update_marker_px();
    spectrum_apply_line_timing();
}

void spectrum_set_stream_params(uint8_t channels, uint8_t bit_resolution, uint32_t sample_freq)
{
    s_pending_stream.valid = true;
    s_pending_stream.channels = channels;
    s_pending_stream.bit_resolution = bit_resolution;
    s_pending_stream.sample_freq = sample_freq;

    if (s.running)
        spectrum_apply_stream_params();
}

esp_err_t spectrum_init(lv_obj_t *canvas, const spectrum_cfg_t *cfg)
{
    if (canvas == NULL || cfg == NULL)
        return ESP_ERR_INVALID_ARG;

    spectrum_lock_init();
    if (!spectrum_lock(portMAX_DELAY))
        return ESP_ERR_NO_MEM;

    if (s_lifecycle == SPEC_ST_RUNNING)
    {
        spectrum_unlock();
        /* Reconnect: the caller created a new canvas widget and the pipeline
         * is still rendering into the old buffer. Rebinding is the only way
         * to keep both alive; returning early would leave LVGL pointing at a
         * deleted object. */
        if (lvgl_port_lock(0))
        {
            s.canvas = canvas;
            lv_canvas_set_buffer(canvas, s.canvas_buf, DISP_W, DISP_H, LV_COLOR_FORMAT_RGB565);
            lvgl_port_unlock();
        }
        ESP_LOGI(TAG, "spectrum already running, canvas rebound");
        return ESP_OK;
    }
    if (s_lifecycle != SPEC_ST_STOPPED)
    {
        spectrum_unlock();
        ESP_LOGW(TAG, "spectrum start blocked, state=%d", (int)s_lifecycle);
        return ESP_ERR_INVALID_STATE;
    }
    s_lifecycle = SPEC_ST_STARTING;
    spectrum_unlock();

    memset(&s, 0, sizeof(s));
    s.canvas = canvas;
    s.cfg = *cfg;
    s.cfg.fft_size = normalize_fft_size(cfg->fft_size);
    s.running = true;

    /* First-frame defaults. app_settings_apply_to_runtime() overwrites them
     * with the stored settings before the first frame is pushed. */
    s.wf_db_floor = -140.0f;
    s.wf_db_ceil = 10.0f;
    s.wf_palette = 4;
    s.wf_gamma = 1.0f;
    /* A frame can be pushed before the stored settings are applied. */
    spectrum_rebuild_wf_lut(s.wf_palette, s.wf_gamma);
    s.view_mode = 0;
    s.cw_pitch_hz = 700;
    s.spec_mode = SPECTRUM_MODE_TRACE_PEAK;
    s.fft_impl = s_fft_impl_default;

    spectrum_apply_stream_params();
    if (cfg->lines_per_sec > 0.0f)
        s.speed_level = spectrum_speed_level_from_lps(cfg->lines_per_sec);
    else
        s.speed_level = spectrum_speed_level_count(); /* fastest */
    spectrum_apply_line_timing();

    for (int i = 0; i < DISP_W; i++)
    {
        s.line_db[i] = s.wf_db_floor;
        s.trace_db[i] = s.wf_db_floor;
        s.peak_db[i] = s.wf_db_floor;
    }

    /* The canvas is the largest buffer and is only touched by LVGL (CPU, not
     * DMA), so PSRAM keeps internal RAM contiguous for the waterfall ring.
     * Boards without PSRAM fall back to internal RAM. */
    const size_t buf_bytes = (size_t)DISP_W * DISP_H * sizeof(uint16_t);
    s.canvas_buf = heap_caps_malloc(buf_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s.canvas_buf)
    {
        ESP_LOGI(TAG, "canvas buffer in PSRAM (%u bytes)", (unsigned)buf_bytes);
    }
    else
    {
        s.canvas_buf = heap_caps_malloc(buf_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        ESP_LOGI(TAG, "canvas buffer in internal RAM (%u bytes)", (unsigned)buf_bytes);
    }
    if (!s.canvas_buf)
    {
        ESP_LOGE(TAG, "canvas_buf alloc failed (%u bytes)", (unsigned)buf_bytes);
        spectrum_mark_init_failed();
        return ESP_ERR_NO_MEM;
    }
    memset(s.canvas_buf, 0, buf_bytes);

    if (lvgl_port_lock(0))
    {
        lv_canvas_set_buffer(canvas, s.canvas_buf, DISP_W, DISP_H, LV_COLOR_FORMAT_RGB565);
        lvgl_port_unlock();
    }

    /* One stored row per displayed row: int16 q8 dB in internal RAM. */
    const size_t wf_bytes = (size_t)WF_STORE_H * DISP_W * sizeof(int16_t);
    s.wf_db = heap_caps_malloc(wf_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s.wf_db)
        s.wf_db = heap_caps_malloc(wf_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s.wf_db)
    {
        ESP_LOGE(TAG, "wf_db alloc failed (%u bytes), internal free=%u",
                 (unsigned)wf_bytes, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        spectrum_cleanup_state(&s);
        spectrum_mark_init_failed();
        return ESP_ERR_NO_MEM;
    }
    const int16_t floor_q8 = spectrum_db_to_q8(s.wf_db_floor);
    for (size_t i = 0; i < (size_t)WF_STORE_H * DISP_W; i++)
        s.wf_db[i] = floor_q8;
    s.wf_write_idx = 0;

    /* The ESP32-S3 AES3 FFT uses 128-bit PIE loads: keep the buffer 16-byte
     * aligned so both implementations see the same input. */
    const uint16_t fft_size = s.cfg.fft_size;
    const size_t fft_bytes = (size_t)fft_size * 2 * sizeof(float);
    s.fft_buf = heap_caps_aligned_alloc(16, fft_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s.fft_buf)
    {
        ESP_LOGE(TAG, "fft_buf alloc failed");
        spectrum_cleanup_state(&s);
        spectrum_mark_init_failed();
        return ESP_ERR_NO_MEM;
    }
    memset(s.fft_buf, 0, fft_bytes);

    s.window = heap_caps_malloc((size_t)fft_size * sizeof(float), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    /* |X|^2 scratch is written once per FFT and read a few times, so PSRAM is
     * fast enough and leaves internal RAM for the ring. */
    s.mag = heap_caps_malloc((size_t)fft_size * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s.mag)
        s.mag = heap_caps_malloc((size_t)fft_size * sizeof(float), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s.window || !s.mag)
    {
        ESP_LOGE(TAG, "window/mag alloc failed");
        spectrum_cleanup_state(&s);
        spectrum_mark_init_failed();
        return ESP_ERR_NO_MEM;
    }
    spectrum_build_hann_window(s.window, (int)fft_size);

    esp_err_t ret = dsps_fft2r_init_fc32(NULL, (int)fft_size);
    if (ret == ESP_ERR_DSP_REINITIALIZED)
        ret = ESP_OK; /* the FFT self test already built the tables */
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "dsps_fft2r_init_fc32 failed: %s", esp_err_to_name(ret));
        spectrum_cleanup_state(&s);
        spectrum_mark_init_failed();
        return ret;
    }
    s.dsp_initialized = true;

    spectrum_update_marker_px();

    BaseType_t ok = xTaskCreatePinnedToCore(spectrum_pipeline_task, "spectrum",
                                            SPECTRUM_TASK_STACK, NULL,
                                            SPECTRUM_TASK_PRIORITY, &s.task_handle,
                                            SPECTRUM_TASK_CORE);
    if (ok != pdTRUE)
    {
        ESP_LOGE(TAG, "Failed to create spectrum task");
        spectrum_cleanup_state(&s);
        spectrum_mark_init_failed();
        return ESP_FAIL;
    }

    if (spectrum_lock(portMAX_DELAY))
    {
        s_lifecycle = SPEC_ST_RUNNING;
        spectrum_unlock();
    }

    ESP_LOGI(TAG, "spectrum init: fft=%u, %.1f lines/s x %lu fft, ring %dx%d q8 (%.1f KB internal)",
             (unsigned)fft_size, s.speed_lps, (unsigned long)s.line_group,
             DISP_W, WF_STORE_H, (float)wf_bytes / 1024.0f);
    return ESP_OK;
}

void spectrum_notify_rx(uac_host_device_handle_t handle)
{
    if (!spectrum_lock(pdMS_TO_TICKS(10)))
        return;

    if (s_lifecycle != SPEC_ST_RUNNING)
    {
        spectrum_unlock();
        return;
    }

    s.uac_handle = handle;
    TaskHandle_t h = s.task_handle;
    spectrum_unlock();

    if (h)
        xTaskNotifyGive(h);
}

/* Called by the pipeline task just before it deletes itself: clearing the
 * handle first means no reader can touch a dead task, and the semaphore lets
 * spectrum_stop() know the task no longer holds any buffer. */
void spectrum_pipeline_task_exited(void)
{
    s.task_handle = NULL;
    if (s_task_exit)
        xSemaphoreGive(s_task_exit);
}

void spectrum_stop(void)
{
    if (!spectrum_lock(portMAX_DELAY))
        return;

    if (s_lifecycle == SPEC_ST_STOPPED)
    {
        spectrum_unlock();
        return;
    }

    s_lifecycle = SPEC_ST_STOPPING;
    s.running = false;
    s.uac_handle = NULL;
    TaskHandle_t task = s.task_handle;
    spectrum_unlock();

    if (task)
    {
        (void)xSemaphoreTake(s_task_exit, 0); /* drop a stale token */
        xTaskNotifyGive(task);
        /* Never vTaskDelete a task that may be holding the LVGL port lock:
         * that would wedge the display task forever. If it does not exit in
         * time, leave the buffers allocated and report it. */
        if (xSemaphoreTake(s_task_exit, pdMS_TO_TICKS(SPECTRUM_TASK_STOP_WAIT_MS)) != pdTRUE)
        {
            ESP_LOGE(TAG, "pipeline task did not exit; leaking buffers");
            return;
        }
    }

    if (!spectrum_lock(portMAX_DELAY))
        return;
    spectrum_cleanup_state(&s);
    s_lifecycle = SPEC_ST_STOPPED;
    spectrum_unlock();

    if (s_init_failures > 0)
    {
        ESP_LOGW(TAG, "spectrum stopped (init failures=%lu)", (unsigned long)s_init_failures);
        s_init_failures = 0;
    }
}

bool spectrum_is_running(void)
{
    bool running = false;
    if (!spectrum_lock(pdMS_TO_TICKS(10)))
        return false;
    running = (s_lifecycle == SPEC_ST_RUNNING);
    spectrum_unlock();
    return running;
}
