#include "app/recovery.h"

#include <string.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "app/runtime.h"
#include "app/timing_config.h"

#define RECOVERY_TIMEOUT_TICKS pdMS_TO_TICKS(APP_RECOVERY_WATCHDOG_MS)
#define RECOVERY_BACKOFF_MS_MAX APP_RECOVERY_BACKOFF_MAX_MS

typedef enum
{
    RECOVERY_ST_RUNNING = 0,
    RECOVERY_ST_DISCONNECTING,
    RECOVERY_ST_RECOVERING,
} recovery_state_t;

typedef struct
{
    uint32_t mark_ms;
    uint32_t close_ms;
    uint32_t total_ms;
    bool was_iq;
    bool valid;
} uac_timing_t;

typedef struct
{
    uint32_t mark_ms;
    uint32_t cat_ms;
    uint32_t ui_ms;
    uint32_t total_ms;
    bool valid;
} cdc_timing_t;

typedef struct
{
    uint32_t down_ms;
    uint32_t host_ms;
    uint32_t up_ms;
    uint32_t total_ms;
    bool valid;
} recovery_timing_t;

static const char *TAG = "recovery";

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static QueueHandle_t s_app_queue;
static TaskHandle_t s_watchdog_task;
static recovery_state_t s_state;
static TickType_t s_cycle_started_at;
static TickType_t s_recovery_started_at;
static bool s_uac_seen;
static bool s_cdc_seen;
static bool s_uac_drop_warned;
static bool s_cdc_drop_warned;
static uac_timing_t s_uac_timing;
static cdc_timing_t s_cdc_timing;
static recovery_timing_t s_recovery_timing;
static uint32_t s_cycle_id;
static int64_t s_last_cycle_done_us;
static uint32_t s_recent_failures;

static const char *state_to_str(recovery_state_t st)
{
    switch (st)
    {
    case RECOVERY_ST_RUNNING:
        return "RUNNING";
    case RECOVERY_ST_DISCONNECTING:
        return "DISCONNECTING";
    case RECOVERY_ST_RECOVERING:
        return "RECOVERING";
    default:
        return "?";
    }
}

static void recovery_watchdog_task(void *arg)
{
    (void)arg;
    while (true)
    {
        TickType_t recovery_started_at = 0;
        bool timed_out = false;

        vTaskDelay(pdMS_TO_TICKS(APP_RECOVERY_WATCHDOG_POLL_MS));
        taskENTER_CRITICAL(&s_lock);
        if (s_state == RECOVERY_ST_RECOVERING)
        {
            TickType_t now = xTaskGetTickCount();
            recovery_started_at = s_recovery_started_at;
            timed_out = (now - recovery_started_at) >= RECOVERY_TIMEOUT_TICKS;
        }
        taskEXIT_CRITICAL(&s_lock);

        if (!timed_out)
            continue;

        const app_evt_t ev = APP_EVT_RECOVERY_TIMEOUT;
        if (s_app_queue)
        {
            (void)xQueueSend(s_app_queue, &ev, 0);
        }
        ESP_LOGE(TAG, "RECOVERY_TIMEOUT age_ms=%lu -> reboot",
                 (unsigned long)((xTaskGetTickCount() - recovery_started_at) * portTICK_PERIOD_MS));
        esp_restart();
    }
}

