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

# shellcheck disable=SC1091
source "$IDF_PATH/export.sh" >/dev/null

cd "$ROOT_DIR"
exec "$@"
