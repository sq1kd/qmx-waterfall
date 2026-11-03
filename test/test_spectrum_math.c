/* Host test for the marker placement and the axis overlay.
 *
 * The marker is the only thing that tells the operator where the radio is
 * listening, and the axis glyphs sit at both panel edges where an off-by-one
 * writes outside the spectrum strip.
 */
#include <stdint.h>
#include <stdio.h>

#include "spectrum/axis.h"
#include "spectrum/freq_map.h"
#include "spectrum/layout.h"

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

enum
{
    FFT = 1024,
    SAMPLE = 1024000,
    HZ_PER_BIN = SAMPLE / FFT,
    GUARD_ROWS = 2,
};

static void test_cw_shift(void)
{
    CHECK(spectrum_mode_cw_shift_hz(3, 700) == 700,
          "CW must shift the marker up by the pitch");
    CHECK(spectrum_mode_cw_shift_hz(7, 700) == -700,
          "CW-R must shift the marker down by the pitch");
    CHECK(spectrum_mode_cw_shift_hz(1, 700) == 0, "USB has no pitch shift");
    CHECK(spectrum_mode_cw_shift_hz(2, 700) == 0, "LSB has no pitch shift");
    CHECK(spectrum_mode_cw_shift_hz(0, 700) == 0, "an unknown mode has no pitch shift");
}

static void test_marker(void)
{
    const uint32_t mid = 255 * (uint32_t)HZ_PER_BIN + HZ_PER_BIN / 2;
    const uint32_t high = (uint32_t)FFT * HZ_PER_BIN;

    CHECK(spectrum_compute_marker_px(0, 1, 0, 0, 0, 0, FFT, SAMPLE, DISP_W) == -1,
          "no marker without an IF frequency");
    CHECK(spectrum_compute_marker_px(mid, 1, 0, 0, 0, 0, FFT, 0, DISP_W) == -1,
          "no marker without a sample rate");
    CHECK(spectrum_compute_marker_px(mid, 1, 0, 0, 0, 0, FFT, SAMPLE, 1) == -1,
          "no marker on a one-pixel axis");
    CHECK(spectrum_compute_marker_px(mid, 1, 0, 0, 0, 0, FFT, SAMPLE, 0) == -1,
          "no marker on an empty axis");
    CHECK(spectrum_compute_marker_px(mid, 1, 0, 0, 0, 0, 1, SAMPLE, DISP_W) == -1,
          "no marker without at least two bins");

    int base = spectrum_compute_marker_px(mid, 1, 0, 0, 0, 1, FFT, SAMPLE, DISP_W);
    CHECK(base == DISP_W / 2, "the centre of the positive half must map to the middle, got %d",
          base);

    int cw = spectrum_compute_marker_px(mid, 3, 4000, 0, 0, 1, FFT, SAMPLE, DISP_W);
    int cwr = spectrum_compute_marker_px(mid, 7, 4000, 0, 0, 1, FFT, SAMPLE, DISP_W);
    CHECK(cw > base, "CW must move the marker towards higher frequencies (%d vs %d)", cw, base);
    CHECK(cwr < base, "CW-R must move the marker towards lower frequencies (%d vs %d)", cwr,
          base);

    int rit = spectrum_compute_marker_px(mid, 1, 0, 4000, 1, 1, FFT, SAMPLE, DISP_W);
    int rit_off = spectrum_compute_marker_px(mid, 1, 0, 4000, 0, 1, FFT, SAMPLE, DISP_W);
    CHECK(rit > base, "RIT on must move the marker (%d vs %d)", rit, base);
    CHECK(rit_off == base, "RIT off must ignore the stored offset (%d vs %d)", rit_off, base);

    CHECK(spectrum_compute_marker_px(high, 1, 0, 0, 0, 0, FFT, SAMPLE, DISP_W) == DISP_W - 1,
          "a marker above the span must clamp to the right edge");
    CHECK(spectrum_compute_marker_px(mid, 1, 0, -(int32_t)high, 1, 0, FFT, SAMPLE, DISP_W) ==
              0,
          "a marker below the span must clamp to the left edge");
    CHECK(spectrum_compute_marker_px(mid, 1, 0, -(int32_t)high, 1, 1, FFT, SAMPLE, DISP_W) ==
              0,
          "a marker below the positive half must clamp to the left edge");
}

