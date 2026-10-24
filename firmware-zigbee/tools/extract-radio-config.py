#!/usr/bin/env python3
"""Reuse the historical register profile verbatim; never copy TX/learning code."""
import pathlib,re,sys
source=pathlib.Path(sys.argv[1]).read_text()
match=re.search(r'static const struct \{ uint8_t reg, val; \} s_regs\[\] = \{(.*?)\n\};',source,re.S)
if not match:
    raise SystemExit('historical CC1101 register table not found; review port')
defines=dict(re.findall(r'^#define (CC_\w+)\s+(0x[0-9A-Fa-f]+)\b',source,re.M))
rows=re.findall(r'\{(CC_\w+),\s*(0x[0-9A-Fa-f]+)\}',match[1])
if len(rows)!=27:
    raise SystemExit(f'unexpected profile length {len(rows)}; review port')
text='/* Generated from firmware/main/cc1101.c, s_regs. Do not edit. */\n'
text+='static const struct { uint8_t reg, value; } pfx_rx_profile[] = {\n'
text+=''.join(f'    {{{defines[name]}, {value}}}, /* {name} */\n' for name,value in rows)
text+='};\n'
pathlib.Path(sys.argv[2]).write_text(text)
