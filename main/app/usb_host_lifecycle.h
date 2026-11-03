#pragma once

#include <stdbool.h>
#include "freertos/FreeRTOS.h"

/* stop/start hooks report the tick at which each phase finished (may be NULL). */
bool usb_host_lifecycle_start(void);
bool usb_host_lifecycle_stop(void);
bool usb_host_lifecycle_restart(TickType_t *stop_done_tick, TickType_t *start_done_tick);