static void log_cycle_summary(void)
{
    TickType_t cycle_started_at;
    uac_timing_t uac;
    cdc_timing_t cdc;
    recovery_timing_t rec;
    uint32_t cycle_id;

    taskENTER_CRITICAL(&s_lock);
    cycle_started_at = s_cycle_started_at;
    uac = s_uac_timing;
    cdc = s_cdc_timing;
    rec = s_recovery_timing;
    cycle_id = s_cycle_id;
    taskEXIT_CRITICAL(&s_lock);

    uint32_t cycle_ms = (uint32_t)((xTaskGetTickCount() - cycle_started_at) * portTICK_PERIOD_MS);
    bool info_level = (cycle_ms >= APP_RECOVERY_SUMMARY_SLOW_MS) ||
                      (APP_RECOVERY_SUMMARY_INFO_EVERY_CYCLES > 0 &&
                       (cycle_id % APP_RECOVERY_SUMMARY_INFO_EVERY_CYCLES) == 0);

    if (rec.valid)
    {
        if (info_level)
        {
            ESP_LOGI(TAG,
                     "cycle=%lu summary ms: down=%lu host=%lu up=%lu total=%lu elapsed=%lu"
                     " uac_close=%lu cdc_cat=%lu ui=%lu iq=%u",
                     (unsigned long)cycle_id,
                     (unsigned long)rec.down_ms,
                     (unsigned long)rec.host_ms,
                     (unsigned long)rec.up_ms,
                     (unsigned long)rec.total_ms,
                     (unsigned long)cycle_ms,
                     (unsigned long)(uac.valid ? uac.close_ms : 0),
                     (unsigned long)(cdc.valid ? cdc.cat_ms : 0),
                     (unsigned long)(cdc.valid ? cdc.ui_ms : 0),
                     (unsigned)(uac.valid && uac.was_iq ? 1u : 0u));
        }
        else
        {
            ESP_LOGD(TAG,
                     "cycle=%lu summary ms: down=%lu host=%lu up=%lu total=%lu elapsed=%lu"
                     " uac_close=%lu cdc_cat=%lu ui=%lu iq=%u",
                     (unsigned long)cycle_id,
                     (unsigned long)rec.down_ms,
                     (unsigned long)rec.host_ms,
                     (unsigned long)rec.up_ms,
                     (unsigned long)rec.total_ms,
                     (unsigned long)cycle_ms,
                     (unsigned long)(uac.valid ? uac.close_ms : 0),
                     (unsigned long)(cdc.valid ? cdc.cat_ms : 0),
                     (unsigned long)(cdc.valid ? cdc.ui_ms : 0),
                     (unsigned)(uac.valid && uac.was_iq ? 1u : 0u));
        }
    }
    else
    {
        if (info_level)
        {
            ESP_LOGI(TAG, "cycle=%lu summary: n/a elapsed=%lu",
                     (unsigned long)cycle_id,
                     (unsigned long)cycle_ms);
        }
        else
        {
            ESP_LOGD(TAG, "cycle=%lu summary: n/a elapsed=%lu",
                     (unsigned long)cycle_id,
                     (unsigned long)cycle_ms);
        }
    }
}

static uint32_t compute_backoff_ms_locked(void)
{
    const int64_t now_us = esp_timer_get_time();
    uint32_t backoff = 0;
    if (s_last_cycle_done_us > 0)
    {
        uint32_t since_done_ms = (uint32_t)((now_us - s_last_cycle_done_us) / 1000);
        if (since_done_ms < 10000)
        {
            if (s_recent_failures < 6)
                s_recent_failures++;
        }
        else
        {
            s_recent_failures = 0;
        }
    }
    if (s_recent_failures > 1)
    {
        uint32_t n = s_recent_failures - 1;
        if (n > 5)
            n = 5;
        backoff = 100u * (1u << n);
        if (backoff > RECOVERY_BACKOFF_MS_MAX)
            backoff = RECOVERY_BACKOFF_MS_MAX;
    }
    return backoff;
}

void recovery_init(QueueHandle_t app_queue)
{
    taskENTER_CRITICAL(&s_lock);
    s_app_queue = app_queue;
    s_state = RECOVERY_ST_RUNNING;
    s_cycle_started_at = 0;
    s_recovery_started_at = 0;
    s_uac_seen = false;
    s_cdc_seen = false;
    s_uac_drop_warned = false;
    s_cdc_drop_warned = false;
    memset(&s_uac_timing, 0, sizeof(s_uac_timing));
    memset(&s_cdc_timing, 0, sizeof(s_cdc_timing));
    memset(&s_recovery_timing, 0, sizeof(s_recovery_timing));
    s_cycle_id = 0;
    s_last_cycle_done_us = 0;
    s_recent_failures = 0;
    taskEXIT_CRITICAL(&s_lock);

    if (s_watchdog_task == NULL)
    {
        BaseType_t ok = xTaskCreatePinnedToCore(recovery_watchdog_task,
                                                "recovery_wd",
                                                3072,
                                                NULL,
                                                2,
                                                &s_watchdog_task,
                                                1);
        if (ok != pdTRUE)
        {
            ESP_LOGE(TAG, "failed to create recovery watchdog task");
        }
    }
}

