#!/usr/bin/env bash
# Run a command inside the ESP-IDF environment for this project.
#
# ESP-IDF resolution order:
#   1. IDF_PATH from the environment (system install, CI image, ...)
#   2. .idf/esp-idf, provisioned by `mise run setup`
#
# Commands always run from the project root so idf.py picks up this project.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IDF_PATH="${IDF_PATH:-$ROOT_DIR/.idf/esp-idf}"

if [ ! -f "$IDF_PATH/export.sh" ]; then
    echo "ESP-IDF not found at $IDF_PATH" >&2
    echo "Run 'mise run setup' or set IDF_PATH to an ESP-IDF checkout." >&2
    exit 1
fi

if [ -n "${IDF_PYTHON_ENV_PATH:-}" ] && [ ! -x "$IDF_PYTHON_ENV_PATH/bin/python" ]; then
    echo "ESP-IDF Python environment not found at $IDF_PYTHON_ENV_PATH" >&2
    echo "Run 'mise run setup', or unset IDF_PYTHON_ENV_PATH to use the IDF default." >&2
    exit 1
fi

# export.sh prints a progress banner on every call; keep it for failures only.
activation_log="$(mktemp)"
if ! source "$IDF_PATH/export.sh" >"$activation_log" 2>&1; then
    cat "$activation_log" >&2
    rm -f "$activation_log"
    echo "ESP-IDF activation failed for $IDF_PATH; run 'mise run setup'." >&2
    exit 1
fi
rm -f "$activation_log"

cd "$ROOT_DIR"
exec "$@"
