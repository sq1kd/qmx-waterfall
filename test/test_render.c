/* Host test for the spectrum drawing math.
 *
 * These checks pin the properties the waterfall depends on:
 *   1. one emitted line scrolls the image by exactly one pixel row and the
 *      top row is the line that was just emitted (no resampling, no
 *      fractional phase -> no shimmer while scrolling);
 *   2. ring wrap keeps the newest line at the top for any number of lines;
 *   3. level mapping clamps at floor/ceiling and lands mid-palette between;
 *   4. the palette range is applied at draw time (history follows a range
 *      change without being cleared);
 *   5. peak hold marks a row above the trace, and only when enabled.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "spectrum/layout.h"
#include "spectrum/palette.h"
#include "spectrum/render.h"

static int s_failures;

#define CHECK(cond, ...)                                \
    do                                                  \
    {                                                   \
        if (!(cond))                                    \
        {                                               \
            s_failures++;                               \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                        \
            printf("\n");                               \
        }                                               \
    } while (0)

static const uint16_t *pal(void)
{
    return spectrum_palette_get_lut(0); /* turbo */
}

static int colour_in_window(uint16_t colour, int lo, int hi)
{
    for (int i = lo; i <= hi; i++)
    {
        if (pal()[i] == colour)
            return 1;
    }
    return 0;
}

/* Render exactly one line through the public path, for comparison. */
static void render_single_line(uint16_t *out, int disp_w, const float *db,
                               float floor_db, float ceil_db)
{
    int16_t ring[8];
    uint32_t idx = 0;
    spectrum_store_wf_row(ring, &idx, 1, disp_w, db);
    spectrum_draw_waterfall(out, disp_w, 1, 0, ring, idx, 1, floor_db, ceil_db, 0);
}

static void test_scroll_is_one_row(void)
{
    enum { W = 4, H = 8 };
    int16_t ring[W * H];
    uint32_t idx = 0;
    uint16_t frame[W * H], prev[W * H], newest[W];
    const float floor_db = 0.0f, ceil_db = 100.0f;

    for (int i = 0; i < W * H; i++)
        ring[i] = spectrum_db_to_q8(floor_db);
    memset(prev, 0, sizeof(prev));

    uint32_t lcg = 7u;
    for (int line = 0; line < 200; line++)
    {
        float db[W];
        for (int x = 0; x < W; x++)
        {
            lcg = lcg * 1664525u + 1013904223u;
            float noise = (float)(lcg >> 24) * (20.0f / 255.0f);
            db[x] = ((line & 1) ? 70.0f : 20.0f) + noise;
        }

        spectrum_store_wf_row(ring, &idx, H, W, db);
        spectrum_draw_waterfall(frame, W, H, 0, ring, idx, H, floor_db, ceil_db, 0);
        render_single_line(newest, W, db, floor_db, ceil_db);

        if (line > 0)
        {
            for (int row = 1; row < H; row++)
            {
                for (int x = 0; x < W; x++)
                {
                    CHECK(frame[row * W + x] == prev[(row - 1) * W + x],
                          "line %d row %d col %d did not scroll exactly one row", line, row, x);
                }
            }
            for (int x = 0; x < W; x++)
            {
                CHECK(frame[x] == newest[x],
                      "line %d col %d top row is not the newest line", line, x);
            }
        }
        memcpy(prev, frame, sizeof(prev));
    }
}

static void test_level_mapping(void)
{
    enum { W = 2, H = 1 };
    int16_t ring[W * H];
    uint32_t idx = 0;
    uint16_t frame[W * H];
    float db[W];

    for (int i = 0; i < W * H; i++)
        ring[i] = spectrum_db_to_q8(-140.0f);

    /* At the floor: darkest palette entry. */
    db[0] = db[1] = -60.0f;
    spectrum_store_wf_row(ring, &idx, H, W, db);
    spectrum_draw_waterfall(frame, W, H, 0, ring, idx, H, -60.0f, 0.0f, 0);
    CHECK(frame[0] == pal()[0], "-60 dB at floor should be palette[0]");

    /* Middle of a -120..0 window: about halfway up the palette. */
    spectrum_draw_waterfall(frame, W, H, 0, ring, idx, H, -120.0f, 0.0f, 0);
    CHECK(colour_in_window(frame[0], 125, 129),
          "-60 dB mid-window should be around palette[127]");

    /* At or above the ceiling: brightest entry. */
    db[0] = 10.0f;
    spectrum_store_wf_row(ring, &idx, H, W, db);
    spectrum_draw_waterfall(frame, W, H, 0, ring, idx, H, -120.0f, 0.0f, 0);
    CHECK(frame[0] == pal()[255], "above ceiling should be palette[255]");

}

