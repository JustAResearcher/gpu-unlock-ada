# SPDX-License-Identifier: GPL-2.0-only
"""Offline observation tests; fixtures are synthetic, never uploaded captures.

Only the public logger's grammar and the reported topology shape are reproduced.
All handles, device paths' hashes, dimensions and counts below are invented.
No real GPU UUID, board identity, ROM hash, firmware binary or EDID is included.
"""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

WIN = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(WIN))
from analyze_diagnostic import analyze_report


DP = '09576e91-6d3f-11d2-8e39-00a0c969723b'
PCI = '4cf5b200-68b8-4ca5-9eec-b23e3f50029a'
GOP = '9042a9de-23dc-4a38-96fb-7aded080516a'
TEXT = '387477c2-69c7-11d2-8e39-00a0c969723b'
EDID = '1c0c34f6-d380-41fa-a049-8ad06c1a66aa'
ACTIVE = 'bd8c1056-9f36-44ec-92a8-a6337f817986'
UNKNOWN = '12345678-1234-5678-9123-456789abcdef'
UNSUPPORTED = '0x8000000000000003'


def path_lines(prefix: str) -> list[str]:
    return [
        f'{prefix}[0] offset=0 type=2 subtype=1 bytes=12',
        f'{prefix}[1] offset=12 type=1 subtype=1 bytes=6',
        f'{prefix}[2] offset=18 type=1 subtype=1 bytes=6',
        f'{prefix}[3] offset=24 type=2 subtype=3 bytes=8',
        f'{prefix}[4] offset=32 type=127 subtype=255 bytes=4',
    ]


def child_lines(handle: int, record: int, protocols: list, physical: bool) -> list[str]:
    lines = [
        f'child-detail handle={handle:#x} console_out=0 console_in=0 stderr=0 observational_only=1',
        f'child-detail protocol_status=0 protocol_count={len(protocols)}',
    ]
    for index, (guid, name, opens) in enumerate(protocols):
        lines.extend([
            f'child-protocol[{index}] guid={guid}',
            f'child-protocol[{index}] name={name}',
            f'child-open protocol_index={index} status=0 records={len(opens)}',
        ])
        for number, (attributes, count, agent, controller) in enumerate(opens):
            lines.append(
                f'child-open[{index}:{number}] attributes={attributes:#x} count={count} '
                f'agent={agent:#x} controller={controller:#x}')
    lines.append('child-detail path_status=0 path_present=1')
    lines.extend(path_lines('child-path'))
    lines.append(f'child-detail path_bytes=36 path_sha256={"a" * 64 if physical else "b" * 64}')
    if physical:
        lines.extend([
            f'child-gop handle={handle:#x} status=0 present=1 mode_present=1',
            'child-gop mode_info=1 info_bytes=36 framebuffer=0x100000000 framebuffer_bytes=1920000',
            'child-gop width=800 height=600 pixel_format=1 pixels_per_scanline=800',
            'child-path lookup_status=0 path_present=1 locate_available=1',
        ])
        lines.extend(path_lines('child-check-path'))
        lines.extend([
            'child-parent locate_status=0 parent=0x100 target=0x100',
            f'display child: record={record} handle={handle:#x} agent=0x200 status=0',
        ])
    else:
        lines.extend([
            f'child-gop handle={handle:#x} status={UNSUPPORTED} present=0 mode_present=0',
            f'child-check handle={handle:#x} reason=gop-protocol status={UNSUPPORTED}',
            f'display child: record={record} handle={handle:#x} agent=0x200 status={UNSUPPORTED}',
        ])
    return lines


