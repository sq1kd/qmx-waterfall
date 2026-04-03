#include "cat/service.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "cat/proto.h"
#include "cat/state.h"

#define CAT_TASK_STACK 4096
#define CAT_TASK_PRIO 2
#define CAT_RX_CHUNK_MAX 128
#define CAT_RX_QUEUE_LEN 16
#define CAT_TICK_MS 50
#define CAT_CMD_TIMEOUT_MS 700
#define CAT_CMD_MAX_RETRY 2
#define CAT_BACKOFF_MIN_MS 500
#define CAT_BACKOFF_MAX_MS 4000
#define CAT_MAX_RECOVERY 5

typedef struct
{
    size_t len;
    uint8_t data[CAT_RX_CHUNK_MAX];
} cat_rx_chunk_t;

typedef enum
{
    CAT_CMD_NONE = 0,
    CAT_CMD_GET_IQ,
    CAT_CMD_SET_IQ_ON,
    CAT_CMD_GET_MODE,
    CAT_CMD_GET_FILTER,
} cat_cmd_id_t;

typedef struct
{
    bool active;
    cat_cmd_id_t id;
    char tx[16];
    TickType_t sent_at;
    uint32_t timeout_ms;
    uint8_t retry;
} cat_pending_t;

static const char *TAG = "cat_service";

#define CAT_STATUS_LOG_PERIOD_MS 5000

static struct
{
    bool running;
    bool auto_enable_iq;
    uint32_t poll_period_ms;
    TaskHandle_t task;
    QueueHandle_t rx_queue;
    SemaphoreHandle_t lock;
    usbh_cdc_port_handle_t port;
    cat_status_t status;
    cat_status_cb_t cb;
    void *cb_ctx;

    char rx_accum[256];
    size_t rx_accum_len;

    cat_pending_t pending;
    TickType_t backoff_until;
    uint8_t recovery_attempts;
    TickType_t last_poll;
    bool poll_filter_due;
    TickType_t poll_filter_due_at;
    TickType_t last_status_log;
} s;

#define CAT_POLL_GAP_MS 80

const char *cat_state_to_str(cat_state_t st)
{
    return cat_state_name(st);
}

static void notify_status(void)
{
    if (s.cb)
    {
        cat_status_t snap;
        xSemaphoreTake(s.lock, portMAX_DELAY);
        snap = s.status;
        xSemaphoreGive(s.lock);
        s.cb(&snap, s.cb_ctx);
    }
}

static void set_state(cat_state_t st)
{
    bool changed = false;
    xSemaphoreTake(s.lock, portMAX_DELAY);
    if (s.status.state != st)
    {
        s.status.state = st;
        changed = true;
    }
    xSemaphoreGive(s.lock);
    if (changed)
    {
        ESP_LOGI(TAG, "state -> %s", cat_state_name(st));
        notify_status();
    }
}

static void bump_timeout(void)
{
    xSemaphoreTake(s.lock, portMAX_DELAY);
    s.status.timeouts++;
    xSemaphoreGive(s.lock);
}

static void bump_retry(void)
{
    xSemaphoreTake(s.lock, portMAX_DELAY);
    s.status.retries++;
    xSemaphoreGive(s.lock);
}

static void bump_parse_error(void)
{
    xSemaphoreTake(s.lock, portMAX_DELAY);
    s.status.parse_errors++;
    xSemaphoreGive(s.lock);
}

static void bump_tx(void)
{
    xSemaphoreTake(s.lock, portMAX_DELAY);
    s.status.tx_cmds++;
    xSemaphoreGive(s.lock);
}

static void bump_rx_frame(void)
{
    xSemaphoreTake(s.lock, portMAX_DELAY);
    s.status.rx_frames++;
    xSemaphoreGive(s.lock);
}

static void send_cmd(cat_cmd_id_t id, const char *cmd, uint32_t timeout_ms)
{
    if (!s.port || !cmd)
    {
        return;
    }

    size_t len = strlen(cmd);
    esp_err_t err = usbh_cdc_write_bytes(s.port, (const uint8_t *)cmd, len, pdMS_TO_TICKS(500));
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "write '%s' failed: %s", cmd, esp_err_to_name(err));
        return;
    }

    bump_tx();
    s.pending.active = true;
    s.pending.id = id;
    s.pending.timeout_ms = timeout_ms;
    s.pending.sent_at = xTaskGetTickCount();
    strlcpy(s.pending.tx, cmd, sizeof(s.pending.tx));
    ESP_LOGD(TAG, "TX: %s", cmd);
}

