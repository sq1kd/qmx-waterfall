#pragma once

/*
 * Single source for the logical screen size (panel rotated 90° CW).
 * The spectrum renderer, the display chrome and the canvas all derive their
 * geometry from here; the physical panel size is pinned in display.c.
 */
#define QMX_SCREEN_W 284
#define QMX_SCREEN_H 76
