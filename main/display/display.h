#pragma once

#include <stddef.h>
#include "lvgl.h"

/*
 * Display status strings — shown on a friendly two-line status view.
 * Call from any FreeRTOS task; functions are internally thread-safe.
 */
#define DISP_MSG_CONNECT_USB "Waiting for device"
#define DISP_MSG_USB_FOUND "Device detected"
#define DISP_MSG_ENABLING_IQ "Preparing spectrum"
#define DISP_MSG_DISCONNECTED "Device disconnected"

/* Logical display dimensions after 90° CW rotation */
#define DISP_LOGICAL_W 284
#define DISP_LOGICAL_H 76

/*
 * Initialise SPI bus, ST7789P3 panel and LVGL.
 * Must be called once before any other display function.
 */
void display_init(void);

/*
 * Update the centered status label.
 * msg must remain valid for the duration of the call (it is copied internally).
 */
void display_set_status(const char *msg);

/*
 * Update two-line status text.
 * title/detail must remain valid for the call duration (copied internally).
 * detail can be NULL or empty to hide the second line.
 */
void display_set_status_lines(const char *title, const char *detail);

/*
 * Switch to spectrum mode: hide the status label, create a full-screen
 * canvas and return it.  The caller passes this to spectrum_init().
 * Returns NULL on failure.
 */
lv_obj_t *display_show_spectrum(void);

/* Switch back to centered status mode and hide spectrum canvas. */
void display_show_status(const char *msg);
