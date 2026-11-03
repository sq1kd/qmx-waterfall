#include "app/evt_worker.h"

#include <stdint.h>
#include "esp_log.h"
#include "app/app_common.h"

static const char *TAG = APP_TAG;

static void evt_worker_task(void *arg)
{
    app_evt_worker_t *w = arg;
    uint8_t *ev = pvPortMalloc(w->item_size);
    if (ev == NULL)
    {
        ESP_LOGE(TAG, "%s: event buffer alloc failed", w->name);
    }
    else
    {
        while (xQueueReceive(w->queue, ev, portMAX_DELAY) == pdTRUE)
        {
            if (!w->handle(w->ctx, ev))
            {
                break;
            }
        }
        vPortFree(ev);
    }

    w->task = NULL;
    if (w->events)
    {
        xEventGroupSetBits(w->events, w->stop_bit);
    }
    vTaskDelete(NULL);
}

bool app_evt_worker_start(app_evt_worker_t *w)
{
    if (!w || !w->handle || w->item_size == 0)
    {
        return false;
    }
    if (w->task != NULL)
    {
        return true;
    }

    bool created = false;
    if (w->queue == NULL)
    {
        w->queue = xQueueCreate(w->queue_len, w->item_size);
        if (w->queue == NULL)
        {
            ESP_LOGE(TAG, "%s: event queue create failed", w->name);
            return false;
        }
        created = true;
    }

    if (w->events)
    {
        xEventGroupClearBits(w->events, w->stop_bit);
    }

    BaseType_t ok = xTaskCreatePinnedToCore(evt_worker_task, w->name, w->stack_words,
                                            w, w->priority, &w->task, w->core_id);
    if (ok != pdTRUE)
    {
        w->task = NULL;
        if (created)
        {
            vQueueDelete(w->queue);
            w->queue = NULL;
        }
        ESP_LOGE(TAG, "%s: event task create failed", w->name);
        return false;
    }
    return true;
}

bool app_evt_worker_post(app_evt_worker_t *w, const void *ev, TickType_t wait)
{
    if (!w || w->queue == NULL || ev == NULL)
    {
        return false;
    }
    return xQueueSend(w->queue, ev, wait) == pdTRUE;
}

bool app_evt_worker_stop(app_evt_worker_t *w, TickType_t wait)
{
    if (!w || w->events == NULL)
    {
        return false;
    }
    EventBits_t bits = xEventGroupWaitBits(w->events, w->stop_bit, pdTRUE, pdFALSE, wait);
    if ((bits & w->stop_bit) == 0)
    {
        ESP_LOGE(TAG, "%s: event task stop timeout", w->name);
        return false;
    }
    return true;
}

void app_evt_worker_destroy(app_evt_worker_t *w)
{
    if (!w)
    {
        return;
    }
    if (w->queue)
    {
        vQueueDelete(w->queue);
        w->queue = NULL;
    }
}
