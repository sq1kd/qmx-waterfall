#!/usr/bin/env bash
# Apply the patches in patches/ to the vendored esp-usb submodule.
#
# The submodule stays pinned to a commit that exists upstream, so fresh clones
# can fetch it; local fixes ride on top as tracked patch files. Re-running is
# safe and a no-op once a patch is applied.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SUB_DIR="$ROOT_DIR/vendor/esp-usb"

if ! git -C "$SUB_DIR" rev-parse --git-dir >/dev/null 2>&1; then
    echo "vendor/esp-usb is not a git checkout; run 'mise bootstrap' first" >&2
    exit 1
fi

shopt -s nullglob
for patch in "$ROOT_DIR"/patches/*.patch; do
    name="$(basename "$patch")"
    if git -C "$SUB_DIR" apply --check "$patch" >/dev/null 2>&1; then
        git -C "$SUB_DIR" apply "$patch"
        echo "applied $name"
    elif git -C "$SUB_DIR" apply --reverse --check "$patch" >/dev/null 2>&1; then
        echo "$name already applied"
    else
        echo "cannot apply $name to vendor/esp-usb (submodule changed? update the patch)" >&2
        exit 1
    fi
done
