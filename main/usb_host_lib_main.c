/*
 * SPDX-FileCopyrightText: 2021-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_intr_alloc.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "usb/usb_host.h"
#include "usb/uac_host.h"
#include "iot_usbh_cdc.h"
#include "display.h"
#include "spectrum.h"
#include "cat_service.h"

#define HOST_LIB_TASK_PRIORITY 2
#define UAC_TASK_PRIORITY 3
#define APP_QUIT_PIN CONFIG_APP_QUIT_PIN
#define MAX_UAC_DEVICES 8
/*
 * CDC-ACM comm interface number (0 for the device in use).
 * The driver auto-discovers the paired data interface via the IAD descriptor.
 */
#define CDC_COMM_ITF_NUM 0

static const char *TAG = "usb_audio_cdc_info";

static QueueHandle_t s_app_queue;
static SemaphoreHandle_t s_dev_mutex;
static uac_host_device_handle_t s_uac_handles[MAX_UAC_DEVICES];
static usbh_cdc_port_handle_t s_cdc_port;

/* Live spectrum controls via idf-monitor (UART console) */
static float s_sp_offset_db = -18.0f;

/* UAC handle + stream config for the IQ audio stream. Written in
 * uac_driver_callback, consumed after CAT service confirms IQ mode. */
static uac_host_device_handle_t s_iq_handle = NULL;
static uac_host_stream_config_t s_iq_stream_cfg = {0};
static bool s_iq_stream_started = false;
static bool s_spectrum_started = false;
static bool s_iq_start_in_progress = false;

static void trigger_iq_start_async(void);

/* --- GPIO quit button ---------------------------------------------------- */

static void gpio_cb(void *arg)
{
    const uint8_t quit = 1;
    BaseType_t xTaskWoken = pdFALSE;
    xQueueSendFromISR(s_app_queue, &quit, &xTaskWoken);
    if (xTaskWoken == pdTRUE)
    {
        portYIELD_FROM_ISR();
    }
}

/* --- UAC handle bookkeeping ---------------------------------------------- */

static void store_uac_handle(uac_host_device_handle_t handle)
{
    xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_UAC_DEVICES; i++)
    {
        if (s_uac_handles[i] == NULL)
        {
            s_uac_handles[i] = handle;
            break;
        }
    }
    xSemaphoreGive(s_dev_mutex);
}

static void remove_uac_handle(uac_host_device_handle_t handle)
{
    xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_UAC_DEVICES; i++)
    {
        if (s_uac_handles[i] == handle)
        {
            s_uac_handles[i] = NULL;
            break;
        }
    }
    xSemaphoreGive(s_dev_mutex);
}

/* --- UAC printing helpers ------------------------------------------------- */

/* USB string descriptors are UTF-16LE; extract the ASCII-range low byte. */
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

/* --- UAC callbacks -------------------------------------------------------- */

static void uac_device_callback(uac_host_device_handle_t handle,
                                const uac_host_device_event_t event, void *arg)
{
    if (event == UAC_HOST_DEVICE_EVENT_RX_DONE)
    {
        /* Wake the spectrum task (core 1) to consume audio data */
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
        remove_uac_handle(handle);
        esp_err_t cerr = uac_host_device_close(handle);
        if (cerr != ESP_OK)
        {
            ESP_LOGW(TAG, "uac_host_device_close: %s", esp_err_to_name(cerr));
        }
    }
}

static void uac_driver_callback(uint8_t addr, uint8_t iface_num,
                                const uac_host_driver_event_t event, void *arg)
{
    /* Only interested in microphone (RX) */
    if (event != UAC_HOST_DRIVER_EVENT_RX_CONNECTED)
    {
        return;
    }

    display_set_status(DISP_MSG_USB_FOUND);

    /*
     * Large ring buffer: ~20 ms of 24-bit stereo @ 48 kHz = 5760 bytes.
     * Round up and double to absorb bursts.  buffer_threshold=0 → driver
     * fires RX_DONE as soon as any data arrives (25% default).
     */
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
    store_uac_handle(handle);
    print_uac_device_info(handle, "Microphone");

    /* Discover stream parameters from the first alt setting */
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

    /* Store handle and stream config — stream is started after CAT service
     * confirms IQ mode is active. */
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
        trigger_iq_start_async();
    }
}

/* --- CDC callbacks -------------------------------------------------------- */

