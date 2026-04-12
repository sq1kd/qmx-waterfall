/*
 * SPDX-FileCopyrightText: 2021-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <assert.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_intr_alloc.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "usb/usb_host.h"
#include "display/display.h"
#include "serial/cli.h"
#include "app/runtime.h"
#include "app/recovery.h"
#include "app/settings.h"
#include "app/timing_config.h"

#define HOST_LIB_TASK_PRIORITY 2
#define UAC_TASK_PRIORITY 3
#define APP_QUIT_PIN CONFIG_APP_QUIT_PIN

static const char *TAG = "usb_audio_cdc_info";

static QueueHandle_t s_app_queue;
static SemaphoreHandle_t s_dev_mutex;
static volatile bool s_usb_lib_stop_requested;
static TaskHandle_t s_usb_lib_task_handle;

static void usb_lib_task(void *arg);

static void configure_prod_log_levels(void)
{
    esp_log_level_set("USB HOST", ESP_LOG_NONE);
    esp_log_level_set("uac-host", ESP_LOG_INFO);
    esp_log_level_set("USBH_CDC", ESP_LOG_ERROR);
    esp_log_level_set("cdc_descriptor", ESP_LOG_WARN);
}

static void gpio_cb(void *arg)
{
    const app_evt_t quit = APP_EVT_QUIT;
    BaseType_t xTaskWoken = pdFALSE;
    xQueueSendFromISR(s_app_queue, &quit, &xTaskWoken);
    if (xTaskWoken == pdTRUE)
    {
        portYIELD_FROM_ISR();
    }
    (void)arg;
}

static void start_usb_host_task(TaskHandle_t *usb_lib_task_hdl)
{
    s_usb_lib_stop_requested = false;
    BaseType_t ret = xTaskCreatePinnedToCore(usb_lib_task, "usb_lib", 4096,
                                             xTaskGetCurrentTaskHandle(),
                                             HOST_LIB_TASK_PRIORITY,
                                             usb_lib_task_hdl, 0);
    assert(ret == pdTRUE);
    ulTaskNotifyTake(false, portMAX_DELAY);
}

static void stop_usb_host_task(TaskHandle_t *usb_lib_task_hdl)
{
    if (!usb_lib_task_hdl || !*usb_lib_task_hdl)
        return;

    app_runtime_shutdown_clients();

    s_usb_lib_stop_requested = true;
    esp_err_t unblk = usb_host_lib_unblock();
    if (unblk != ESP_OK && unblk != ESP_ERR_INVALID_STATE)
    {
        ESP_LOGW(TAG, "usb_host_lib_unblock: %s", esp_err_to_name(unblk));
    }

    /* Wait for usb_lib task to stop event loop and notify. */
    if (ulTaskNotifyTake(false, pdMS_TO_TICKS(APP_USB_LIB_STOP_WAIT_MS)) == 0)
    {
        ESP_LOGW(TAG, "usb_lib task did not finish in time");
    }
    if (s_usb_lib_task_handle == *usb_lib_task_hdl)
    {
        s_usb_lib_task_handle = NULL;
    }
    *usb_lib_task_hdl = NULL;

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
    (void)app_runtime_retry_invalid_state("usb_host_uninstall", usb_host_uninstall, NULL);
}

static void usb_lib_task(void *arg)
{
    ESP_LOGI(TAG, "Installing USB Host Library");
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LOWMED,
        .peripheral_map = BIT0,
    };
    ESP_ERROR_CHECK(usb_host_install(&host_config));
    s_usb_lib_task_handle = xTaskGetCurrentTaskHandle();
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
    s_usb_lib_task_handle = NULL;
    vTaskDelete(NULL);
}

void app_main(void)
{
    configure_prod_log_levels();
    ESP_LOGI(TAG, "USB audio + CDC device info example");

    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);
    ESP_ERROR_CHECK(app_settings_init());

    display_init();

    s_dev_mutex = xSemaphoreCreateMutex();
    assert(s_dev_mutex);
    s_app_queue = xQueueCreate(8, sizeof(app_evt_t));
    assert(s_app_queue);

    app_runtime_init(s_dev_mutex, UAC_TASK_PRIORITY);
    recovery_init(s_app_queue);

    const gpio_config_t input_pin = {
        .pin_bit_mask = BIT64(APP_QUIT_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&input_pin));
    ESP_ERROR_CHECK(gpio_install_isr_service(ESP_INTR_FLAG_LOWMED));
    ESP_ERROR_CHECK(gpio_isr_handler_add(APP_QUIT_PIN, gpio_cb, NULL));

    TaskHandle_t usb_lib_task_hdl = NULL;
    start_usb_host_task(&usb_lib_task_hdl);
    app_runtime_install_usb_clients();

    ESP_LOGI(TAG, "Drivers installed. Connect device. Press BOOT (GPIO %d) to quit.",
             APP_QUIT_PIN);

    serial_cli_start();

    bool running = true;
    while (running)
    {
        app_evt_t ev;
        xQueueReceive(s_app_queue, &ev, portMAX_DELAY);

        if (ev == APP_EVT_RECOVERY_START)
        {
            uint32_t cycle_id = recovery_get_cycle_id();
            uint32_t backoff_ms = recovery_get_backoff_ms();
            TickType_t t0 = xTaskGetTickCount();
            ESP_LOGI(TAG, "USB recover start cycle=%lu backoff=%lums",
                     (unsigned long)cycle_id,
                     (unsigned long)backoff_ms);
            if (backoff_ms > 0)
            {
                vTaskDelay(pdMS_TO_TICKS(backoff_ms));
            }
            stop_usb_host_task(&usb_lib_task_hdl);
            TickType_t t_down = xTaskGetTickCount();
            start_usb_host_task(&usb_lib_task_hdl);
            TickType_t t_host = xTaskGetTickCount();
            app_runtime_install_usb_clients();
            TickType_t t_up = xTaskGetTickCount();
            recovery_record_recovery_timing((uint32_t)((t_down - t0) * portTICK_PERIOD_MS),
                                            (uint32_t)((t_host - t_down) * portTICK_PERIOD_MS),
                                            (uint32_t)((t_up - t_host) * portTICK_PERIOD_MS),
                                            (uint32_t)((t_up - t0) * portTICK_PERIOD_MS));
            ESP_LOGI(TAG, "USB recover done cycle=%lu", (unsigned long)cycle_id);
            recovery_mark_done();
            continue;
        }

        if (ev == APP_EVT_RECOVERY_DONE)
        {
            continue;
        }

        if (ev == APP_EVT_RECOVERY_TIMEOUT)
        {
            ESP_LOGE(TAG, "Recovery timeout event");
            continue;
        }

        if (ev == APP_EVT_QUIT)
        {
            ESP_LOGI(TAG, "Shutting down...");
            running = false;
        }
    }

    stop_usb_host_task(&usb_lib_task_hdl);
    (void)app_settings_flush_now();

    gpio_isr_handler_remove(APP_QUIT_PIN);
    vQueueDelete(s_app_queue);
    vSemaphoreDelete(s_dev_mutex);
    ESP_LOGI(TAG, "Done");
}
