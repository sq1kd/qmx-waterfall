#include "app/runtime.h"

#include <string.h>
#include "esp_log.h"
#include "app/runtime_priv.h"
#include "cat/service.h"
#include "spectrum/spectrum.h"
#include "display/display.h"

static const char *TAG = "runtime";

SemaphoreHandle_t s_dev_mutex;
int s_uac_task_priority;

uac_host_device_handle_t s_uac_handles[APP_RUNTIME_MAX_UAC_DEVICES];
usbh_cdc_port_handle_t s_cdc_port;
uac_host_device_handle_t s_iq_handle;
uac_host_stream_config_t s_iq_stream_cfg;
bool s_iq_stream_started;
bool s_spectrum_started;
bool s_iq_start_in_progress;

void app_runtime_set_stream_state(bool iq_started, bool spectrum_started, bool iq_start_in_progress)
{
    xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
    s_iq_stream_started = iq_started;
    s_spectrum_started = spectrum_started;
    s_iq_start_in_progress = iq_start_in_progress;
    xSemaphoreGive(s_dev_mutex);
}

void app_runtime_init(SemaphoreHandle_t dev_mutex, int uac_task_priority)
{
    s_dev_mutex = dev_mutex;
    s_uac_task_priority = uac_task_priority;
    memset(s_uac_handles, 0, sizeof(s_uac_handles));
    s_cdc_port = NULL;
    s_iq_handle = NULL;
    memset(&s_iq_stream_cfg, 0, sizeof(s_iq_stream_cfg));
    s_iq_stream_started = false;
    s_spectrum_started = false;
    s_iq_start_in_progress = false;
}

void app_runtime_install_usb_clients(void)
{
    app_runtime_uac_install_driver();
    app_runtime_cdc_install_driver();
}

void app_runtime_shutdown_clients(void)
{
    TickType_t t0 = xTaskGetTickCount();
    spectrum_stop();
    TickType_t t1 = xTaskGetTickCount();
    app_runtime_set_stream_state(false, false, false);
    app_runtime_close_all_uac_handles();
    TickType_t t2 = xTaskGetTickCount();
    cat_service_stop();
    TickType_t t3 = xTaskGetTickCount();
    app_runtime_uac_uninstall_driver();
    if (spectrum_is_running())
    {
        ESP_LOGW(TAG, "invariant: spectrum still running after uac_uninstall");
    }
    TickType_t t4 = xTaskGetTickCount();
    app_runtime_close_cdc_port_if_open();
    app_runtime_cdc_uninstall_driver();
    TickType_t t5 = xTaskGetTickCount();
    display_show_status(DISP_MSG_DISCONNECTED);
    TickType_t t6 = xTaskGetTickCount();

    ESP_LOGI(TAG,
             "shutdown timings ms: spectrum=%lu uac_close=%lu cat=%lu uac_uninst=%lu cdc=%lu ui=%lu total=%lu",
             (unsigned long)((t1 - t0) * portTICK_PERIOD_MS),
             (unsigned long)((t2 - t1) * portTICK_PERIOD_MS),
             (unsigned long)((t3 - t2) * portTICK_PERIOD_MS),
             (unsigned long)((t4 - t3) * portTICK_PERIOD_MS),
             (unsigned long)((t5 - t4) * portTICK_PERIOD_MS),
             (unsigned long)((t6 - t5) * portTICK_PERIOD_MS),
             (unsigned long)((t6 - t0) * portTICK_PERIOD_MS));
}
