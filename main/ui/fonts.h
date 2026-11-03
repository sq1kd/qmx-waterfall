#pragma once

#include "lvgl.h"

/* Montserrat 10/12 px with Latin Extended-A, so the Polish diacritics
 * (ą ć ę ł ń ó ś ź ż) render on the 76 px panel. Generated with lv_font_conv:
 *
 *   lv_font_conv --no-compress --bpp 4 --size 10 \
 *     --font Montserrat-Medium.ttf -r 0x20-0x7F -r 0xA0-0x17F \
 *     --format lvgl --lv-include lvgl.h --force-fast-kern-format \
 *     -o main/ui/fonts/lv_font_qmx_10.c
 *
 * (and the same with --size 12 for lv_font_qmx_12.c). */
LV_FONT_DECLARE(lv_font_qmx_10);
LV_FONT_DECLARE(lv_font_qmx_12);