static void test_peak_marker(void)
{
    enum { W = 4, SH = 10 };
    uint16_t canvas[W * SH];
    float db[W] = {-80.0f, -80.0f, -80.0f, -80.0f};
    float peak[W] = {40.0f, 40.0f, 40.0f, 40.0f};
    const uint16_t bg = spectrum_rgb565(0, 0, 8);
    const uint16_t fg = spectrum_rgb565(0, 255, 0);
    const uint16_t pk = spectrum_rgb565(255, 224, 64);

    spectrum_draw_spectrum_strip_db(canvas, W, SH, db, peak, -120.0f, 0.0f, bg, fg, pk);
    CHECK(canvas[0 * W + 0] == pk, "peak at the ceiling should set row 0");
    CHECK(canvas[6 * W + 0] == fg, "-80 dB of -120..0 should draw the trace at row 6");

    spectrum_draw_spectrum_strip_db(canvas, W, SH, db, NULL, -120.0f, 0.0f, bg, fg, pk);
    for (int i = 0; i < W * SH; i++)
    {
        CHECK(canvas[i] != pk, "peak colour drawn while peak hold is off");
    }
}

/* With the spectrum off the strip shrinks to SPEC_H_MIN and the waterfall
 * takes the rest; peak-only draws a dot and no trace. */
static void test_modes_and_geometry(void)
{
    enum { W = 4, H = DISP_H, SH_MIN = SPEC_H_MIN, RING = WF_STORE_H };
    static uint16_t canvas[W * H];
    static int16_t ring[RING * W];
    uint32_t idx = 0;
    float line[W] = {-80.0f, -80.0f, -80.0f, -80.0f};
    const uint16_t bg = spectrum_rgb565(0, 0, 8);
    const uint16_t fg = spectrum_rgb565(0, 255, 0);
    const uint16_t pk = spectrum_rgb565(255, 224, 64);

    for (int i = 0; i < RING * W; i++)
        ring[i] = spectrum_db_to_q8(-140.0f);
    for (int n = 0; n < RING + 7; n++) /* wrap the ring */
        spectrum_store_wf_row(ring, &idx, RING, W, line);

    const int wf_h = H - SH_MIN;
    spectrum_draw_spectrum_strip_db(canvas, W, SH_MIN, NULL, NULL, -120.0f, 0.0f, bg, fg, pk);
    spectrum_draw_waterfall(canvas, W, wf_h, SH_MIN, ring, idx, RING, -120.0f, 0.0f, 0);

    for (int r = 0; r < SH_MIN; r++)
        for (int x = 0; x < W; x++)
            CHECK(canvas[r * W + x] == bg, "mode off: strip row %d is not empty", r);
    for (int r = SH_MIN; r < H; r++)
        for (int x = 0; x < W; x++)
            CHECK(canvas[r * W + x] != bg, "mode off: waterfall row %d was not drawn", r);

    float peak[W] = {40.0f, 40.0f, 40.0f, 40.0f};
    spectrum_draw_spectrum_strip_db(canvas, W, 10, NULL, peak, -120.0f, 0.0f, bg, fg, pk);
    CHECK(canvas[0] == pk, "peak-only: no peak dot at the ceiling");
    for (int i = 0; i < W * 10; i++)
        CHECK(canvas[i] != fg, "peak-only: trace was drawn without trace data");
}

int main(void)
{
    test_scroll_is_one_row();
    test_modes_and_geometry();
    test_level_mapping();
    test_peak_marker();

    if (s_failures)
    {
        printf("%d check(s) failed\n", s_failures);
        return 1;
    }
    printf("render tests passed\n");
    return 0;
}
