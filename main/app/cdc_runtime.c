#include "app/runtime_priv.h"
#include "app/recovery.h"
#include "app/timing_config.h"

#include <stdio.h>
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "display/display.h"
#include "cat/service.h"
#include "spectrum/spectrum.h"

static const char *TAG = "usb_audio_cdc_info";

#define CDC_COMM_ITF_NUM 0
#define CDC_EVT_QUEUE_LEN 8
typedef enum
{
    CDC_EVT_CONNECTED = 1,
    CDC_EVT_DISCONNECTED,
    CDC_EVT_STOP,
} cdc_evt_id_t;

typedef struct
{
    cdc_evt_id_t id;
    usbh_cdc_device_event_data_t data;
} cdc_evt_t;

static QueueHandle_t s_cdc_evt_queue;
static TaskHandle_t s_cdc_evt_task;

static bool queue_cdc_event(cdc_evt_id_t id, const usbh_cdc_device_event_data_t *event_data)
{
    if (!s_cdc_evt_queue)
        return false;
    cdc_evt_t ev = {
        .id = id,
    };
    if (event_data)
        ev.data = *event_data;
    return xQueueSend(s_cdc_evt_queue, &ev, 0) == pdTRUE;
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
        static uint32_t s_last_serial_state = 0xFFFFFFFFu;
        usb_cdc_serial_state_t *state = (usb_cdc_serial_state_t *)notif->Data;
        uint32_t packed = (state->dcd ? 1u : 0u) |
                          ((state->dsr ? 1u : 0u) << 1) |
                          ((state->ring ? 1u : 0u) << 2) |
                          ((state->framing_err ? 1u : 0u) << 3);
        if (packed != s_last_serial_state)
        {
            s_last_serial_state = packed;
            ESP_LOGD(TAG, "[CDC] SerialState DCD=%d DSR=%d RI=%d FE=%d",
                     state->dcd, state->dsr, state->ring, state->framing_err);
        }
        break;
    }
    case USB_CDC_NOTIFY_NETWORK_CONNECTION:
        ESP_LOGD(TAG, "[CDC] NetworkConnection: %s", notif->wValue ? "connected" : "disconnected");
        break;
    default:
        ESP_LOGD(TAG, "[CDC] Notification 0x%02x", notif->bNotificationCode);
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
        ESP_LOGD(TAG, "  CDC Control interface: %d", notif_intf->bInterfaceNumber);
    }
    if (data_intf)
    {
        ESP_LOGD(TAG, "  CDC Data interface:    %d", data_intf->bInterfaceNumber);
    }
}

static void handle_cdc_disconnected(void)
{
    TickType_t t0 = xTaskGetTickCount();
    if (xSemaphoreTake(s_dev_mutex, pdMS_TO_TICKS(APP_DEV_MUTEX_TIMEOUT_MS)) == pdTRUE)
    {
        /* Mark stream dead first, then stop producers/consumers. */
        s_cdc_port = NULL;
        s_iq_handle = NULL;
        s_iq_stream_started = false;
        s_iq_start_in_progress = false;
        xSemaphoreGive(s_dev_mutex);
    }
    else
    {
        ESP_LOGW(TAG, "disconnect: dev_mutex timeout");
    }
    TickType_t t_mark = xTaskGetTickCount();

    app_runtime_set_stream_state(false, false, false);
    spectrum_notify_rx(NULL);
    cat_service_stop();
    TickType_t t_cat = xTaskGetTickCount();
    display_set_status(DISP_MSG_DISCONNECTED);
    TickType_t t_ui = xTaskGetTickCount();
    recovery_record_cdc_timing((uint32_t)((t_mark - t0) * portTICK_PERIOD_MS),
                               (uint32_t)((t_cat - t_mark) * portTICK_PERIOD_MS),
                               (uint32_t)((t_ui - t_cat) * portTICK_PERIOD_MS),
                               (uint32_t)((t_ui - t0) * portTICK_PERIOD_MS));
}

static void cdc_dev_event_callback(usbh_cdc_device_event_t event,
                                   usbh_cdc_device_event_data_t *event_data,
                                   void *user_ctx)
{
    switch (event)
    {
    case CDC_HOST_DEVICE_EVENT_CONNECTED:
        xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
        s_disc.cdc_pending = false;
        bool cdc_drop_logged = s_disc.cdc_drop_logged;
        xSemaphoreGive(s_dev_mutex);
        if (!queue_cdc_event(CDC_EVT_CONNECTED, event_data) && !cdc_drop_logged)
        {
            xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
            s_disc.cdc_drop_logged = true;
            xSemaphoreGive(s_dev_mutex);
            ESP_LOGW(TAG, "drop CDC connect event (queue full)");
        }
        break;
    case CDC_HOST_DEVICE_EVENT_DISCONNECTED:
    {
        xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
        bool pending = s_disc.cdc_pending;
        s_disc.in_progress = true;
        xSemaphoreGive(s_dev_mutex);
        if (!pending && recovery_on_disconnect(RECOVERY_SRC_CDC))
        {
            cat_service_request_stop_fast();
            xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
            s_disc.cdc_pending = true;
            bool was_drop_logged = s_disc.cdc_drop_logged;
            xSemaphoreGive(s_dev_mutex);
            if (!queue_cdc_event(CDC_EVT_DISCONNECTED, event_data) && !was_drop_logged)
            {
                xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
                s_disc.cdc_pending = false;
                s_disc.cdc_drop_logged = true;
                xSemaphoreGive(s_dev_mutex);
                ESP_LOGW(TAG, "drop CDC disconnect event (queue full)");
                (void)recovery_request();
            }
        }
        break;
    }
    default:
        break;
    }
    (void)user_ctx;
}

