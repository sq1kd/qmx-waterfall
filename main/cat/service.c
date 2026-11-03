#include "cat/service.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "app/timing_config.h"

#define CAT_TASK_STACK 4096
#define CAT_TASK_PRIO 2
#define CAT_RX_CHUNK_MAX 128
#define CAT_RX_QUEUE_LEN 16
#define CAT_TICK_MS 50
#define CAT_BACKOFF_MIN_MS 500
#define CAT_BACKOFF_MAX_MS 4000
#define CAT_MAX_RECOVERY 5
#define CAT_CONNECT_WARMUP_MS 700
#define CAT_STATUS_LOG_PERIOD_MS 5000

/* IQ mode is session-only on the radio and is cleared when the operator
 * leaves the configuration menu, so it has to be watched and restored. */
#define CAT_IQ_POLL_PERIOD_MS 2000
#define CAT_IQ_POLL_MIN_MS 500

enum
{
    CAT_DEFAULT_POLL_PERIOD_MS = 1000,
};

typedef struct
{
    size_t len;
    uint8_t data[CAT_RX_CHUNK_MAX];
} cat_rx_chunk_t;

static const char *TAG = "cat_service";

cat_svc_t g_cat;

const char *cat_state_name(cat_state_t st)
{
    switch (st)
    {
    case CAT_ST_DISCONNECTED:
        return "disconnected";
    case CAT_ST_PORT_OPEN:
        return "port_open";
    case CAT_ST_SYNC:
        return "sync";
    case CAT_ST_PROBE:
        return "probe";
    case CAT_ST_READY:
        return "ready";
    case CAT_ST_IQ_ENABLING:
        return "iq_enabling";
    case CAT_ST_IQ_VERIFY:
        return "iq_verify";
    case CAT_ST_IQ_ACTIVE:
        return "iq_active";
    case CAT_ST_DEGRADED:
        return "degraded";
    case CAT_ST_FAULT:
        return "fault";
    default:
        return "unknown";
    }
}

void cat_status_bump(uint32_t *counter)
{
    if (!g_cat.lock)
    {
        return;
    }
    xSemaphoreTake(g_cat.lock, portMAX_DELAY);
    (*counter)++;
    xSemaphoreGive(g_cat.lock);
}

void cat_svc_notify_status(void)
{
    if (!g_cat.cb || !g_cat.lock)
    {
        return;
    }

    cat_status_t snap;
    xSemaphoreTake(g_cat.lock, portMAX_DELAY);
    snap = g_cat.status;
    xSemaphoreGive(g_cat.lock);
    g_cat.cb(&snap, g_cat.cb_ctx);
}

void cat_svc_set_state(cat_state_t st)
{
    if (!g_cat.lock)
    {
        return;
    }

    bool changed = false;
    xSemaphoreTake(g_cat.lock, portMAX_DELAY);
    if (g_cat.status.state != st)
    {
        g_cat.status.state = st;
        changed = true;
    }
    xSemaphoreGive(g_cat.lock);
    if (!changed)
    {
        return;
    }

    if (st == CAT_ST_DEGRADED || st == CAT_ST_FAULT)
    {
        ESP_LOGW(TAG, "state -> %s", cat_state_name(st));
    }
    else
    {
        ESP_LOGD(TAG, "state -> %s", cat_state_name(st));
    }
    cat_svc_notify_status();
}

bool cat_link_tx_write(const char *cmd)
{
    if (!g_cat.port || !cmd)
    {
        return false;
    }

    TickType_t now = xTaskGetTickCount();
    if (now < g_cat.next_tx_allowed_at)
    {
        return false;
    }

    esp_err_t err = usbh_cdc_write_bytes(g_cat.port, (const uint8_t *)cmd, strlen(cmd),
                                         pdMS_TO_TICKS(CAT_WRITE_TIMEOUT_MS));
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "write '%s' failed: %s", cmd, esp_err_to_name(err));
        cat_status_bump(&g_cat.status.tx_errors);
        g_cat.tx_failures++;
        if (g_cat.tx_failures >= CAT_TX_FAIL_LIMIT)
        {
            g_cat.tx_failures = 0;
            cat_link_enter_degraded();
        }
        return false;
    }

    g_cat.tx_failures = 0;
    g_cat.next_tx_allowed_at = now + pdMS_TO_TICKS(CAT_MIN_TX_SPACING_MS);
    cat_status_bump(&g_cat.status.tx_cmds);
    return true;
}

void cat_link_note_good_reply(void)
{
    g_cat.recovery_attempts = 0;
    g_cat.tx_failures = 0;
}

static uint32_t backoff_delay_ms(uint8_t attempt)
{
    uint32_t delay = CAT_BACKOFF_MIN_MS;
    for (uint8_t i = 1; i < attempt && delay < CAT_BACKOFF_MAX_MS; i++)
    {
        delay <<= 1;
    }
    return (delay > CAT_BACKOFF_MAX_MS) ? CAT_BACKOFF_MAX_MS : delay;
}

