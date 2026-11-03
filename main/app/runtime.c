#include "app/runtime.h"

#include <string.h>
#include "freertos/task.h"
#include "esp_log.h"
#include "app/app_common.h"
#include "app/runtime_priv.h"
#include "app/timing_config.h"
#include "cat/service.h"
#include "spectrum/spectrum.h"
#include "display/display.h"
#include "ui/spectrum_ui.h"

static const char *TAG = APP_TAG;

SemaphoreHandle_t s_dev_mutex;
int s_uac_task_priority;

uac_host_device_handle_t s_uac_handles[APP_RUNTIME_MAX_UAC_DEVICES];
usbh_cdc_port_handle_t s_cdc_port;
uac_host_device_handle_t s_iq_handle;
uac_host_stream_config_t s_iq_stream_cfg;
bool s_iq_stream_started;
bool s_spectrum_started;
bool s_iq_start_in_progress;
runtime_disconnect_state_t s_disc;
runtime_install_state_t s_install;
static uint32_t s_conn_epoch;

esp_err_t app_runtime_retry_invalid_state(const char *op_name, app_runtime_noarg_fn_t fn)
{
    if (!fn)
    {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ESP_ERR_INVALID_STATE;
    uint32_t attempts = 0;
    while (attempts < APP_USB_UNINSTALL_RETRY_MAX_ATTEMPTS)
    {
        err = fn();
        attempts++;
        if (err != ESP_ERR_INVALID_STATE || attempts >= APP_USB_UNINSTALL_RETRY_MAX_ATTEMPTS)
        {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(APP_USB_UNINSTALL_RETRY_DELAY_MS));
    }

    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "%s: %s (attempts=%lu)",
                 op_name ? op_name : "retry_op",
                 esp_err_to_name(err),
                 (unsigned long)attempts);
    }
    else if (attempts > 1)
    {
        ESP_LOGI(TAG, "%s recovered after %lu retries",
                 op_name ? op_name : "retry_op",
                 (unsigned long)(attempts - 1));
    }

    return err;
}

uint32_t app_runtime_conn_epoch_get(void)
{
    uint32_t epoch;
    xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
    epoch = s_conn_epoch;
    xSemaphoreGive(s_dev_mutex);
    return epoch;
}

uint32_t app_runtime_conn_epoch_bump(void)
{
    uint32_t epoch;
    xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
    s_conn_epoch++;
    epoch = s_conn_epoch;
    xSemaphoreGive(s_dev_mutex);
    return epoch;
}

bool app_runtime_conn_epoch_is_current(uint32_t epoch)
{
    bool same;
    xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
    same = (s_conn_epoch == epoch);
    xSemaphoreGive(s_dev_mutex);
    return same;
}

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
    memset(&s_disc, 0, sizeof(s_disc));
    memset(&s_install, 0, sizeof(s_install));
    s_conn_epoch = 0;
}

void app_runtime_install_usb_clients(void)
{
    app_runtime_uac_install_driver();
    app_runtime_cdc_install_driver();
}

bool app_runtime_clients_uninstalled(void)
{
    bool done;
    xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
    done = !s_install.uac_installed && !s_install.cdc_installed;
    xSemaphoreGive(s_dev_mutex);
    return done;
}

static bool iq_start_in_progress(void)
{
    xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
    bool in_progress = s_iq_start_in_progress;
    xSemaphoreGive(s_dev_mutex);
    return in_progress;
}

void app_runtime_shutdown_clients(void)
{
    (void)app_runtime_conn_epoch_bump();
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(APP_TASK_STOP_WAIT_MS);
    while (iq_start_in_progress())
    {
        if ((int32_t)(deadline - xTaskGetTickCount()) <= 0)
        {
            ESP_LOGW(TAG, "IQ start still in progress at shutdown");
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(APP_TASK_STOP_POLL_MS));
    }

    TickType_t t0 = xTaskGetTickCount();
    spectrum_stop();
    spectrum_ui_stop();
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
    display_show_disconnected();
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