static void send_cmd_no_wait(cat_cmd_id_t id, const char *cmd)
{
    if (!s.port || !cmd)
    {
        return;
    }

    size_t len = strlen(cmd);
    esp_err_t err = usbh_cdc_write_bytes(s.port, (const uint8_t *)cmd, len, pdMS_TO_TICKS(500));
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "write '%s' failed: %s", cmd, esp_err_to_name(err));
        return;
    }

    bump_tx();
    ESP_LOGD(TAG, "TX: %s", cmd);
    (void)id;
}

static void set_iq_status(bool enabled)
{
    xSemaphoreTake(s.lock, portMAX_DELAY);
    s.status.iq_enabled = enabled;
    s.status.iq_known = true;
    xSemaphoreGive(s.lock);
    notify_status();
}

static void apply_frame_fields(const char *frame)
{
    bool changed = false;
    bool iq = false;
    char mode[16] = {0};
    char filter[24] = {0};
    
    uint32_t filter_hz = 0;

    bool have_iq = cat_proto_parse_iq_value(frame, &iq);
    bool have_mode = cat_proto_parse_mode(frame, mode, sizeof(mode));
    bool have_filter = cat_proto_parse_filter(frame, filter, sizeof(filter), &filter_hz);
    

    xSemaphoreTake(s.lock, portMAX_DELAY);
    if (have_iq)
    {
        s.status.iq_enabled = iq;
        s.status.iq_known = true;
        changed = true;
    }
    if (have_mode)
    {
        if (strncmp(s.status.mode, mode, sizeof(s.status.mode)) != 0)
        {
            strlcpy(s.status.mode, mode, sizeof(s.status.mode));
            s.status.mode_code = cat_proto_parse_mode_code(mode);
            changed = true;
        }
    }
    if (have_filter)
    {
        if (strncmp(s.status.filter, filter, sizeof(s.status.filter)) != 0 || s.status.filter_hz != filter_hz)
        {
            strlcpy(s.status.filter, filter, sizeof(s.status.filter));
            s.status.filter_hz = filter_hz;
            changed = true;
        }
    }
    xSemaphoreGive(s.lock);

    if (changed)
    {
        notify_status();
    }
}

static void clear_pending(void)
{
    s.pending.active = false;
    s.pending.id = CAT_CMD_NONE;
    s.pending.tx[0] = '\0';
    s.pending.sent_at = 0;
    s.pending.timeout_ms = 0;
    s.pending.retry = 0;
}

static bool frame_matches_pending(const char *frame)
{
    switch (s.pending.id)
    {
    case CAT_CMD_GET_IQ:
        return strncmp(frame, "Q9", 2) == 0;
    case CAT_CMD_SET_IQ_ON:
        return false;
    case CAT_CMD_GET_MODE:
        return strncmp(frame, "MD", 2) == 0;
    case CAT_CMD_GET_FILTER:
        return (strncmp(frame, "FW", 2) == 0) || (strncmp(frame, "BW", 2) == 0);
    default:
        return false;
    }
}

static bool is_optional_cmd(cat_cmd_id_t id)
{
    return false;
}

static void handle_frame(const char *frame)
{
    if (!frame || frame[0] == '\0')
    {
        return;
    }

    bump_rx_frame();
    ESP_LOGD(TAG, "RX: %s", frame);
    apply_frame_fields(frame);

    if (s.pending.active && frame_matches_pending(frame))
    {
        cat_cmd_id_t done = s.pending.id;
        clear_pending();

        switch (done)
        {
        case CAT_CMD_GET_IQ:
        {
            bool iq_on = false;
            if (cat_proto_parse_iq_value(frame, &iq_on))
            {
                set_iq_status(iq_on);
                if (s.status.state == CAT_ST_SYNC)
                {
                    set_state(CAT_ST_PROBE);
                }
                else if (s.status.state == CAT_ST_IQ_VERIFY)
                {
                    if (iq_on)
                    {
                        set_state(CAT_ST_IQ_ACTIVE);
                        s.recovery_attempts = 0;
                    }
                    else
                    {
                        set_state(CAT_ST_READY);
                    }
                }
            }
            break;
        }
        case CAT_CMD_SET_IQ_ON:
            set_state(CAT_ST_IQ_VERIFY);
            break;
        case CAT_CMD_GET_MODE:
        case CAT_CMD_GET_FILTER:
        default:
            break;
        }
    }
}

static void process_rx_bytes(const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++)
    {
        char c = (char)data[i];
        if (c == ';' || c == '\n' || c == '\r')
        {
            if (s.rx_accum_len > 0)
            {
                s.rx_accum[s.rx_accum_len] = '\0';
                handle_frame(s.rx_accum);
                s.rx_accum_len = 0;
            }
            continue;
        }

        if (s.rx_accum_len + 1 < sizeof(s.rx_accum))
        {
            s.rx_accum[s.rx_accum_len++] = c;
        }
        else
        {
            s.rx_accum_len = 0;
            bump_parse_error();
        }
    }
}

