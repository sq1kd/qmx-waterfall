#include "cat/service.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "cat_iq";

enum
{
    CAT_IQ_RETRY_FAST_MS = 1000,
    CAT_IQ_RETRY_MAX_FAST = 3,
    CAT_IQ_RETRY_CAP_MS = 10000,
};

static uint32_t iq_retry_delay_ms(uint8_t attempts)
{
    uint32_t delay = CAT_IQ_RETRY_FAST_MS;
    for (uint8_t i = CAT_IQ_RETRY_MAX_FAST; i < attempts; i++)
    {
        if (delay >= CAT_IQ_RETRY_CAP_MS)
        {
            return CAT_IQ_RETRY_CAP_MS;
        }
        delay <<= 1;
    }
    return (delay > CAT_IQ_RETRY_CAP_MS) ? CAT_IQ_RETRY_CAP_MS : delay;
}

bool cat_iq_ready_advance(TickType_t now)
{
    bool known;
    bool enabled;

    xSemaphoreTake(g_cat.lock, portMAX_DELAY);
    known = g_cat.status.iq_known;
    enabled = g_cat.status.iq_enabled;
    if (known && enabled)
    {
        g_cat.status.iq_recovering = false;
    }
    xSemaphoreGive(g_cat.lock);

    if (known && enabled)
    {
        g_cat.iq_attempts = 0;
        g_cat.iq_next_poll = now + pdMS_TO_TICKS(g_cat.iq_poll_period_ms);
        cat_svc_set_state(CAT_ST_IQ_ACTIVE);
        return true;
    }

    if (!g_cat.auto_enable_iq)
    {
        return false;
    }

    g_cat.iq_attempts = 0;
    g_cat.iq_retry_at = now;
    cat_svc_set_state(CAT_ST_IQ_ENABLING);
    return true;
}

bool cat_iq_health_poll(TickType_t now)
{
    if (!g_cat.auto_enable_iq || now < g_cat.iq_next_poll)
    {
        return false;
    }
    g_cat.iq_next_poll = now + pdMS_TO_TICKS(g_cat.iq_poll_period_ms);
    cat_cmd_send(CAT_CMD_IQ_HEALTH);
    return true;
}

void cat_iq_recovery_begin(void)
{
    xSemaphoreTake(g_cat.lock, portMAX_DELAY);
    g_cat.status.iq_drops++;
    g_cat.status.iq_recovering = true;
    xSemaphoreGive(g_cat.lock);

    g_cat.iq_attempts = 0;
    g_cat.iq_retry_at = xTaskGetTickCount();
    ESP_LOGW(TAG, "IQ mode dropped by radio, re-enabling");
    cat_svc_set_state(CAT_ST_IQ_ENABLING);
}

void cat_iq_attempt_failed(void)
{
    g_cat.iq_attempts++;
    uint32_t delay = iq_retry_delay_ms(g_cat.iq_attempts);
    g_cat.iq_retry_at = xTaskGetTickCount() + pdMS_TO_TICKS(delay);
    ESP_LOGW(TAG, "IQ enable not confirmed, retry %u in %lums",
             (unsigned)g_cat.iq_attempts, (unsigned long)delay);
    cat_svc_set_state(CAT_ST_IQ_ENABLING);
}

void cat_iq_recovery_succeeded(void)
{
    TickType_t now = xTaskGetTickCount();
    bool was_recovering;
    uint32_t drops;
    uint32_t reenables;

    xSemaphoreTake(g_cat.lock, portMAX_DELAY);
    was_recovering = g_cat.status.iq_recovering;
    g_cat.status.iq_recovering = false;
    if (was_recovering)
    {
        g_cat.status.iq_reenables++;
    }
    drops = g_cat.status.iq_drops;
    reenables = g_cat.status.iq_reenables;
    xSemaphoreGive(g_cat.lock);

    g_cat.iq_attempts = 0;
    g_cat.iq_next_poll = now + pdMS_TO_TICKS(g_cat.iq_poll_period_ms);

    if (was_recovering)
    {
        /* The operator may have changed mode/filter in the menu that
         * dropped IQ mode; refresh them right after recovery. */
        g_cat.poll_filter_due = true;
        g_cat.poll_filter_due_at = now + pdMS_TO_TICKS(CAT_POLL_GAP_MS);
        ESP_LOGI(TAG, "IQ mode restored (drops=%lu reenables=%lu)",
                 (unsigned long)drops, (unsigned long)reenables);
    }
    cat_svc_set_state(CAT_ST_IQ_ACTIVE);
}
