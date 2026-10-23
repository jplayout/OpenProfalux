#!/usr/bin/env bash
# Build only: no serial access, flash or NVS erase.
set -euo pipefail
cd "$(dirname "$0")/.."
idf.py -B build-usb-debug -DIDF_TARGET=esp32c6 \
    -DSDKCONFIG="$PWD/build-usb-debug/sdkconfig" \
    -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.usb-debug.defaults" \
    reconfigure build
python3 - <<'CHECK'
from pathlib import Path
config = Path('build-usb-debug/sdkconfig').read_text()
for setting in ('CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y', 'CONFIG_OPF_ZB_USB_DIAGNOSTIC=y',
                'CONFIG_OPF_ZB_POLL_INTERVAL_MS=1000', 'CONFIG_ZB_ZED=y'):
    assert setting in config, setting
assert 'CONFIG_ESP_CONSOLE_UART_DEFAULT=y' not in config
print('PASS: USB diagnostic configuration, ZED, 1000 ms')
CHECK
