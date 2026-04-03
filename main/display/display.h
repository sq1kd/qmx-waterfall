#pragma once

#include <stddef.h>
#include "lvgl.h"

/*
 * Display status strings — shown centered on the screen.
 * Call from any FreeRTOS task; the function is internally thread-safe.
 */
#define DISP_MSG_CONNECT_USB "Connect USB device"
#define DISP_MSG_USB_FOUND "USB device found"
#define DISP_MSG_ENABLING_IQ "Enabling IQ mode..."
#define DISP_MSG_DISCONNECTED "USB disconnected"

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
 * Switch to spectrum mode: hide the status label, create a full-screen
 * canvas and return it.  The caller passes this to spectrum_init().
 * Returns NULL on failure.
 */
lv_obj_t *display_show_spectrum(void);

/* Switch back to centered status mode and hide spectrum canvas. */
void display_show_status(const char *msg);
