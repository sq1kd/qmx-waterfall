#!/bin/sh
# Host-side checks for the spectrum drawing math and the UI string table
# (no ESP-IDF, no LVGL).
set -eu

root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cc=${CC:-cc}
render_out="${TMPDIR:-/tmp}/qmx-render-test"
i18n_out="${TMPDIR:-/tmp}/qmx-i18n-test"

"$cc" -std=c11 -O2 -Wall -Wextra -Werror -I"$root/main" \
    "$root/test/test_render.c" \
    "$root/main/spectrum/render.c" \
    "$root/main/spectrum/palette.c" \
    -o "$render_out" -lm

"$render_out"

"$cc" -std=c11 -O2 -Wall -Wextra -Werror -I"$root/main" \
    "$root/test/test_i18n.c" \
    "$root/main/ui/i18n.c" \
    -o "$i18n_out"

"$i18n_out"
