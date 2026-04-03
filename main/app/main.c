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
#include "driver/gpio.h"
#include "usb/usb_host.h"
#include "display/display.h"
#include "serial/cli.h"
#include "app/runtime.h"

#define HOST_LIB_TASK_PRIORITY 2
#define UAC_TASK_PRIORITY 3
#define APP_QUIT_PIN CONFIG_APP_QUIT_PIN

static const char *TAG = "usb_audio_cdc_info";

static QueueHandle_t s_app_queue;
static SemaphoreHandle_t s_dev_mutex;

static void gpio_cb(void *arg)
{
    const uint8_t quit = 1;
    BaseType_t xTaskWoken = pdFALSE;
    xQueueSendFromISR(s_app_queue, &quit, &xTaskWoken);
    if (xTaskWoken == pdTRUE)
    {
        portYIELD_FROM_ISR();
    }
    (void)arg;
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
    xTaskNotifyGive(arg);

    bool has_clients = true;
    bool has_devices = false;
    while (has_clients)
    {
        uint32_t event_flags;
        ESP_ERROR_CHECK(usb_host_lib_handle_events(portMAX_DELAY, &event_flags));
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS)
        {
            if (ESP_OK == usb_host_device_free_all())
            {
                has_clients = false;
            }
            else
            {
                has_devices = true;
            }
        }
        if (has_devices && (event_flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE))
        {
            has_clients = false;
        }
    }
    ESP_LOGI(TAG, "No more clients, uninstalling USB Host Library");
    ESP_ERROR_CHECK(usb_host_uninstall());
    xTaskNotifyGive(arg);
    vTaskSuspend(NULL);
}

void app_main(void)
{
    ESP_LOGI(TAG, "USB audio + CDC device info example");

    display_init();

    s_dev_mutex = xSemaphoreCreateMutex();
    assert(s_dev_mutex);
    s_app_queue = xQueueCreate(4, sizeof(uint8_t));
    assert(s_app_queue);

    app_runtime_init(s_dev_mutex, UAC_TASK_PRIORITY);

    const gpio_config_t input_pin = {
        .pin_bit_mask = BIT64(APP_QUIT_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&input_pin));
    ESP_ERROR_CHECK(gpio_install_isr_service(ESP_INTR_FLAG_LOWMED));
    ESP_ERROR_CHECK(gpio_isr_handler_add(APP_QUIT_PIN, gpio_cb, NULL));

    TaskHandle_t usb_lib_task_hdl;
    BaseType_t ret = xTaskCreatePinnedToCore(usb_lib_task, "usb_lib", 4096,
                                             xTaskGetCurrentTaskHandle(),
                                             HOST_LIB_TASK_PRIORITY,
                                             &usb_lib_task_hdl, 0);
    assert(ret == pdTRUE);

    ulTaskNotifyTake(false, portMAX_DELAY);

    app_runtime_install_usb_clients();

    ESP_LOGI(TAG, "Drivers installed. Connect device. Press BOOT (GPIO %d) to quit.",
             APP_QUIT_PIN);

    serial_cli_start();

    uint8_t quit;
    xQueueReceive(s_app_queue, &quit, portMAX_DELAY);
    ESP_LOGI(TAG, "Shutting down...");

    app_runtime_shutdown_clients();

    ulTaskNotifyTake(false, portMAX_DELAY);
    vTaskDelete(usb_lib_task_hdl);

    gpio_isr_handler_remove(APP_QUIT_PIN);
    vQueueDelete(s_app_queue);
    vSemaphoreDelete(s_dev_mutex);
    ESP_LOGI(TAG, "Done");
}