static void cdc_evt_task(void *arg)
{
    (void)arg;
    while (true)
    {
        cdc_evt_t ev;
        if (xQueueReceive(s_cdc_evt_queue, &ev, portMAX_DELAY) != pdTRUE)
            continue;

        xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
        s_disc.cdc_drop_logged = false;
        xSemaphoreGive(s_dev_mutex);
        if (ev.id == CDC_EVT_STOP)
            break;

        if (ev.id == CDC_EVT_CONNECTED)
        {
            handle_cdc_connected(&ev.data);
        }
        else if (ev.id == CDC_EVT_DISCONNECTED)
        {
            handle_cdc_disconnected();
            (void)recovery_request();
        }
    }

    s_cdc_evt_task = NULL;
    if (s_install.stop_events)
    {
        xEventGroupSetBits(s_install.stop_events, APP_STOPBIT_CDC_TASK);
    }
    vTaskDelete(NULL);
}

void app_runtime_close_cdc_port_if_open(void)
{
    usbh_cdc_port_handle_t port_to_close = NULL;
    if (xSemaphoreTake(s_dev_mutex, pdMS_TO_TICKS(APP_DEV_MUTEX_TIMEOUT_MS)) == pdTRUE)
    {
        port_to_close = s_cdc_port;
        s_cdc_port = NULL;
        xSemaphoreGive(s_dev_mutex);
    }
    else
    {
        ESP_LOGW(TAG, "close_cdc_port: dev_mutex timeout");
    }

    if (port_to_close != NULL)
    {
        usbh_cdc_port_close(port_to_close);
    }
}

void app_runtime_cdc_install_driver(void)
{
    xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
    bool already_installed = s_install.cdc_installed;
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
    xEventGroupClearBits(s_install.stop_events, APP_STOPBIT_CDC_TASK);

    s_cdc_evt_queue = xQueueCreate(CDC_EVT_QUEUE_LEN, sizeof(cdc_evt_t));
    if (!s_cdc_evt_queue)
    {
        ESP_LOGE(TAG, "failed to create CDC event queue");
        abort();
    }
    BaseType_t ok = xTaskCreate(cdc_evt_task, "cdc_evt", 4096, NULL,
                                s_uac_task_priority - 1, &s_cdc_evt_task);
    if (ok != pdTRUE)
    {
        vQueueDelete(s_cdc_evt_queue);
        s_cdc_evt_queue = NULL;
        ESP_LOGE(TAG, "failed to create CDC event task");
        abort();
    }

    const usbh_cdc_driver_config_t cdc_config = {
        .task_stack_size = 4096,
        .task_priority = s_uac_task_priority,
        .task_coreid = 0,
        .skip_init_usb_host_driver = true,
    };
    ESP_ERROR_CHECK(usbh_cdc_driver_install(&cdc_config));

    ESP_ERROR_CHECK(usbh_cdc_register_dev_event_cb(ESP_USB_DEVICE_MATCH_ID_ANY,
                                                   cdc_dev_event_callback, NULL));
    xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
    s_install.cdc_installed = true;
    xSemaphoreGive(s_dev_mutex);
}

void app_runtime_cdc_uninstall_driver(void)
{
    xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
    bool installed = s_install.cdc_installed;
    xSemaphoreGive(s_dev_mutex);
    if (!installed)
    {
        return;
    }
    if (s_cdc_evt_queue)
    {
        (void)queue_cdc_event(CDC_EVT_STOP, NULL);
    }
    if (s_cdc_evt_task && s_install.stop_events)
    {
        EventBits_t bits = xEventGroupWaitBits(s_install.stop_events,
                                               APP_STOPBIT_CDC_TASK,
                                               pdTRUE,
                                               pdFALSE,
                                               pdMS_TO_TICKS(APP_TASK_STOP_WAIT_MS));
        if ((bits & APP_STOPBIT_CDC_TASK) == 0)
        {
            ESP_LOGW(TAG, "CDC event task stop timeout");
        }
    }
    if (s_cdc_evt_queue)
    {
        vQueueDelete(s_cdc_evt_queue);
        s_cdc_evt_queue = NULL;
    }
    xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
    s_disc.cdc_pending = false;
    s_disc.cdc_drop_logged = false;
    s_disc.in_progress = false;
    xSemaphoreGive(s_dev_mutex);

    (void)app_runtime_retry_invalid_state("usbh_cdc_driver_uninstall",
                                          usbh_cdc_driver_uninstall,
                                          NULL);
    xSemaphoreTake(s_dev_mutex, portMAX_DELAY);
    s_install.cdc_installed = false;
    xSemaphoreGive(s_dev_mutex);
}
