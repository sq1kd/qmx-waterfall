#include "app/runtime_priv.h"
#include "app/recovery.h"
#include "app/settings.h"
#include "app/timing_config.h"

#include <stdio.h>
#include <stdlib.h>
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "display/display.h"
#include "spectrum/spectrum.h"
#include "cat/service.h"

static const char *TAG = "usb_audio_cdc_info";

#define UAC_EVT_QUEUE_LEN 8
typedef enum
{
    UAC_EVT_DISCONNECT = 1,
    UAC_EVT_STOP,
} uac_evt_id_t;

typedef struct
{
    uac_evt_id_t id;
    uac_host_device_handle_t handle;
} uac_evt_t;

static QueueHandle_t s_uac_evt_queue;
static TaskHandle_t s_uac_evt_task;
static TickType_t s_next_iq_start_allowed_tick;

static bool runtime_uac_disconnect_latched(void)
{
    bool latched;
    xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
    latched = s_disc.uac_seen;
    xSemaphoreGive(s_dev_mutex);
    return latched;
}

static bool app_runtime_queue_uac_event(uac_evt_id_t id, uac_host_device_handle_t handle)
{
    if (s_uac_evt_queue == NULL)
    {
        return false;
    }
    uac_evt_t ev = {
        .id = id,
        .handle = handle,
    };
    return xQueueSend(s_uac_evt_queue, &ev, 0) == pdTRUE;
}

static void app_runtime_handle_uac_disconnect(uac_host_device_handle_t handle)
{
    if (handle == NULL)
    {
        return;
    }

    TickType_t t0 = xTaskGetTickCount();
    bool was_iq = false;
    xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
    s_disc.in_progress = true;
    s_disc.uac_seen = true;
    xSemaphoreGive(s_dev_mutex);
    app_runtime_set_stream_state(false, false, false);
    if (xSemaphoreTake(s_dev_mutex, pdMS_TO_TICKS(APP_DEV_MUTEX_TIMEOUT_MS)) != pdTRUE)
    {
        ESP_LOGW(TAG, "disconnect: dev_mutex timeout (stage 1)");
        xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
        s_disc.in_progress = false;
        xSemaphoreGive(s_dev_mutex);
        return;
    }
    for (int i = 0; i < APP_RUNTIME_MAX_UAC_DEVICES; i++)
    {
        if (s_uac_handles[i] == handle)
        {
            s_uac_handles[i] = NULL;
            break;
        }
    }
    TickType_t t_mark = xTaskGetTickCount();
    if (s_iq_handle == handle)
    {
        s_iq_handle = NULL;
        s_iq_stream_started = false;
        s_iq_start_in_progress = false;
        was_iq = true;
    }
    xSemaphoreGive(s_dev_mutex);

    TickType_t tc0 = xTaskGetTickCount();
    esp_err_t cerr = uac_host_device_close(handle);
    TickType_t tc1 = xTaskGetTickCount();
    if (cerr != ESP_OK && cerr != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGW(TAG, "uac_host_device_close: %s", esp_err_to_name(cerr));
    }
    recovery_record_uac_timing((uint32_t)((t_mark - t0) * portTICK_PERIOD_MS),
                               (uint32_t)((tc1 - tc0) * portTICK_PERIOD_MS),
                               (uint32_t)((tc1 - t0) * portTICK_PERIOD_MS),
                               was_iq);

    /* Completed one disconnect cycle; keep IQ start blocked until reconnect. */
    xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
    s_disc.in_progress = false;
    xSemaphoreGive(s_dev_mutex);
}

