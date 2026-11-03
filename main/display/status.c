/* Status view: brand header, status line and the scrolling stage rail. */

#include "display/display_priv.h"

#include <stdio.h>
#include <string.h>

#include "ui/fonts.h"
#include "ui/i18n.h"
#include "ui/theme.h"

/* Status layout on the 284×76 logical screen. The header rule sits 4 px
 * above the body block; the body gap is 3 px so the 17 px title line box and
 * the 13 px detail line box fit between the rule and the rail without
 * overlapping (76 = 18 rule + 3 gap + 17 title + 13 detail + 21 rail + 4). */
enum
{
    SCREEN_PAD = 4,
    TITLE_LINE_H = 17, /* lv_font_qmx_12 line height */
    SMALL_LINE_H = 13, /* lv_font_qmx_10 line height */
    HDR_X = SCREEN_PAD,
    HDR_Y = 0,
    RULE_Y = 17,
    RULE_H = 1,
    HEADER_GAP = 3,
    BODY_Y = RULE_Y + RULE_H + HEADER_GAP,
    ICON_X = 5,
    ICON_Y = BODY_Y + 1,
    ICON_SIZE = 22,
    STATUS_X = 33,
    TITLE_Y = BODY_Y,
    DETAIL_Y = TITLE_Y + TITLE_LINE_H,
    RAIL_H = 21,
    RAIL_Y = DISP_LOGICAL_H - RAIL_H - SCREEN_PAD,
    CHIP_W = 66,
    CHIP_H = 17,
    CHIP_GAP = 3,
    CHIP_DOT = 5,
    CHIP_DOT_X = 6,
    CHIP_LABEL_X = 12,
    CHIP_LABEL_W = CHIP_W - CHIP_LABEL_X - 3,
    /* Three chips (204 px) fit the 284 px rail, so the last stage is
     * visible well before it becomes active. */
    RAIL_LOOKAHEAD = 2,
    DISP_STAGE_DETAIL_LEN = 24,
};

_Static_assert(DETAIL_Y == TITLE_Y + TITLE_LINE_H,
               "detail line must start below the title line box");
_Static_assert(RULE_Y + RULE_H <= BODY_Y, "header rule overlaps the status body");
_Static_assert(TITLE_Y + TITLE_LINE_H <= RAIL_Y, "title overlaps the stage rail");
_Static_assert(DETAIL_Y + SMALL_LINE_H <= RAIL_Y, "detail overlaps the stage rail");
_Static_assert(ICON_Y + ICON_SIZE <= RAIL_Y, "status icon overlaps the stage rail");
_Static_assert(RAIL_Y + RAIL_H <= DISP_LOGICAL_H, "stage rail leaves the screen");
_Static_assert(CHIP_H + 4 <= RAIL_H, "stage chip does not fit the rail");

typedef enum
{
    ICON_WAIT = 0,
    ICON_CONNECT,
    ICON_ERROR,
} icon_state_t;

static lv_obj_t *s_brand = NULL;
static lv_obj_t *s_credit = NULL;
static lv_obj_t *s_rule = NULL;
static lv_obj_t *s_icon = NULL;
static lv_obj_t *s_icon_symbol = NULL;
static lv_obj_t *s_icon_badge = NULL;
static lv_obj_t *s_title = NULL;
static lv_obj_t *s_detail = NULL;

/* One horizontal, auto-scrolling rail of stage chips. */
static lv_obj_t *s_rail = NULL;
static lv_obj_t *s_chip[DISP_STAGE_COUNT];
static lv_obj_t *s_chip_dot[DISP_STAGE_COUNT];
static lv_obj_t *s_chip_label[DISP_STAGE_COUNT];
static char s_stage_detail_buf[DISP_STAGE_COUNT][DISP_STAGE_DETAIL_LEN];
static disp_stage_state_t s_stage_state[DISP_STAGE_COUNT];