def synthetic_report(*, auxiliary_protocols=None, include_auxiliary=True, ending='\n') -> bytes:
    """One physical console output plus a sibling with passive EDID metadata."""
    lines = [
        'gpu-unlock-ada Windows preboot port 0.4.1',
        'RUN00',
        'PROFILE_TAG=diagnostic-only mode=diagnostic',
        'DIAGNOSTIC_ONLY: GPU writes, firmware execution, reset and driver detach are disabled.',
        'GOP physical=1 target=1 expect_detached=0 status=0',
        'CHECK stage=display status=0',
        f'ownership: records={5 if include_auxiliary else 4} exclusive=0 status=0',
    ]
    records = [
        (DP, 0x1, 7, 0x900, 0),
        (DP, 0x8, 1, 0x200, 0x300),
        (DP, 0x8, 1, 0x200, 0x400),
        (PCI, 0x10, 1, 0x200, 0x100),
        (PCI, 0x2, 2, 0x999, 0x100),
    ]
    if not include_auxiliary:
        del records[2]
    for index, (guid, attrs, count, agent, controller) in enumerate(records):
        lines.extend([
            f'owner[{index}] protocol={guid[:8]} attributes={attrs:#x} count={count} '
            f'agent={agent:#x} controller={controller:#x}',
            f'owner-protocol[{index}] guid={guid}',
        ])
    physical = [
        (TEXT, 'SIMPLE_TEXT_OUT', [(1, 2, 0x900, 0), (0x10, 1, 0x500, 0x300),
                                  (8, 1, 0x500, 0x700)]),
        (GOP, 'GOP', [(1, 4, 0x900, 0), (0x10, 1, 0x600, 0x300)]),
        (ACTIVE, 'OTHER', []),
        (EDID, 'OTHER', [(2, 1, 0x200, 0x300)]),
        (DP, 'DEVICE_PATH', [(1, 5, 0x900, 0), (4, 2, 0x999, 0x300)]),
    ]
    auxiliary = auxiliary_protocols if auxiliary_protocols is not None else [
        (EDID, 'OTHER', []),
        (DP, 'DEVICE_PATH', [(1, 5, 0x900, 0), (4, 2, 0x999, 0x400),
                             (2, 2, 0x100, 0x400)]),
    ]
    lines.extend(child_lines(0x300, 1, physical, True))
    if include_auxiliary:
        lines.extend(child_lines(0x400, 2, auxiliary, False))
        lines.extend([
            f'CHECK stage=owner-observe status={UNSUPPORTED}',
            f'FINAL mode=diagnostic status={UNSUPPORTED} stage=owner-observe gpu_writes=0',
        ])
    else:
        lines.extend([
            'release candidate: agent=0x200 image=0xa00 source=0x100 base=0x20000000 '
            'bytes=4096 version=0x10 code_type=3 children=1',
            'CHECK stage=owner-observe status=0',
            'DIAGNOSTIC_COMPLETE eligible=1 gpu_writes=0',
            'FINAL mode=diagnostic status=0 stage=complete gpu_writes=0',
        ])
    return (ending.join(lines) + ending).encode('ascii')


def replace_once(data: bytes, old: str, new: str) -> bytes:
    needle = old.encode('ascii')
    if data.count(needle) != 1:
        raise AssertionError(f'mutation must match exactly once: {old!r}')
    return data.replace(needle, new.encode('ascii'), 1)


