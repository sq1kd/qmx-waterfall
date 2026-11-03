#!/usr/bin/env bash
#MISE description="Run idf.py for the selected board (QMX_BOARD): mise run idf -- <idf.py args>"
#MISE depends=["vendor-patches"]
# idf.py inside the project-local ESP-IDF, for one board profile.
#
# boards/<id>/sdkconfig.defaults is layered on top of the project defaults and
# CONFIG_IDF_TARGET in it selects the chip. Each board gets its own build
# directory and generated sdkconfig under build/<id>/; the project-root
# sdkconfig is never read.
#
# IDF_PATH and IDF_PYTHON_ENV_PATH come from mise.toml, so an activated
# ESP-IDF shell cannot change what the tasks use.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

board=${QMX_BOARD:?QMX_BOARD is not set; run through mise}
profile="boards/$board/sdkconfig.defaults"
if [ ! -f "$profile" ]; then
    echo "unknown board '$board' (have: $(ls boards | paste -sd' ' -))" >&2
    exit 2
fi
target=$(sed -n 's/^CONFIG_IDF_TARGET="\(.*\)"$/\1/p' "$profile")
if [ -z "$target" ]; then
    echo "$profile does not set CONFIG_IDF_TARGET" >&2
    exit 2
fi

if [ ! -f "$IDF_PATH/export.sh" ] || [ ! -x "$IDF_PYTHON_ENV_PATH/bin/python" ]; then
    echo "ESP-IDF is not provisioned under .idf/; run 'mise bootstrap'." >&2
    exit 1
fi

# export.sh prints a progress banner on every call; keep it for failures only.
activation_log="$(mktemp)"
if ! source "$IDF_PATH/export.sh" >"$activation_log" 2>&1; then
    cat "$activation_log" >&2
    rm -f "$activation_log"
    echo "ESP-IDF activation failed for $IDF_PATH; run 'mise bootstrap'." >&2
    exit 1
fi
rm -f "$activation_log"

build_dir="$root/build/$board"
mkdir -p "$build_dir"
export IDF_TARGET="$target"
exec idf.py \
    -B "$build_dir" \
    -D "SDKCONFIG=$build_dir/sdkconfig" \
    -D "SDKCONFIG_DEFAULTS=$root/sdkconfig.defaults;$root/$profile" \
    "$@"
