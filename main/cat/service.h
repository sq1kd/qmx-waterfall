#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "iot_usbh_cdc.h"

typedef enum
{
    CAT_ST_DISCONNECTED = 0,
    CAT_ST_PORT_OPEN,
    CAT_ST_SYNC,
    CAT_ST_PROBE,
    CAT_ST_READY,
    CAT_ST_IQ_ENABLING,
    CAT_ST_IQ_VERIFY,
    CAT_ST_IQ_ACTIVE,
    CAT_ST_DEGRADED,
    CAT_ST_FAULT,
} cat_state_t;

typedef struct
{
    cat_state_t state;
    bool iq_enabled;
    bool iq_known;
    char mode[16];
    uint8_t mode_code;
    char filter[24];
    uint32_t filter_hz;
    int32_t rit_hz;
    bool rit_on;
    uint32_t rx_frames;
    uint32_t tx_cmds;
    uint32_t parse_errors;
    uint32_t timeouts;
    uint32_t retries;
} cat_status_t;

typedef void (*cat_status_cb_t)(const cat_status_t *status, void *user_ctx);

typedef struct
{
    bool auto_enable_iq;
    uint32_t poll_period_ms; /* mode/filter polling; default 1000 */
    cat_status_cb_t status_cb;
    void *status_cb_ctx;
} cat_service_config_t;

void cat_service_default_config(cat_service_config_t *cfg);

esp_err_t cat_service_start(usbh_cdc_port_handle_t port, const cat_service_config_t *cfg);
void cat_service_stop(void);

/* Fast non-blocking stop hint for disconnect callbacks. */
void cat_service_request_stop_fast(void);

/* Push raw bytes from CDC recv callback into CAT service. */
void cat_service_on_rx_bytes(const uint8_t *data, size_t len);

bool cat_service_get_status(cat_status_t *out);

const char *cat_state_to_str(cat_state_t st);