class DiagnosticAnalysisTests(unittest.TestCase):
    def test_two_child_capture_classifies_observations_never_authorizes(self):
        data = synthetic_report()
        result = analyze_report(data)
        self.assertEqual(result['schema_version'], 1)
        self.assertEqual(result['source_sha256'], hashlib.sha256(data).hexdigest())
        self.assertEqual(result['report_version'], '0.4.1')
        self.assertIs(result['unlock_authorized'], False)
        self.assertIs(result['dependency_graph_complete'], False)
        self.assertEqual(result['eligibility'], 'not-established')
        self.assertEqual(result['final']['gpu_writes'], 0)
        self.assertEqual(result['final']['status'], int(UNSUPPORTED, 16))
        self.assertEqual(result['final']['stage'], 'owner-observe')
        self.assertEqual(result['target']['handle'], 0x100)
        self.assertEqual(result['target']['owner_agent'], 0x200)
        children = {child['handle']: child for child in result['children']}
        self.assertEqual(set(children), {0x300, 0x400})
        self.assertEqual(children[0x300]['classification'], 'physical-gop-output')
        self.assertEqual(children[0x400]['classification'], 'passive-edid-output')
        self.assertTrue(result['blockers'])

    def test_successful_diagnostic_eligible_marker_never_authorizes_unlock(self):
        result = analyze_report(synthetic_report(include_auxiliary=False))
        self.assertEqual(result['final']['status'], 0)
        self.assertEqual(result['final']['stage'], 'complete')
        self.assertIs(result['unlock_authorized'], False)
        self.assertIs(result['dependency_graph_complete'], False)
        self.assertEqual(result['eligibility'], 'not-established')
        self.assertTrue(result['blockers'])

    def test_no_direct_console_handle_does_not_hide_downstream_consumers(self):
        result = analyze_report(synthetic_report())
        physical = next(child for child in result['children'] if child['handle'] == 0x300)
        consumers = physical['blocking_consumers']
        self.assertGreaterEqual(len(consumers), 2)
        serialized = json.dumps(consumers)
        self.assertIn(TEXT, serialized)
        self.assertIn(GOP, serialized)
        self.assertIs(result['unlock_authorized'], False)

    def test_auxiliary_parentage_is_not_invented(self):
        result = analyze_report(synthetic_report())
        auxiliary = next(child for child in result['children'] if child['handle'] == 0x400)
        # A parent BY_CHILD relationship was logged, but the diagnostic stopped
        # before LocateDevicePath for this child. Its path hash is not ancestry.
        self.assertIsNone(auxiliary['path']['resolved_parent'])
        self.assertIs(auxiliary['path']['validated_pci_ancestry'], False)
        self.assertIs(auxiliary['path']['hash_verified'], False)
        self.assertIs(auxiliary['edid_contents_captured'], False)
        self.assertEqual(auxiliary['physical_connectedness'], 'unknown')
        self.assertIs(result['unlock_authorized'], False)

    def test_crlf_input_is_supported_and_hashes_original_bytes(self):
        data = synthetic_report(ending='\r\n')
        result = analyze_report(data)
        self.assertEqual(result['source_sha256'], hashlib.sha256(data).hexdigest())
        self.assertEqual([x['classification'] for x in result['children']],
                         ['physical-gop-output', 'passive-edid-output'])

    def test_passive_reader_counts_do_not_turn_into_unlock_permission(self):
        data = replace_once(synthetic_report(),
                            'owner[0] protocol=09576e91 attributes=0x1 count=7',
                            'owner[0] protocol=09576e91 attributes=0x1 count=11')
        result = analyze_report(data)
        self.assertIs(result['unlock_authorized'], False)
        self.assertTrue(result['blockers'])

    def test_unknown_protocol_is_preserved_and_cannot_be_passive_edid(self):
        protocols = [(EDID, 'OTHER', []), (DP, 'DEVICE_PATH', []), (UNKNOWN, 'OTHER', [])]
        result = analyze_report(synthetic_report(auxiliary_protocols=protocols))
        child = next(x for x in result['children'] if x['handle'] == 0x400)
        self.assertNotEqual(child['classification'], 'passive-edid-output')
        self.assertIn(UNKNOWN, json.dumps(child['protocols']))
        self.assertTrue(result['blockers'])
        self.assertIs(result['unlock_authorized'], False)

    def test_friendly_protocol_name_cannot_spoof_the_edid_guid(self):
        protocols = [(UNKNOWN, 'EDID_DISCOVERED', []), (DP, 'DEVICE_PATH', [])]
        try:
            result = analyze_report(synthetic_report(auxiliary_protocols=protocols))
        except ValueError:
            return
        child = next(x for x in result['children'] if x['handle'] == 0x400)
        self.assertNotEqual(child['classification'], 'passive-edid-output')
        self.assertTrue(result['blockers'])
        self.assertIs(result['unlock_authorized'], False)

    def test_active_or_unknown_open_on_edid_child_never_classifies_passive(self):
        for attributes in (0x3, 0x10, 0x8, 0x20, 0x30, 0x80):
            with self.subTest(attributes=attributes):
                protocols = [(EDID, 'OTHER', []),
                             (DP, 'DEVICE_PATH', [(attributes, 1, 0x999, 0x400)])]
                try:
                    result = analyze_report(synthetic_report(auxiliary_protocols=protocols))
                except ValueError:
                    continue
                child = next(x for x in result['children'] if x['handle'] == 0x400)
                self.assertNotEqual(child['classification'], 'passive-edid-output')
                self.assertTrue(result['blockers'])
                self.assertIs(result['unlock_authorized'], False)

    def test_edid_active_without_gop_does_not_mean_passive_output(self):
        result = analyze_report(synthetic_report(auxiliary_protocols=[
            (EDID, 'OTHER', []), (DP, 'DEVICE_PATH', []), (ACTIVE, 'OTHER', [])]))
        child = next(x for x in result['children'] if x['handle'] == 0x400)
        self.assertNotEqual(child['classification'], 'passive-edid-output')
        self.assertIs(result['unlock_authorized'], False)

    def test_gop_query_error_or_returned_interface_does_not_prove_absence(self):
        original = f'child-gop handle=0x400 status={UNSUPPORTED} present=0 mode_present=0'
        for status, present in ((UNSUPPORTED, 1), ('0x8000000000000007', 0),
                                ('0x8000000000000009', 0)):
            with self.subTest(status=status, present=present):
                changed = (f'child-gop handle=0x400 status={status} '
                           f'present={present} mode_present=0')
                result = analyze_report(replace_once(synthetic_report(), original, changed))
                child = next(x for x in result['children'] if x['handle'] == 0x400)
                self.assertEqual(child['classification'], 'unclassified')
                self.assertIn('gop-absence-unestablished',
                              [blocker['code'] for blocker in result['blockers']])
                self.assertIs(result['unlock_authorized'], False)

    def test_truncation_at_each_topology_line_is_rejected(self):
        data = synthetic_report()
        lines = data.splitlines(keepends=True)
        start = next(i for i, line in enumerate(lines) if line.startswith(b'ownership:'))
        for cut in range(start, len(lines)):
            with self.subTest(cut=cut):
                with self.assertRaises(ValueError):
                    analyze_report(b''.join(lines[:cut]))

    def test_duplicate_owned_records_protocols_or_child_sections_are_rejected(self):
        data = synthetic_report()
        lines = data.decode('ascii').splitlines(keepends=True)
        tokens = ('owner[0]', 'owner-protocol[0]', 'child-protocol[0] guid=',
                  'child-open[0:0]', 'child-detail path_bytes=', 'display child: record=1',
                  'CHECK stage=owner-observe', 'FINAL mode=diagnostic')
        for token in tokens:
            with self.subTest(token=token):
                index = next(i for i, line in enumerate(lines) if line.startswith(token))
                duplicate = lines[:index] + [lines[index]] + lines[index:]
                with self.assertRaises(ValueError):
                    analyze_report(''.join(duplicate).encode('ascii'))
        start = data.index(b'child-detail handle=0x300')
        end = data.index(b'child-detail handle=0x400')
        with self.assertRaises(ValueError):
            analyze_report(data[:end] + data[start:end] + data[end:])

    def test_reordered_owned_records_and_children_are_rejected(self):
        data = synthetic_report()
        text = data.decode('ascii')
        start = text.index('owner[0]')
        middle = text.index('owner[1]')
        end = text.index('owner[2]')
        with self.assertRaises(ValueError):
            analyze_report((text[:start] + text[middle:end] + text[start:middle] + text[end:]).encode())
        start = text.index('child-detail handle=0x300')
        middle = text.index('child-detail handle=0x400')
        end = text.index('CHECK stage=owner-observe')
        with self.assertRaises(ValueError):
            analyze_report((text[:start] + text[middle:end] + text[start:middle] + text[end:]).encode())

    def test_zero_active_handles_are_rejected_even_when_cross_references_agree(self):
        for handle in (b'0x100', b'0x200', b'0x400'):
            with self.subTest(handle=handle):
                data = re.sub(rb'\b' + handle + rb'\b', b'0x0', synthetic_report())
                with self.assertRaises(ValueError):
                    analyze_report(data)

    def test_topology_records_after_final_are_not_silently_ignored(self):
        data = synthetic_report()
        for suffix in (
                b'owner[0] protocol=09576e91 attributes=0x1 count=1 agent=0x900 controller=0\n',
                b'child-detail handle=0x400 console_out=0 console_in=0 stderr=0 observational_only=1\n',
                b'ownership: records=0 exclusive=0 status=0\n'):
            with self.subTest(suffix=suffix):
                with self.assertRaises(ValueError):
                    analyze_report(data + suffix)

    def test_owner_checks_cannot_be_hidden_in_hardware_preamble(self):
        for marker in (
                'CHECK stage=owner-observe status=0',
                f'CHECK stage=owner-observe status={UNSUPPORTED}',
                'DIAGNOSTIC_COMPLETE eligible=1 gpu_writes=0',
                'FINAL mode=diagnostic status=0 stage=complete gpu_writes=0'):
            with self.subTest(marker=marker):
                data = synthetic_report().replace(b'ownership:',
                    marker.encode('ascii') + b'\nownership:', 1)
                with self.assertRaises(ValueError):
                    analyze_report(data)

    def test_identity_index_and_status_corruption_is_rejected(self):
        mutations = [
            ('owner-protocol[0] guid='+DP, 'owner-protocol[0] guid='+PCI),
            ('owner[0] protocol=09576e91', 'owner[7] protocol=09576e91'),
            ('owner-protocol[4] guid='+PCI, 'owner-protocol[3] guid='+PCI),
            ('ownership: records=5', 'ownership: records=6'),
            ('ownership: records=5 exclusive=0 status=0',
             'ownership: records=5 exclusive=1 status=0'),
            ('ownership: records=5 exclusive=0 status=0',
             'ownership: records=5 exclusive=0 status=0x8000000000000007'),
            ('child-detail handle=0x400 ', 'child-detail handle=0x401 '),
            ('display child: record=2 handle=0x400', 'display child: record=1 handle=0x400'),
            ('display child: record=2 handle=0x400 agent=0x200',
             'display child: record=2 handle=0x400 agent=0x201'),
            ('child-gop handle=0x400 status='+UNSUPPORTED+' present=0 mode_present=0',
             'child-gop handle=0x400 status=0 present=0 mode_present=0'),
            ('child-check handle=0x400 reason=gop-protocol status='+UNSUPPORTED,
             'child-check handle=0x400 reason=gop-protocol status=0'),
            ('CHECK stage=owner-observe status='+UNSUPPORTED,
             'CHECK stage=owner-observe status=0'),
            ('FINAL mode=diagnostic status='+UNSUPPORTED+' stage=owner-observe gpu_writes=0',
             'FINAL mode=diagnostic status=0 stage=complete gpu_writes=0'),
            ('FINAL mode=diagnostic status='+UNSUPPORTED+' stage=owner-observe gpu_writes=0',
             'FINAL mode=diagnostic status='+UNSUPPORTED+' stage=owner-observe gpu_writes=1'),
        ]
        for old, new in mutations:
            with self.subTest(old=old, new=new):
                with self.assertRaises(ValueError):
                    analyze_report(replace_once(synthetic_report(), old, new))

    def test_metadata_count_and_size_limits_are_enforced(self):
        mutations = [
            ('ownership: records=5', 'ownership: records=4294967295'),
            ('child-detail protocol_status=0 protocol_count=2',
             'child-detail protocol_status=0 protocol_count=3'),
            ('child-detail protocol_status=0 protocol_count=2',
             'child-detail protocol_status=0 protocol_count=4294967295'),
            ('child-detail protocol_status=0 protocol_count=2',
             'child-detail protocol_status=0x8000000000000007 protocol_count=2'),
            ('child-open protocol_index=1 status=0 records=2',
             'child-open protocol_index=1 status=0 records=4294967295'),
            ('child-detail path_bytes=36 path_sha256='+'b'*64,
             'child-detail path_bytes=35 path_sha256='+'b'*64),
            ('child-detail path_bytes=36 path_sha256='+'b'*64,
             'child-detail path_bytes=5000 path_sha256='+'b'*64),
            ('child-detail path_bytes=36 path_sha256='+'b'*64,
             'child-detail path_bytes=36 path_sha256='+'b'*63),
            ('owner[0] protocol=09576e91 attributes=0x1 count=7 agent=0x900',
             'owner[0] protocol=09576e91 attributes=0x1 count=7 agent=0x10000000000000000'),
        ]
        for old, new in mutations:
            with self.subTest(old=old, new=new):
                with self.assertRaises(ValueError):
                    analyze_report(replace_once(synthetic_report(), old, new))

    def test_auxiliary_device_path_structure_cannot_be_fabricated(self):
        data = synthetic_report()
        split = data.index(b'child-detail handle=0x400 ')
        prefix, child = data[:split], data[split:]
        mutations = [
            ('child-path[3] offset=24 type=2 subtype=3 bytes=8',
             'child-path[3] offset=24 type=2 subtype=3 bytes=0'),
            ('child-path[3] offset=24 type=2 subtype=3 bytes=8',
             'child-path[3] offset=25 type=2 subtype=3 bytes=8'),
            ('child-path[4] offset=32 type=127 subtype=255 bytes=4',
             'child-path[4] offset=32 type=127 subtype=1 bytes=4'),
            ('child-path[4] offset=32 type=127 subtype=255 bytes=4',
             'child-path[4] offset=32 type=127 subtype=255 bytes=5'),
            ('child-path[4] offset=32 type=127 subtype=255 bytes=4\n', ''),
        ]
        for old, new in mutations:
            with self.subTest(old=old, new=new):
                with self.assertRaises(ValueError):
                    analyze_report(prefix + replace_once(child, old, new))

    def test_contradictory_successful_gop_geometry_is_not_trusted(self):
        mutations = [
            ('framebuffer=0x100000000 framebuffer_bytes=1920000',
             'framebuffer=0 framebuffer_bytes=1920000'),
            ('width=800 height=600 pixel_format=1 pixels_per_scanline=800',
             'width=800 height=0 pixel_format=1 pixels_per_scanline=800'),
            ('width=800 height=600 pixel_format=1 pixels_per_scanline=800',
             'width=800 height=600 pixel_format=1 pixels_per_scanline=799'),
            ('mode_info=1 info_bytes=36', 'mode_info=1 info_bytes=1'),
            ('framebuffer_bytes=1920000', 'framebuffer_bytes=4'),
        ]
        for old, new in mutations:
            with self.subTest(old=old, new=new):
                try:
                    result = analyze_report(replace_once(synthetic_report(), old, new))
                except ValueError:
                    continue
                child = next(x for x in result['children'] if x['handle'] == 0x300)
                self.assertNotEqual(child['classification'], 'physical-gop-output')
                self.assertTrue(result['blockers'])
                self.assertIs(result['unlock_authorized'], False)

    def test_non_ascii_nul_and_oversized_reports_are_rejected(self):
        for data in (b'\xff'+synthetic_report(), synthetic_report()+b'\x00', b'x'*(1024*1024)):
            with self.subTest(length=len(data)):
                with self.assertRaises(ValueError):
                    analyze_report(data)

    def test_cli_json_is_offline_and_does_not_modify_report(self):
        with tempfile.TemporaryDirectory() as temporary:
            report = Path(temporary) / 'synthetic.txt'
            data = synthetic_report()
            report.write_bytes(data)
            result = subprocess.run([sys.executable, str(WIN/'analyze_diagnostic.py'),
                                     str(report), '--json'],
                                    check=True, capture_output=True, text=True, timeout=5)
            value = json.loads(result.stdout)
            self.assertIs(value['unlock_authorized'], False)
            self.assertEqual(report.read_bytes(), data)
            self.assertEqual(list(Path(temporary).iterdir()), [report])


if __name__ == '__main__':
    unittest.main()
