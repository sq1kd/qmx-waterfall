#!/usr/bin/env bash
# Run a command inside this project's ESP-IDF environment.
#
# ESP-IDF and its Python environment are project-local (.idf/esp-idf,
# .idf/venv), provisioned by `mise bootstrap`; ambient IDF_PATH settings are
# ignored so local runs and CI use the same tree.
#
# Commands always run from the project root so idf.py picks up this project.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IDF_PATH="$ROOT_DIR/.idf/esp-idf"
IDF_PYTHON_ENV_PATH="$ROOT_DIR/.idf/venv"
export IDF_PATH IDF_PYTHON_ENV_PATH

if [ ! -f "$IDF_PATH/export.sh" ]; then
    echo "ESP-IDF not found at $IDF_PATH" >&2
    echo "Run 'mise bootstrap' to provision ESP-IDF." >&2
    exit 1
fi

if [ -n "${IDF_PYTHON_ENV_PATH:-}" ] && [ ! -x "$IDF_PYTHON_ENV_PATH/bin/python" ]; then
    echo "ESP-IDF Python environment not found at $IDF_PYTHON_ENV_PATH" >&2
    echo "Run 'mise bootstrap' to provision the Python environment." >&2
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

cd "$ROOT_DIR"
exec "$@"
