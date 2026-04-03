#pragma once

/*
 * Centralized app/runtime timing knobs.
 *
 * Defaults in this file are intentionally conservative for stability while
 * still keeping recovery fast. Tune here first before changing call sites.
 */

/* Shared lock timeout for state transitions around USB disconnect/reconnect. */
#define APP_DEV_MUTEX_TIMEOUT_MS 200u

/* Recovery watchdog: reboot if one cycle is stuck for too long. */
#define APP_RECOVERY_WATCHDOG_MS 5000u
#define APP_RECOVERY_WATCHDOG_POLL_MS 200u

/* Exponential recovery backoff cap for repeated short-interval failures. */
#define APP_RECOVERY_BACKOFF_MAX_MS 4000u

/* USB host loop and stop coordination. */
#define APP_USB_LIB_EVENT_TIMEOUT_MS 250u
#define APP_USB_LIB_STOP_WAIT_MS 1500u

/* IQ/UAC start behavior. */
#define APP_IQ_RESUME_RETRY_DELAY_MS 1000u
#define APP_IQ_RESUME_RETRY_MAX_ATTEMPTS 5u
#define APP_IQ_START_BACKOFF_AFTER_RESUME_FAIL_MS 1500u
#define APP_IQ_START_BACKOFF_AFTER_SPECTRUM_FAIL_MS 1000u

/* Common task stop polling helpers used during uninstall. */
#define APP_TASK_STOP_MAX_POLLS 50u
#define APP_TASK_STOP_POLL_MS 10u

/* Retry window for transient INVALID_STATE during teardown. */
#define APP_USB_UNINSTALL_RETRY_MAX_ATTEMPTS 20u
#define APP_USB_UNINSTALL_RETRY_DELAY_MS 10u

/* Recovery summary emission policy for INFO vs DEBUG. */
#define APP_RECOVERY_SUMMARY_SLOW_MS 150u
#define APP_RECOVERY_SUMMARY_INFO_EVERY_CYCLES 10u
