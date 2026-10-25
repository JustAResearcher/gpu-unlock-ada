#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Bind a separate release/restoration rehearsal to a reviewed 0.4.2 capture.

These observations never authorize the unlock transaction. The old unlock
profile parser remains deliberately separate and continues rejecting 0.4.2.
Captures are unauthenticated; runtime checks and explicit review are required.
"""
from __future__ import annotations

from collections import Counter
import copy
import hashlib
import re
import uuid

from board_profile import FIELDS, HEX

PCI = '4cf5b200-68b8-4ca5-9eec-b23e3f50029a'
PATH = '09576e91-6d3f-11d2-8e39-00a0c969723b'
GOP = '9042a9de-23dc-4a38-96fb-7aded080516a'
TEXT = '387477c2-69c7-11d2-8e39-00a0c969723b'
INPUT = '387477c1-69c7-11d2-8e39-00a0c969723b'
EDID = '1c0c34f6-d380-41fa-a049-8ad06c1a66aa'
ACTIVE_EDID = 'bd8c1056-9f36-44ec-92a8-a6337f817986'
ROOT_BRIDGE = '2f707ebb-4a1a-11d4-9a38-0090273fc14d'
UNSUPPORTED = 0x8000000000000003
MAX_IMAGE = 16 * 1024 * 1024


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def number(text: str, maximum: int = 2**64 - 1) -> int:
    require(bool(re.fullmatch(r'(?:0x[0-9a-f]+|[0-9]+)', text)), 'invalid integer')
    require(len(text) <= 20, 'oversized integer')
    value = int(text, 16 if text.startswith('0x') else 10)
    require(value <= maximum, 'integer outside range')
    return value


def digest(text: str) -> str:
    require(bool(re.fullmatch('[0-9a-f]{64}', text)) and text != '0' * 64,
            'invalid SHA-256 digest')
    return text


def guid(text: str) -> str:
    require(bool(re.fullmatch('[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}', text)),
            'invalid protocol GUID')
    require(int(uuid.UUID(text)) != 0, 'zero protocol GUID')
    return text


def lines(data: bytes, limit: int) -> list[str]:
    require(isinstance(data, bytes) and 0 < len(data) <= limit, 'capture size limit')
    try:
        text = data.decode('ascii').replace('\r\n', '\n')
    except UnicodeDecodeError as error:
        raise ValueError('capture must be ASCII') from error
    require(text.endswith('\n') and '\r' not in text and '\x00' not in text,
            'truncated or malformed capture')
    result = text[:-1].split('\n')
    require(all(line and len(line) <= 1024 for line in result), 'invalid capture line')
    return result


class Reader:
    def __init__(self, data: bytes):
        self.lines = lines(data, 2 * 1024 * 1024)
        self.position = 0

    def record(self, tag: str, keys: str) -> dict[str, str]:
        require(self.position < len(self.lines), 'incomplete ' + tag)
        line = self.lines[self.position]
        self.position += 1
        parts = line.split(' ')
        require(parts[0] == tag and len(parts) == len(keys.split()) + 1,
                'unexpected record; expected ' + tag)
        result = {}
        for part in parts[1:]:
            key, sep, value = part.partition('=')
            require(sep == '=' and bool(value) and key not in result,
                    'malformed or duplicate record field')
            result[key] = value
        require(set(result) == set(keys.split()), 'unexpected fields in ' + tag)
        return result

    def ints(self, tag: str, keys: str) -> dict[str, int]:
        return {key: number(value) for key, value in self.record(tag, keys).items()}


def _report(data: bytes) -> dict:
    rows = lines(data, 131072)
    require(rows[0] == 'gpu-unlock-ada Windows preboot port 0.4.2', 'requires 0.4.2 report')
    require(rows.count('PROFILE_TAG=diagnostic-only mode=diagnostic') == 1,
            'report is not a diagnostic')
    require(rows.count('DIAGNOSTIC_ONLY: GPU writes, firmware execution, reset and driver detach are disabled.') == 1,
            'diagnostic safety marker missing')
    expected = [('target', 0), ('bar0', 0), ('cold-state', 0), ('prom', 0),
                ('firmware-capture', 0), ('display', 0), ('owner-observe', UNSUPPORTED)]
    checks = []
    for row in rows:
        if row.startswith('CHECK '):
            match = re.fullmatch(r'CHECK stage=([a-z0-9-]+) status=(0x[0-9a-f]+|[0-9]+)', row)
            require(match is not None, 'malformed check record')
            checks.append((match[1], number(match[2])))
    require(checks == expected, 'capture prerequisite failed or unexpected diagnostic path')
    final = 'FINAL mode=diagnostic status=0x8000000000000003 stage=owner-observe gpu_writes=0'
    require(rows[-1] == final and sum(row.startswith('FINAL ') for row in rows) == 1,
            'requires completed owner-observe refusal with zero GPU writes')
    require(not any('eligible=1' in row or 'UNLOCK_' in row for row in rows),
            'unlock evidence is not a rehearsal capture')
    values = {}
    for row in rows:
        if not row.startswith('PROFILE '):
            continue
        for part in row[8:].split(' '):
            key, sep, value = part.partition('=')
            require(sep == '=' and key in FIELDS and key not in values,
                    'unknown or duplicate board field')
            if key.endswith('sha256'):
                values[key] = digest(value)
            else:
                pattern = '[0-9a-f]+' if key in HEX else '[0-9]+'
                require(bool(re.fullmatch(pattern, value)) and len(value) <= 16,
                        'invalid board value')
                values[key] = int(value, 16 if key in HEX else 10)
    require(set(values) == FIELDS, 'incomplete board profile')
    fixed = dict(version=1, id=0x270510de, subsystem=0xe1301462, segment=0,
                 bus=1, device=0, function=0, chip=0x193000a1,
                 bar_size=0x1000000, prom_size=1048576, fwsec_parsed=1)
    require(all(values[k] == v for k, v in fixed.items()), 'unsupported rehearsal board')
    require(values['bar_base'] >= 0x1000000 and values['bar_base'] % 0x1000000 == 0
            and values['bar_base'] + values['bar_size'] <= 2**64,
            'invalid BAR address')
    require(16000 <= values['vram_mib'] <= 16384 and 512 <= values['rom_size'] <= 2097152,
            'unsupported VRAM/ROM size')
    config = []
    for row in rows:
        if not row.startswith('config['):
            continue
        match = re.fullmatch(r'config\[([0-9a-f]{2})\]=([0-9a-f]{8}) ([0-9a-f]{8}) ([0-9a-f]{8}) ([0-9a-f]{8})', row)
        require(match is not None and int(match[1], 16) == len(config) * 4,
                'invalid initial PCI configuration')
        config.extend(int(match[i], 16) for i in range(2, 6))
    require(len(config) == 64 and config[0] == values['id'] and config[11] == values['subsystem']
            and config[2] == 0x030000a1 and config[4] == values['bar_base']
            and config[1] & 7 == 3, 'board profile/configuration mismatch')
    values['report_sha256'] = hashlib.sha256(data).hexdigest()
    return values


def _graph(reader: Reader, phase: int) -> tuple[dict, list[tuple]]:
    info = reader.ints('GRAPH', 'phase status handles protocols opens active_edges')
    require(info['phase'] == phase and info['status'] == 0, 'graph capture failed')
    require(1 <= info['handles'] <= 4096 and info['handles'] <= info['protocols'] <= 16384
            and 0 < info['active_edges'] <= 4096 and info['active_edges'] <= info['opens'] <= 131072,
            'invalid graph bounds')
    edges = []
    seen = set()
    for index in range(info['active_edges']):
        edge = reader.ints('EDGE', 'phase index handle agent controller attributes count')
        protocol = reader.record('GUID', 'kind index value')
        require(edge['phase'] == phase and edge['index'] == index
                and protocol['kind'] == 'edge' and number(protocol['index']) == index,
                'graph index mismatch')
        require(edge['handle'] and edge['agent'] and edge['controller'] and 1 <= edge['count'] <= 2**32-1
                and edge['attributes'] in (8, 16, 32, 48), 'unsupported active relationship')
        item = (edge['handle'], edge['agent'], edge['controller'],
                guid(protocol['value']), edge['attributes'], edge['count'])
        require(item[:5] not in seen, 'duplicate graph relationship')
        seen.add(item[:5])
        edges.append(item)
    return info, edges


def _path(reader: Reader, kind: str, handle: int, maximum: int = 4096) -> bytes:
    hashed = reader.record('HASH', 'kind handle bytes sha256')
    require(hashed['kind'] == kind and number(hashed['handle']) == handle, 'path hash association')
    count = number(hashed['bytes'], maximum)
    require(4 <= count <= maximum, 'path length')
    data = bytearray()
    while len(data) < count:
        part = reader.record('PATH_BYTES', 'kind handle offset hex')
        require(part['kind'] == kind and number(part['handle']) == handle
                and number(part['offset']) == len(data), 'path chunk association')
        require(bool(re.fullmatch('[0-9a-f]+', part['hex'])) and len(part['hex']) % 2 == 0,
                'invalid path bytes')
        chunk = bytes.fromhex(part['hex'])
        require(len(chunk) == min(32, count - len(data)), 'invalid path chunk length')
        data.extend(chunk)
    require(hashlib.sha256(data).hexdigest() == digest(hashed['sha256']), 'path digest mismatch')
    offset = 0
    nodes = 0
    while offset < count:
        require(offset + 4 <= count, 'truncated device path node')
        length = int.from_bytes(data[offset+2:offset+4], 'little')
        require(4 <= length <= count-offset and nodes < 128, 'invalid device path node')
        if data[offset] == 127:
            require(data[offset:offset+4] == b'\x7f\xff\x04\x00' and offset+4 == count,
                    'invalid device path end')
        else:
            require(offset+length < count, 'device path lacks end')
        offset += length
        nodes += 1
    require(data[-4:] == b'\x7f\xff\x04\x00', 'device path lacks end')
    return bytes(data)


def _endpoint(reader: Reader, target: int, console: int) -> dict:
    value = reader.ints('ENDPOINT', 'handle status protocols')
    handle = value['handle']
    require(handle != 0 and value['status'] == 0 and 1 <= value['protocols'] <= 64,
            'endpoint capture failed')
    protocols = []
    for index in range(value['protocols']):
        row = reader.record('GUID', 'kind index value')
        require(row['kind'] == 'endpoint' and number(row['index']) == index, 'endpoint GUID index')
        protocols.append(guid(row['value']))
    require(len(set(protocols)) == len(protocols), 'duplicate endpoint protocol')
    row = reader.ints('ENDPOINT_PATH', 'handle status present')
    require(row['handle'] == handle, 'endpoint path association')
    if handle == console:
        require(row['status'] == UNSUPPORTED and row['present'] == 0 and PATH not in protocols,
                'unsupported virtual console path')
        path = None
    else:
        require(row['status'] == 0 and row['present'] == 1 and PATH in protocols,
                'endpoint path unavailable')
        path = _path(reader, 'endpoint', handle)
        row = reader.ints('ENDPOINT_PCI', 'handle status parent target')
        require(row == dict(handle=handle, status=0, parent=target, target=target),
                'endpoint is not on the target GPU')
    return dict(handle=handle, protocols=protocols, path=path)


def _driver(reader: Reader) -> dict:
    row = reader.ints('DRIVER', 'agent binding_status binding')
    agent = row['agent']
    require(agent and row['binding_status'] == 0 and row['binding'], 'binding capture failed')
    binding = reader.ints('BINDING', 'agent handle image version supported start stop')
    require(binding['agent'] == agent and binding['handle'] == agent and binding['image']
            and binding['version'] <= 2**32-1, 'binding association')
    row = reader.ints('DRIVER_IMAGE', 'agent status interface')
    require(row['agent'] == agent and row['status'] == 0 and row['interface'], 'loaded image capture failed')
    image = reader.ints('IMAGE', 'agent image device parent base bytes code_type data_type')
    require(image['agent'] == agent and image['image'] == binding['image'] and image['device']
            and image['parent'] and image['base'] and 512 <= image['bytes'] <= MAX_IMAGE
            and image['base'] + image['bytes'] <= 2**64 and image['code_type'] == 3
            and image['data_type'] == 4, 'invalid loaded image')
    path = _path(reader, 'driver-file', agent, 32)
    row = reader.record('HASH', 'kind handle bytes sha256')
    require(row['kind'] == 'loaded-image' and number(row['handle']) == agent
            and number(row['bytes']) == image['bytes'], 'loaded image hash association')
    digest(row['sha256'])
    pe = reader.ints('PE', 'agent status stop_offset canonical_valid')
    require(pe['agent'] == agent and pe['status'] == 0 and pe['canonical_valid'] == 1,
            'PE identity capture failed')
    hashes = reader.record('PE_METADATA_HASH', 'agent headers relocations')
    require(number(hashes['agent']) == agent, 'PE hash association')
    digest(hashes['headers'])
    digest(hashes['relocations'])
    layout = reader.ints('PE_LAYOUT', 'agent preferred_base image_bytes headers entry reloc_rva reloc_bytes relocations executable_relocations sections stop_section')
    require(layout['agent'] == agent and layout['image_bytes'] == image['bytes']
            and 64 <= layout['headers'] < image['bytes'] and 1 <= layout['sections'] <= 32
            and layout['stop_section'] < layout['sections'] and layout['executable_relocations'] == 0
            and 0 < layout['relocations'] <= 65536 and 8 <= layout['reloc_bytes'] <= image['bytes']
            and layout['headers'] <= layout['reloc_rva']
            and layout['reloc_rva'] + layout['reloc_bytes'] <= image['bytes'], 'unsupported PE layout')
    sections = []
    executable = []
    for index in range(layout['sections']):
        row = reader.record('SECTION', 'agent index name rva bytes flags executable relocations')
        require(number(row['agent']) == agent and number(row['index']) == index,
                'section association')
        require(bool(re.fullmatch('[A-Za-z0-9._-]{1,8}', row['name'])), 'invalid section name')
        section = {key: number(value) for key, value in row.items() if key != 'name'}
        require(section['flags'] <= 2**32-1 and section['executable'] in (0, 1)
                and section['executable'] == bool(section['flags'] & 0x20000000)
                and layout['headers'] <= section['rva'] and section['bytes'] > 0
                and section['rva'] + section['bytes'] <= image['bytes']
                and section['relocations'] <= layout['relocations'], 'invalid PE section')
        require(all(section['rva'] + section['bytes'] <= old['rva']
                    or old['rva'] + old['bytes'] <= section['rva'] for old in sections),
                'overlapping PE sections')
        if section['executable']:
            hashed = reader.record('SECTION_HASH', 'agent index raw canonical')
            require(number(hashed['agent']) == agent and number(hashed['index']) == index
                    and section['relocations'] == 0, 'executable section hash association')
            require(digest(hashed['raw']) == digest(hashed['canonical']),
                    'unsupported executable normalization')
            section['code_sha256'] = hashed['raw']
            executable.append(section)
        sections.append(section)
    require(len(executable) == 1 and sum(s['relocations'] for s in sections) == layout['relocations'],
            'unsupported executable/relocation layout')
    code = executable[0]
    require(code['index'] == layout['stop_section'], 'stop function section mismatch')
    offsets = {key + '_offset': binding[key] - image['base'] for key in ('supported', 'start', 'stop')}
    require(pe['stop_offset'] == offsets['stop_offset'] and
            all(code['rva'] <= offset < code['rva'] + code['bytes']
                for offset in [*offsets.values(), layout['entry']]), 'callback/entry outside executable section')
    require(any(s['rva'] <= layout['reloc_rva'] and layout['reloc_rva'] + layout['reloc_bytes'] <= s['rva'] + s['bytes']
                and not s['executable'] for s in sections), 'relocations outside non-executable section')
    return dict(agent=agent, image=image, version=binding['version'], image_bytes=image['bytes'],
                **offsets, file_path=path.hex(), file_path_bytes=len(path), code_rva=code['rva'],
                code_bytes=code['bytes'], code_sha256=code['code_sha256'], section_count=layout['sections'],
                headers_bytes=layout['headers'], entry_rva=layout['entry'], relocation_rva=layout['reloc_rva'],
                relocation_bytes=layout['reloc_bytes'], relocation_count=layout['relocations'],
                relocation_sha256=hashes['relocations'])


def parse_capture(report_bytes: bytes, topology_bytes: bytes) -> dict:
    """Return immutable identity inputs, not permission to unlock the GPU."""
    board = _report(report_bytes)
    reader = Reader(topology_bytes)
    require(reader.record('CAPTURE_FORMAT', 'version diagnostic read_only unlock_authorized') ==
            dict(version='1', diagnostic='0.4.2', read_only='1', unlock_authorized='0'),
            'unsupported capture format')
    target_record = reader.ints('TARGET', 'handle application')
    target, application = target_record['handle'], target_record['application']
    require(target and application and target != application, 'invalid capture target')
    first, edges = _graph(reader, 0)
    memory = reader.ints('MEMORY_MAP', 'status bytes stride version')
    require(memory['status'] == 0 and memory['version'] == 1 and 40 <= memory['stride'] <= 256
            and 0 < memory['bytes'] <= 262144 and memory['bytes'] % memory['stride'] == 0
            and memory['bytes'] // memory['stride'] <= 4096, 'memory map capture failed')
    console = reader.ints('CONSOLE', 'out in error text keys stderr')
    require(all(console.values()) and console['out'] == console['in'] == console['error']
            and console['text'] == console['stderr'] and console['out'] not in (target, application),
            'unsupported console topology')
    callbacks = []
    for kind in range(3):
        row = reader.ints('CONSOLE_CALLBACK', 'kind reset read_key wait' if kind == 2 else 'kind reset output set_mode')
        require(row['kind'] == kind and all(v for k, v in row.items() if k != 'kind'),
                'missing console callbacks')
        callbacks.append(row)
    require({k: v for k, v in callbacks[0].items() if k != 'kind'} ==
            {k: v for k, v in callbacks[1].items() if k != 'kind'}, 'stderr callback mismatch')
    endpoint_list = [_endpoint(reader, target, console['out']) for _ in range(4)]
    endpoints = {e['handle']: e for e in endpoint_list}
    require(len(endpoints) == 4 and target in endpoints and console['out'] in endpoints,
            'duplicate/missing endpoint')
    driver_list = [_driver(reader) for _ in range(3)]
    drivers = {d['agent']: d for d in driver_list}
    require(len(drivers) == 3 and not (set(drivers) & (set(endpoints) | {application})),
            'driver handle alias')
    second, later = _graph(reader, 1)
    require({k: v for k, v in first.items() if k != 'phase'} ==
            {k: v for k, v in second.items() if k != 'phase'} and Counter(edges) == Counter(later),
            'ownership graph changed during capture')
    end = reader.ints('CAPTURE_END', 'metadata_status graph_first graph_second stable_active_graph endpoints drivers gpu_writes unlock_authorized')
    require(end == dict(metadata_status=0, graph_first=0, graph_second=0, stable_active_graph=1,
                        endpoints=4, drivers=3, gpu_writes=0, unlock_authorized=0)
            and reader.position == len(reader.lines), 'incomplete/failed capture or trailing data')
    physical = [e for e in endpoint_list if GOP in e['protocols'] and e['handle'] != console['out']]
    passive = [e for e in endpoint_list if set(e['protocols']) == {PATH, EDID}]
    require(len(physical) == len(passive) == 1, 'requires one GOP and one EDID-only output')
    gop, edid = physical[0], passive[0]
    require(gop['handle'] != edid['handle'] and {TEXT, EDID, ACTIVE_EDID, PATH} <= set(gop['protocols'])
            and PCI in endpoints[target]['protocols'] and {TEXT, INPUT} <= set(endpoints[console['out']]['protocols']),
            'unsupported endpoint protocols')
    pci_path = endpoints[target]['path']
    require(len(pci_path) == 28 and pci_path[:4] == b'\x02\x01\x0c\x00'
            and pci_path[12:24] == bytes.fromhex('010106000001010106000000'), 'unsupported GPU PCI path')
    for output in (gop, edid):
        path = output['path']
        require(len(path) == 36 and path[:24] == pci_path[:24] and path[24:28] == b'\x02\x03\x08\x00',
                'output path is not an ACPI ADR child of GPU')
    require(gop['path'] != edid['path'], 'output path alias')
    protected = [e for e in edges if e[0] in endpoints]
    pci_owner = [e for e in protected if e[0] == target and e[3:] == (PCI, 16, 1) and e[2] == target]
    text_owner = [e for e in protected if e[0] == gop['handle'] and e[3:] == (TEXT, 16, 1) and e[2] == gop['handle']]
    gfx_owner = [e for e in protected if e[0] == gop['handle'] and e[3:] == (GOP, 16, 1) and e[2] == gop['handle']]
    require(len(pci_owner) == len(text_owner) == len(gfx_owner) == 1, 'ambiguous driver ownership')
    nv, out, gfx = pci_owner[0][1], text_owner[0][1], gfx_owner[0][1]
    require({nv, out, gfx} == set(drivers), 'driver identities do not match graph')
    expected = [(target, nv, target, PCI, 16, 1),
                (target, nv, gop['handle'], PATH, 8, 1),
                (target, nv, edid['handle'], PATH, 8, 1),
                (gop['handle'], out, gop['handle'], TEXT, 16, 1),
                (gop['handle'], out, console['out'], TEXT, 8, 1),
                (gop['handle'], gfx, gop['handle'], GOP, 16, 1)]
    require(Counter(protected) == Counter(expected), 'unknown protected ownership relationship')
    # Drivers must not own additional controllers, including outside the four
    # captured endpoints. Incoming keyboard/pointer relationships remain global
    # runtime invariants rather than permission to detach those devices.
    require(all(e in expected for e in edges if e[1] in drivers), 'driver has additional consumers')
    incoming = [e for e in edges if e[2] == target and e not in expected]
    require(len(incoming) == 1 and incoming[0][3:] == (ROOT_BRIDGE, 8, 1)
            and incoming[0][0] not in endpoints and incoming[0][1] not in drivers
            and (incoming[0][0], incoming[0][1], incoming[0][0], ROOT_BRIDGE, 16, 1) in edges,
            'missing or unknown upstream PCI relationship')
    require(all(e in expected for e in edges if e[2] in (gop['handle'], edid['handle'])),
            'unknown incoming GPU-output relationship')
    require(drivers[nv]['image']['device'] == target and drivers[nv]['image']['image'] == nv
            and drivers[nv]['version'] == 0x70011 and drivers[out]['version'] == drivers[gfx]['version'] == 0x10,
            'unsupported driver source/version')
    # Known firmware-file path classes, with complete bytes pinned in output.
    require(bytes.fromhex(drivers[nv]['file_path']) == bytes.fromhex('04041c006e00760067006f0070002d006100640031007800780000007fff0400'),
            'unsupported NVIDIA driver filename')
    for agent in (out, gfx):
        path = bytes.fromhex(drivers[agent]['file_path'])
        require(len(path) == 24 and path[:4] == b'\x04\x06\x14\x00', 'console driver is not a firmware-file path')
    require(drivers[out]['file_path'] != drivers[gfx]['file_path'], 'console driver path alias')
    output_code_start = drivers[out]['image']['base'] + drivers[out]['code_rva']
    output_code_end = output_code_start + drivers[out]['code_bytes']
    require(all(output_code_start <= value < output_code_end for row in callbacks
                for key, value in row.items() if key not in ('kind', 'wait')), 'console callback is outside pinned output driver')
    roles = dict(pci=endpoints[target], gop=gop, edid=edid, console=endpoints[console['out']])
    # Raw runtime handles/bases and arbitrary loaded-image data hashes are not
    # reusable identities and are intentionally absent from the generated data.
    clean_drivers = [{k: v for k, v in drivers[agent].items() if k not in ('agent', 'image')}
                     for agent in (out, gfx, nv)]
    return dict(schema_version=1, purpose='firmware-release-restoration-rehearsal',
                unlock_authorized=False, rehearsal_hardware_tested=False, board=board,
                topology_sha256=hashlib.sha256(topology_bytes).hexdigest(),
                endpoints={role: dict(path=e['path'].hex() if e['path'] is not None else None,
                                      protocols=sorted(e['protocols'])) for role, e in roles.items()},
                drivers=clean_drivers,
                console_offsets=[callbacks[kind][key] - drivers[out]['image']['base']
                                 for kind, key in ((0, 'reset'), (0, 'output'), (0, 'set_mode'),
                                                   (2, 'reset'), (2, 'read_key'))],
                captured_active_edges=len(edges))


def _bytes(hexadecimal: str) -> str:
    return '{' + ','.join(f'0x{value:02x}' for value in bytes.fromhex(hexadecimal)) + '}'


def validate_restoration_evidence(profile: dict) -> dict:
    """Validate the serialized, narrow stage-4 exception before emitting it.

    A report digest records provenance, not authenticity. The runtime still
    checks the original board/driver identities and every ownership transition.
    """
    e = profile.get('restoration_evidence')
    keys = {'schema_version', 'source_version', 'report_sha256', 'profile_tag',
            'observed_stage', 'unlock_attempted', 'restoration_completed', 'baseline', 'deferred'}
    require(isinstance(e, dict) and set(e) == keys, 'missing or malformed restoration evidence')
    require(type(e['schema_version']) is int and e['schema_version'] == 1
            and e['source_version'] == '0.5.1-rehearsal'
            and type(e['observed_stage']) is int and e['observed_stage'] == 4
            and e['unlock_attempted'] is False and e['restoration_completed'] is False,
            'invalid restoration evidence scope')
    require(isinstance(e['report_sha256'],str) and isinstance(e['profile_tag'],str), 'invalid restoration digests')
    digest(e['report_sha256'])
    require(digest(e['profile_tag']) == profile['board']['report_sha256'], 'restoration profile mismatch')
    mode_keys = {'max_mode', 'mode', 'info_bytes', 'framebuffer_base', 'framebuffer_bytes', 'info'}
    info_keys = {'version', 'width', 'height', 'pixel_format', 'stride', 'masks'}
    for name in ('baseline', 'deferred'):
        mode = e[name]
        require(isinstance(mode, dict) and set(mode) == mode_keys, 'invalid mode evidence fields')
        require(all(type(mode[k]) is int and 0 <= mode[k] < 2**64 for k in mode_keys-{'info'}),
                'invalid mode evidence value')
        info = mode['info']
        require(isinstance(info, dict) and set(info) == info_keys, 'invalid mode-info evidence fields')
        require(all(type(info[k]) is int and 0 <= info[k] < 2**32 for k in info_keys-{'masks'}),
                'invalid mode-info evidence value')
        require(isinstance(info['masks'], list) and len(info['masks']) == 4
                and all(type(v) is int and 0 <= v < 2**32 for v in info['masks']), 'invalid pixel masks')
        require(1 <= mode['max_mode'] <= 256 and mode['info_bytes'] == 36
                and info['version'] == 0 and 0 < info['width'] <= info['stride'] <= 32768
                and 0 < info['height'] <= 32768 and info['pixel_format'] in (0, 1),
                'unsupported display evidence geometry')
    before, after = e['baseline'], e['deferred']
    require(before['mode'] < before['max_mode'] and before['framebuffer_base'] > 0
            and 0 < before['framebuffer_bytes'] <= 256*1024*1024
            and before['framebuffer_base'] + before['framebuffer_bytes'] <= 2**64
            and before['info']['stride'] * before['info']['height'] * 4 <= before['framebuffer_bytes'],
            'baseline framebuffer is not initialized')
    require(after['max_mode'] == before['max_mode'] and after['mode'] == before['max_mode']+1
            and after['info_bytes'] == before['info_bytes'] and after['info'] == before['info']
            and after['framebuffer_base'] == after['framebuffer_bytes'] == 0,
            'not the observed deferred-framebuffer state')
    return e


def bind_restoration_report(profile: dict, data: bytes) -> dict:
    """Bind complete 0.5.1 refusal evidence; no new hardware action is allowed.

    Parse the whole report with an allowlisted diagnostic grammar. The ordered
    event stream must end at the exact zero-framebuffer refusal, with no failed
    prerequisites, unverified releases, retry, unlock or Windows handoff.
    """
    rows = lines(data, 131072)
    final = 'FINAL mode=firmware-rehearsal status=0x8000000000000003 stage=release-and-restore mutated=1 unlock_attempted=0'
    require(rows[-1] == final and sum(row.startswith('FINAL ') for row in rows) == 1,
            'restoration report lacks a unique terminal refusal')
    require(len(rows) >= 4 and rows[0] == 'gpu-unlock-ada Windows preboot port 0.5.1-rehearsal'
            and re.fullmatch(r'RUN[0-9]{2}', rows[1]) is not None
            and rows[2] == 'PROFILE_TAG='+profile['board']['report_sha256']+' mode=firmware-rehearsal'
            and rows[3] == 'REHEARSAL_ONLY: active firmware driver callbacks; no unlock payload, direct GPU writes, DMA, FLR, flashing or persistent boot changes.',
            'requires the matching 0.5.1 rehearsal report')
    # These informational records are fully matched but do not authorize the
    # exception. Ordered CHECK/event records below establish the execution path.
    n = r'(?:0x[0-9a-f]+|[0-9]+)'
    information = [
        r'PCI enumeration: status=0 handles=[0-9]+', r'PCI target matches=1 status=0',
        r'target location: status=0',
        r'target BDF=0:1:0\.0 checks=1 pm='+n+r' pcie='+n+r' msi='+n+r' msix='+n,
        r'bar0 query: status=0',
        r'bar0 descriptor: type=0 granularity=32 min='+n+r' max='+n+r' length='+n+r' translation=0',
        r'cold state: chip='+n+r' FEAT='+n+r' GFW='+n+'/'+n+'/'+n+r' WPR2_HI=0 SS0='+n+r' SS1='+n,
        r'gpu_unlock: ROM chain: pos='+n+r' data='+n+r' bytes=[0-9]+ type='+n+r' last=[0-9]+',
        r'gpu_unlock: ROM expansion: base='+n+r' ext='+n+r' adjust='+n,
        r'gpu_unlock: ROM BIT: offset='+n+r' size=[0-9]+ stride=[0-9]+ tokens=[0-9]+ adjust='+n,
        r'gpu_unlock: ROM ucode table: offset='+n+r' version=[0-9]+ header=[0-9]+ stride=[0-9]+ entries=[0-9]+',
        r'gpu_unlock: FWSEC descriptor: offset='+n+r' ROM_bytes=[0-9]+ header='+n,
        r'gpu_unlock: FWSEC geometry: image_off='+n+r' size='+n+r' imem='+n+r' va='+n+r' pa='+n+r' dmem='+n+r' pa='+n+r' pkc='+n+r' interface='+n+r' engine='+n+r' ucode=[0-9]+ signatures=[0-9]+ versions='+n+r' bytes=[0-9]+',
    ]
    values, occurrences, config, events = {}, Counter(), [], []
    for row in rows[4:]:
        if row.startswith('PROFILE '):
            local = set()
            for part in row[8:].split(' '):
                key, sep, value = part.partition('=')
                require(sep == '=' and key in FIELDS and key not in local, 'invalid rehearsal board field')
                local.add(key)
                if key.endswith('sha256'):
                    value = digest(value)
                else:
                    pattern = '[0-9a-f]+' if key in HEX else '[0-9]+'
                    require(re.fullmatch(pattern, value) is not None and len(value) <= 16, 'invalid rehearsal board value')
                    value = int(value, 16 if key in HEX else 10)
                require(value == profile['board'][key], 'rehearsal board differs from capture')
                values[key] = value; occurrences[key] += 1
            continue
        if row.startswith('config['):
            match = re.fullmatch(r'config\[([0-9a-f]{2})\]=([0-9a-f]{8}) ([0-9a-f]{8}) ([0-9a-f]{8}) ([0-9a-f]{8})', row)
            require(match is not None and int(match[1], 16) == len(config)*4, 'invalid rehearsal PCI record')
            config.extend(int(match[i], 16) for i in range(2, 6)); continue
        if any(re.fullmatch(pattern, row) for pattern in information):
            continue
        events.append(row)
    require(set(values) == FIELDS and all(occurrences[k] == (2 if k in ('chip','vram_mib') else 1) for k in FIELDS),
            'missing or duplicate rehearsal board field')
    require(len(config) == 64 and config[0] == values['id'] and config[11] == values['subsystem']
            and config[2] == 0x030000a1 and config[4] == values['bar_base'] and config[1]&7 == 3,
            'rehearsal PCI identity mismatch')
    r = Reader(('\n'.join(events)+'\n').encode())
    def exact(text):
        require(r.position < len(r.lines) and r.lines[r.position] == text, 'unexpected restoration event: '+text)
        r.position += 1
    def check(stage): exact('CHECK stage='+stage+' status=0')
    exact('SecureBoot query: status=0 bytes=1 value=0')
    for stage in ('secure-boot','target','bar0','cold-state','prom'): check(stage)
    exact('GOP physical=1 target=1 expect_detached=0 status=0'); check('display')
    preload = r.record('WINDOWS_PRELOAD', 'internal_gpt_esp candidates bytes sha256 same_boot_pin')
    require(preload['internal_gpt_esp'] == preload['candidates'] == preload['same_boot_pin'] == '1'
            and 512 <= number(preload['bytes']) <= 32*1024*1024, 'invalid Windows preload evidence')
    digest(preload['sha256']); check('windows-preload')
    def gop(stage, reason):
        obs = r.record('REHEARSAL_GOP_OBSERVATION', 'stage reason handle expected original console interface readable')
        require(obs['stage'] == str(stage) and obs.pop('reason') == reason, 'unexpected GOP observation')
        obs = {k:number(v) for k,v in obs.items()}
        require(obs['readable'] == 1 and all(obs[k] for k in ('handle','expected','original','console','interface'))
                and obs['handle'] == obs['expected'] and obs['handle'] != obs['console'], 'unreadable or unexpected GOP')
        cb = r.ints('REHEARSAL_GOP_CALLBACKS', 'stage query set blt mode_ptr mode_readable')
        require(cb['stage'] == stage and cb['mode_readable'] == 1
                and all(cb[k] for k in ('query','set','blt','mode_ptr')), 'invalid GOP callbacks')
        mode = r.ints('REHEARSAL_GOP_MODE', 'stage max mode info_ptr info_bytes info_readable framebuffer framebuffer_bytes')
        require(mode['stage'] == stage and mode['info_readable'] == 1 and mode['info_ptr'], 'unreadable GOP mode')
        info = r.record('REHEARSAL_GOP_INFO', 'stage version width height format stride masks')
        masks = info.pop('masks').split('/')
        require(len(masks) == 4, 'invalid GOP mask record')
        info = {k:number(v, 2**32-1) for k,v in info.items()}
        require(info.pop('stage') == stage, 'invalid GOP info stage')
        info['pixel_format'] = info.pop('format'); info['masks'] = [number(v,2**32-1) for v in masks]
        cleaned = dict(max_mode=mode['max'],mode=mode['mode'],info_bytes=mode['info_bytes'],
                       framebuffer_base=mode['framebuffer'],framebuffer_bytes=mode['framebuffer_bytes'],info=info)
        return obs,cb,cleaned
    before_obs,before_cb,before = gop(0,'baseline')
    require(before_obs['handle'] == before_obs['original'], 'baseline GOP remap')
    prepared = r.ints('REHEARSAL_PREPARE','status edges drivers outputs')
    require(prepared['status'] == 0 and 6 <= prepared['edges'] <= 4096
            and prepared['drivers'] == 3 and prepared['outputs'] == 2, 'rehearsal preparation failed')
    check('rehearsal-preflight')
    for text in ('READY: firmware release/restoration rehearsal; unlock payload is absent.',
                 'Press R to release three boot-display drivers, reconnect them, then start Windows only after verification.',
                 'This active firmware test may blank or hang the display. If Windows has not appeared after 2 minutes, power OFF, remove USB and restore security settings.',
                 'Esc or 120 seconds cancels before release. No automatic retry.'):
        exact(text)
    for stage in ('arming','watchdog','armed-config','armed-cold'): check(stage)
    exact('REHEARSAL_INTENT: R received; beginning staged release/restoration.')
    for stage,removed in ((1,2),(2,3),(3,6)):
        exact(f'REHEARSAL_INTENT stage={stage} operation=disconnect')
        exact(f'REHEARSAL_RETURN stage={stage} status=0')
        exact(f'REHEARSAL_STAGE_VERIFIED stage={stage} edges={prepared["edges"]-removed}')
    exact('REHEARSAL_INTENT stage=4 operation=connect')
    exact('REHEARSAL_SUPPORTED stage=4 status=0')
    exact('REHEARSAL_RETURN stage=4 status=0')
    exact('REHEARSAL_DISPLAY_REFUSED stage=4 reason=zero-framebuffer-base status=0x8000000000000003')
    after_obs,after_cb,after = gop(4,'zero-framebuffer-base')
    require(after_obs['original'] == before_obs['original'] and after_obs['console'] == before_obs['console']
            and all(after_cb[k] == before_cb[k] for k in ('query','set','blt')), 'restored GOP identity changed')
    exact('REHEARSAL_VERIFY_FAILED stage=4 check=display status=0x8000000000000003')
    exact(final)
    require(r.position == len(r.lines), 'extra restoration events')
    result = copy.deepcopy(profile)
    require('restoration_evidence' not in result, 'restoration evidence already bound')
    result['restoration_evidence'] = dict(schema_version=1,source_version='0.5.1-rehearsal',
        report_sha256=hashlib.sha256(data).hexdigest(),profile_tag=profile['board']['report_sha256'],
        observed_stage=4,unlock_attempted=False,restoration_completed=False,baseline=before,deferred=after)
    validate_restoration_evidence(result)
    return result


def _guid(value: str) -> str:
    data = uuid.UUID(value)
    return '{0x%08x,0x%04x,0x%04x,{%s}}' % (
        data.time_low, data.time_mid, data.time_hi_version,
        ','.join(f'0x{byte:02x}' for byte in data.bytes[8:]))


def rehearsal_header(profile: dict) -> str:
    """Emit a generated-only C header, separate from unlock eligibility."""
    require(profile.get('unlock_authorized') is False and profile.get('rehearsal_hardware_tested') is False
            and profile.get('purpose') == 'firmware-release-restoration-rehearsal', 'not a rehearsal profile')
    evidence = validate_restoration_evidence(profile)
    before, after = evidence['baseline'], evidence['deferred']
    output = ['/* Generated from reviewed metadata; NOT unlock authorization. */', '#pragma once',
              '#include <Uefi.h>', '#include <Protocol/GraphicsOutput.h>', '#define GU_REHEARSAL_PROFILE_VERSION 2',
              'typedef struct {', '  UINT32 version;',
              '  UINTN image_bytes,supported_offset,start_offset,stop_offset,code_rva,code_bytes;',
              '  UINT8 file_path[32]; UINTN file_path_bytes; const char *code_sha256;',
              '  UINTN section_count,headers_bytes,entry_rva,relocation_rva,relocation_bytes,relocation_count;',
              '  const char *relocation_sha256;', '} GU_REHEARSAL_DRIVER_PIN;']
    output += ['#define GU_REHEARSAL_RESTORATION_SHA256 "'+evidence['report_sha256']+'"',
               'static const BOOLEAN GuRehearsalDeferredEnabled=TRUE;']
    for c_type, suffix, value in (
        ('UINT32','MaxMode',before['max_mode']),('UINT32','ActiveMode',before['mode']),
        ('UINT32','Mode',after['mode']),('UINTN','InfoBytes',before['info_bytes']),
        ('UINT64','FrameBufferBase',before['framebuffer_base']),
        ('UINTN','FrameBufferBytes',before['framebuffer_bytes'])):
        output.append(f'static const {c_type} GuRehearsalDeferred{suffix}=0x{value:x}ULL;')
    info = before['info']
    output.append('static const EFI_GRAPHICS_OUTPUT_MODE_INFORMATION GuRehearsalDeferredInfo={' +
                  ','.join(str(info[k])+'U' for k in ('version','width','height','pixel_format')) +
                  ',{'+','.join(f'0x{value:x}U' for value in info['masks'])+'},'+str(info['stride'])+'U};')
    output.append('static const UINTN GuRehearsalConsoleOffsets[5]={' +
                  ','.join(f'0x{offset:x}U' for offset in profile['console_offsets']) + '};')
    for role, title in (('pci', 'Pci'), ('gop', 'Gop'), ('edid', 'Edid'), ('console', 'Console')):
        endpoint = profile['endpoints'][role]
        if endpoint['path'] is not None:
            output.append(f'static const UINT8 GuRehearsal{title}Path[]={_bytes(endpoint["path"])};')
        output.append(f'static const EFI_GUID GuRehearsal{title}Protocols[]={{' +
                      ','.join(_guid(value) for value in endpoint['protocols']) + '};')
    output.append('static const GU_REHEARSAL_DRIVER_PIN GuRehearsalDrivers[3]={')
    for driver in profile['drivers']:
        fields = []
        for key, value in driver.items():
            rendered = _bytes(value) if key == 'file_path' else f'"{value}"' if key.endswith('sha256') else f'0x{value:x}U'
            fields.append(f'.{key}={rendered}')
        output.append('  {' + ','.join(fields) + '},')
    output.append('};')
    return '\n'.join(output) + '\n'