static void cdc_notif_callback(usbh_cdc_port_handle_t port,
                               iot_cdc_notification_t *notif, void *arg)
{
    switch (notif->bNotificationCode)
    {
    case USB_CDC_NOTIFY_SERIAL_STATE:
    {
        usb_cdc_serial_state_t *state = (usb_cdc_serial_state_t *)notif->Data;
        ESP_LOGI(TAG, "[CDC] SerialState  DCD=%d DSR=%d RI=%d framing_err=%d",
                 state->dcd, state->dsr, state->ring, state->framing_err);
        break;
    }
    case USB_CDC_NOTIFY_NETWORK_CONNECTION:
        ESP_LOGI(TAG, "[CDC] NetworkConnection: %s", notif->wValue ? "connected" : "disconnected");
        break;
    default:
        ESP_LOGI(TAG, "[CDC] Notification 0x%02x", notif->bNotificationCode);
        break;
    }
}

static void cdc_recv_callback(usbh_cdc_port_handle_t port, void *arg)
{
    (void)port;
    (void)arg;
    size_t len = 0;
    usbh_cdc_get_rx_buffer_size(port, &len);
    if (len > 0)
    {
        uint8_t buf[128];
        size_t rd = (len >= sizeof(buf)) ? (sizeof(buf) - 1) : len;
        usbh_cdc_read_bytes(port, buf, &rd, 0);
        cat_service_on_rx_bytes(buf, rd);
    }
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

static void trigger_iq_start_async(void)
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
        BaseType_t ok = xTaskCreate(iq_start_task, "iq_start", 4096, NULL, UAC_TASK_PRIORITY - 1, NULL);
        if (ok != pdTRUE)
        {
            xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
            s_iq_start_in_progress = false;
            xSemaphoreGive(s_dev_mutex);
            ESP_LOGE(TAG, "Failed to create IQ start task");
        }
    }
}

static void cat_status_callback(const cat_status_t *st, void *user_ctx)
{
    (void)user_ctx;
    if (!st)
    {
        return;
    }

    char msg[96];
    spectrum_set_filter_params(st->mode_code, st->filter_hz);

    if (st->state == CAT_ST_IQ_ACTIVE)
    {
        snprintf(msg, sizeof(msg), "CAT: %s  M:%s  F:%s",
                 cat_state_to_str(st->state),
                 st->mode[0] ? st->mode : "?",
                 st->filter[0] ? st->filter : "?");
        display_set_status(msg);
        trigger_iq_start_async();
        return;
    }

    if (st->state == CAT_ST_IQ_ENABLING || st->state == CAT_ST_IQ_VERIFY)
    {
        display_set_status(DISP_MSG_ENABLING_IQ);
        return;
    }

    if (st->state == CAT_ST_DEGRADED || st->state == CAT_ST_FAULT)
    {
        snprintf(msg, sizeof(msg), "CAT %s t/o:%lu retry:%lu",
                 cat_state_to_str(st->state),
                 (unsigned long)st->timeouts,
                 (unsigned long)st->retries);
        display_set_status(msg);
        return;
    }

    snprintf(msg, sizeof(msg), "CAT: %s  M:%s  F:%s",
             cat_state_to_str(st->state),
             st->mode[0] ? st->mode : "?",
             st->filter[0] ? st->filter : "?");
    display_set_status(msg);
}

#define CLI_UART_NUM UART_NUM_0
#define CLI_BUF_SZ 128

static void handle_cli_line(const char *line)
{
    float val = 0.0f;
    int mode = -1;

    if (sscanf(line, "sp off %f", &val) == 1)
    {
        s_sp_offset_db = val;
        spectrum_set_level_offset_db(val);
        ESP_LOGI(TAG, "[sp] level_offset_db=%.1f", s_sp_offset_db);
    }
    else if (strncmp(line, "sp clear", 8) == 0)
    {
        spectrum_clear();
        ESP_LOGI(TAG, "[sp] cleared");
    }
    else if (sscanf(line, "sp view %d", &mode) == 1)
    {
        if (mode < 0) mode = 0;
        if (mode > 1) mode = 1;
        spectrum_set_view_mode((uint8_t)mode);
        ESP_LOGI(TAG, "[sp] view=%d (0=full 1=pos-only)", mode);
    }
    else if (strncmp(line, "sp range ", 9) == 0)
    {
        float floor_db = 0.0f, ceil_db = 0.0f;
        if (sscanf(line + 9, "%f %f", &floor_db, &ceil_db) == 2)
        {
            spectrum_set_wf_range(floor_db, ceil_db);
            ESP_LOGI(TAG, "[sp] range=%.1f..%.1f dB", floor_db, ceil_db);
        }
        else
        {
            ESP_LOGI(TAG, "[sp] usage: sp range <floor_dB> <ceil_dB>");
        }
    }
    else if (strncmp(line, "sp", 2) == 0)
    {
        ESP_LOGI(TAG, "[sp] usage: sp off <dB> | sp clear | sp view <0|1> | sp range <floor> <ceil>");
    }
}

