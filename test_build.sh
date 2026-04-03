#!/usr/bin/env bash
source $HOME/.espressif/v6.0/esp-idf/export.sh || echo "Export failed, idf.py won't work"
idf.py build
