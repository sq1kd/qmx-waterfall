#pragma once

#include <stddef.h>
#include "lvgl.h"

/*
 * Display status strings — shown on a friendly two-line status view.
 * Call from any FreeRTOS task; functions are internally thread-safe.
 */
#define DISP_MSG_CONNECT_USB "Waiting for device"
#define DISP_MSG_DISCONNECTED "Device disconnected"

/* Logical display dimensions after 90° CW rotation */
#define DISP_LOGICAL_W 284
#define DISP_LOGICAL_H 76

/* Startup progress checklist. Fixed row order, one row per stage. */
typedef enum
{
    DISP_STAGE_USB = 0,
    DISP_STAGE_AUDIO,
    DISP_STAGE_CAT,
    DISP_STAGE_RADIO,
    DISP_STAGE_IQ,
    DISP_STAGE_SPECTRUM,
    DISP_STAGE_COUNT,
} disp_stage_t;

typedef enum
{
    DISP_STAGE_PENDING = 0,
    DISP_STAGE_ACTIVE,
    DISP_STAGE_DONE,
    DISP_STAGE_FAILED,
} disp_stage_state_t;

/*
 * Show the progress checklist (idempotent; resets rows only when it was
 * hidden). Safe from any task. Replaces the two-line status view.
 */
void display_show_progress(void);

/*
 * Update one stage; detail is right-aligned and may be NULL to keep the
 * current text. No-op when the checklist is not on screen.
 */
void display_stage(disp_stage_t stage, disp_stage_state_t state, const char *detail);

/*
 * Called once the waterfall has produced its first frame: marks the
 * spectrum stage done and hides the checklist.
 */
void display_progress_finish(void);

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

/*
 * Delete the spectrum canvas widget. Must be called before the canvas
 * backing buffer is freed: LVGL keeps a pointer to it and would otherwise
 * render freed memory after a teardown. Idempotent; waits up to 100 ms for
 * the LVGL port lock (call with the spectrum pipeline already stopped).
 * Returns false when the port lock could not be taken and the widget is
 * still attached, in which case the buffer must not be freed.
 */
bool display_spectrum_detach(void);

/* Switch back to centered status mode and hide spectrum canvas. */
void display_show_status(const char *msg);
