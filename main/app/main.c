/*
 * SPDX-FileCopyrightText: 2021-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <assert.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_intr_alloc.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "display/display.h"
#include "serial/cli.h"
#include "app/app_common.h"
#include "app/runtime.h"
#include "app/recovery.h"
#include "app/settings.h"
#include "app/timing_config.h"
#include "app/usb_host_lifecycle.h"
#include "ui/spectrum_ui.h"

#define UAC_TASK_PRIORITY 3

static const char *TAG = APP_TAG;

static QueueHandle_t s_app_queue;
static SemaphoreHandle_t s_dev_mutex;

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

static void handle_recovery_start(void)
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

    TickType_t t_down = 0;
    TickType_t t_host = 0;
    if (!usb_host_lifecycle_restart(&t_down, &t_host))
    {
        /* Bringing the host back up would run install over a live instance;
         * leave the watchdog to reboot. */
        ESP_LOGE(TAG, "USB recover aborted cycle=%lu", (unsigned long)cycle_id);
        return;
    }
    app_runtime_install_usb_clients();
    TickType_t t_up = xTaskGetTickCount();
    recovery_record_recovery_timing((uint32_t)((t_down - t0) * portTICK_PERIOD_MS),
                                    (uint32_t)((t_host - t_down) * portTICK_PERIOD_MS),
                                    (uint32_t)((t_up - t_host) * portTICK_PERIOD_MS),
                                    (uint32_t)((t_up - t0) * portTICK_PERIOD_MS));
    ESP_LOGI(TAG, "USB recover done cycle=%lu", (unsigned long)cycle_id);
    recovery_mark_done();
}

void app_main(void)
{
    configure_prod_log_levels();

    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);
    ESP_ERROR_CHECK(app_settings_init());

    display_init();
    spectrum_ui_init();

    s_dev_mutex = xSemaphoreCreateMutex();
    assert(s_dev_mutex);
    s_app_queue = xQueueCreate(8, sizeof(app_evt_t));
    assert(s_app_queue);

    app_runtime_init(s_dev_mutex, UAC_TASK_PRIORITY);
    recovery_init(s_app_queue);

    const gpio_config_t input_pin = {
        .pin_bit_mask = BIT64(CONFIG_APP_QUIT_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    ESP_ERROR_CHECK(gpio_config(&input_pin));
    ESP_ERROR_CHECK(gpio_install_isr_service(ESP_INTR_FLAG_LOWMED));
    ESP_ERROR_CHECK(gpio_isr_handler_add(CONFIG_APP_QUIT_PIN, gpio_cb, NULL));

    if (!usb_host_lifecycle_start())
    {
        abort();
    }
    app_runtime_install_usb_clients();

    ESP_LOGI(TAG, "Drivers installed. Connect device. Press BOOT (GPIO %d) to quit.",
             CONFIG_APP_QUIT_PIN);

    serial_cli_start();

    bool running = true;
    while (running)
    {
        app_evt_t ev;
        if (xQueueReceive(s_app_queue, &ev, portMAX_DELAY) != pdTRUE)
        {
            continue;
        }

        switch (ev)
        {
        case APP_EVT_RECOVERY_START:
            handle_recovery_start();
            break;
        case APP_EVT_RECOVERY_DONE:
            break;
        case APP_EVT_RECOVERY_TIMEOUT:
            ESP_LOGE(TAG, "Recovery timeout event");
            break;
        case APP_EVT_QUIT:
            ESP_LOGI(TAG, "Shutting down...");
            running = false;
            break;
        default:
            break;
        }
    }

    (void)usb_host_lifecycle_stop();
    display_show_stopped();
    (void)app_settings_flush_now();

    gpio_isr_handler_remove(CONFIG_APP_QUIT_PIN);
    vQueueDelete(s_app_queue);
    vSemaphoreDelete(s_dev_mutex);
    ESP_LOGI(TAG, "Done");
}
