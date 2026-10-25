# SPDX-License-Identifier: GPL-2.0-only
"""Strict rehearsal profile tests use only invented capture data.

The grammar and public protocol GUIDs are real; addresses, firmware-file GUIDs,
ROM/code hashes, and capture inventories below are synthetic. No uploaded log
or firmware code is embedded in these tests.
"""
from __future__ import annotations

import hashlib
import copy
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest

WIN = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(WIN))
from board_profile import parse_report, header as board_header
from rehearsal_profile import (parse_capture, rehearsal_header, bind_restoration_report, validate_restoration_evidence, PCI, PATH, GOP,
                               TEXT, INPUT, EDID, ACTIVE_EDID, ROOT_BRIDGE)


def fixture() -> tuple[bytes, bytes]:
    """A stable two-output, three-driver synthetic firmware inventory."""
    pci, gop, edid, console = 0x100, 0x200, 0x300, 0x400
    out, gfx, nv, app = 0x500, 0x600, 0x700, 0x800
    root, bus, keyboard, keydriver = 0x900, 0xa00, 0xb00, 0xc00
    fields = dict(version=1, id='270510de', subsystem='e1301462', segment=0,
                  bus=1, device=0, function=0, bar_base='83000000', bar_size='1000000',
                  chip='193000a1', vram_mib=16376, prom_size=1048576, prom_sha256='11'*32,
                  rom_size=150528, rom_sha256='22'*32, fwsec_parsed=1)
    report = ['gpu-unlock-ada Windows preboot port 0.4.2', 'RUN01',
              'PROFILE_TAG=diagnostic-only mode=diagnostic',
              'DIAGNOSTIC_ONLY: GPU writes, firmware execution, reset and driver detach are disabled.']
    config = [0] * 64
    config[0], config[1], config[2], config[4], config[11] = 0x270510de, 3, 0x030000a1, 0x83000000, 0xe1301462
    for index in range(0, 64, 4):
        report.append(f'config[{index*4:02x}]=' + ' '.join(f'{v:08x}' for v in config[index:index+4]))
    report.append('PROFILE ' + ' '.join(f'{key}={value}' for key, value in fields.items()))
    for stage in ('target', 'bar0', 'cold-state', 'prom', 'firmware-capture', 'display'):
        report.append(f'CHECK stage={stage} status=0')
    report += ['CHECK stage=owner-observe status=0x8000000000000003',
               'FINAL mode=diagnostic status=0x8000000000000003 stage=owner-observe gpu_writes=0']
    rows = ['CAPTURE_FORMAT version=1 diagnostic=0.4.2 read_only=1 unlock_authorized=0',
            f'TARGET handle={pci:#x} application={app:#x}']
    edges = [(root, bus, root, ROOT_BRIDGE, 16), (root, bus, pci, ROOT_BRIDGE, 8),
             (pci, nv, pci, PCI, 16), (pci, nv, gop, PATH, 8), (pci, nv, edid, PATH, 8),
             (gop, out, gop, TEXT, 16), (gop, out, console, TEXT, 8), (gop, gfx, gop, GOP, 16),
             (keyboard, keydriver, keyboard, INPUT, 16), (keyboard, keydriver, console, INPUT, 8)]

    def graph(phase):
        rows.append(f'GRAPH phase={phase} status=0 handles=20 protocols=50 opens=60 active_edges={len(edges)}')
        for index, (handle, agent, controller, protocol, attributes) in enumerate(edges):
            rows.append(f'EDGE phase={phase} index={index} handle={handle:#x} agent={agent:#x} controller={controller:#x} attributes={attributes:#x} count=1')
            rows.append(f'GUID kind=edge index={index} value={protocol}')

    def path(kind, handle, value):
        rows.append(f'HASH kind={kind} handle={handle:#x} bytes={len(value)} sha256={hashlib.sha256(value).hexdigest()}')
        for offset in range(0, len(value), 32):
            rows.append(f'PATH_BYTES kind={kind} handle={handle:#x} offset={offset} hex={value[offset:offset+32].hex()}')

    graph(0)
    rows += ['MEMORY_MAP status=0 bytes=480 stride=48 version=1',
             f'CONSOLE out={console:#x} in={console:#x} error={console:#x} text=0x10000 keys=0x11000 stderr=0x10000',
             'CONSOLE_CALLBACK kind=0 reset=0x2000280 output=0x2000290 set_mode=0x20002a0',
             'CONSOLE_CALLBACK kind=1 reset=0x2000280 output=0x2000290 set_mode=0x20002a0',
             'CONSOLE_CALLBACK kind=2 reset=0x20002b0 read_key=0x20002c0 wait=0x12000']
    pci_path = bytes.fromhex('02010c00d041030a000000000101060000010101060000007fff0400')
    gop_path = pci_path[:-4] + bytes.fromhex('02030800100301807fff0400')
    edid_path = pci_path[:-4] + bytes.fromhex('02030800300301807fff0400')
    endpoints = [(pci, [PCI, PATH], pci_path), (console, [TEXT, INPUT], None),
                 (gop, [GOP, TEXT, PATH, EDID, ACTIVE_EDID], gop_path), (edid, [EDID, PATH], edid_path)]
    for handle, protocols, devicepath in endpoints:
        rows.append(f'ENDPOINT handle={handle:#x} status=0 protocols={len(protocols)}')
        for index, protocol in enumerate(protocols):
            rows.append(f'GUID kind=endpoint index={index} value={protocol}')
        if devicepath is None:
            rows.append(f'ENDPOINT_PATH handle={handle:#x} status=0x8000000000000003 present=0')
        else:
            rows.append(f'ENDPOINT_PATH handle={handle:#x} status=0 present=1')
            path('endpoint', handle, devicepath)
            rows.append(f'ENDPOINT_PCI handle={handle:#x} status=0 parent={pci:#x} target={pci:#x}')
    for index, agent in enumerate((out, gfx, nv)):
        base = 0x2000000 + index*0x100000
        image = agent if agent == nv else agent+1
        version = 0x70011 if agent == nv else 0x10
        source = pci if agent == nv else 0xd00
        file = (bytes.fromhex('04041c006e00760067006f0070002d006100640031007800780000007fff0400')
                if agent == nv else bytes.fromhex('04061400') + bytes([index+1])*16 + bytes.fromhex('7fff0400'))
        rows += [f'DRIVER agent={agent:#x} binding_status=0 binding={0x13000+index*0x100:#x}',
                 f'BINDING agent={agent:#x} handle={agent:#x} image={image:#x} version={version:#x} supported={base+0x220:#x} start={base+0x230:#x} stop={base+0x240:#x}',
                 f'DRIVER_IMAGE agent={agent:#x} status=0 interface={0x14000+index*0x100:#x}',
                 f'IMAGE agent={agent:#x} image={image:#x} device={source:#x} parent=0xe00 base={base:#x} bytes=4096 code_type=3 data_type=4']
        path('driver-file', agent, file)
        rows += [f'HASH kind=loaded-image handle={agent:#x} bytes=4096 sha256={"33"*32}',
                 f'PE agent={agent:#x} status=0 stop_offset=0x240 canonical_valid=1',
                 f'PE_METADATA_HASH agent={agent:#x} headers={"44"*32} relocations={"55"*32}',
                 f'PE_LAYOUT agent={agent:#x} preferred_base={base:#x} image_bytes=4096 headers=512 entry=0x210 reloc_rva=0xe00 reloc_bytes=16 relocations=1 executable_relocations=0 sections=3 stop_section=0',
                 f'SECTION agent={agent:#x} index=0 name=.text___ rva=0x200 bytes=2048 flags=0x60000020 executable=1 relocations=0',
                 f'SECTION_HASH agent={agent:#x} index=0 raw={"66"*32} canonical={"66"*32}',
                 f'SECTION agent={agent:#x} index=1 name=.data___ rva=0xa00 bytes=1024 flags=0xc0000040 executable=0 relocations=1',
                 f'SECTION agent={agent:#x} index=2 name=.reloc__ rva=0xe00 bytes=16 flags=0x40000040 executable=0 relocations=0']
    graph(1)
    rows.append('CAPTURE_END metadata_status=0 graph_first=0 graph_second=0 stable_active_graph=1 endpoints=4 drivers=3 gpu_writes=0 unlock_authorized=0')
    return ('\n'.join(report)+'\n').encode(), ('\n'.join(rows)+'\n').encode()