static void app_runtime_uac_evt_task(void *arg)
{
    (void)arg;
    while (true)
    {
        uac_evt_t ev;
        if (xQueueReceive(s_uac_evt_queue, &ev, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }

        if (ev.id == UAC_EVT_STOP)
        {
            break;
        }
        if (ev.id == UAC_EVT_DISCONNECT)
        {
            app_runtime_handle_uac_disconnect(ev.handle);
            (void)recovery_request();
        }
    }

    s_uac_evt_task = NULL;
    if (s_install.stop_events)
    {
        xEventGroupSetBits(s_install.stop_events, APP_STOPBIT_UAC_TASK);
    }
    vTaskDelete(NULL);
}

static void log_wstr(const char *label, const wchar_t *wstr)
{
    char buf[UAC_STR_DESC_MAX_LENGTH + 1];
    int i = 0;
    while (i < UAC_STR_DESC_MAX_LENGTH && wstr[i] != L'\0')
    {
        buf[i] = (char)(wstr[i] & 0xFF);
        i++;
    }
    buf[i] = '\0';
    if (i > 0)
    {
        ESP_LOGD(TAG, "  %-16s %s", label, buf);
    }
}

static void print_uac_device_info(uac_host_device_handle_t handle, const char *type_str)
{
    uac_host_dev_info_t info;
    if (uac_host_get_device_info(handle, &info) != ESP_OK)
    {
        return;
    }
    ESP_LOGD(TAG, "=== UAC %s connected ===", type_str);
    ESP_LOGD(TAG, "  %-16s 0x%04X", "VID:", info.VID);
    ESP_LOGD(TAG, "  %-16s 0x%04X", "PID:", info.PID);
    log_wstr("Manufacturer:", info.iManufacturer);
    log_wstr("Product:", info.iProduct);
    log_wstr("Serial:", info.iSerialNumber);
    ESP_LOGD(TAG, "  %-16s %d  (%d alt settings)", "Interface:", info.iface_num, info.iface_alt_num);
}

static void app_runtime_store_uac_handle(uac_host_device_handle_t handle)
{
    xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
    for (int i = 0; i < APP_RUNTIME_MAX_UAC_DEVICES; i++)
    {
        if (s_uac_handles[i] == NULL)
        {
            s_uac_handles[i] = handle;
            break;
        }
    }
    xSemaphoreGive(s_dev_mutex);
}

static void start_iq_stream_and_spectrum(void)
{
    TickType_t now = xTaskGetTickCount();
    if (s_next_iq_start_allowed_tick != 0 && now < s_next_iq_start_allowed_tick)
    {
        uint32_t wait_ms = (uint32_t)((s_next_iq_start_allowed_tick - now) * portTICK_PERIOD_MS);
        ESP_LOGW(TAG, "IQ start backoff active, wait %lums", (unsigned long)wait_ms);
        return;
    }

    xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
    if (s_iq_stream_started)
    {
        xSemaphoreGive(s_dev_mutex);
        return;
    }

    uac_host_device_handle_t iq_handle = s_iq_handle;
    uac_host_stream_config_t iq_stream_cfg = s_iq_stream_cfg;
    xSemaphoreGive(s_dev_mutex);

    if (iq_handle == NULL || runtime_uac_disconnect_latched())
    {
        return;
    }

    uint32_t epoch = app_runtime_conn_epoch_get();
    uac_host_stream_config_t start_cfg = iq_stream_cfg;
    start_cfg.flags |= FLAG_STREAM_SUSPEND_AFTER_START;

    esp_err_t serr = uac_host_device_start(iq_handle, &start_cfg);
    if (serr != ESP_OK)
    {
        ESP_LOGE(TAG, "uac_host_device_start failed: %s", esp_err_to_name(serr));
        return;
    }

    for (int attempt = 1; attempt <= (int)APP_IQ_RESUME_RETRY_MAX_ATTEMPTS; attempt++)
    {
        if (!app_runtime_conn_epoch_is_current(epoch) || runtime_uac_disconnect_latched())
        {
            ESP_LOGW(TAG, "IQ start aborted due to disconnect");
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(APP_IQ_RESUME_RETRY_DELAY_MS));
        serr = uac_host_device_resume(iq_handle);
        if (serr == ESP_OK)
        {
            ESP_LOGI(TAG, "UAC stream active (%u ch, %u bit, %lu Hz) [attempt %d]",
                     iq_stream_cfg.channels, iq_stream_cfg.bit_resolution,
                     (unsigned long)iq_stream_cfg.sample_freq, attempt);
            break;
        }
        ESP_LOGD(TAG, "UAC resume attempt %d/%u failed: %s",
                 attempt,
                 (unsigned)APP_IQ_RESUME_RETRY_MAX_ATTEMPTS,
                 esp_err_to_name(serr));
    }
    if (serr != ESP_OK)
    {
        ESP_LOGE(TAG, "UAC stream did not start after %u attempts",
                 (unsigned)APP_IQ_RESUME_RETRY_MAX_ATTEMPTS);
        s_next_iq_start_allowed_tick = xTaskGetTickCount() + pdMS_TO_TICKS(APP_IQ_START_BACKOFF_AFTER_RESUME_FAIL_MS);
        return;
    }

    xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
    s_iq_stream_started = true;
    xSemaphoreGive(s_dev_mutex);

    xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
    bool spectrum_started = s_spectrum_started;
    xSemaphoreGive(s_dev_mutex);

    if (!spectrum_started)
    {
        if (!app_runtime_conn_epoch_is_current(epoch) || runtime_uac_disconnect_latched())
        {
            ESP_LOGW(TAG, "spectrum init aborted due to disconnect");
            return;
        }
        lv_obj_t *canvas = display_show_spectrum();
        if (canvas != NULL)
        {
            spectrum_cfg_t sp_cfg;
            spectrum_default_cfg(&sp_cfg);
            esp_err_t sp_err = spectrum_init(canvas, &sp_cfg);
            if (sp_err != ESP_OK)
            {
                ESP_LOGE(TAG, "spectrum_init failed: %s", esp_err_to_name(sp_err));
                s_next_iq_start_allowed_tick = xTaskGetTickCount() + pdMS_TO_TICKS(APP_IQ_START_BACKOFF_AFTER_SPECTRUM_FAIL_MS);
                xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
                s_spectrum_started = false;
                xSemaphoreGive(s_dev_mutex);
            }
            else
            {
                app_settings_apply_to_runtime();
                cat_status_t cat_st;
                if (cat_service_get_status(&cat_st))
                {
                    spectrum_set_filter_params(cat_st.mode_code, cat_st.filter_hz);
                }
                xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
                s_spectrum_started = true;
                xSemaphoreGive(s_dev_mutex);
            }
        }
    }
}

static void iq_start_task(void *arg)
{
    (void)arg;
    start_iq_stream_and_spectrum();
    xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
    s_iq_start_in_progress = false;
    xSemaphoreGive(s_dev_mutex);
    vTaskDelete(NULL);
}

static void uac_device_callback(uac_host_device_handle_t handle,
                                const uac_host_device_event_t event, void *arg)
{
    if (event == UAC_HOST_DEVICE_EVENT_RX_DONE)
    {
        if (runtime_uac_disconnect_latched())
        {
            return;
        }
        spectrum_notify_rx(handle);
        return;
    }
    if (event == UAC_HOST_DRIVER_EVENT_DISCONNECTED)
    {
        cat_service_request_stop_fast();
        xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
        s_disc.in_progress = true;
        s_disc.uac_seen = true;
        bool pending = s_disc.uac_pending;
        xSemaphoreGive(s_dev_mutex);
        /* Stop spectrum read loop immediately from callback context. */
        spectrum_notify_rx(NULL);

        if (pending || !recovery_on_disconnect(RECOVERY_SRC_UAC))
        {
            return;
        }
        xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
        s_disc.uac_pending = true;
        xSemaphoreGive(s_dev_mutex);
        if (!app_runtime_queue_uac_event(UAC_EVT_DISCONNECT, handle))
        {
            bool should_log = false;
            xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
            if (!s_disc.uac_drop_logged)
            {
                s_disc.uac_drop_logged = true;
                should_log = true;
            }
            s_disc.uac_pending = false;
            xSemaphoreGive(s_dev_mutex);
            if (should_log)
            {
                ESP_LOGW(TAG, "drop UAC disconnect event (queue full)");
            }
            (void)recovery_request();
        }
    }
    (void)arg;
}

static void uac_driver_callback(uint8_t addr, uint8_t iface_num,
                                const uac_host_driver_event_t event, void *arg)
{
    if (event != UAC_HOST_DRIVER_EVENT_RX_CONNECTED)
    {
        return;
    }

    display_set_status(DISP_MSG_USB_FOUND);

    const uac_host_device_config_t dev_config = {
        .addr = addr,
        .iface_num = iface_num,
        .buffer_size = 12288,
        .buffer_threshold = 0,
        .callback = uac_device_callback,
        .callback_arg = NULL,
    };

    uac_host_device_handle_t handle;
    esp_err_t err = uac_host_device_open(&dev_config, &handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to open UAC Microphone (addr=%d iface=%d): %s",
                 addr, iface_num, esp_err_to_name(err));
        return;
    }
    app_runtime_store_uac_handle(handle);
    xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
    s_disc.in_progress = false;
    s_disc.uac_pending = false;
    s_disc.uac_drop_logged = false;
    s_disc.uac_seen = false;
    xSemaphoreGive(s_dev_mutex);
    s_next_iq_start_allowed_tick = 0;
    print_uac_device_info(handle, "Microphone");

    uint8_t channels = 2;
    uint8_t bit_res = 24;
    uint32_t freq = 48000;
    uac_host_dev_info_t info = {0};
    uint8_t alt_count = 1;
    if (uac_host_get_device_info(handle, &info) == ESP_OK && info.iface_alt_num > 0)
    {
        alt_count = info.iface_alt_num;
    }

    bool stream_params_set = false;
    for (uint8_t alt = 1; alt <= alt_count; alt++)
    {
        uac_host_dev_alt_param_t ap = {0};
        if (uac_host_get_device_alt_param(handle, alt, &ap) == ESP_OK)
        {
            uint32_t sf = (ap.sample_freq_type > 0) ? ap.sample_freq[0] : ap.sample_freq_lower;
            ESP_LOGD(TAG, "Alt%u fmt=%u ch=%u bits=%u sf=%lu",
                     alt, ap.format, ap.channels, ap.bit_resolution, (unsigned long)sf);

            if (!stream_params_set)
            {
                channels = ap.channels;
                bit_res = ap.bit_resolution;
                freq = sf;
                stream_params_set = true;
            }
        }
    }

    if (stream_params_set)
    {
        ESP_LOGD(TAG, "Stream: %u ch, %u bit, %lu Hz", channels, bit_res, (unsigned long)freq);
    }
    else
    {
        ESP_LOGW(TAG, "No valid UAC alt settings read; using defaults");
    }

    spectrum_set_stream_params(channels, bit_res, freq);

    xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
    s_iq_handle = handle;
    s_iq_stream_cfg.channels = channels;
    s_iq_stream_cfg.bit_resolution = bit_res;
    s_iq_stream_cfg.sample_freq = freq;
    s_iq_stream_cfg.flags = 0;
    xSemaphoreGive(s_dev_mutex);

    cat_status_t cat_st;
    if (cat_service_get_status(&cat_st) && cat_st.state == CAT_ST_IQ_ACTIVE)
    {
        app_runtime_trigger_iq_start_async();
    }
    (void)arg;
}

