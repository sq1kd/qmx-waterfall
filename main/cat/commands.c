#include "cat/service.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "cat/proto.h"

static const char *TAG = "cat_cmd";

typedef struct
{
    bool have_iq;
    bool iq_on;
    bool have_mode;
    uint8_t mode_code;
    char mode[CAT_MODE_MAX];
    bool have_filter;
    char filter[CAT_FILTER_MAX];
    uint32_t filter_hz;
    bool have_if;
    int32_t rit_hz;
    bool rit_on;
    bool have_version;
    char version[CAT_FW_MAX];
} cat_frame_t;

typedef struct
{
    const char *tx;
    const char *prefix[2];
    bool optional;
    bool (*on_reply)(const cat_frame_t *fields);
    void (*on_timeout)(void);
} cat_cmd_t;

static bool on_reply_iq(const cat_frame_t *fields);
static bool on_reply_version(const cat_frame_t *fields);
static bool on_reply_health(const cat_frame_t *fields);
static void on_timeout_version(void);

static const cat_cmd_t cat_cmds[CAT_CMD_COUNT] = {
    [CAT_CMD_GET_IQ] = { .tx = "Q9;", .prefix = {"Q9"}, .on_reply = on_reply_iq },
    [CAT_CMD_SET_IQ_ON] = { .tx = "Q91;" },
    [CAT_CMD_GET_IF] = { .tx = "IF;", .prefix = {"IF"} },
    [CAT_CMD_GET_FILTER] = { .tx = "FW;", .prefix = {"FW", "BW"} },
    [CAT_CMD_GET_VERSION] = { .tx = "VN;",
                              .prefix = {"VN"},
                              .optional = true,
                              .on_reply = on_reply_version,
                              .on_timeout = on_timeout_version },
    [CAT_CMD_IQ_HEALTH] = { .tx = "Q9;", .prefix = {"Q9"}, .optional = true, .on_reply = on_reply_health },
};

static const cat_cmd_t *cmd_lookup(cat_cmd_id_t id)
{
    if ((unsigned)id >= (unsigned)CAT_CMD_COUNT)
    {
        return NULL;
    }
    return &cat_cmds[id];
}

static bool cmd_matches(const cat_cmd_t *cmd, const char *frame)
{
    for (size_t i = 0; i < sizeof(cmd->prefix) / sizeof(cmd->prefix[0]); i++)
    {
        const char *prefix = cmd->prefix[i];
        if (prefix && strncmp(frame, prefix, strlen(prefix)) == 0)
        {
            return true;
        }
    }
    return false;
}

static bool cmd_tx(cat_cmd_id_t id, bool track)
{
    const cat_cmd_t *cmd = cmd_lookup(id);
    if (!cmd || !cmd->tx)
    {
        return false;
    }
    if (!cat_link_tx_write(cmd->tx))
    {
        return false;
    }

    ESP_LOGD(TAG, "TX: %s", cmd->tx);
    if (!track)
    {
        return true;
    }

    g_cat.pending.active = true;
    g_cat.pending.id = id;
    g_cat.pending.sent_at = xTaskGetTickCount();
    g_cat.pending.retry = 0;
    return true;
}

bool cat_cmd_send(cat_cmd_id_t id)
{
    return cmd_tx(id, true);
}

bool cat_cmd_send_bare(cat_cmd_id_t id)
{
    return cmd_tx(id, false);
}

void cat_cmd_clear_pending(void)
{
    g_cat.pending.active = false;
    g_cat.pending.id = CAT_CMD_COUNT;
    g_cat.pending.sent_at = 0;
    g_cat.pending.retry = 0;
}

static bool on_reply_iq(const cat_frame_t *fields)
{
    if (!fields->have_iq)
    {
        return false;
    }

    if (g_cat.status.state == CAT_ST_SYNC)
    {
        g_cat.probe_step = CAT_PROBE_VERSION;
        cat_svc_set_state(CAT_ST_PROBE);
    }
    else if (g_cat.status.state == CAT_ST_IQ_VERIFY)
    {
        if (fields->iq_on)
        {
            cat_iq_recovery_succeeded();
        }
        else
        {
            cat_iq_attempt_failed();
        }
    }
    return true;
}

static bool on_reply_version(const cat_frame_t *fields)
{
    g_cat.probe_step = CAT_PROBE_FILTER;
    if (!fields->have_version)
    {
        return false;
    }

    xSemaphoreTake(g_cat.lock, portMAX_DELAY);
    strlcpy(g_cat.status.fw_version, fields->version, sizeof(g_cat.status.fw_version));
    xSemaphoreGive(g_cat.lock);
    cat_svc_notify_status();
    ESP_LOGI(TAG, "radio firmware %s", fields->version);
    return true;
}