def restoration_fixture(profile=None) -> bytes:
    """Invented complete refusal report; no uploaded addresses or hashes."""
    report, topology = fixture()
    if profile is None: profile = parse_capture(report, topology)
    rows = ['gpu-unlock-ada Windows preboot port 0.5.1-rehearsal','RUN05',
            'PROFILE_TAG='+profile['board']['report_sha256']+' mode=firmware-rehearsal',
            'REHEARSAL_ONLY: active firmware driver callbacks; no unlock payload, direct GPU writes, DMA, FLR, flashing or persistent boot changes.',
            'SecureBoot query: status=0 bytes=1 value=0', 'CHECK stage=secure-boot status=0']
    rows += [row for row in report.decode().splitlines() if row.startswith(('PROFILE ','config['))]
    rows += ['CHECK stage='+stage+' status=0' for stage in ('target','bar0','cold-state','prom')]
    rows += ['GOP physical=1 target=1 expect_detached=0 status=0','CHECK stage=display status=0',
             'WINDOWS_PRELOAD internal_gpt_esp=1 candidates=1 bytes=4096 sha256='+'77'*32+' same_boot_pin=1',
             'CHECK stage=windows-preload status=0']
    def gop(stage):
        reason = 'baseline' if stage == 0 else 'zero-framebuffer-base'
        handle = '0x200' if stage == 0 else '0x250'
        mode, framebuffer, size = (0,'0x90000000',1920000) if stage == 0 else (3,'0',0)
        return [f'REHEARSAL_GOP_OBSERVATION stage={stage} reason={reason} handle={handle} expected={handle} original=0x200 console=0x400 interface=0x10000 readable=1',
                f'REHEARSAL_GOP_CALLBACKS stage={stage} query=0x2200260 set=0x2200270 blt=0x2200280 mode_ptr=0x11000 mode_readable=1',
                f'REHEARSAL_GOP_MODE stage={stage} max=2 mode={mode} info_ptr=0x12000 info_bytes=36 info_readable=1 framebuffer={framebuffer} framebuffer_bytes={size}',
                f'REHEARSAL_GOP_INFO stage={stage} version=0 width=800 height=600 format=1 stride=800 masks=0xff0000/0xff00/0xff/0xff000000']
    rows += gop(0)
    rows += ['REHEARSAL_PREPARE status=0 edges=11 drivers=3 outputs=2',
             'CHECK stage=rehearsal-preflight status=0',
             'READY: firmware release/restoration rehearsal; unlock payload is absent.',
             'Press R to release three boot-display drivers, reconnect them, then start Windows only after verification.',
             'This active firmware test may blank or hang the display. If Windows has not appeared after 2 minutes, power OFF, remove USB and restore security settings.',
             'Esc or 120 seconds cancels before release. No automatic retry.']
    rows += ['CHECK stage='+stage+' status=0' for stage in ('arming','watchdog','armed-config')]
    rows += ['PROFILE chip=193000a1 vram_mib=16376','CHECK stage=armed-cold status=0',
             'REHEARSAL_INTENT: R received; beginning staged release/restoration.']
    for stage,edges in ((1,9),(2,8),(3,5)):
        rows += [f'REHEARSAL_INTENT stage={stage} operation=disconnect',
                 f'REHEARSAL_RETURN stage={stage} status=0',
                 f'REHEARSAL_STAGE_VERIFIED stage={stage} edges={edges}']
    rows += ['REHEARSAL_INTENT stage=4 operation=connect','REHEARSAL_SUPPORTED stage=4 status=0',
             'REHEARSAL_RETURN stage=4 status=0',
             'REHEARSAL_DISPLAY_REFUSED stage=4 reason=zero-framebuffer-base status=0x8000000000000003']
    rows += gop(4)
    rows += ['REHEARSAL_VERIFY_FAILED stage=4 check=display status=0x8000000000000003',
             'FINAL mode=firmware-rehearsal status=0x8000000000000003 stage=release-and-restore mutated=1 unlock_attempted=0']
    return ('\n'.join(rows)+'\n').encode()


