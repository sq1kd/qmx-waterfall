#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

echo "Project dependencies"
echo "===================="

if [ -d "$ROOT_DIR/vendor/esp-usb/.git" ]; then
  echo "vendored esp-usb: $(git -C "$ROOT_DIR/vendor/esp-usb" rev-parse --short HEAD)"
else
  echo "vendored esp-usb: not a git checkout"
fi

if [ -f "$ROOT_DIR/dependencies.lock" ]; then
  echo
  echo "Locked managed components:"
  grep -nE "name:|version:" "$ROOT_DIR/dependencies.lock" | sed -E 's/^/  /'
else
  echo "dependencies.lock not found"
fi