bool recovery_on_disconnect(recovery_source_t source)
{
    bool started_cycle = false;
    bool drop_dup_uac = false;
    bool drop_dup_cdc = false;
    recovery_state_t log_state = RECOVERY_ST_RUNNING;

    taskENTER_CRITICAL(&s_lock);
    if (s_state == RECOVERY_ST_RUNNING)
    {
        s_state = RECOVERY_ST_DISCONNECTING;
        s_cycle_id++;
        s_cycle_started_at = xTaskGetTickCount();
        memset(&s_uac_timing, 0, sizeof(s_uac_timing));
        memset(&s_cdc_timing, 0, sizeof(s_cdc_timing));
        memset(&s_recovery_timing, 0, sizeof(s_recovery_timing));
        s_uac_seen = false;
        s_cdc_seen = false;
        s_uac_drop_warned = false;
        s_cdc_drop_warned = false;
        started_cycle = true;
    }

    if (source == RECOVERY_SRC_UAC)
    {
        if (s_uac_seen)
        {
            if (!s_uac_drop_warned)
            {
                s_uac_drop_warned = true;
                drop_dup_uac = true;
            }
            log_state = s_state;
            taskEXIT_CRITICAL(&s_lock);
            if (started_cycle)
            {
                ESP_LOGD(TAG, "cycle=%lu start state=%s",
                         (unsigned long)s_cycle_id,
                         state_to_str(RECOVERY_ST_DISCONNECTING));
            }
            if (drop_dup_uac)
            {
                ESP_LOGW(TAG, "drop duplicate UAC disconnect in state=%s", state_to_str(log_state));
            }
            return false;
        }
        s_uac_seen = true;
        taskEXIT_CRITICAL(&s_lock);
        if (started_cycle)
        {
            ESP_LOGD(TAG, "cycle=%lu start state=%s",
                     (unsigned long)s_cycle_id,
                     state_to_str(RECOVERY_ST_DISCONNECTING));
        }
        return true;
    }

    if (source == RECOVERY_SRC_CDC)
    {
        if (s_cdc_seen)
        {
            if (!s_cdc_drop_warned)
            {
                s_cdc_drop_warned = true;
                drop_dup_cdc = true;
            }
            log_state = s_state;
            taskEXIT_CRITICAL(&s_lock);
            if (started_cycle)
            {
                ESP_LOGD(TAG, "cycle=%lu start state=%s",
                         (unsigned long)s_cycle_id,
                         state_to_str(RECOVERY_ST_DISCONNECTING));
            }
            if (drop_dup_cdc)
            {
                ESP_LOGW(TAG, "drop duplicate CDC disconnect in state=%s", state_to_str(log_state));
            }
            return false;
        }
        s_cdc_seen = true;
        taskEXIT_CRITICAL(&s_lock);
        if (started_cycle)
        {
            ESP_LOGD(TAG, "cycle=%lu start state=%s",
                     (unsigned long)s_cycle_id,
                     state_to_str(RECOVERY_ST_DISCONNECTING));
        }
        return true;
    }

    taskEXIT_CRITICAL(&s_lock);
    if (started_cycle)
    {
        ESP_LOGD(TAG, "cycle=%lu start state=%s",
                 (unsigned long)s_cycle_id,
                 state_to_str(RECOVERY_ST_DISCONNECTING));
    }
    return false;
}

