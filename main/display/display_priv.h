#pragma once

#include <stdbool.h>

#include "display/display.h"
#include "lvgl.h"

/*
 * What the chrome shows. The spectrum canvas has its own lifetime: a
 * disconnect does not delete it because the pipeline may still be drawing,
 * so its presence is tracked separately from the view.
 */
typedef enum
{
    VIEW_BOOT = 0,
    VIEW_PROGRESS,
    VIEW_SPECTRUM,
    VIEW_DISCONNECTED,
    VIEW_STOPPED,
} display_view_t;

/* Bring up SPI, the ST7789 driver and the LVGL port; NULL if either fails. */
lv_display_t *display_panel_init(void);

/* status.c: every function below runs with the LVGL port lock held. */
void display_status_create_locked(lv_obj_t *scr);
void display_status_set_visible_locked(bool visible);
void display_status_refresh_locked(display_view_t view);
void display_status_rail_show_locked(bool visible);
void display_status_rail_reset_locked(void);
void display_status_stage_update_locked(display_view_t view, disp_stage_t stage,
                                        disp_stage_state_t state,
                                        const char *detail);
void display_status_finish_locked(void);