void app_runtime_close_all_uac_handles(void)
{
    xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
    for (int i = 0; i < APP_RUNTIME_MAX_UAC_DEVICES; i++)
    {
        if (s_uac_handles[i] != NULL)
        {
            esp_err_t cerr = uac_host_device_close(s_uac_handles[i]);
            if (cerr != ESP_OK && cerr != ESP_ERR_INVALID_STATE)
            {
                ESP_LOGW(TAG, "uac_host_device_close: %s", esp_err_to_name(cerr));
            }
            s_uac_handles[i] = NULL;
        }
    }
    xSemaphoreGive(s_dev_mutex);
}

void app_runtime_uac_install_driver(void)
{
    xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
    bool already_installed = s_install.uac_installed;
    xSemaphoreGive(s_dev_mutex);
    if (already_installed)
    {
        return;
    }
    if (s_install.stop_events == NULL)
    {
        s_install.stop_events = xEventGroupCreate();
        if (!s_install.stop_events)
        {
            ESP_LOGE(TAG, "failed to create stop event group");
            abort();
        }
    }
    xEventGroupClearBits(s_install.stop_events, APP_STOPBIT_UAC_TASK);

    s_uac_evt_queue = xQueueCreate(UAC_EVT_QUEUE_LEN, sizeof(uac_evt_t));
    if (s_uac_evt_queue == NULL)
    {
        ESP_LOGE(TAG, "failed to create UAC event queue");
        abort();
    }
    BaseType_t ok = xTaskCreate(app_runtime_uac_evt_task, "uac_evt", 4096, NULL,
                                s_uac_task_priority - 1, &s_uac_evt_task);
    if (ok != pdTRUE)
    {
        vQueueDelete(s_uac_evt_queue);
        s_uac_evt_queue = NULL;
        ESP_LOGE(TAG, "failed to create UAC event task");
        abort();
    }

    const uac_host_driver_config_t uac_config = {
        .create_background_task = true,
        .task_priority = s_uac_task_priority,
        .stack_size = 4096,
        .core_id = 0,
        .callback = uac_driver_callback,
        .callback_arg = NULL,
    };
    ESP_ERROR_CHECK(uac_host_install(&uac_config));
    xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
    s_install.uac_installed = true;
    xSemaphoreGive(s_dev_mutex);
}

