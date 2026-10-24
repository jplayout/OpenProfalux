#!/usr/bin/env python3
"""Run from firmware-zigbee inside IDF after the diagnostic + normal builds."""
from pathlib import Path
import hashlib,json,subprocess
rows=[]
for variant,directory,ms in [('usb-simulation','build-usb-debug',1000),
                            ('usb-radio-diagnostic','build-radio-diagnostic',1000),
                            ('normal-1000','build-validation/build',1000),
                            ('normal-2000','build-validation/build',2000),
                            ('normal-5000','build-validation/build',5000)]:
    build=Path(directory)
    normal=variant.startswith('normal')
    binary=Path(f'build-validation/profalux-{ms}.bin') if normal else build/'profalux_zigbee_sed.bin'
    config=Path(f'build-validation/config-{ms}.txt') if normal else build/'sdkconfig'
    cfg=config.read_text()
    assert 'CONFIG_ZB_ZED=y' in cfg
    assert f'CONFIG_OPF_ZB_POLL_INTERVAL_MS={ms}' in cfg
    radio=variant=='usb-radio-diagnostic'
    assert f'CONFIG_OPF_RADIO_MODE={1 if radio else 0}' in cfg
    assert ('CONFIG_OPF_ZB_USB_DIAGNOSTIC=y' in cfg) == (not normal)
    assert ('CONFIG_OPF_RADIO_CONSOLE=y' in cfg) == radio
    assert (build/'partition_table/partition-table.bin').read_bytes()==Path('build-usb-debug/partition_table/partition-table.bin').read_bytes()
    map_path=Path(f'build-validation/map-{ms}.map') if normal else build/'profalux_zigbee_sed.map'
    size=json.loads(subprocess.check_output(['python','-m','esp_idf_size','--format','json',str(map_path)]))
    data=binary.read_bytes()
    rows.append(dict(variant=variant,poll_ms=ms,path=str(binary),bytes=len(data),sha256=hashlib.sha256(data).hexdigest(),
                     static_ram=size['used_diram'],ram_data=size['diram_data'],ram_bss=size['diram_bss'],ram_code=size['diram_text'],
                     linker_ram_remain=size['diram_remain'],map_path=str(map_path),
                     flash_code=size['flash_code'],flash_rodata=size['flash_rodata'],result='PASS'))
Path('validation/step4-build-results.json').write_text(json.dumps(dict(idf='5.5.4',zigbee='2.0.5',target='esp32c6',builds=rows),indent=2)+'\n')
for row in rows: print(row['variant'],row['bytes'],row['static_ram'],row['linker_ram_remain'])