static void enter_degraded(void)
{
    s.recovery_attempts++;
    if (s.recovery_attempts > CAT_MAX_RECOVERY)
    {
        set_state(CAT_ST_FAULT);
        clear_pending();
        return;
    }

    set_state(CAT_ST_DEGRADED);
    uint32_t backoff = CAT_BACKOFF_MIN_MS << (s.recovery_attempts - 1);
    if (backoff > CAT_BACKOFF_MAX_MS)
    {
        backoff = CAT_BACKOFF_MAX_MS;
    }
    s.backoff_until = xTaskGetTickCount() + pdMS_TO_TICKS(backoff);
}

static void check_timeout(void)
{
    if (!s.pending.active)
    {
        return;
    }
    TickType_t now = xTaskGetTickCount();
    TickType_t elapsed = now - s.pending.sent_at;
    if ((uint32_t)(elapsed * portTICK_PERIOD_MS) < s.pending.timeout_ms)
    {
        return;
    }

    bump_timeout();
    if (is_optional_cmd(s.pending.id))
    {
        clear_pending();
        return;
    }

    if (s.pending.retry < CAT_CMD_MAX_RETRY)
    {
        s.pending.retry++;
        bump_retry();
        cat_cmd_id_t id = s.pending.id;
        char cmd[16];
        strlcpy(cmd, s.pending.tx, sizeof(cmd));
        clear_pending();
        send_cmd(id, cmd, CAT_CMD_TIMEOUT_MS);
        return;
    }

    clear_pending();
    enter_degraded();
}

static void try_state_progress(void)
{
    if (!s.running || !s.port)
    {
        return;
    }
    if (s.pending.active)
    {
        return;
    }

    TickType_t now = xTaskGetTickCount();
    if ((s.status.state == CAT_ST_DEGRADED || s.status.state == CAT_ST_FAULT) && now < s.backoff_until)
    {
        return;
    }

    switch (s.status.state)
    {
    case CAT_ST_PORT_OPEN:
        set_state(CAT_ST_SYNC);
        send_cmd(CAT_CMD_GET_IQ, "Q9;", CAT_CMD_TIMEOUT_MS);
        break;

    case CAT_ST_SYNC:
        send_cmd(CAT_CMD_GET_IQ, "Q9;", CAT_CMD_TIMEOUT_MS);
        break;

    case CAT_ST_PROBE:
        set_state(CAT_ST_READY);
        break;

    case CAT_ST_READY:
        if (s.status.iq_known && s.status.iq_enabled)
        {
            set_state(CAT_ST_IQ_ACTIVE);
            break;
        }
        if (s.auto_enable_iq && (!s.status.iq_known || !s.status.iq_enabled))
        {
            set_state(CAT_ST_IQ_ENABLING);
            break;
        }
        if (s.poll_filter_due)
        {
            if (now >= s.poll_filter_due_at)
            {
                send_cmd(CAT_CMD_GET_FILTER, "FW;", CAT_CMD_TIMEOUT_MS);
                s.poll_filter_due = false;
            }
            break;
        }
        if ((uint32_t)((now - s.last_poll) * portTICK_PERIOD_MS) >= s.poll_period_ms)
        {
            s.last_poll = now;
            send_cmd(CAT_CMD_GET_MODE, "MD;", CAT_CMD_TIMEOUT_MS);
            s.poll_filter_due = true;
            s.poll_filter_due_at = now + pdMS_TO_TICKS(CAT_POLL_GAP_MS);
        }
        break;

    case CAT_ST_IQ_ENABLING:
        send_cmd_no_wait(CAT_CMD_SET_IQ_ON, "Q91;");
        set_state(CAT_ST_IQ_VERIFY);
        break;

    case CAT_ST_IQ_VERIFY:
        send_cmd(CAT_CMD_GET_IQ, "Q9;", CAT_CMD_TIMEOUT_MS);
        break;

    case CAT_ST_IQ_ACTIVE:
        if (s.poll_filter_due)
        {
            if (now >= s.poll_filter_due_at)
            {
                send_cmd(CAT_CMD_GET_FILTER, "FW;", CAT_CMD_TIMEOUT_MS);
                s.poll_filter_due = false;
            }
            break;
        }
        if ((uint32_t)((now - s.last_poll) * portTICK_PERIOD_MS) >= s.poll_period_ms)
        {
            s.last_poll = now;
            send_cmd(CAT_CMD_GET_MODE, "MD;", CAT_CMD_TIMEOUT_MS);
            s.poll_filter_due = true;
            s.poll_filter_due_at = now + pdMS_TO_TICKS(CAT_POLL_GAP_MS);
        }
        break;

    case CAT_ST_DEGRADED:
        s.recovery_attempts = 0;
        set_state(CAT_ST_SYNC);
        break;

    case CAT_ST_FAULT:
        set_state(CAT_ST_DEGRADED);
        break;

    case CAT_ST_DISCONNECTED:
    default:
        break;
    }
}

