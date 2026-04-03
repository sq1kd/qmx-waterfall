#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

typedef enum
{
    APP_EVT_QUIT = 1,
    APP_EVT_RECOVERY_START,
    APP_EVT_RECOVERY_DONE,
    APP_EVT_RECOVERY_TIMEOUT,
} app_evt_t;

void app_runtime_init(SemaphoreHandle_t dev_mutex, int uac_task_priority);
void app_runtime_install_usb_clients(void);
void app_runtime_shutdown_clients(void);
