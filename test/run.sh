#!/bin/sh
# Host-side checks for the spectrum drawing math (no ESP-IDF, no LVGL).
set -eu

root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cc=${CC:-cc}
out="${TMPDIR:-/tmp}/qmx-render-test"

"$cc" -std=c11 -O2 -Wall -Wextra -Werror -I"$root/main" \
    "$root/test/test_render.c" \
    "$root/main/spectrum/render.c" \
    "$root/main/spectrum/palette.c" \
    -o "$out" -lm

"$out"