static bool on_reply_health(const cat_frame_t *fields)
{
    if (!fields->have_iq)
    {
        return false;
    }
    if (!fields->iq_on && g_cat.status.state == CAT_ST_IQ_ACTIVE)
    {
        cat_iq_recovery_begin();
    }
    return true;
}

static void on_timeout_version(void)
{
    /* The version probe is informational; skip it and move on. */
    g_cat.probe_step = CAT_PROBE_FILTER;
}

static void frame_apply(const char *frame, cat_frame_t *fields)
{
    memset(fields, 0, sizeof(*fields));
    fields->have_iq = cat_proto_parse_iq_value(frame, &fields->iq_on);
    fields->have_mode = cat_proto_parse_mode(frame, fields->mode, sizeof(fields->mode));
    fields->have_filter = cat_proto_parse_filter(frame, fields->filter, sizeof(fields->filter),
                                                 &fields->filter_hz);
    fields->have_if = cat_proto_parse_if(frame, &fields->mode_code, &fields->rit_hz, &fields->rit_on);
    fields->have_version = cat_proto_parse_version(frame, fields->version, sizeof(fields->version));

    if (fields->have_if)
    {
        snprintf(fields->mode, sizeof(fields->mode), "%u", (unsigned)fields->mode_code);
        fields->have_mode = true;
    }

    bool changed = false;
    bool mode_changed = false;

    xSemaphoreTake(g_cat.lock, portMAX_DELAY);
    if (fields->have_iq && (g_cat.status.iq_enabled != fields->iq_on || !g_cat.status.iq_known))
    {
        g_cat.status.iq_enabled = fields->iq_on;
        g_cat.status.iq_known = true;
        changed = true;
    }
    if (fields->have_mode)
    {
        uint8_t mode_code = cat_proto_parse_mode_code(fields->mode);
        if (strncmp(g_cat.status.mode, fields->mode, sizeof(g_cat.status.mode)) != 0 ||
            g_cat.status.mode_code != mode_code)
        {
            strlcpy(g_cat.status.mode, fields->mode, sizeof(g_cat.status.mode));
            mode_changed = (g_cat.status.mode_code != mode_code);
            g_cat.status.mode_code = mode_code;
            changed = true;
        }
    }
    if (fields->have_if &&
        (g_cat.status.rit_hz != fields->rit_hz || g_cat.status.rit_on != fields->rit_on))
    {
        g_cat.status.rit_hz = fields->rit_hz;
        g_cat.status.rit_on = fields->rit_on;
        changed = true;
    }
    if (fields->have_filter &&
        (strncmp(g_cat.status.filter, fields->filter, sizeof(g_cat.status.filter)) != 0 ||
         g_cat.status.filter_hz != fields->filter_hz))
    {
        strlcpy(g_cat.status.filter, fields->filter, sizeof(g_cat.status.filter));
        g_cat.status.filter_hz = fields->filter_hz;
        changed = true;
    }
    xSemaphoreGive(g_cat.lock);

    if (mode_changed)
    {
        /* A mode change invalidates the filter: refresh it before polling IF. */
        g_cat.poll_filter_due = true;
        g_cat.poll_filter_due_at = xTaskGetTickCount() + pdMS_TO_TICKS(CAT_POLL_GAP_MS);
    }

    if (changed)
    {
        cat_svc_notify_status();
    }
}

void cat_frame_handle(const char *frame)
{
    if (!frame || frame[0] == '\0')
    {
        return;
    }

    cat_status_bump(&g_cat.status.rx_frames);
    ESP_LOGD(TAG, "RX: %s", frame);

    cat_frame_t fields;
    frame_apply(frame, &fields);

    if (!g_cat.pending.active)
    {
        return;
    }

    cat_cmd_id_t id = g_cat.pending.id;
    const cat_cmd_t *cmd = cmd_lookup(id);
    if (!cmd || !cmd_matches(cmd, frame))
    {
        return;
    }

    cat_cmd_clear_pending();
    bool valid = cmd->on_reply ? cmd->on_reply(&fields) : true;
    if (valid)
    {
        cat_link_note_good_reply();
    }
}

