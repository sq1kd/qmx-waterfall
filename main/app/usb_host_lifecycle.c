#include "app/usb_host_lifecycle.h"

#include "freertos/task.h"
#include "esp_log.h"
#include "esp_intr_alloc.h"
#include "usb/usb_host.h"
#include "app/app_common.h"
#include "app/runtime.h"
#include "app/timing_config.h"

#define HOST_LIB_TASK_PRIORITY 2

static const char *TAG = APP_TAG;

static volatile bool s_usb_lib_stop_requested;
static TaskHandle_t s_usb_lib_task_handle;

static void usb_lib_task(void *arg);

static void usb_lib_task(void *arg)
{
    ESP_LOGI(TAG, "Installing USB Host Library");
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LOWMED,
        .peripheral_map = BIT0,
    };
    ESP_ERROR_CHECK(usb_host_install(&host_config));
    xTaskNotifyGive(arg);

    while (!s_usb_lib_stop_requested)
    {
        uint32_t event_flags;
        esp_err_t err = usb_host_lib_handle_events(pdMS_TO_TICKS(APP_USB_LIB_EVENT_TIMEOUT_MS), &event_flags);
        if (err == ESP_ERR_TIMEOUT)
        {
            continue;
        }
        if (err != ESP_OK)
        {
            if (!s_usb_lib_stop_requested)
            {
                ESP_LOGW(TAG, "usb_host_lib_handle_events: %s", esp_err_to_name(err));
            }
            continue;
        }
    }

    xTaskNotifyGive(arg);
    vTaskDelete(NULL);
}

bool usb_host_lifecycle_start(void)
{
    if (s_usb_lib_task_handle != NULL)
    {
        return true;
    }

    s_usb_lib_stop_requested = false;
    /* A stale notification from a previously timed-out stop would otherwise
     * satisfy the ready-wait below on the next start. */
    (void)ulTaskNotifyTake(pdTRUE, 0);

    BaseType_t ret = xTaskCreatePinnedToCore(usb_lib_task, "usb_lib", 4096,
                                             xTaskGetCurrentTaskHandle(),
                                             HOST_LIB_TASK_PRIORITY,
                                             &s_usb_lib_task_handle, 0);
    if (ret != pdTRUE)
    {
        s_usb_lib_task_handle = NULL;
        ESP_LOGE(TAG, "failed to create usb_lib task");
        return false;
    }
    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    return true;
}

bool usb_host_lifecycle_stop(void)
{
    if (s_usb_lib_task_handle == NULL)
    {
        return true;
    }

    app_runtime_shutdown_clients();

    s_usb_lib_stop_requested = true;
    esp_err_t unblk = usb_host_lib_unblock();
    if (unblk != ESP_OK && unblk != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGW(TAG, "usb_host_lib_unblock: %s", esp_err_to_name(unblk));
    }

    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(APP_USB_LIB_STOP_WAIT_MS)) == 0)
    {
        ESP_LOGE(TAG, "usb_lib task did not finish in time; leaving USB host installed");
        s_usb_lib_task_handle = NULL;
        return false;
    }
    s_usb_lib_task_handle = NULL;

    if (!app_runtime_clients_uninstalled())
    {
        ESP_LOGW(TAG, "usb host teardown before clients fully uninstalled");
    }

    esp_err_t free_ret = usb_host_device_free_all();
    if (free_ret != ESP_OK && free_ret != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGW(TAG, "usb_host_device_free_all: %s", esp_err_to_name(free_ret));
    }

    ESP_LOGI(TAG, "No more clients, uninstalling USB Host Library");
    return app_runtime_retry_invalid_state("usb_host_uninstall", usb_host_uninstall) == ESP_OK;
}

bool usb_host_lifecycle_restart(TickType_t *stop_done_tick, TickType_t *start_done_tick)
{
    bool stopped = usb_host_lifecycle_stop();
    if (stop_done_tick)
    {
        *stop_done_tick = xTaskGetTickCount();
    }
    if (!stopped)
    {
        return false;
    }

    bool started = usb_host_lifecycle_start();
    if (start_done_tick)
    {
        *start_done_tick = xTaskGetTickCount();
    }
    return started;
}
