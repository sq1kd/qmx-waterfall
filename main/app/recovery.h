#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

typedef enum
{
    RECOVERY_SRC_UAC = 1,
    RECOVERY_SRC_CDC,
} recovery_source_t;

typedef enum
{
    RECOVERY_EVT_UAC_DISCONNECTED = 1,
    RECOVERY_EVT_CDC_DISCONNECTED,
    RECOVERY_EVT_RECOVERY_START,
    RECOVERY_EVT_RECOVERY_DONE,
    RECOVERY_EVT_RECOVERY_TIMEOUT,
} recovery_event_t;

void recovery_init(QueueHandle_t app_queue);
bool recovery_on_disconnect(recovery_source_t source);
bool recovery_request(void);
void recovery_mark_done(void);
bool recovery_is_active(void);
uint32_t recovery_get_cycle_id(void);
uint32_t recovery_get_backoff_ms(void);

void recovery_record_uac_timing(uint32_t mark_ms, uint32_t close_ms, uint32_t total_ms, bool was_iq);
void recovery_record_cdc_timing(uint32_t mark_ms, uint32_t cat_ms, uint32_t ui_ms, uint32_t total_ms);
void recovery_record_recovery_timing(uint32_t down_ms, uint32_t host_ms, uint32_t up_ms, uint32_t total_ms);