void cat_link_enter_degraded(void)
{
    cat_cmd_clear_pending();
    g_cat.recovery_attempts++;
    if (g_cat.recovery_attempts > CAT_MAX_RECOVERY)
    {
        ESP_LOGE(TAG, "no usable replies after %u recovery attempts, link fault",
                 (unsigned)g_cat.recovery_attempts);
        cat_svc_set_state(CAT_ST_FAULT);
        return;
    }

    uint32_t backoff = backoff_delay_ms(g_cat.recovery_attempts);
    g_cat.backoff_until = xTaskGetTickCount() + pdMS_TO_TICKS(backoff);
    cat_svc_set_state(CAT_ST_DEGRADED);
}

static void rx_accum_overflow(void)
{
    cat_status_bump(&g_cat.status.rx_overflows);
    g_cat.rx_accum_len = 0;
    g_cat.rx_discard = true;
}

/* A dropped RX chunk loses bytes mid-frame; the partial frame must never be
 * parsed, so discard everything up to the next delimiter. */
static bool rx_drop_pending(void)
{
    uint32_t seq = g_cat.rx_drop_seq;
    if (seq == g_cat.rx_drop_seen)
    {
        return false;
    }
    g_cat.rx_drop_seen = seq;
    return true;
}

static void process_rx_bytes(const uint8_t *data, size_t len)
{
    if (rx_drop_pending())
    {
        g_cat.rx_accum_len = 0;
        g_cat.rx_discard = true;
    }

    for (size_t i = 0; i < len; i++)
    {
        char c = (char)data[i];
        if (c == ';' || c == '\n' || c == '\r')
        {
            bool dropped = rx_drop_pending();
            bool discard = g_cat.rx_discard || dropped;
            if (!discard && g_cat.rx_accum_len > 0)
            {
                g_cat.rx_accum[g_cat.rx_accum_len] = '\0';
                cat_frame_handle(g_cat.rx_accum);
            }
            g_cat.rx_discard = false;
            g_cat.rx_accum_len = 0;
            continue;
        }

        if (g_cat.rx_discard)
        {
            continue;
        }

        if (g_cat.rx_accum_len + 1 < sizeof(g_cat.rx_accum))
        {
            g_cat.rx_accum[g_cat.rx_accum_len++] = c;
        }
        else
        {
            rx_accum_overflow();
        }
    }
}

static void log_status_periodic(void)
{
    TickType_t now = xTaskGetTickCount();
    if ((uint32_t)((now - g_cat.last_status_log) * portTICK_PERIOD_MS) < CAT_STATUS_LOG_PERIOD_MS)
    {
        return;
    }
    g_cat.last_status_log = now;

    cat_status_t st;
    if (!cat_service_get_status(&st))
    {
        return;
    }
    ESP_LOGD(TAG, "status state=%s fw=%s iq=%d drops=%lu reen=%lu mode=%s filter=%s "
                  "rit=%ld(%d) to=%lu rt=%lu txerr=%lu rxovf=%lu rxdrops=%lu",
             cat_state_name(st.state),
             st.fw_version[0] ? st.fw_version : "?",
             st.iq_enabled ? 1 : 0,
             (unsigned long)st.iq_drops,
             (unsigned long)st.iq_reenables,
             st.mode[0] ? st.mode : "?",
             st.filter[0] ? st.filter : "?",
             (long)st.rit_hz,
             st.rit_on ? 1 : 0,
             (unsigned long)st.timeouts,
             (unsigned long)st.retries,
             (unsigned long)st.tx_errors,
             (unsigned long)st.rx_overflows,
             (unsigned long)st.rx_queue_drops);
}

static void cat_task(void *arg)
{
    (void)arg;

    while (g_cat.running)
    {
        cat_rx_chunk_t chunk;
        if (xQueueReceive(g_cat.rx_queue, &chunk, pdMS_TO_TICKS(CAT_TICK_MS)) == pdTRUE)
        {
            process_rx_bytes(chunk.data, chunk.len);
        }

        cat_check_timeout();
        cat_state_progress();
        log_status_periodic();
    }

    /* cat_service_stop() waits for this flag before freeing anything. */
    g_cat.task_exited = true;
    vTaskDelete(NULL);
}

void cat_service_default_config(cat_service_config_t *cfg)
{
    if (!cfg)
    {
        return;
    }
    cfg->auto_enable_iq = true;
    cfg->poll_period_ms = CAT_DEFAULT_POLL_PERIOD_MS;
    cfg->iq_poll_period_ms = CAT_IQ_POLL_PERIOD_MS;
    cfg->status_cb = NULL;
    cfg->status_cb_ctx = NULL;
}