void cat_check_timeout(void)
{
    if (!g_cat.pending.active)
    {
        return;
    }

    TickType_t elapsed = xTaskGetTickCount() - g_cat.pending.sent_at;
    if ((uint32_t)(elapsed * portTICK_PERIOD_MS) < CAT_CMD_TIMEOUT_MS)
    {
        return;
    }

    cat_cmd_id_t id = g_cat.pending.id;
    const cat_cmd_t *cmd = cmd_lookup(id);
    cat_status_bump(&g_cat.status.timeouts);

    if (!cmd)
    {
        cat_cmd_clear_pending();
        return;
    }

    if (cmd->optional)
    {
        cat_cmd_clear_pending();
        if (cmd->on_timeout)
        {
            cmd->on_timeout();
        }
        return;
    }

    if (g_cat.pending.retry < CAT_CMD_MAX_RETRY)
    {
        g_cat.pending.retry++;
        g_cat.pending.sent_at = xTaskGetTickCount();
        cat_status_bump(&g_cat.status.retries);
        cat_link_tx_write(cmd->tx);
        return;
    }

    cat_cmd_clear_pending();
    cat_link_enter_degraded();
}

/* Mode changes invalidate the filter, so a pending filter poll pre-empts the
 * IF poll and consumes the tick until it is due. */
static bool poll_filter_then_if(TickType_t now)
{
    if (!g_cat.poll_filter_due)
    {
        return false;
    }
    if (now >= g_cat.poll_filter_due_at)
    {
        cat_cmd_send(CAT_CMD_GET_FILTER);
        g_cat.poll_filter_due = false;
    }
    return true;
}

static void poll_if_due(TickType_t now)
{
    if ((uint32_t)((now - g_cat.last_poll) * portTICK_PERIOD_MS) < g_cat.poll_period_ms)
    {
        return;
    }
    g_cat.last_poll = now;
    cat_cmd_send(CAT_CMD_GET_IF);
}

static void state_port_open(void)
{
    cat_svc_set_state(CAT_ST_SYNC);
    cat_cmd_send(CAT_CMD_GET_IQ);
}

static void state_sync(void)
{
    cat_cmd_send(CAT_CMD_GET_IQ);
}

static void state_probe(TickType_t now)
{
    if (g_cat.probe_step == CAT_PROBE_VERSION)
    {
        /* Informational only; a silent or unsupported radio must not
         * affect the link state. */
        cat_cmd_send(CAT_CMD_GET_VERSION);
        return;
    }

    g_cat.poll_filter_due = true;
    g_cat.poll_filter_due_at = now + pdMS_TO_TICKS(CAT_POLL_GAP_MS);
    cat_svc_set_state(CAT_ST_READY);
}

static void state_ready(TickType_t now)
{
    if (cat_iq_ready_advance(now))
    {
        return;
    }
    if (!poll_filter_then_if(now))
    {
        poll_if_due(now);
    }
}

static void state_iq_enabling(TickType_t now)
{
    if (now < g_cat.iq_retry_at)
    {
        return;
    }
    if (cat_cmd_send_bare(CAT_CMD_SET_IQ_ON))
    {
        cat_svc_set_state(CAT_ST_IQ_VERIFY);
    }
}

static void state_iq_verify(void)
{
    cat_cmd_send(CAT_CMD_GET_IQ);
}

static void state_iq_active(TickType_t now)
{
    if (poll_filter_then_if(now))
    {
        return;
    }
    if (!cat_iq_health_poll(now))
    {
        poll_if_due(now);
    }
}

void cat_state_progress(void)
{
    if (!g_cat.running || !g_cat.port)
    {
        return;
    }
    if (g_cat.pending.active)
    {
        return;
    }

    TickType_t now = xTaskGetTickCount();
    if (now < g_cat.warmup_until)
    {
        return;
    }

    switch (g_cat.status.state)
    {
    case CAT_ST_PORT_OPEN:
        state_port_open();
        break;
    case CAT_ST_SYNC:
        state_sync();
        break;
    case CAT_ST_PROBE:
        state_probe(now);
        break;
    case CAT_ST_READY:
        state_ready(now);
        break;
    case CAT_ST_IQ_ENABLING:
        state_iq_enabling(now);
        break;
    case CAT_ST_IQ_VERIFY:
        state_iq_verify();
        break;
    case CAT_ST_IQ_ACTIVE:
        state_iq_active(now);
        break;
    case CAT_ST_DEGRADED:
        if (now >= g_cat.backoff_until)
        {
            cat_svc_set_state(CAT_ST_SYNC);
        }
        break;
    case CAT_ST_FAULT:
        /* Latched: only a service restart clears it. */
        break;
    case CAT_ST_DISCONNECTED:
    default:
        break;
    }
}