void app_runtime_uac_uninstall_driver(void)
{
    xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
    bool installed = s_install.uac_installed;
    xSemaphoreGive(s_dev_mutex);
    if (!installed)
    {
        return;
    }
    if (s_uac_evt_queue)
    {
        (void)app_runtime_queue_uac_event(UAC_EVT_STOP, NULL);
    }
    if (s_uac_evt_task && s_install.stop_events)
    {
        EventBits_t bits = xEventGroupWaitBits(s_install.stop_events,
                                               APP_STOPBIT_UAC_TASK,
                                               pdTRUE,
                                               pdFALSE,
                                               pdMS_TO_TICKS(APP_TASK_STOP_WAIT_MS));
        if ((bits & APP_STOPBIT_UAC_TASK) == 0)
        {
            ESP_LOGW(TAG, "UAC event task stop timeout");
        }
    }
    if (s_uac_evt_queue)
    {
        vQueueDelete(s_uac_evt_queue);
        s_uac_evt_queue = NULL;
    }
    xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
    s_disc.uac_pending = false;
    s_disc.in_progress = false;
    s_disc.uac_drop_logged = false;
    s_disc.uac_seen = false;
    xSemaphoreGive(s_dev_mutex);
    s_next_iq_start_allowed_tick = 0;

    esp_err_t err = uac_host_uninstall();
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "uac_host_uninstall: %s", esp_err_to_name(err));
    }
    xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
    s_install.uac_installed = false;
    xSemaphoreGive(s_dev_mutex);
}

void app_runtime_trigger_iq_start_async(void)
{
    xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
    bool should_start = !s_iq_stream_started && !s_iq_start_in_progress;
    if (should_start)
    {
        s_iq_start_in_progress = true;
    }
    xSemaphoreGive(s_dev_mutex);

    if (should_start)
    {
        BaseType_t ok = xTaskCreate(iq_start_task, "iq_start", 4096, NULL, s_uac_task_priority - 1, NULL);
        if (ok != pdTRUE)
        {
            xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
            s_iq_start_in_progress = false;
            xSemaphoreGive(s_dev_mutex);
            ESP_LOGE(TAG, "Failed to create IQ start task");
        }
    }
}
