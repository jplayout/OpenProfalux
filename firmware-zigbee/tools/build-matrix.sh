#!/usr/bin/env bash
# Run inside the official IDF 5.5.4 image. Build only, no serial device access.
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build-validation validation
for interval in 1000 2000 5000; do
    printf 'CONFIG_OPF_ZB_POLL_INTERVAL_MS=%s\n' "$interval" > build-validation/poll.defaults
    idf.py -B build-validation/build -DIDF_TARGET=esp32c6 \
        -DSDKCONFIG="$PWD/build-validation/sdkconfig-$interval" \
        -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;build-validation/poll.defaults" \
        reconfigure build > "build-validation/build-$interval.log" 2>&1
    cp build-validation/build/profalux_zigbee_sed.bin "build-validation/profalux-$interval.bin"
    cp "build-validation/sdkconfig-$interval" "build-validation/config-$interval.txt"
    tail -18 "build-validation/build-$interval.log"
done
python3 - <<'PY'
from pathlib import Path
import hashlib,json
results=[]
for ms in (1000,2000,5000):
    data=Path(f'build-validation/profalux-{ms}.bin').read_bytes()
    config=Path(f'build-validation/config-{ms}.txt').read_text()
    for expected in ('CONFIG_ZB_ZED=y','CONFIG_PM_ENABLE=y',
                     'CONFIG_FREERTOS_USE_TICKLESS_IDLE=y', f'CONFIG_OPF_ZB_POLL_INTERVAL_MS={ms}'):
        assert expected in config, expected
    results.append(dict(poll_ms=ms,bytes=len(data),sha256=hashlib.sha256(data).hexdigest(),result='PASS'))
Path('validation/build-results.json').write_text(json.dumps(dict(idf='5.5.4',zigbee='2.0.5',target='esp32c6',builds=results),indent=2)+'\n')
PY
