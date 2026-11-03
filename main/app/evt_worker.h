#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"

typedef struct
{
    const char *name;
    size_t item_size;
    UBaseType_t queue_len;
    EventGroupHandle_t events;
    EventBits_t stop_bit;
    uint32_t stack_words;
    UBaseType_t priority;
    BaseType_t core_id;
    void *ctx;
    bool (*handle)(void *ctx, const void *ev);
    QueueHandle_t queue;
    TaskHandle_t task;
} app_evt_worker_t;

bool app_evt_worker_start(app_evt_worker_t *w);
bool app_evt_worker_post(app_evt_worker_t *w, const void *ev, TickType_t wait);
bool app_evt_worker_stop(app_evt_worker_t *w, TickType_t wait);
void app_evt_worker_destroy(app_evt_worker_t *w);
