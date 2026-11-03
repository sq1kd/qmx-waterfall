#!/bin/sh
# Host-side checks for the protocol parsers, the spectrum math and the UI
# string table (no ESP-IDF, no LVGL), built with ASan/UBSan.
set -eu

root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cc=${CC:-cc}
tmp="${TMPDIR:-/tmp}"

run_test()
{
    name=$1
    shift
    out="$tmp/qmx-$name-test"
    "$cc" -std=c11 -O2 -Wall -Wextra -Werror \
        -fsanitize=address,undefined -fno-omit-frame-pointer \
        -I"$root/main" "$@" -o "$out" -lm
    "$out"
}

run_test render \
    "$root/test/test_render.c" \
    "$root/main/spectrum/render.c" \
    "$root/main/spectrum/palette.c"

run_test i18n \
    "$root/test/test_i18n.c" \
    "$root/main/ui/i18n.c"

run_test cat_proto \
    "$root/test/test_cat_proto.c" \
    "$root/main/cat/proto.c"

run_test spectrum_math \
    "$root/test/test_spectrum_math.c" \
    "$root/main/spectrum/axis.c" \
    "$root/main/spectrum/freq_map.c"
