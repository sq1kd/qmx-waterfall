#pragma once

#include <stddef.h>
#include "lvgl.h"
#include "ui/geometry.h"

/* Logical display dimensions after 90° CW rotation. */
#define DISP_LOGICAL_W QMX_SCREEN_W
#define DISP_LOGICAL_H QMX_SCREEN_H

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
 * Show the progress view (brand header, status line and a single scrolling
 * rail of stage chips) and put every stage back to pending. Idempotent while
 * the same connection is in progress. Safe from any task.
 */
void display_show_progress(void);

/*
 * Update one stage; detail is technical text and may be NULL to keep the
 * current value. No-op when the progress view is not on screen.
 */
void display_stage(disp_stage_t stage, disp_stage_state_t state, const char *detail);

/*
 * Marks the spectrum stage done and hides the checklist. Returns false when
 * the LVGL port lock could not be taken, in which case nothing changed and
 * the call should be retried (the spectrum pipeline retries every frame
 * while display_progress_active() is true).
 */
bool display_progress_finish(void);

/* True while the startup checklist is on screen. */
bool display_progress_active(void);

/*
 * Deliberate shutdown: replace the status view with a "stopped" screen so the
 * frame left on the panel does not read as a fault. Called once, after the
 * USB teardown.
 */
void display_show_stopped(void);

/*
 * Initialise SPI bus, ST7789P3 panel and LVGL.
 * Must be called once before any other display function.
 */
void display_init(void);

/*
 * Show the disconnected view. The stage rail is reset to pending and shown
 * only when no spectrum canvas is still on screen; a late disconnect waits
 * for display_spectrum_detach() to uncover it. Safe from any task; safe to
 * call repeatedly.
 */
void display_show_disconnected(void);

/*
 * Re-render every visible status string in the active language. Called by the
 * settings layer when the language changes.
 */
void display_language_changed(void);

/*
 * Switch to spectrum mode: hide the status view, create a full-screen
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
