#pragma once

#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "app/runtime.h"
#include "usb/uac_host.h"
#include "iot_usbh_cdc.h"

#define APP_RUNTIME_MAX_UAC_DEVICES 8

extern SemaphoreHandle_t s_dev_mutex;
extern int s_uac_task_priority;

extern uac_host_device_handle_t s_uac_handles[APP_RUNTIME_MAX_UAC_DEVICES];
extern usbh_cdc_port_handle_t s_cdc_port;
extern uac_host_device_handle_t s_iq_handle;
extern uac_host_stream_config_t s_iq_stream_cfg;
extern bool s_iq_stream_started;
extern bool s_spectrum_started;
extern bool s_iq_start_in_progress;

typedef struct
{
    bool uac_pending;
    bool cdc_pending;
    bool uac_drop_logged;
    bool cdc_drop_logged;
    bool uac_seen;
} runtime_disconnect_state_t;

typedef struct
{
    EventGroupHandle_t stop_events;
    bool uac_installed;
    bool cdc_installed;
} runtime_install_state_t;

extern runtime_disconnect_state_t s_disc;
extern runtime_install_state_t s_install;

#define APP_STOPBIT_UAC_TASK (1U << 0)
#define APP_STOPBIT_CDC_TASK (1U << 1)

void app_runtime_set_stream_state(bool iq_started, bool spectrum_started, bool iq_start_in_progress);
void app_runtime_close_all_uac_handles(void);
void app_runtime_close_cdc_port_if_open(void);

void app_runtime_uac_install_driver(void);
void app_runtime_uac_uninstall_driver(void);
void app_runtime_trigger_iq_start_async(void);

void app_runtime_cdc_install_driver(void);
void app_runtime_cdc_uninstall_driver(void);
