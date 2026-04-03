#include "app/runtime_priv.h"

#include <stdio.h>
#include "freertos/task.h"
#include "esp_log.h"
#include "display/display.h"
#include "spectrum/spectrum.h"
#include "cat/service.h"

static const char *TAG = "usb_audio_cdc_info";

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
        ESP_LOGI(TAG, "  %-16s %s", label, buf);
    }
}

static void print_uac_device_info(uac_host_device_handle_t handle, const char *type_str)
{
    uac_host_dev_info_t info;
    if (uac_host_get_device_info(handle, &info) != ESP_OK)
    {
        return;
    }
    ESP_LOGI(TAG, "=== UAC %s connected ===", type_str);
    ESP_LOGI(TAG, "  %-16s 0x%04X", "VID:", info.VID);
    ESP_LOGI(TAG, "  %-16s 0x%04X", "PID:", info.PID);
    log_wstr("Manufacturer:", info.iManufacturer);
    log_wstr("Product:", info.iProduct);
    log_wstr("Serial:", info.iSerialNumber);
    ESP_LOGI(TAG, "  %-16s %d  (%d alt settings)", "Interface:", info.iface_num, info.iface_alt_num);
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

static void app_runtime_remove_uac_handle(uac_host_device_handle_t handle)
{
    xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
    for (int i = 0; i < APP_RUNTIME_MAX_UAC_DEVICES; i++)
    {
        if (s_uac_handles[i] == handle)
        {
            s_uac_handles[i] = NULL;
            break;
        }
    }
    xSemaphoreGive(s_dev_mutex);
}

static void start_iq_stream_and_spectrum(void)
{
    xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
    if (s_iq_stream_started)
    {
        xSemaphoreGive(s_dev_mutex);
        return;
    }

    uac_host_device_handle_t iq_handle = s_iq_handle;
    uac_host_stream_config_t iq_stream_cfg = s_iq_stream_cfg;
    xSemaphoreGive(s_dev_mutex);

    if (iq_handle == NULL)
    {
        return;
    }

    uac_host_stream_config_t start_cfg = iq_stream_cfg;
    start_cfg.flags |= FLAG_STREAM_SUSPEND_AFTER_START;

    esp_err_t serr = uac_host_device_start(iq_handle, &start_cfg);
    if (serr != ESP_OK)
    {
        ESP_LOGE(TAG, "uac_host_device_start failed: %s", esp_err_to_name(serr));
        return;
    }

    for (int attempt = 1; attempt <= 5; attempt++)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
        serr = uac_host_device_resume(iq_handle);
        if (serr == ESP_OK)
        {
            ESP_LOGI(TAG, "UAC stream active (%u ch, %u bit, %lu Hz) [attempt %d]",
                     iq_stream_cfg.channels, iq_stream_cfg.bit_resolution,
                     (unsigned long)iq_stream_cfg.sample_freq, attempt);
            break;
        }
        ESP_LOGW(TAG, "UAC resume attempt %d/5 failed: %s", attempt, esp_err_to_name(serr));
    }
    if (serr != ESP_OK)
    {
        ESP_LOGE(TAG, "UAC stream did not start after 5 attempts");
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
        lv_obj_t *canvas = display_show_spectrum();
        if (canvas != NULL)
        {
            spectrum_cfg_t sp_cfg;
            spectrum_default_cfg(&sp_cfg);
            esp_err_t sp_err = spectrum_init(canvas, &sp_cfg);
            if (sp_err != ESP_OK)
            {
                ESP_LOGE(TAG, "spectrum_init failed: %s", esp_err_to_name(sp_err));
            }
            else
            {
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
        spectrum_notify_rx(handle);
        return;
    }
    if (event == UAC_HOST_DRIVER_EVENT_DISCONNECTED)
    {
        ESP_LOGI(TAG, "UAC device disconnected");
        spectrum_stop();
        xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
        if (s_iq_handle == handle)
        {
            s_iq_handle = NULL;
        }
        xSemaphoreGive(s_dev_mutex);
        app_runtime_remove_uac_handle(handle);
        esp_err_t cerr = uac_host_device_close(handle);
        if (cerr != ESP_OK)
        {
            ESP_LOGW(TAG, "uac_host_device_close: %s", esp_err_to_name(cerr));
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
    print_uac_device_info(handle, "Microphone");

    uac_host_dev_alt_param_t alt_param = {0};
    uint8_t channels = 2;
    uint8_t bit_res = 24;
    uint32_t freq = 48000;
    if (uac_host_get_device_alt_param(handle, 1, &alt_param) == ESP_OK)
    {
        channels = alt_param.channels;
        bit_res = alt_param.bit_resolution;
        freq = (alt_param.sample_freq_type > 0)
                   ? alt_param.sample_freq[0]
                   : alt_param.sample_freq_lower;
        ESP_LOGI(TAG, "Stream: %u ch, %u bit, %lu Hz", channels, bit_res, (unsigned long)freq);
    }
    else
    {
        ESP_LOGW(TAG, "uac_host_get_device_alt_param(1) failed; using defaults");
    }

    for (uint8_t alt = 0; alt < 8; alt++)
    {
        uac_host_dev_alt_param_t ap = {0};
        if (uac_host_get_device_alt_param(handle, alt, &ap) == ESP_OK)
        {
            uint32_t sf = (ap.sample_freq_type > 0) ? ap.sample_freq[0] : ap.sample_freq_lower;
            ESP_LOGI(TAG, "Alt%u fmt=%u ch=%u bits=%u sf=%lu",
                     alt, ap.format, ap.channels, ap.bit_resolution, (unsigned long)sf);
        }
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
            uac_host_device_close(s_uac_handles[i]);
            s_uac_handles[i] = NULL;
        }
    }
    xSemaphoreGive(s_dev_mutex);
}

void app_runtime_uac_install_driver(void)
{
    const uac_host_driver_config_t uac_config = {
        .create_background_task = true,
        .task_priority = s_uac_task_priority,
        .stack_size = 4096,
        .core_id = 0,
        .callback = uac_driver_callback,
        .callback_arg = NULL,
    };
    ESP_ERROR_CHECK(uac_host_install(&uac_config));
}

void app_runtime_uac_uninstall_driver(void)
{
    ESP_ERROR_CHECK(uac_host_uninstall());
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
