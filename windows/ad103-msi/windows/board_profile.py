#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Validate a completed read-only UEFI report and produce an exact board profile."""
from __future__ import annotations
import hashlib
import re

FIELDS = {
    'version', 'id', 'subsystem', 'segment', 'bus', 'device', 'function',
    'bar_base', 'bar_size', 'chip', 'vram_mib', 'prom_size', 'prom_sha256',
    'rom_size', 'rom_sha256', 'fwsec_parsed',
}
HEX = {'id', 'subsystem', 'bar_base', 'bar_size', 'chip'}

def parse_report(data: bytes) -> dict:
    if len(data) > 131072:
        raise ValueError('diagnostic report exceeds 128 KiB')
    text = data.decode('ascii').replace('\r\n', '\n')
    if text.count('DIAGNOSTIC_COMPLETE eligible=1 gpu_writes=0\n') != 1:
        raise ValueError('a completed eligible read-only diagnostic is required')
    final = re.findall(r'^FINAL .*$', text, re.M)
    if final != ['FINAL mode=diagnostic status=0 stage=complete gpu_writes=0']:
        raise ValueError('diagnostic did not finish cleanly with zero GPU writes')
    values = {}
    for line in text.splitlines():
        if not line.startswith('PROFILE '):
            continue
        for pair in line[8:].split():
            key, sep, value = pair.partition('=')
            if not sep or key not in FIELDS or key in values:
                raise ValueError('unknown, malformed, or duplicate profile field')
            if key.endswith('sha256'):
                if not re.fullmatch('[0-9a-f]{64}', value) or value == '0'*64:
                    raise ValueError('invalid SHA-256')
                values[key] = value
            else:
                pattern = '[0-9a-f]+' if key in HEX else '[0-9]+'
                if not re.fullmatch(pattern, value) or len(value) > 16:
                    raise ValueError('invalid numeric profile field')
                values[key] = int(value, 16 if key in HEX else 10)
    if set(values) != FIELDS:
        raise ValueError('incomplete diagnostic profile')
    v = values
    if v['version'] != 1 or v['id'] != 0x270510de:
        raise ValueError('only RTX 4070 Ti SUPER 10de:2705 is accepted')
    if (not 0 < v['subsystem'] < 0xffffffff or not 0 < v['chip'] <= 0xffffffff or
            v['chip'] & 0xfff00000 != 0x19300000):
        raise ValueError('invalid subsystem or non-AD103 chip')
    if not (0 <= v['segment'] <= 65535 and 0 <= v['bus'] <= 255 and
            0 <= v['device'] <= 31 and v['function'] == 0):
        raise ValueError('invalid PCI address')
    if not (v['bar_size'] == 0x1000000 and v['bar_base'] >= 0x1000000 and
            not v['bar_base'] % 0x1000000 and v['bar_base']+v['bar_size'] <= 2**64):
        raise ValueError('unsupported BAR0 layout')
    if not (16000 <= v['vram_mib'] <= 16384 and v['prom_size'] == 1048576 and
            512 <= v['rom_size'] <= 2097152 and v['fwsec_parsed'] == 1):
        raise ValueError('unsupported memory or ROM layout')
    v['report_sha256'] = hashlib.sha256(data).hexdigest()
    return v

def header(v: dict) -> str:
    mappings = {
        'GU_PROFILE_VERSION': 'version', 'GU_PROFILE_ID': 'id',
        'GU_BOARD_SUBSYSTEM': 'subsystem', 'GU_BOARD_CHIP': 'chip',
        'GU_PROFILE_SEGMENT': 'segment', 'GU_PROFILE_BUS': 'bus',
        'GU_PROFILE_DEVICE': 'device', 'GU_PROFILE_FUNCTION': 'function',
        'GU_PROFILE_BAR_BASE': 'bar_base', 'GU_PROFILE_BAR_SIZE': 'bar_size',
        'GU_PROFILE_ROM_SIZE': 'rom_size', 'GU_PROFILE_VRAM': 'vram_mib',
    }
    lines = ['/* Generated from a read-only report; this does not certify hardware compatibility. */']
    for macro, key in mappings.items():
        suffix = 'ULL' if key in {'bar_base', 'bar_size'} else 'U'
        lines.append(f'#define {macro} 0x{v[key]:x}{suffix}')
    for macro, key in [('GU_PROFILE_PROM_HASH','prom_sha256'),('GU_PROFILE_ROM_HASH','rom_sha256'),
                       ('GU_PROFILE_TAG','report_sha256')]:
        lines.append(f'#define {macro} "{v[key]}"')
    return '\n'.join(lines)+'\n'
