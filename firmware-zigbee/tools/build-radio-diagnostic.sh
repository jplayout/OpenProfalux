#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
idf.py -B build-radio-diagnostic -DIDF_TARGET=esp32c6 \
    -DSDKCONFIG="$PWD/build-radio-diagnostic/sdkconfig" \
    -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.usb-debug.defaults;sdkconfig.radio-diagnostic.defaults" \
    reconfigure build