bool recovery_request(void)
{
    bool should_enqueue = false;
    uint32_t cycle_id = 0;

    taskENTER_CRITICAL(&s_lock);
    if (!s_app_queue)
    {
        taskEXIT_CRITICAL(&s_lock);
        return false;
    }
    if (s_state == RECOVERY_ST_RECOVERING)
    {
        taskEXIT_CRITICAL(&s_lock);
        return false;
    }

    s_state = RECOVERY_ST_RECOVERING;
    s_recovery_started_at = xTaskGetTickCount();
    cycle_id = s_cycle_id;
    should_enqueue = true;
    taskEXIT_CRITICAL(&s_lock);

    if (!should_enqueue)
    {
        return false;
    }

    const app_evt_t ev = APP_EVT_RECOVERY_START;
    if (xQueueSend(s_app_queue, &ev, 0) != pdTRUE)
    {
        ESP_LOGW(TAG, "failed to enqueue RECOVERY_START; watchdog remains active");
        return false;
    }
    ESP_LOGD(TAG, "cycle=%lu state=RECOVERING", (unsigned long)cycle_id);
    return true;
}

void recovery_mark_done(void)
{
    log_cycle_summary();

    taskENTER_CRITICAL(&s_lock);
    s_state = RECOVERY_ST_RUNNING;
    s_uac_seen = false;
    s_cdc_seen = false;
    s_uac_drop_warned = false;
    s_cdc_drop_warned = false;
    s_last_cycle_done_us = esp_timer_get_time();
    taskEXIT_CRITICAL(&s_lock);

    const app_evt_t ev = APP_EVT_RECOVERY_DONE;
    if (s_app_queue)
    {
        (void)xQueueSend(s_app_queue, &ev, 0);
    }
}

uint32_t recovery_get_cycle_id(void)
{
    uint32_t cycle_id;
    taskENTER_CRITICAL(&s_lock);
    cycle_id = s_cycle_id;
    taskEXIT_CRITICAL(&s_lock);
    return cycle_id;
}

uint32_t recovery_get_backoff_ms(void)
{
    uint32_t backoff;
    taskENTER_CRITICAL(&s_lock);
    backoff = compute_backoff_ms_locked();
    taskEXIT_CRITICAL(&s_lock);
    return backoff;
}

bool recovery_is_active(void)
{
    bool active;
    taskENTER_CRITICAL(&s_lock);
    active = s_state != RECOVERY_ST_RUNNING;
    taskEXIT_CRITICAL(&s_lock);
    return active;
}

void recovery_record_uac_timing(uint32_t mark_ms, uint32_t close_ms, uint32_t total_ms, bool was_iq)
{
    taskENTER_CRITICAL(&s_lock);
    s_uac_timing.mark_ms = mark_ms;
    s_uac_timing.close_ms = close_ms;
    s_uac_timing.total_ms = total_ms;
    s_uac_timing.was_iq = was_iq;
    s_uac_timing.valid = true;
    taskEXIT_CRITICAL(&s_lock);
}

void recovery_record_cdc_timing(uint32_t mark_ms, uint32_t cat_ms, uint32_t ui_ms, uint32_t total_ms)
{
    taskENTER_CRITICAL(&s_lock);
    s_cdc_timing.mark_ms = mark_ms;
    s_cdc_timing.cat_ms = cat_ms;
    s_cdc_timing.ui_ms = ui_ms;
    s_cdc_timing.total_ms = total_ms;
    s_cdc_timing.valid = true;
    taskEXIT_CRITICAL(&s_lock);
}

void recovery_record_recovery_timing(uint32_t down_ms, uint32_t host_ms, uint32_t up_ms, uint32_t total_ms)
{
    taskENTER_CRITICAL(&s_lock);
    s_recovery_timing.down_ms = down_ms;
    s_recovery_timing.host_ms = host_ms;
    s_recovery_timing.up_ms = up_ms;
    s_recovery_timing.total_ms = total_ms;
    s_recovery_timing.valid = true;
    taskEXIT_CRITICAL(&s_lock);
}