class RehearsalProfileTests(unittest.TestCase):
    def setUp(self):
        self.report, self.topology = fixture()

    def test_complete_observation_is_rehearsal_only(self):
        profile = parse_capture(self.report, self.topology)
        self.assertIs(profile['unlock_authorized'], False)
        self.assertIs(profile['rehearsal_hardware_tested'], False)
        self.assertEqual(profile['captured_active_edges'], 10)
        self.assertEqual([d['version'] for d in profile['drivers']], [0x10, 0x10, 0x70011])
        self.assertEqual(profile['topology_sha256'], hashlib.sha256(self.topology).hexdigest())
        with self.assertRaises(ValueError):
            parse_report(self.report)
        # Input handles, image load addresses and mutable-image hashes are not
        # included as reusable identity in the profile output.
        for driver in profile['drivers']:
            self.assertNotIn('agent', driver)
            self.assertNotIn('image', driver)
            self.assertNotIn('base', driver)
        self.assertNotIn('33'*32, json.dumps(profile))

    def test_crlf_is_normalized_without_changing_meaning(self):
        profile = parse_capture(self.report.replace(b'\n', b'\r\n'), self.topology.replace(b'\n', b'\r\n'))
        self.assertEqual(profile['board']['id'], 0x270510de)

    def test_iteration_order_is_not_an_ownership_change(self):
        rows = self.topology.decode().splitlines()
        start = next(i for i, row in enumerate(rows) if row.startswith('GRAPH phase=1 ')) + 1
        records = [rows[i:i+2] for i in range(start, start+20, 2)]
        reordered = []
        for index, record in enumerate(reversed(records)):
            reordered.extend(re.sub(r'index=[0-9]+', f'index={index}', row) for row in record)
        rows[start:start+20] = reordered
        profile = parse_capture(self.report, ('\n'.join(rows)+'\n').encode())
        self.assertEqual(profile['captured_active_edges'], 10)

    def test_report_prerequisites_and_scope(self):
        changes = [(b'0.4.2', b'0.4.1'), (b'firmware-capture status=0', b'firmware-capture status=7'),
                   (b'subsystem=e1301462', b'subsystem=51321462'), (b'bus=1', b'bus=2'),
                   (b'gpu_writes=0', b'gpu_writes=1'), (b'270510de 00000003', b'270510de 00000007'),
                   (b'rom_size=150528', b'rom_size=1'), (b'fwsec_parsed=1', b'fwsec_parsed=0'),
                   (b'chip=193000a1', b'chip=194000a1'), (b'config[10]', b'config[20]'),
                   (b'bar_base=83000000', b'bar_base=84000000'), (b'version=1', b'version=1 version=1')]
        for before, after in changes:
            with self.subTest(before=before), self.assertRaises(ValueError):
                self.assertIn(before, self.report)
                parse_capture(self.report.replace(before, after, 1), self.topology)

    def test_topology_terminal_and_bounds(self):
        changes = [(b'unlock_authorized=0', b'unlock_authorized=1'), (b'read_only=1', b'read_only=0'),
                   (b'metadata_status=0', b'metadata_status=1'), (b'stable_active_graph=1', b'stable_active_graph=0'),
                   (b'handles=20', b'handles=5000'), (b'active_edges=10', b'active_edges=9'),
                   (b'stride=48', b'stride=0'), (b'bytes=480 stride', b'bytes=481 stride'),
                   (b'kind=0 reset', b'kind=1 reset'), (b'error=0x400', b'error=0x401'),
                   (b'present=0', b'present=1'), (b'parent=0x100 target', b'parent=0x101 target'),
                   (b'offset=32 hex', b'offset=31 hex'), (b'executable_relocations=0', b'executable_relocations=1'),
                   (b'canonical_valid=1', b'canonical_valid=0'), (b'stop_offset=0x240', b'stop_offset=0x241'),
                   (b'image_bytes=4096', b'image_bytes=4097'), (b'sections=3', b'sections=2'),
                   (b'stop_section=0', b'stop_section=1'), (b'rva=0xa00', b'rva=0x900'),
                   (b'raw='+b'66'*32, b'raw='+b'77'*32), (b'supported=0x2000220', b'supported=0x2001000'),
                   (b'code_type=3', b'code_type=1'), (b'version=0x70011', b'version=0x7000b'),
                   (b'index=0 name=.text___', b'index=1 name=.text___')]
        for before, after in changes:
            with self.subTest(before=before), self.assertRaises(ValueError):
                self.assertIn(before, self.topology)
                parse_capture(self.report, self.topology.replace(before, after, 1))

    def test_truncated_extra_unknown_duplicate_records(self):
        for data in (self.topology[:-1], self.topology[:100], self.topology+b'EXTRA x=1\n',
                     self.topology+self.topology, b'\x00'+self.topology,
                     self.topology.replace(b'TARGET handle=', b'TARGET unexpected=1 handle=', 1),
                     self.topology.replace(b'count=1\n', b'count=1 count=1\n', 1),
                     self.topology.replace(b'CAPTURE_END ', b'UNRECOGNIZED ', 1)):
            with self.subTest(data=data[:30]), self.assertRaises(ValueError):
                parse_capture(self.report, data)

    def test_complete_graph_disagrees_even_when_end_says_stable(self):
        old = b'EDGE phase=1 index=9 handle=0xb00 agent=0xc00 controller=0x400'
        changed = self.topology.replace(old, old.replace(b'0xc00', b'0xc01'))
        with self.assertRaisesRegex(ValueError, 'graph changed'):
            parse_capture(self.report, changed)

    def test_missing_keyboard_relationship_is_rejected(self):
        data = self.topology.replace(b'phase=1 index=9 handle=0xb00', b'phase=1 index=9 handle=0xb01')
        with self.assertRaises(ValueError):
            parse_capture(self.report, data)

    def test_unknown_protected_consumer_even_in_both_snapshots(self):
        data = self.topology.replace(b'handle=0x200 agent=0x600 controller=0x200 attributes=0x10',
                                     b'handle=0x300 agent=0x600 controller=0x300 attributes=0x10')
        with self.assertRaises(ValueError):
            parse_capture(self.report, data)

    def test_duplicate_graph_relationship_is_not_hidden_by_count(self):
        # Replace one complete input relationship with another differing only
        # in OpenCount; duplicate-key rejection must precede graph comparison.
        data = self.topology.replace(b'handle=0xb00 agent=0xc00 controller=0x400 attributes=0x8 count=1',
                                     b'handle=0xb00 agent=0xc00 controller=0xb00 attributes=0x10 count=2')
        with self.assertRaisesRegex(ValueError, 'duplicate graph'):
            parse_capture(self.report, data)

    def test_all_record_boundary_truncations_are_refused(self):
        offset = 0
        for line in self.topology.splitlines(keepends=True)[:-1]:
            offset += len(line)
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                parse_capture(self.report, self.topology[:offset])

    def test_path_payload_does_not_match_hash(self):
        data = self.topology.replace(b'hex=02010c00d041030a', b'hex=02010c00d041030b', 1)
        with self.assertRaisesRegex(ValueError, 'digest mismatch'):
            parse_capture(self.report, data)

    def test_invalid_path_even_with_matching_hash(self):
        old = bytes.fromhex('02010c00d041030a000000000101060000010101060000007fff0400')
        new = b'\x02\x01\xff\xff' + old[4:]
        data = self.topology.replace(old.hex().encode(), new.hex().encode())
        data = data.replace(hashlib.sha256(old).hexdigest().encode(), hashlib.sha256(new).hexdigest().encode())
        with self.assertRaisesRegex(ValueError, 'device path node'):
            parse_capture(self.report, data)

    def test_generated_headers_compile_and_contain_no_unlock_enable(self):
        profile = parse_capture(self.report, self.topology)
        profile = bind_restoration_report(profile, restoration_fixture(profile))
        header = rehearsal_header(profile)
        self.assertNotIn('GU_ENABLE_UNLOCK', header)
        self.assertNotIn('0x2000000', header)
        self.assertIn('GuRehearsalConsoleProtocols', header)
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root/'rehearsal_profile.h').write_text(header)
            (root/'board_profile.h').write_text(board_header(profile['board']))
            (root/'test.c').write_text('#include "rehearsal_profile.h"\n#include "board_profile.h"\nint main(void) {return GuRehearsalDrivers[2].version != 0x70011;}\n')
            result = subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-fshort-wchar',
                                     '-I'+str(WIN/'deps/edk2'), '-I'+str(WIN/'deps/edk2/X64'),
                                     str(root/'test.c'), '-o', str(root/'test')], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
            self.assertEqual(subprocess.run([str(root/'test')]).returncode, 0)


