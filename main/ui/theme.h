#pragma once

/*
 * Shared RGB888 colours for the status screen and the settings overlay.
 * Graphite surfaces with one warm accent; state colours stay muted so the
 * chrome does not compete with the waterfall.
 */
#define UI_SURFACE 0x101418
#define UI_BORDER 0x2B333C
#define UI_TEXT 0xE4E7EB
#define UI_MUTED 0x8A94A0
#define UI_ACCENT 0xC8965A
#define UI_SUCCESS 0x6FA97C
#define UI_ERROR 0xB4544F

/* Startup stage chips. One triple per state; the dot shares the state colour
 * except while pending. */
#define UI_CHIP_PENDING_BORDER 0x242C35
#define UI_CHIP_PENDING_BG 0x0C1015
#define UI_CHIP_PENDING_FG 0x6D7784
#define UI_CHIP_PENDING_DOT 0x3A434E
#define UI_CHIP_ACTIVE_BG 0x1B1813
#define UI_CHIP_DONE_BORDER 0x35513F
#define UI_CHIP_DONE_BG 0x101A14
#define UI_CHIP_DONE_FG 0x9DB8A5
#define UI_CHIP_FAILED_BORDER 0x5A2F2D
#define UI_CHIP_FAILED_BG 0x1C1212
#define UI_CHIP_FAILED_FG 0xD08B86
