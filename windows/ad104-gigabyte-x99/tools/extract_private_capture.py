#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Extract matching private test inputs; no firmware execution or GPU access."""
import argparse
import hashlib
import json
from pathlib import Path

PROM = 'c81eb48fc6cb4f320d302e98f88db7101dfc1e4c3e21de82f57a6c8233ad3fb4'
CODE = {
    '3ff0f364e3ce0ddd1a1c28b9d66abf49822451fb126e4cbec59daf6be9ed75b8': (307, 19872),
    '1bb42730366614ba5207025300e0b75be2283f7632555569f18cdcc5a0456083': (312, 7280),
    '77632902f0ceb0ba2848200f396ab61eacd7910e9cce7c535f65d6cdbe2b6c32': (393, 137711),
}

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('capture', type=Path)
    p.add_argument('--out', required=True, type=Path, help='New private destination')
    a = p.parse_args()
    if a.capture.stat().st_size > 16 * 1024 * 1024:
        p.error('Capture exceeds the bounded schema-4 input size')
    j = json.loads(a.capture.read_text(encoding='utf-8-sig'))
    if j.get('schema') != 4 or not j['physical_prom'].get('complete'):
        p.error('Requires a complete schema-4 PROM capture')
    rom = bytes.fromhex(j['physical_prom']['hex'])
    if len(rom) != 1048576 or hashlib.sha256(rom).hexdigest() != PROM:
        p.error('ROM differs from the reviewed profile; do not bypass admission')
    files = {'ad104-physical-prom.bin': rom}
    for agent in j['platform']['gpu_agents']:
        code = agent['code']
        data = bytes.fromhex(code['hex'])
        digest = hashlib.sha256(data).hexdigest()
        if digest not in CODE or digest != code['loaded_sha256']:
            p.error('Loaded driver code differs from the reviewed profile')
        label, size = CODE[digest]
        name = f'driver-{label}.bin'  # Stable fixture labels, not current handles.
        if len(data) != size or name in files:
            p.error('Duplicate or truncated code snapshot')
        files[name] = data
    if len(files) != 4:
        p.error('All three reviewed driver profiles are required')
    a.out.mkdir(parents=True, exist_ok=False)
    for name, data in files.items():
        (a.out / name).write_bytes(data)
    print('Extracted 4 private files. Keep them out of Git and public uploads.')

if __name__ == '__main__':
    main()
