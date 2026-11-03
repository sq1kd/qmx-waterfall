#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_err.h"

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

typedef esp_err_t (*app_runtime_noarg_fn_t)(void);
esp_err_t app_runtime_retry_invalid_state(const char *op_name, app_runtime_noarg_fn_t fn);
uint32_t app_runtime_conn_epoch_get(void);
uint32_t app_runtime_conn_epoch_bump(void);
bool app_runtime_conn_epoch_is_current(uint32_t epoch);
bool app_runtime_clients_uninstalled(void);
