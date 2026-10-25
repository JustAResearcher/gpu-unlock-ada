#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Extract the five private host-test inputs from a matching cold capture."""
import argparse
import hashlib
import json
from pathlib import Path

PROM = '46b0ff2e697582fccefde3cbcb62eaff423bdce76f853040445030a7c5cc81fb'
CODE = {
    '0febfc8415226067e906adecb896ab45b03e969eac360a01ccfe81dd04c8a47e': (310, 20384),
    'ece28d4e20f28f5ca16bbe0a49ad6b0ad33956d55851c4ed620f617a6c237097': (315, 7712),
    '216c7270fa08ad9e4c3694d34d825e9e904554a1af5ffa2aeead4317c5537b37': (406, 138183),
}
SUCCESS = '0x0000000000000000'


def binary(value, size):
    if not isinstance(value, str) or len(value) != size * 2:
        raise ValueError('Missing or truncated hexadecimal bytes')
    result = bytes.fromhex(value)
    if len(result) != size:
        raise ValueError('Hexadecimal payload length differs')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    parser.add_argument('--out', required=True, type=Path, help='New private destination')
    args = parser.parse_args()
    if args.capture.stat().st_size > 8 * 1024 * 1024:
        parser.error('Capture exceeds the schema-5 8 MiB limit')
    data = json.loads(args.capture.read_text(encoding='utf-8-sig'))
    if (not isinstance(data, dict) or data.get('schema') != 5
            or data.get('probe_version') != '0.5.0-4070s-readonly-cold'
            or data.get('probe_mode') != 'readonly-cold-4070s'
            or data.get('gpu_writes') is not False or data.get('unlock_attempted') is not False
            or data.get('pci_enumeration_complete') is not True
            or data.get('unique_expected_identity_observed') is not True):
        parser.error('Requires a complete read-only schema-5 cold capture')
    displays = data.get('display_devices')
    identity = {'vendor': '0x10de', 'device_id': '0x2783', 'subsystem_vendor': '0x1458',
                'subsystem_device': '0x413b', 'revision': 0xa1, 'exact_expected_identity': True}
    if (not isinstance(displays, list) or len(displays) != 1
            or not isinstance(displays[0], dict)
            or any(displays[0].get(k) != v for k, v in identity.items())):
        parser.error('Display identity differs from this board profile')
    if data.get('config4096_status') != SUCCESS:
        parser.error('Full PCI configuration read did not succeed')
    config = binary(data.get('config4096'), 4096)
    if (config[:4] != bytes.fromhex('de108327') or config[8] != 0xa1
            or config[44:48] != bytes.fromhex('58143b41')
            or config[:64] != binary(displays[0].get('config64'), 64)
            or config[:256] != binary(data.get('config256'), 256)):
        parser.error('PCI identity or overlapping snapshots differ')
    prom = data.get('physical_prom')
    required = {'guard_passed': True, 'identity_stable': True, 'complete': True,
                'offset': '0x00300000', 'length': 1048576, 'read_attempts': 259,
                'data_read_status': SUCCESS}
    if not isinstance(prom, dict) or any(prom.get(k) != v for k, v in required.items()):
        parser.error('Physical PROM observation is incomplete')
    rom = binary(prom.get('hex'), 1048576)
    if hashlib.sha256(rom).hexdigest() != PROM or prom.get('sha256') != PROM:
        parser.error('PROM differs from this board profile')
    platform = data.get('platform')
    if (not isinstance(platform, dict) or platform.get('available') is not True
            or platform.get('complete') is not True):
        parser.error('Loaded-driver observation is incomplete')
    agents = platform.get('gpu_agents')
    if not isinstance(agents, list) or len(agents) != 3:
        parser.error('Exactly three reviewed driver profiles are required')
    files = {'physical-prom.bin': rom, 'pci-config4096.bin': config}
    for agent in agents:
        code = agent.get('code') if isinstance(agent, dict) else None
        if not isinstance(code, dict) or code.get('loaded_sha256') not in CODE:
            parser.error('Loaded driver code differs from this board profile')
        label, size = CODE[code['loaded_sha256']]
        raw = binary(code.get('hex'), size)
        name = f'driver-{label}-code.bin'  # Stable fixture labels, not firmware handles.
        if (code.get('length') != size or hashlib.sha256(raw).hexdigest() != code['loaded_sha256']
                or name in files):
            parser.error('Duplicate, truncated or inconsistent driver code')
        files[name] = raw
    args.out.mkdir(parents=True, exist_ok=False)
    for name, raw in files.items():
        (args.out / name).write_bytes(raw)
    print('Extracted five private inputs. Keep them outside public commits.')


if __name__ == '__main__':
    main()
