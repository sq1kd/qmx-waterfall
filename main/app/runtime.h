#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

void app_runtime_init(SemaphoreHandle_t dev_mutex, int uac_task_priority);
void app_runtime_install_usb_clients(void);
void app_runtime_shutdown_clients(void);
