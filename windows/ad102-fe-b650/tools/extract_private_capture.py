#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Extract matching private test inputs; no firmware execution or GPU access."""
import argparse
import hashlib
import json
from pathlib import Path

PROM = 'c03931779344e3b73002f6f8da2870327ac24242a6a8c76f9a42f11f1ca9f780'
# Labels are the agent handle indexes in AD102 FE CAP0001 (tests/capture_graph.h).
CODE = {
    'd29cd3870651c478cc58e94766b5eb0ced0ae51c2894a37efd82aa9048a9aa07': (397, 22968),
    '66be205b854b1d136f64e9fd3843d0914ab20169ba9d19dc6d1216307e2b923a': (402, 10336),
    'f20170c20f4605ace5f11921d2f9c5f0a5e17bbd7fda1d18753d37b758090c59': (514, 137767),
}

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('capture', type=Path)
    p.add_argument('--out', required=True, type=Path, help='New private destination')
    a = p.parse_args()
    if a.capture.stat().st_size > 16 * 1024 * 1024:
        p.error('Capture exceeds the bounded schema-4 input size')
    j = json.loads(a.capture.read_text(encoding='utf-8-sig'))
    if j.get('schema') != 4 or j.get('probe_target') != 'ad102-fe' or not j['physical_prom'].get('complete'):
        p.error('Requires a complete schema-4 ad102-fe PROM capture')
    rom = bytes.fromhex(j['physical_prom']['hex'])
    if len(rom) != 1048576 or hashlib.sha256(rom).hexdigest() != PROM:
        p.error('ROM differs from the reviewed profile; do not bypass admission')
    files = {'ad102-fe-physical-prom.bin': rom}
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