enum
{
    SENTINEL = 0xBEEF,
    AXIS_COLOR = 0x0001,
    LABEL_COLOR = 0x0002,
};

static uint16_t s_canvas[DISP_W * (SPEC_H + GUARD_ROWS)];

static void clear_canvas(void)
{
    for (size_t i = 0; i < sizeof(s_canvas) / sizeof(s_canvas[0]); i++)
        s_canvas[i] = SENTINEL;
}

static int label_at(int x, int spec_h)
{
    for (int y = 0; y < spec_h - 1; y++)
    {
        if (s_canvas[y * DISP_W + x] == LABEL_COLOR)
            return 1;
    }
    return 0;
}

static void draw_axis(uint8_t view_mode, int spec_h)
{
    clear_canvas();
    spectrum_draw_axis_overlay(s_canvas, DISP_W, spec_h, 12000, view_mode, FFT, 48000, true,
                               AXIS_COLOR, LABEL_COLOR);

    for (int y = spec_h; y < spec_h + GUARD_ROWS; y++)
        for (int x = 0; x < DISP_W; x++)
            CHECK(s_canvas[y * DISP_W + x] == SENTINEL,
                  "view %u h %d: axis wrote below the strip at (%d,%d)", view_mode, spec_h, x,
                  y);
    for (int x = 0; x < DISP_W; x++)
        CHECK(s_canvas[(spec_h - 1) * DISP_W + x] == AXIS_COLOR,
              "view %u h %d: baseline missing at x=%d", view_mode, spec_h, x);

    CHECK(label_at(0, spec_h), "view %u h %d: no glyph at the left edge", view_mode, spec_h);
    CHECK(label_at(DISP_W - 1, spec_h), "view %u h %d: no glyph at the right edge", view_mode,
          spec_h);
}

static void test_axis(void)
{
    draw_axis(0, SPEC_H_MIN);
    draw_axis(1, SPEC_H_MIN);
    draw_axis(0, SPEC_H);
    draw_axis(1, SPEC_H);

    clear_canvas();
    spectrum_draw_axis_overlay(s_canvas, DISP_W, SPEC_H, 0, 0, FFT, 48000, false, AXIS_COLOR,
                               LABEL_COLOR);
    for (size_t i = 0; i < sizeof(s_canvas) / sizeof(s_canvas[0]); i++)
        CHECK(s_canvas[i] == SENTINEL, "a disabled axis must not draw");

    clear_canvas();
    spectrum_draw_axis_overlay(s_canvas, 1, SPEC_H, 0, 0, FFT, 48000, true, AXIS_COLOR,
                               LABEL_COLOR);
    for (size_t i = 0; i < sizeof(s_canvas) / sizeof(s_canvas[0]); i++)
        CHECK(s_canvas[i] == SENTINEL, "a one-pixel axis must not draw");

    clear_canvas();
    spectrum_draw_axis_overlay(s_canvas, DISP_W, 7, 0, 0, FFT, 48000, true, AXIS_COLOR,
                               LABEL_COLOR);
    for (size_t i = 0; i < sizeof(s_canvas) / sizeof(s_canvas[0]); i++)
        CHECK(s_canvas[i] == SENTINEL, "a strip too short for a glyph must not draw");

    clear_canvas();
    spectrum_draw_axis_overlay(s_canvas, DISP_W, SPEC_H, 0, 0, 1, 48000, true, AXIS_COLOR,
                               LABEL_COLOR);
    for (size_t i = 0; i < sizeof(s_canvas) / sizeof(s_canvas[0]); i++)
        CHECK(s_canvas[i] == SENTINEL, "a single-bin axis must not draw");

    spectrum_draw_axis_overlay(NULL, DISP_W, SPEC_H, 0, 0, FFT, 48000, true, AXIS_COLOR,
                               LABEL_COLOR);
}

int main(void)
{
    test_cw_shift();
    test_marker();
    test_axis();

    if (s_failures)
    {
        printf("%d check(s) failed\n", s_failures);
        return 1;
    }
    printf("spectrum math tests passed\n");
    return 0;
}