esp_err_t cat_service_start(usbh_cdc_port_handle_t port, const cat_service_config_t *cfg)
{
    if (!port)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (g_cat.running || g_cat.task)
    {
        return ESP_ERR_INVALID_STATE;
    }

    memset(&g_cat, 0, sizeof(g_cat));
    g_cat.lock = xSemaphoreCreateMutex();
    g_cat.rx_queue = xQueueCreate(CAT_RX_QUEUE_LEN, sizeof(cat_rx_chunk_t));
    if (!g_cat.lock || !g_cat.rx_queue)
    {
        if (g_cat.rx_queue)
        {
            vQueueDelete(g_cat.rx_queue);
        }
        if (g_cat.lock)
        {
            vSemaphoreDelete(g_cat.lock);
        }
        memset(&g_cat, 0, sizeof(g_cat));
        return ESP_ERR_NO_MEM;
    }

    cat_service_config_t local_cfg;
    cat_service_default_config(&local_cfg);
    if (cfg)
    {
        local_cfg = *cfg;
    }

    g_cat.auto_enable_iq = local_cfg.auto_enable_iq;
    g_cat.poll_period_ms = (local_cfg.poll_period_ms == 0)
                               ? CAT_DEFAULT_POLL_PERIOD_MS
                               : local_cfg.poll_period_ms;
    g_cat.iq_poll_period_ms = (local_cfg.iq_poll_period_ms == 0)
                                  ? CAT_IQ_POLL_PERIOD_MS
                                  : local_cfg.iq_poll_period_ms;
    if (g_cat.iq_poll_period_ms < CAT_IQ_POLL_MIN_MS)
    {
        g_cat.iq_poll_period_ms = CAT_IQ_POLL_MIN_MS;
    }
    g_cat.cb = local_cfg.status_cb;
    g_cat.cb_ctx = local_cfg.status_cb_ctx;
    g_cat.port = port;
    g_cat.status.state = CAT_ST_PORT_OPEN;
    g_cat.running = true;
    g_cat.last_poll = xTaskGetTickCount();
    g_cat.last_status_log = xTaskGetTickCount();
    g_cat.warmup_until = xTaskGetTickCount() + pdMS_TO_TICKS(CAT_CONNECT_WARMUP_MS);
    g_cat.next_tx_allowed_at = xTaskGetTickCount();

    BaseType_t ok = xTaskCreate(cat_task, "cat_svc", CAT_TASK_STACK, NULL, CAT_TASK_PRIO, &g_cat.task);
    if (ok != pdTRUE)
    {
        g_cat.running = false;
        vQueueDelete(g_cat.rx_queue);
        vSemaphoreDelete(g_cat.lock);
        memset(&g_cat, 0, sizeof(g_cat));
        return ESP_FAIL;
    }

    cat_svc_notify_status();
    ESP_LOGI(TAG, "started");
    return ESP_OK;
}

void cat_service_stop(void)
{
    if (!g_cat.running && !g_cat.task)
    {
        return;
    }

    g_cat.running = false;
    g_cat.port = NULL;

    TaskHandle_t task = g_cat.task;
    if (task)
    {
        TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(CAT_STOP_WAIT_MS);
        while (!g_cat.task_exited && (int32_t)(deadline - xTaskGetTickCount()) > 0)
        {
            vTaskDelay(pdMS_TO_TICKS(APP_TASK_STOP_POLL_MS));
        }
    }

    if (task && !g_cat.task_exited)
    {
        /* Deleting a task that may still sit inside a CDC write leaves the
         * USB stack with a dangling caller, so leak it and its resources. */
        ESP_LOGE(TAG, "cat task did not exit within %d ms; leaking it", (int)CAT_STOP_WAIT_MS);
        return;
    }

    if (g_cat.rx_queue)
    {
        vQueueDelete(g_cat.rx_queue);
    }
    if (g_cat.lock)
    {
        vSemaphoreDelete(g_cat.lock);
    }
    memset(&g_cat, 0, sizeof(g_cat));
    ESP_LOGI(TAG, "stopped");
}

void cat_service_request_stop_fast(void)
{
    if (!g_cat.running)
    {
        return;
    }

    /* Immediately stop new TX from other tasks/callback paths. */
    g_cat.port = NULL;
    cat_cmd_clear_pending();
}

void cat_service_on_rx_bytes(const uint8_t *data, size_t len)
{
    if (!g_cat.running || !g_cat.rx_queue || !data || len == 0)
    {
        return;
    }

    size_t off = 0;
    while (off < len)
    {
        cat_rx_chunk_t chunk;
        size_t part = len - off;
        if (part > CAT_RX_CHUNK_MAX)
        {
            part = CAT_RX_CHUNK_MAX;
        }
        memcpy(chunk.data, data + off, part);
        chunk.len = part;
        if (xQueueSend(g_cat.rx_queue, &chunk, 0) != pdTRUE)
        {
            cat_status_bump(&g_cat.status.rx_queue_drops);
            g_cat.rx_drop_seq++;
            return;
        }
        off += part;
    }
}

bool cat_service_get_status(cat_status_t *out)
{
    if (!out || !g_cat.lock)
    {
        return false;
    }
    xSemaphoreTake(g_cat.lock, portMAX_DELAY);
    *out = g_cat.status;
    xSemaphoreGive(g_cat.lock);
    return true;
}