static void cat_task(void *arg)
{
    while (s.running)
    {
        cat_rx_chunk_t chunk;
        if (xQueueReceive(s.rx_queue, &chunk, pdMS_TO_TICKS(CAT_TICK_MS)) == pdTRUE)
        {
            process_rx_bytes(chunk.data, chunk.len);
        }

        check_timeout();
        try_state_progress();

        TickType_t now = xTaskGetTickCount();
        if ((uint32_t)((now - s.last_status_log) * portTICK_PERIOD_MS) >= CAT_STATUS_LOG_PERIOD_MS)
        {
            s.last_status_log = now;
            cat_status_t st;
            xSemaphoreTake(s.lock, portMAX_DELAY);
            st = s.status;
            xSemaphoreGive(s.lock);
            ESP_LOGI(TAG, "status state=%s iq=%d mode=%s filter=%s to=%lu rt=%lu",
                     cat_state_name(st.state),
                     st.iq_enabled ? 1 : 0,
                     st.mode[0] ? st.mode : "?",
                     st.filter[0] ? st.filter : "?",
                     
                     (unsigned long)st.timeouts,
                     (unsigned long)st.retries);
        }
    }
    s.task = NULL;
    vTaskDelete(NULL);
}

void cat_service_default_config(cat_service_config_t *cfg)
{
    if (!cfg)
    {
        return;
    }
    cfg->auto_enable_iq = true;
    cfg->poll_period_ms = 1000;
    cfg->status_cb = NULL;
    cfg->status_cb_ctx = NULL;
}

esp_err_t cat_service_start(usbh_cdc_port_handle_t port, const cat_service_config_t *cfg)
{
    if (!port)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (s.running)
    {
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s, 0, sizeof(s));
    s.lock = xSemaphoreCreateMutex();
    s.rx_queue = xQueueCreate(CAT_RX_QUEUE_LEN, sizeof(cat_rx_chunk_t));
    if (!s.lock || !s.rx_queue)
    {
        if (s.rx_queue)
            vQueueDelete(s.rx_queue);
        if (s.lock)
            vSemaphoreDelete(s.lock);
        memset(&s, 0, sizeof(s));
        return ESP_ERR_NO_MEM;
    }

    cat_service_config_t local_cfg;
    cat_service_default_config(&local_cfg);
    if (cfg)
    {
        local_cfg = *cfg;
    }

    s.auto_enable_iq = local_cfg.auto_enable_iq;
    s.poll_period_ms = (local_cfg.poll_period_ms == 0) ? 1000 : local_cfg.poll_period_ms;
    s.cb = local_cfg.status_cb;
    s.cb_ctx = local_cfg.status_cb_ctx;
    s.port = port;
    s.status.state = CAT_ST_PORT_OPEN;
    s.running = true;
    s.last_poll = xTaskGetTickCount();
    s.poll_filter_due = false;
    s.poll_filter_due_at = 0;
    s.last_status_log = xTaskGetTickCount();

    BaseType_t ok = xTaskCreate(cat_task, "cat_svc", CAT_TASK_STACK, NULL, CAT_TASK_PRIO, &s.task);
    if (ok != pdTRUE)
    {
        s.running = false;
        vQueueDelete(s.rx_queue);
        vSemaphoreDelete(s.lock);
        memset(&s, 0, sizeof(s));
        return ESP_FAIL;
    }

    notify_status();
    ESP_LOGI(TAG, "started");
    return ESP_OK;
}

void cat_service_stop(void)
{
    if (!s.running)
    {
        return;
    }

    s.running = false;
    TaskHandle_t h = s.task;
    if (h)
    {
        for (int i = 0; i < 50 && s.task != NULL; i++)
        {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    if (s.task)
    {
        vTaskDelete(s.task);
        s.task = NULL;
    }
    if (s.rx_queue)
    {
        vQueueDelete(s.rx_queue);
    }
    if (s.lock)
    {
        vSemaphoreDelete(s.lock);
    }
    memset(&s, 0, sizeof(s));
    ESP_LOGI(TAG, "stopped");
}

void cat_service_on_rx_bytes(const uint8_t *data, size_t len)
{
    if (!s.running || !s.rx_queue || !data || len == 0)
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
        if (xQueueSend(s.rx_queue, &chunk, 0) != pdTRUE)
        {
            bump_parse_error();
            return;
        }
        off += part;
    }
}

bool cat_service_get_status(cat_status_t *out)
{
    if (!out || !s.lock)
    {
        return false;
    }
    xSemaphoreTake(s.lock, portMAX_DELAY);
    *out = s.status;
    xSemaphoreGive(s.lock);
    return true;
}