static void monitor_cli_task(void *arg)
{
    (void)arg;
    uint8_t rx[64];
    char line[CLI_BUF_SZ];
    size_t line_len = 0;

    ESP_LOGI(TAG, "UART CLI ready on UART0. Commands: sp ...");
    while (true)
    {
        int n = uart_read_bytes(CLI_UART_NUM, rx, sizeof(rx), pdMS_TO_TICKS(50));
        if (n <= 0)
        {
            continue;
        }

        for (int i = 0; i < n; i++)
        {
            char c = (char)rx[i];
            if (c == '\r' || c == '\n')
            {
                if (line_len > 0)
                {
                    line[line_len] = '\0';
                    handle_cli_line(line);
                    line_len = 0;
                }
                continue;
            }
            if (line_len + 1 < sizeof(line))
            {
                line[line_len++] = c;
            }
            else
            {
                line_len = 0;
            }
        }
    }
}

static void cdc_dev_event_callback(usbh_cdc_device_event_t event,
                                   usbh_cdc_device_event_data_t *event_data,
                                   void *user_ctx)
{
    switch (event)
    {
    case CDC_HOST_DEVICE_EVENT_CONNECTED:
    {
        ESP_LOGI(TAG, "=== CDC device connected (addr=%d) ===",
                 event_data->new_dev.dev_addr);

        const usbh_cdc_port_config_t port_cfg = {
            .dev_addr = event_data->new_dev.dev_addr,
            .itf_num = CDC_COMM_ITF_NUM,
            .in_ringbuf_size = 512,
            .out_ringbuf_size = 512,
            .in_transfer_buffer_size = 512,
            .out_transfer_buffer_size = 512,
            .cbs = {
                .recv_data = cdc_recv_callback,
                .notif_cb = cdc_notif_callback,
                .user_data = NULL,
            },
        };

        usbh_cdc_port_handle_t port = NULL;
        esp_err_t err = usbh_cdc_port_open(&port_cfg, &port);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "CDC port open failed: %s", esp_err_to_name(err));
            break;
        }

        xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
        s_cdc_port = port;
        s_iq_stream_started = false;
        s_spectrum_started = false;
        s_iq_start_in_progress = false;
        xSemaphoreGive(s_dev_mutex);

        cat_service_config_t cat_cfg;
        cat_service_default_config(&cat_cfg);
        cat_cfg.auto_enable_iq = true;
        cat_cfg.poll_period_ms = 1000;
        cat_cfg.status_cb = cat_status_callback;
        cat_cfg.status_cb_ctx = NULL;
        err = cat_service_start(port, &cat_cfg);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "cat_service_start failed: %s", esp_err_to_name(err));
        }

        /* Log which interfaces were claimed */
        const usb_intf_desc_t *notif_intf = NULL, *data_intf = NULL;
        usbh_cdc_port_get_intf_desc(port, &notif_intf, &data_intf);
        if (notif_intf)
        {
            ESP_LOGI(TAG, "  CDC Control interface: %d", notif_intf->bInterfaceNumber);
        }
        if (data_intf)
        {
            ESP_LOGI(TAG, "  CDC Data interface:    %d", data_intf->bInterfaceNumber);
        }
        break;
    }
    case CDC_HOST_DEVICE_EVENT_DISCONNECTED:
        /*
         * The driver already auto-closes the port after this callback returns.
         * Only invalidate our reference — do NOT call usbh_cdc_port_close here.
         */
        ESP_LOGI(TAG, "=== CDC device disconnected ===");
        cat_service_stop();
        display_set_status(DISP_MSG_DISCONNECTED);
        xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
        s_cdc_port = NULL;
        s_iq_stream_started = false;
        s_spectrum_started = false;
        s_iq_start_in_progress = false;
        xSemaphoreGive(s_dev_mutex);
        break;
    default:
        break;
    }
}

/* --- USB host library task ------------------------------------------------ */