class RestorationEvidenceTests(unittest.TestCase):
    def setUp(self):
        self.profile = parse_capture(*fixture())
        self.report = restoration_fixture(self.profile)

    def test_complete_refusal_binds_only_the_observed_state(self):
        result = bind_restoration_report(self.profile,self.report)
        e = result['restoration_evidence']
        self.assertNotIn('restoration_evidence',self.profile)
        self.assertEqual(e['report_sha256'],hashlib.sha256(self.report).hexdigest())
        self.assertEqual(e['profile_tag'],self.profile['board']['report_sha256'])
        self.assertEqual(e['baseline']['mode'],0)
        self.assertEqual(e['deferred']['mode'],3)
        self.assertEqual(e['deferred']['framebuffer_base'],0)
        self.assertEqual(e['deferred']['info'],e['baseline']['info'])
        self.assertNotIn('query',json.dumps(e))
        self.assertNotIn('handle',json.dumps(e))
        self.assertIn('GuRehearsalDeferredEnabled=TRUE',rehearsal_header(result))
        self.assertIn('GuRehearsalDeferredMode=0x3ULL',rehearsal_header(result))
        with self.assertRaises(ValueError): rehearsal_header(self.profile)
        with self.assertRaises(ValueError): bind_restoration_report(result,self.report)

    def test_restoration_crlf_preserves_values_but_binds_exact_bytes(self):
        changed = self.report.replace(b'\n',b'\r\n')
        e = bind_restoration_report(self.profile,changed)['restoration_evidence']
        self.assertEqual(e['report_sha256'],hashlib.sha256(changed).hexdigest())
        self.assertEqual(e['baseline']['max_mode'],2)

    def test_scope_order_and_prerequisites_cannot_be_bypassed(self):
        changes = [(b'0.5.1-rehearsal',b'0.5.0-rehearsal'),
                   (b'SecureBoot query: status=0 bytes=1 value=0',b'SecureBoot query: status=0 bytes=1 value=1'),
                   (b'CHECK stage=arming status=0',b'CHECK stage=arming status=1'),
                   (b'CHECK stage=armed-cold status=0',b'CHECK stage=armed-config status=0'),
                   (b'REHEARSAL_PREPARE status=0 edges=11',b'REHEARSAL_PREPARE status=0 edges=10'),
                   (b'REHEARSAL_STAGE_VERIFIED stage=2 edges=8',b'REHEARSAL_STAGE_VERIFIED stage=2 edges=9'),
                   (b'REHEARSAL_SUPPORTED stage=4 status=0',b'REHEARSAL_SUPPORTED stage=4 status=1'),
                   (b'REHEARSAL_RETURN stage=4 status=0',b'REHEARSAL_RETURN stage=4 status=1'),
                   (b'unlock_attempted=0',b'unlock_attempted=1'),
                   (b'mutated=1',b'mutated=0'), (b'GPU writes',b'unrecognized'),
                   (b'270510de 00000003',b'270510de 00000007'),
                   (b'chip=193000a1',b'chip=193000a2'),
                   (b'PROFILE_TAG='+self.profile['board']['report_sha256'].encode(),b'PROFILE_TAG='+b'ff'*32)]
        for before,after in changes:
            with self.subTest(before=before),self.assertRaises(ValueError):
                self.assertIn(before,self.report)
                bind_restoration_report(self.profile,self.report.replace(before,after,1))

    def test_display_evidence_must_have_exact_deferred_shape(self):
        changes = [(b'stage=4 query=0x2200260',b'stage=4 query=0x2200261'),
                   (b'expected=0x250',b'expected=0x251'),
                   (b'original=0x200 console=0x400',b'original=0x201 console=0x400'),
                   (b'stage=4 max=2 mode=3',b'stage=4 max=2 mode=2'),
                   (b'stage=4 max=2',b'stage=4 max=3'),
                   (b'stage=0 max=2 mode=0',b'stage=0 max=2 mode=2'),
                   (b'framebuffer=0 framebuffer_bytes=0',b'framebuffer=1 framebuffer_bytes=0'),
                   (b'framebuffer=0 framebuffer_bytes=0',b'framebuffer=0 framebuffer_bytes=1'),
                   (b'framebuffer_bytes=1920000',b'framebuffer_bytes=100'),
                   (b'info_bytes=36',b'info_bytes=35'),
                   (b'info_readable=1',b'info_readable=0'),
                   (b'stage=4 version=0 width=800',b'stage=4 version=0 width=799'),
                   (b'format=1',b'format=3'),
                   (b'stride=800',b'stride=799'),
                   (b'masks=0xff0000/0xff00/0xff/0xff000000',b'masks=0/0/0'),
                   (b'reason=zero-framebuffer-base',b'reason=missing-callback')]
        for before,after in changes:
            with self.subTest(before=before),self.assertRaises(ValueError):
                self.assertIn(before,self.report)
                bind_restoration_report(self.profile,self.report.replace(before,after,1))

    def test_unknown_duplicate_truncated_and_extra_records_are_refused(self):
        records = [self.report[:-1],self.report+b'PCI enumeration: status=0 handles=30\n',
                   self.report+b'EXTRA x=1\n',self.report+self.report,b'\x00'+self.report,
                   self.report.replace(b'REHEARSAL_RETURN stage=4 status=0\n',b'REHEARSAL_RETURN stage=4 status=0\n'*2),
                   self.report.replace(b'CHECK stage=prom status=0',b'CHECK stage=prom status=0\nUNLOCK_SUCCESS'),
                   self.report.replace(b'max=2 mode=3',b'max=2 max=2 mode=3'),
                   self.report.replace(b'info_bytes=36',b'info_bytes=9999999999999999999999999',1)]
        for data in records:
            with self.subTest(data=data[-100:]),self.assertRaises(ValueError):
                bind_restoration_report(self.profile,data)
        offset = 0
        for line in self.report.splitlines(keepends=True)[:-1]:
            offset += len(line)
            with self.subTest(offset=offset),self.assertRaises(ValueError):
                bind_restoration_report(self.profile,self.report[:offset])

    def test_serialized_evidence_cannot_weaken_header_policy(self):
        original = bind_restoration_report(self.profile,self.report)
        mutations = [lambda e:e.update(schema_version=True),lambda e:e.update(source_version='0.5.2-rehearsal'),
                     lambda e:e.update(observed_stage=5),lambda e:e.update(unlock_attempted=True),
                     lambda e:e.update(restoration_completed=True),lambda e:e.update(extra=1),
                     lambda e:e.update(report_sha256=None),lambda e:e.update(report_sha256='not-a-hash'),
                     lambda e:e.update(profile_tag='ff'*32),lambda e:e['deferred'].update(mode=2),
                     lambda e:e['deferred'].update(framebuffer_base=1),lambda e:e['baseline'].update(mode=False),
                     lambda e:e['baseline']['info'].update(masks=[0,0,0,2**32]),
                     lambda e:e['deferred']['info'].update(stride=801)]
        for mutate in mutations:
            changed = copy.deepcopy(original); mutate(changed['restoration_evidence'])
            with self.subTest(e=changed['restoration_evidence']),self.assertRaises(ValueError):
                rehearsal_header(changed)

    def test_build_requires_restoration_input_without_running_compiler(self):
        result = subprocess.run([sys.executable,str(WIN/'build.py'),'--rehearsal',
                                 '--profile','synthetic.txt','--topology','synthetic-topology.txt'],
                                capture_output=True,text=True)
        self.assertNotEqual(result.returncode,0)
        self.assertIn('--restoration-report',result.stderr)


if __name__ == '__main__':
    unittest.main()