void display_status_set_visible_locked(bool visible)
{
    if (s_brand)
        lv_obj_set_hidden(s_brand, !visible);
    if (s_credit)
        lv_obj_set_hidden(s_credit, !visible);
    if (s_rule)
        lv_obj_set_hidden(s_rule, !visible);
    if (s_icon)
        lv_obj_set_hidden(s_icon, !visible);
    if (s_title)
        lv_obj_set_hidden(s_title, !visible);
    if (s_detail)
        lv_obj_set_hidden(s_detail, !visible);
}

static void icon_set_state_locked(icon_state_t state)
{
    if (!s_icon || !s_icon_symbol || !s_icon_badge)
        return;

    uint32_t color = UI_MUTED;
    if (state == ICON_CONNECT)
        color = UI_ACCENT;
    else if (state == ICON_ERROR)
        color = UI_ERROR;

    lv_obj_set_style_border_color(s_icon, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_style_text_color(s_icon_symbol, lv_color_hex(color), LV_PART_MAIN);
    lv_obj_set_hidden(s_icon_badge, state != ICON_ERROR);
}

static void chip_apply_locked(disp_stage_t stage)
{
    if ((int)stage < 0 || stage >= (int)DISP_STAGE_COUNT || s_chip[stage] == NULL)
        return;

    uint32_t border = UI_CHIP_PENDING_BORDER;
    uint32_t bg = UI_CHIP_PENDING_BG;
    uint32_t fg = UI_CHIP_PENDING_FG;
    uint32_t dot = UI_CHIP_PENDING_DOT;

    switch (s_stage_state[stage])
    {
    case DISP_STAGE_ACTIVE:
        border = UI_ACCENT;
        bg = UI_CHIP_ACTIVE_BG;
        fg = UI_TEXT;
        dot = UI_ACCENT;
        break;
    case DISP_STAGE_DONE:
        border = UI_CHIP_DONE_BORDER;
        bg = UI_CHIP_DONE_BG;
        fg = UI_CHIP_DONE_FG;
        dot = UI_SUCCESS;
        break;
    case DISP_STAGE_FAILED:
        border = UI_CHIP_FAILED_BORDER;
        bg = UI_CHIP_FAILED_BG;
        fg = UI_CHIP_FAILED_FG;
        dot = UI_ERROR;
        break;
    default:
        break;
    }

    lv_obj_set_style_border_color(s_chip[stage], lv_color_hex(border), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_chip[stage], lv_color_hex(bg), LV_PART_MAIN);
    lv_obj_set_style_text_color(s_chip_label[stage], lv_color_hex(fg), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_chip_dot[stage], lv_color_hex(dot), LV_PART_MAIN);
}

static int stage_active_latest(void)
{
    for (int i = (int)DISP_STAGE_COUNT - 1; i >= 0; i--)
    {
        if (s_stage_state[i] == DISP_STAGE_ACTIVE)
            return i;
    }
    return -1;
}

static int stage_failed_first(void)
{
    for (int i = 0; i < (int)DISP_STAGE_COUNT; i++)
    {
        if (s_stage_state[i] == DISP_STAGE_FAILED)
            return i;
    }
    return -1;
}

static ui_text_t stage_text_key(disp_stage_t stage)
{
    switch (stage)
    {
    case DISP_STAGE_USB:
        return TXT_STAGE_USB;
    case DISP_STAGE_AUDIO:
        return TXT_STAGE_AUDIO;
    case DISP_STAGE_CAT:
        return TXT_STAGE_CONTROL;
    case DISP_STAGE_RADIO:
        return TXT_STAGE_RADIO;
    case DISP_STAGE_IQ:
        return TXT_STAGE_IQ;
    default:
        return TXT_STAGE_SPECTRUM;
    }
}

/* "<stage> · <technical detail>" for the status line. */
static void compose_stage_detail(int stage, char *buf, size_t n)
{
    if (stage < 0 || stage >= (int)DISP_STAGE_COUNT)
    {
        (void)snprintf(buf, n, "%s", ui_tr(TXT_CONNECT_DETAIL));
        return;
    }

    const char *stage_name = ui_tr(stage_text_key((disp_stage_t)stage));
    const char *detail = s_stage_detail_buf[stage];
    if (detail[0] != '\0')
        (void)snprintf(buf, n, "%s · %s", stage_name, detail);
    else
        (void)snprintf(buf, n, "%s", stage_name);
}

static void refresh_status_text_locked(display_view_t view)
{
    if (!s_title || !s_detail || view == VIEW_SPECTRUM)
        return;

    char detail[64];

    switch (view)
    {
    case VIEW_BOOT:
        lv_label_set_text(s_title, ui_tr(TXT_WAITING));
        lv_label_set_text(s_detail, ui_tr(TXT_PLUG_IN));
        icon_set_state_locked(ICON_WAIT);
        break;

    case VIEW_PROGRESS:
    {
        const int failed = stage_failed_first();
        if (failed >= 0)
        {
            lv_label_set_text(s_title, ui_tr(TXT_PROBLEM));
            compose_stage_detail(failed, detail, sizeof(detail));
            icon_set_state_locked(ICON_ERROR);
        }
        else
        {
            lv_label_set_text(s_title, ui_tr(TXT_CONNECTING));
            compose_stage_detail(stage_active_latest(), detail, sizeof(detail));
            icon_set_state_locked(ICON_CONNECT);
        }
        lv_label_set_text(s_detail, detail);
        break;
    }

    case VIEW_DISCONNECTED:
        lv_label_set_text(s_title, ui_tr(TXT_DISCONNECTED));
        lv_label_set_text(s_detail, ui_tr(TXT_RECONNECT));
        icon_set_state_locked(ICON_ERROR);
        break;

    case VIEW_STOPPED:
        lv_label_set_text(s_title, ui_tr(TXT_STOPPED));
        lv_label_set_text(s_detail, ui_tr(TXT_STOPPED_DETAIL));
        icon_set_state_locked(ICON_WAIT);
        break;

    default:
        break;
    }
}

void display_status_refresh_locked(display_view_t view)
{
    for (int i = 0; i < (int)DISP_STAGE_COUNT; i++)
    {
        if (s_chip_label[i])
            lv_label_set_text(s_chip_label[i], ui_tr(stage_text_key((disp_stage_t)i)));
    }
    refresh_status_text_locked(view);
}

void display_status_rail_reset_locked(void)
{
    for (int i = 0; i < (int)DISP_STAGE_COUNT; i++)
    {
        s_stage_detail_buf[i][0] = '\0';
        s_stage_state[i] = DISP_STAGE_PENDING;
        chip_apply_locked((disp_stage_t)i);
    }
    if (s_rail)
    {
        lv_obj_set_style_opa(s_rail, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_scroll_to_x(s_rail, 0, LV_ANIM_OFF);
    }
}

/* Keep the active chip and the two after it on screen. The last stages arrive
 * in a burst, so reveal them while an earlier stage is still active instead of
 * chasing each chip as it flips. */
static void rail_reveal_stage_locked(int stage)
{
    if (!s_rail || stage < 0 || stage >= (int)DISP_STAGE_COUNT)
        return;

    int last = stage + RAIL_LOOKAHEAD;
    if (last >= (int)DISP_STAGE_COUNT)
        last = (int)DISP_STAGE_COUNT - 1;

    lv_obj_update_layout(s_rail);

    lv_area_t first_a;
    lv_area_t last_a;
    lv_area_t rail_a;
    lv_obj_get_coords(s_chip[stage], &first_a);
    lv_obj_get_coords(s_chip[last], &last_a);
    lv_obj_get_coords(s_rail, &rail_a);

    const int32_t margin = 4;
    int32_t scroll_x = lv_obj_get_scroll_x(s_rail);

    if (last_a.x2 > rail_a.x2 - margin)
        scroll_x += last_a.x2 - (rail_a.x2 - margin);
    if (first_a.x1 < rail_a.x1 + margin)
        scroll_x -= (rail_a.x1 + margin) - first_a.x1;

    lv_obj_scroll_to_x(s_rail, scroll_x, LV_ANIM_ON);
}

void display_status_stage_update_locked(display_view_t view, disp_stage_t stage,
                                        disp_stage_state_t state,
                                        const char *detail)
{
    if ((int)stage < 0 || stage >= (int)DISP_STAGE_COUNT || s_chip[stage] == NULL)
        return;

    s_stage_state[stage] = state;
    if (detail != NULL)
        strlcpy(s_stage_detail_buf[stage], detail, DISP_STAGE_DETAIL_LEN);

    chip_apply_locked(stage);
    refresh_status_text_locked(view);

    if (state == DISP_STAGE_ACTIVE || state == DISP_STAGE_FAILED)
        rail_reveal_stage_locked((int)stage);
}

void display_status_finish_locked(void)
{
    if (s_rail == NULL)
        return;

    if (s_stage_state[DISP_STAGE_SPECTRUM] != DISP_STAGE_FAILED)
    {
        s_stage_state[DISP_STAGE_SPECTRUM] = DISP_STAGE_DONE;
        strlcpy(s_stage_detail_buf[DISP_STAGE_SPECTRUM], ui_tr(TXT_DETAIL_RUNNING),
                DISP_STAGE_DETAIL_LEN);
        chip_apply_locked(DISP_STAGE_SPECTRUM);
    }
}

static void status_create_locked(lv_obj_t *scr)
{
    s_brand = lv_label_create(scr);
    lv_label_set_text(s_brand, ui_tr(TXT_BRAND));
    lv_obj_set_style_text_color(s_brand, lv_color_hex(UI_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_brand, &lv_font_qmx_12, LV_PART_MAIN);
    lv_obj_set_pos(s_brand, HDR_X, HDR_Y);

    s_credit = lv_label_create(scr);
    lv_label_set_text(s_credit, ui_tr(TXT_CREDIT));
    lv_obj_set_style_text_color(s_credit, lv_color_hex(UI_MUTED), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_credit, &lv_font_qmx_10, LV_PART_MAIN);
    lv_obj_align(s_credit, LV_ALIGN_TOP_RIGHT, -SCREEN_PAD, 2);

    s_rule = lv_obj_create(scr);
    lv_obj_remove_style_all(s_rule);
    lv_obj_set_size(s_rule, DISP_LOGICAL_W - (2 * SCREEN_PAD), RULE_H);
    lv_obj_set_pos(s_rule, SCREEN_PAD, RULE_Y);
    lv_obj_set_style_bg_color(s_rule, lv_color_hex(UI_BORDER), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_rule, LV_OPA_COVER, LV_PART_MAIN);

    s_icon = lv_obj_create(scr);
    lv_obj_remove_style_all(s_icon);
    lv_obj_set_size(s_icon, ICON_SIZE, ICON_SIZE);
    lv_obj_set_pos(s_icon, ICON_X, ICON_Y);
    lv_obj_set_style_radius(s_icon, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_icon, lv_color_hex(UI_SURFACE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_icon, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_icon, 1, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_icon, 0, LV_PART_MAIN);

    s_icon_symbol = lv_label_create(s_icon);
    lv_label_set_text(s_icon_symbol, LV_SYMBOL_USB);
    lv_obj_set_style_text_font(s_icon_symbol, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_center(s_icon_symbol);

    s_icon_badge = lv_obj_create(s_icon);
    lv_obj_remove_style_all(s_icon_badge);
    lv_obj_set_size(s_icon_badge, 8, 8);
    lv_obj_align(s_icon_badge, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_radius(s_icon_badge, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_icon_badge, lv_color_hex(UI_ERROR), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_icon_badge, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_icon_badge, 0, LV_PART_MAIN);

    s_title = lv_label_create(scr);
    lv_obj_set_pos(s_title, STATUS_X, TITLE_Y);
    lv_obj_set_width(s_title, DISP_LOGICAL_W - STATUS_X - 4);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(s_title, lv_color_hex(UI_TEXT), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_title, &lv_font_qmx_12, LV_PART_MAIN);

    s_detail = lv_label_create(scr);
    lv_obj_set_pos(s_detail, STATUS_X, DETAIL_Y);
    lv_obj_set_width(s_detail, DISP_LOGICAL_W - STATUS_X - 4);
    lv_label_set_long_mode(s_detail, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(s_detail, lv_color_hex(UI_MUTED), LV_PART_MAIN);
    lv_obj_set_style_text_font(s_detail, &lv_font_qmx_10, LV_PART_MAIN);
}

static void rail_create_locked(lv_obj_t *scr)
{
    s_rail = lv_obj_create(scr);
    lv_obj_remove_style_all(s_rail);
    lv_obj_set_size(s_rail, DISP_LOGICAL_W, RAIL_H);
    lv_obj_set_pos(s_rail, 0, RAIL_Y);
    lv_obj_set_style_bg_opa(s_rail, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_pad_left(s_rail, SCREEN_PAD, LV_PART_MAIN);
    lv_obj_set_style_pad_right(s_rail, SCREEN_PAD, LV_PART_MAIN);
    lv_obj_set_style_pad_top(s_rail, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(s_rail, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_column(s_rail, CHIP_GAP, LV_PART_MAIN);
    lv_obj_set_scroll_dir(s_rail, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(s_rail, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(s_rail, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_rail, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    for (int i = 0; i < (int)DISP_STAGE_COUNT; i++)
    {
        lv_obj_t *chip = lv_obj_create(s_rail);
        lv_obj_remove_style_all(chip);
        lv_obj_set_size(chip, CHIP_W, CHIP_H);
        lv_obj_set_style_radius(chip, 3, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(chip, 1, LV_PART_MAIN);
        lv_obj_set_style_pad_all(chip, 0, LV_PART_MAIN);
        s_chip[i] = chip;

        s_chip_dot[i] = lv_obj_create(chip);
        lv_obj_remove_style_all(s_chip_dot[i]);
        lv_obj_set_size(s_chip_dot[i], CHIP_DOT, CHIP_DOT);
        lv_obj_set_pos(s_chip_dot[i], CHIP_DOT_X, (CHIP_H - CHIP_DOT) / 2);
        lv_obj_set_style_radius(s_chip_dot[i], LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_chip_dot[i], LV_OPA_COVER, LV_PART_MAIN);

        s_chip_label[i] = lv_label_create(chip);
        lv_obj_set_pos(s_chip_label[i], CHIP_LABEL_X, 1);
        lv_obj_set_width(s_chip_label[i], CHIP_LABEL_W);
        lv_label_set_long_mode(s_chip_label[i], LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_font(s_chip_label[i], &lv_font_qmx_10, LV_PART_MAIN);
        lv_obj_set_style_text_color(s_chip_label[i], lv_color_hex(UI_CHIP_PENDING_FG),
                                    LV_PART_MAIN);
    }

    lv_obj_set_style_opa(s_rail, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_hidden(s_rail, true);
    display_status_rail_reset_locked();
}

void display_status_create_locked(lv_obj_t *scr)
{
    status_create_locked(scr);
    rail_create_locked(scr);
}

void display_status_rail_show_locked(bool visible)
{
    if (!s_rail)
        return;

    lv_obj_set_hidden(s_rail, !visible);
    if (visible)
        lv_obj_move_foreground(s_rail);
}
