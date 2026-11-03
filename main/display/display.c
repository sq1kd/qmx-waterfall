/* Public display API and the view state machine. */

#include "display/display.h"
#include "display/display_priv.h"

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

static const char *TAG = "display";

static lv_display_t *s_disp = NULL;
static lv_obj_t *s_canvas = NULL;

/* Read from the spectrum task without the port lock (progress_active). */
static volatile display_view_t s_view = VIEW_BOOT;
static volatile bool s_rail_pinned = false;

static void display_canvas_destroy_locked(void);

static void display_apply_view_locked(void)
{
    if (s_disp == NULL)
        return;

    display_status_set_visible_locked(s_view != VIEW_SPECTRUM);
    display_status_refresh_locked(s_view);

    bool show_rail;
    switch (s_view)
    {
    case VIEW_BOOT:
        /* Keep the checklist on the waiting screen too: the first class
         * driver callbacks arrive after enumeration, and without this the
         * rail would pop in with several stages already done. */
    case VIEW_PROGRESS:
        show_rail = true;
        break;
    case VIEW_SPECTRUM:
        show_rail = s_rail_pinned;
        break;
    case VIEW_STOPPED:
        show_rail = false;
        break;
    case VIEW_DISCONNECTED:
        /* A late disconnect can race a new spectrum start: while the canvas
         * still owns the screen the reset rail must stay hidden, until
         * display_spectrum_detach() deletes it. */
        show_rail = (s_canvas == NULL);
        break;
    default:
        show_rail = false;
        break;
    }

    display_status_rail_show_locked(show_rail);
}

void display_init(void)
{
    s_disp = display_panel_init();
    if (s_disp == NULL)
        return;

    if (lvgl_port_lock(0))
    {
        lv_obj_t *scr = lv_display_get_screen_active(s_disp);
        lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

        display_status_create_locked(scr);
        s_view = VIEW_BOOT;
        display_apply_view_locked();

        lvgl_port_unlock();
    }

    ESP_LOGI(TAG, "Display initialised (%d×%d)", DISP_LOGICAL_W, DISP_LOGICAL_H);
}

void display_language_changed(void)
{
    if (s_disp == NULL)
        return;

    if (!lvgl_port_lock(100))
        return;

    display_status_refresh_locked(s_view);

    lvgl_port_unlock();
}

static void display_canvas_destroy_locked(void)
{
    if (s_canvas)
    {
        lv_obj_del(s_canvas);
        s_canvas = NULL;
    }
}

bool display_spectrum_detach(void)
{
    if (!s_disp)
        return true;

    /* The pipeline is stopped by the caller, so the lock is normally free;
     * the timeout only guards against a wedged LVGL task, where handing the
     * caller a "still attached" answer beats deadlocking the teardown. */
    /* lvgl_port_lock() takes milliseconds (0 would mean "wait forever"). */
    if (!lvgl_port_lock(100))
        return false;

    display_canvas_destroy_locked();
    display_apply_view_locked();

    lvgl_port_unlock();
    return true;
}

lv_obj_t *display_show_spectrum(void)
{
    lv_obj_t *canvas = NULL;
    if (s_disp == NULL)
        return NULL;

    if (lvgl_port_lock(0))
    {
        lv_obj_t *scr = lv_display_get_screen_active(s_disp);

        /* A retried start must not stack canvases (the old one would keep a
         * pointer to a freed buffer). */
        display_canvas_destroy_locked();

        /* The rail only stays over the canvas while this connection is still
         * progressing; a stale disconnect/finish that raced the new start
         * must not pin it there. */
        s_rail_pinned = display_progress_active();
        s_view = VIEW_SPECTRUM;

        lv_obj_set_style_bg_color(scr, lv_color_black(), LV_PART_MAIN);

        /* Full-screen canvas — buffer is attached by spectrum_init() */
        canvas = lv_canvas_create(scr);
        lv_obj_set_size(canvas, DISP_LOGICAL_W, DISP_LOGICAL_H);
        lv_obj_align(canvas, LV_ALIGN_TOP_LEFT, 0, 0);
        s_canvas = canvas;

        display_apply_view_locked();

        lvgl_port_unlock();
    }
    return canvas;
}

void display_show_disconnected(void)
{
    if (s_disp == NULL)
        return;

    if (lvgl_port_lock(100))
    {
        /* Do not delete the canvas here: the CDC disconnect path calls this
         * while the spectrum task may still be rendering. display_spectrum_detach()
         * deletes it once the pipeline is stopped, which uncovers this view. */
        s_view = VIEW_DISCONNECTED;
        s_rail_pinned = false;

        /* A finished connection leaves no residual stage state. */
        display_status_rail_reset_locked();
        display_apply_view_locked();

        lvgl_port_unlock();
    }
}

void display_show_progress(void)
{
    if (s_view == VIEW_SPECTRUM && !s_rail_pinned)
        return;
    if (s_disp == NULL)
        return;

    if (!lvgl_port_lock(100))
        return;

    if (s_view == VIEW_BOOT || s_view == VIEW_DISCONNECTED)
        display_status_rail_reset_locked();

    s_view = VIEW_PROGRESS;
    s_rail_pinned = true;
    display_apply_view_locked();

    lvgl_port_unlock();
}

void display_stage(disp_stage_t stage, disp_stage_state_t state, const char *detail)
{
    if ((int)stage < 0 || stage >= (int)DISP_STAGE_COUNT)
        return;
    if (!lvgl_port_lock(100))
        return;

    if (display_progress_active())
        display_status_stage_update_locked(s_view, stage, state, detail);

    lvgl_port_unlock();
}

bool display_progress_finish(void)
{
    if (!lvgl_port_lock(100))
    {
        return false;
    }

    /* A disconnect can win the lock while the first frame is in flight.
     * Finishing then would hide the disconnected view and leave the screen
     * blank. The rail must not survive over the disconnected view either:
     * it would float over the next connection's canvas. */
    if (s_view == VIEW_DISCONNECTED)
    {
        display_status_rail_show_locked(false);
        s_rail_pinned = false;
        lvgl_port_unlock();
        return true;
    }

    display_status_finish_locked();
    s_view = VIEW_SPECTRUM;
    s_rail_pinned = false;
    display_apply_view_locked();

    lvgl_port_unlock();
    return true;
}

bool display_progress_active(void)
{
    return s_view == VIEW_PROGRESS || (s_view == VIEW_SPECTRUM && s_rail_pinned);
}

void display_show_stopped(void)
{
    if (s_disp == NULL)
        return;

    if (lvgl_port_lock(100))
    {
        s_view = VIEW_STOPPED;
        s_rail_pinned = false;
        display_apply_view_locked();
        lvgl_port_unlock();
        ESP_LOGI(TAG, "shutdown view set");
    }
}
