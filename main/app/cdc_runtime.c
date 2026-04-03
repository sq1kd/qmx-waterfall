#include "app/runtime_priv.h"

#include <stdio.h>
#include "esp_log.h"
#include "display/display.h"
#include "cat/service.h"
#include "spectrum/spectrum.h"

static const char *TAG = "usb_audio_cdc_info";

#define CDC_COMM_ITF_NUM 0

static void cat_status_callback(const cat_status_t *st, void *user_ctx)
{
    (void)user_ctx;
    if (!st)
    {
        return;
    }

    char msg[96];
    spectrum_set_filter_params(st->mode_code, st->filter_hz);
    spectrum_set_cat_tuning(st->mode_code, st->rit_hz, st->rit_on);

    if (st->state == CAT_ST_IQ_ACTIVE)
    {
        snprintf(msg, sizeof(msg), "CAT: %s  M:%s  F:%s",
                 cat_state_to_str(st->state),
                 st->mode[0] ? st->mode : "?",
                 st->filter[0] ? st->filter : "?");
        display_set_status(msg);
        app_runtime_trigger_iq_start_async();
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
    (void)port;
    (void)arg;
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

static void handle_cdc_connected(const usbh_cdc_device_event_data_t *event_data)
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
        return;
    }

    xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
    s_cdc_port = port;
    xSemaphoreGive(s_dev_mutex);
    app_runtime_set_stream_state(false, false, false);

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
}

static void handle_cdc_disconnected(void)
{
    ESP_LOGI(TAG, "=== CDC device disconnected ===");
    cat_service_stop();
    display_set_status(DISP_MSG_DISCONNECTED);

    xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
    s_cdc_port = NULL;
    xSemaphoreGive(s_dev_mutex);
    app_runtime_set_stream_state(false, false, false);
}

static void cdc_dev_event_callback(usbh_cdc_device_event_t event,
                                   usbh_cdc_device_event_data_t *event_data,
                                   void *user_ctx)
{
    switch (event)
    {
    case CDC_HOST_DEVICE_EVENT_CONNECTED:
        handle_cdc_connected(event_data);
        break;
    case CDC_HOST_DEVICE_EVENT_DISCONNECTED:
        handle_cdc_disconnected();
        break;
    default:
        break;
    }
    (void)user_ctx;
}

void app_runtime_close_cdc_port_if_open(void)
{
    xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
    usbh_cdc_port_handle_t port_to_close = s_cdc_port;
    s_cdc_port = NULL;
    xSemaphoreGive(s_dev_mutex);

    if (port_to_close != NULL)
    {
        usbh_cdc_port_close(port_to_close);
    }
}

void app_runtime_cdc_install_driver(void)
{
    const usbh_cdc_driver_config_t cdc_config = {
        .task_stack_size = 4096,
        .task_priority = s_uac_task_priority,
        .task_coreid = 0,
        .skip_init_usb_host_driver = true,
    };
    ESP_ERROR_CHECK(usbh_cdc_driver_install(&cdc_config));

    ESP_ERROR_CHECK(usbh_cdc_register_dev_event_cb(ESP_USB_DEVICE_MATCH_ID_ANY,
                                                   cdc_dev_event_callback, NULL));
}

void app_runtime_cdc_uninstall_driver(void)
{
    ESP_ERROR_CHECK(usbh_cdc_driver_uninstall());
}