static void usb_lib_task(void *arg)
{
    ESP_LOGI(TAG, "Installing USB Host Library");
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LOWMED,
        .peripheral_map = BIT0,
    };
    ESP_ERROR_CHECK(usb_host_install(&host_config));
    xTaskNotifyGive(arg); /* signal app_main: install done */

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
    xTaskNotifyGive(arg); /* signal app_main: uninstall done */
    vTaskSuspend(NULL);
}

/* --- Entry point ---------------------------------------------------------- */

void app_main(void)
{
    ESP_LOGI(TAG, "USB audio + CDC device info example");

    display_init();

    s_dev_mutex = xSemaphoreCreateMutex();
    assert(s_dev_mutex);
    s_app_queue = xQueueCreate(4, sizeof(uint8_t));
    assert(s_app_queue);

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

    /* Wait for USB host library to be installed */
    ulTaskNotifyTake(false, portMAX_DELAY);

    /*
     * Install UAC driver.
     * create_background_task=true: driver spawns its own task for UAC client
     * events (usb_host_client_handle_events), separate from usb_lib_task which
     * handles library-level events (usb_host_lib_handle_events).
     */
    const uac_host_driver_config_t uac_config = {
        .create_background_task = true,
        .task_priority = UAC_TASK_PRIORITY,
        .stack_size = 4096,
        .core_id = 0,
        .callback = uac_driver_callback,
        .callback_arg = NULL,
    };
    ESP_ERROR_CHECK(uac_host_install(&uac_config));

    /*
     * Install CDC driver.
     * skip_init_usb_host_driver=true: do NOT let the CDC driver start its own
     * usb_lib_task — usb_lib_task above owns the USB host library lifecycle.
     */
    const usbh_cdc_driver_config_t cdc_config = {
        .task_stack_size = 4096,
        .task_priority = UAC_TASK_PRIORITY,
        .task_coreid = 0,
        .skip_init_usb_host_driver = true,
    };
    ESP_ERROR_CHECK(usbh_cdc_driver_install(&cdc_config));

    /* Match any USB device — both audio and CDC will share the same physical device */
    ESP_ERROR_CHECK(usbh_cdc_register_dev_event_cb(ESP_USB_DEVICE_MATCH_ID_ANY,
                                                   cdc_dev_event_callback, NULL));

    ESP_LOGI(TAG, "Drivers installed. Connect device. Press BOOT (GPIO %d) to quit.",
             APP_QUIT_PIN);

    uart_driver_install(CLI_UART_NUM, 1024, 0, 0, NULL, 0);

    /* Runtime tuning via UART monitor without touching CDC/CAT link. */
    xTaskCreate(monitor_cli_task, "mon_cli", 4096, NULL, 1, NULL);

    /* Block until quit button pressed */
    uint8_t quit;
    xQueueReceive(s_app_queue, &quit, portMAX_DELAY);
    ESP_LOGI(TAG, "Shutting down...");

    /* 1. Close all open UAC device handles */
    xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_UAC_DEVICES; i++)
    {
        if (s_uac_handles[i] != NULL)
        {
            uac_host_device_close(s_uac_handles[i]);
            s_uac_handles[i] = NULL;
        }
    }
    xSemaphoreGive(s_dev_mutex);

    /* 2. Stop CAT before tearing down USB clients. */
    cat_service_stop();

    /* 3. Deregister UAC client */
    ESP_ERROR_CHECK(uac_host_uninstall());

    /*
     * 4. Close CDC port if device is still connected.
     *    On a prior disconnect the driver auto-closes and our callback clears
     *    s_cdc_port, so this only executes when shut down while connected.
     */
    xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
    usbh_cdc_port_handle_t port_to_close = s_cdc_port;
    s_cdc_port = NULL;
    xSemaphoreGive(s_dev_mutex);

    if (port_to_close != NULL)
    {
        usbh_cdc_port_close(port_to_close);
    }

    /* 5. Deregister CDC client — no clients left, USB lib fires NO_CLIENTS */
    ESP_ERROR_CHECK(usbh_cdc_driver_uninstall());

    /* 6. Wait for usb_lib_task to finish uninstalling the USB host library */
    ulTaskNotifyTake(false, portMAX_DELAY);
    vTaskDelete(usb_lib_task_hdl);

    gpio_isr_handler_remove(APP_QUIT_PIN);
    vQueueDelete(s_app_queue);
    vSemaphoreDelete(s_dev_mutex);
    ESP_LOGI(TAG, "Done");
}
