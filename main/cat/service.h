#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
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

enum
{
    CAT_FW_MAX = 16,
    CAT_MODE_MAX = 16,
    CAT_FILTER_MAX = 24,
};

typedef struct
{
    cat_state_t state;
    bool iq_enabled;
    bool iq_known;
    bool iq_recovering;
    char fw_version[CAT_FW_MAX];
    char mode[CAT_MODE_MAX];
    uint8_t mode_code;
    char filter[CAT_FILTER_MAX];
    uint32_t filter_hz;
    int32_t rit_hz;
    bool rit_on;
    uint32_t rx_frames;
    uint32_t tx_cmds;
    uint32_t timeouts;
    uint32_t retries;
    uint32_t iq_drops;
    uint32_t iq_reenables;
    uint32_t tx_errors;
    uint32_t rx_overflows;
    uint32_t rx_queue_drops;
} cat_status_t;

typedef void (*cat_status_cb_t)(const cat_status_t *status, void *user_ctx);

typedef struct
{
    bool auto_enable_iq;
    uint32_t poll_period_ms;    /* mode/filter polling; default 1000 */
    uint32_t iq_poll_period_ms; /* Q9 health polling while IQ active; default 2000 */
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

const char *cat_state_name(cat_state_t st);

/*
 * Internal plumbing shared by cat/service.c, cat/commands.c and cat/iq.c.
 * Not part of the service API; app code must use the declarations above.
 */

enum
{
    CAT_RX_ACCUM_MAX = 256,
    CAT_CMD_TIMEOUT_MS = 700,
    CAT_CMD_MAX_RETRY = 2,
    CAT_POLL_GAP_MS = 250,
    CAT_MIN_TX_SPACING_MS = 150,
    CAT_WRITE_TIMEOUT_MS = 500,
    /* Strictly longer than CAT_WRITE_TIMEOUT_MS so a task stuck in a write
     * cannot be deleted while the USB stack still owns its context. */
    CAT_STOP_WAIT_MS = 1000,
    CAT_TX_FAIL_LIMIT = 3,
};

typedef enum
{
    CAT_CMD_GET_IQ = 0,
    CAT_CMD_SET_IQ_ON,
    CAT_CMD_GET_IF,
    CAT_CMD_GET_FILTER,
    CAT_CMD_GET_VERSION,
    CAT_CMD_IQ_HEALTH,
    CAT_CMD_COUNT,
} cat_cmd_id_t;

typedef enum
{
    CAT_PROBE_VERSION = 0,
    CAT_PROBE_FILTER,
} cat_probe_step_t;

typedef struct
{
    bool active;
    cat_cmd_id_t id;
    TickType_t sent_at;
    uint8_t retry;
} cat_pending_t;

typedef struct
{
    bool running;
    bool auto_enable_iq;
    uint32_t poll_period_ms;
    TaskHandle_t task;
    QueueHandle_t rx_queue;
    SemaphoreHandle_t lock;
    usbh_cdc_port_handle_t port;
    cat_status_t status;
    cat_status_cb_t cb;
    void *cb_ctx;

    char rx_accum[CAT_RX_ACCUM_MAX];
    size_t rx_accum_len;
    bool rx_discard;
    volatile uint32_t rx_drop_seq;
    uint32_t rx_drop_seen;

    cat_pending_t pending;
    TickType_t backoff_until;
    uint8_t recovery_attempts;
    uint8_t tx_failures;
    cat_probe_step_t probe_step;
    uint32_t iq_poll_period_ms;
    TickType_t iq_next_poll;
    TickType_t iq_retry_at;
    uint8_t iq_attempts;
    TickType_t last_poll;
    bool poll_filter_due;
    TickType_t poll_filter_due_at;
    TickType_t last_status_log;
    TickType_t warmup_until;
    TickType_t next_tx_allowed_at;
    volatile bool task_exited;
} cat_svc_t;

extern cat_svc_t g_cat;

void cat_status_bump(uint32_t *counter);
void cat_svc_notify_status(void);
void cat_svc_set_state(cat_state_t st);
bool cat_link_tx_write(const char *cmd);
void cat_link_note_good_reply(void);
void cat_link_enter_degraded(void);

bool cat_cmd_send(cat_cmd_id_t id);
bool cat_cmd_send_bare(cat_cmd_id_t id);
void cat_cmd_clear_pending(void);
void cat_check_timeout(void);
void cat_state_progress(void);
void cat_frame_handle(const char *frame);

bool cat_iq_ready_advance(TickType_t now);
bool cat_iq_health_poll(TickType_t now);
void cat_iq_recovery_begin(void);
void cat_iq_attempt_failed(void);
void cat_iq_recovery_succeeded(void);
