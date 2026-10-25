# SPDX-License-Identifier: GPL-2.0-only
"""Staged build provenance tests use invented firmware reports exclusively."""
from __future__ import annotations

import copy
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

WIN = Path(__file__).resolve().parents[1]
sys.path.insert(0,str(WIN))
from rehearsal_profile import parse_capture, bind_restoration_report, rehearsal_header
from staged_profile import bind_rehearsal_success, staged_header, validate_staged_evidence, FINAL
from test_rehearsal_profile import fixture, restoration_fixture


def success_fixture(profile=None) -> bytes:
    """Fully ordered synthetic report matching the public 0.5.2 log format."""
    if profile is None:
        profile = parse_capture(*fixture())
        profile = bind_restoration_report(profile,restoration_fixture(profile))
    old = restoration_fixture(profile).decode().splitlines()
    bdf = 'target BDF=0:1:0.0 checks=1 pm=0x60 pcie=0x78 msi=0x68 msix=0'
    bar = ['bar0 query: status=0',
           'bar0 descriptor: type=0 granularity=32 min=0x83000000 max=0x83ffffff length=0x1000000 translation=0',
           'PROFILE bar_base=83000000 bar_size=1000000']
    cold = ['PROFILE chip=193000a1 vram_mib=16376',
            'cold state: chip=0x193000a1 FEAT=0xffffff8f GFW=0x8b8f/0x3ff/0x10 WPR2_HI=0 SS0=0x55005150 SS1=0x7']
    rows = [old[0].replace('0.5.1','0.5.2'),'RUN06',old[2],old[3],
            'SecureBoot query: status=0 bytes=1 value=0','CHECK stage=secure-boot status=0',
            'PCI enumeration: status=0 handles=30','PCI target matches=1 status=0',
            'target location: status=0']
    rows += [row for row in old if row.startswith('config[')]
    rows += [bdf,'PROFILE version=1 id=270510de subsystem=e1301462 segment=0 bus=1 device=0 function=0',
             'CHECK stage=target status=0']
    rows += bar+['CHECK stage=bar0 status=0']+cold+['CHECK stage=cold-state status=0']
    rows += ['gpu_unlock: ROM chain: pos=0 data=0x52494350 bytes=1024 type=0 last=128',
             'gpu_unlock: ROM expansion: base=0 ext=0x100 adjust=0x100',
             'gpu_unlock: ROM BIT: offset=0x100 size=12 stride=6 tokens=2 adjust=0x100',
             'gpu_unlock: ROM ucode table: offset=0x200 version=1 header=6 stride=6 entries=2',
             'gpu_unlock: FWSEC descriptor: offset=0x300 ROM_bytes=1048576 header=1',
             'gpu_unlock: FWSEC geometry: image_off=0x400 size=0x100 imem=0x80 va=0 pa=0 dmem=0x80 pa=0 pkc=0x10 interface=0x1c engine=0x400 ucode=9 signatures=2 versions=0x3 bytes=768',
             'PROFILE prom_size=1048576 prom_sha256='+'11'*32,
             'PROFILE rom_size=150528 rom_sha256='+'22'*32,
             'PROFILE fwsec_parsed=1','CHECK stage=prom status=0',
             'GOP physical=1 target=1 expect_detached=0 status=0','CHECK stage=display status=0',
             'WINDOWS_PRELOAD internal_gpt_esp=1 candidates=1 bytes=4096 sha256='+'77'*32+' same_boot_pin=1',
             'CHECK stage=windows-preload status=0']
    rows += [row for row in old if row.startswith(('REHEARSAL_GOP_OBSERVATION stage=0 ',
              'REHEARSAL_GOP_CALLBACKS stage=0 ','REHEARSAL_GOP_MODE stage=0 ','REHEARSAL_GOP_INFO stage=0 '))]
    rows += ['REHEARSAL_PREPARE status=0 edges=11 drivers=3 outputs=2',
             'CHECK stage=rehearsal-preflight status=0']
    rows += old[old.index('READY: firmware release/restoration rehearsal; unlock payload is absent.'):][:4]
    rows += ['CHECK stage=arming status=0','CHECK stage=watchdog status=0',
             'target location: status=0',bdf,'CHECK stage=armed-config status=0']
    rows += cold+['CHECK stage=armed-cold status=0',
                 'REHEARSAL_INTENT: R received; beginning staged release/restoration.']
    for stage,edges in ((1,9),(2,8),(3,5)):
        rows += [f'REHEARSAL_INTENT stage={stage} operation=disconnect',
                 f'REHEARSAL_RETURN stage={stage} status=0',
                 f'REHEARSAL_STAGE_VERIFIED stage={stage} edges={edges}']
    deferred = 'REHEARSAL_GOP_DEFERRED stage=4 accepted=1 mode=3 max=2 framebuffer=0 framebuffer_bytes=0'
    rows += ['REHEARSAL_INTENT stage=4 operation=connect','REHEARSAL_SUPPORTED stage=4 status=0',
             'REHEARSAL_RETURN stage=4 status=0',deferred,'REHEARSAL_STAGE_VERIFIED stage=4 edges=8',
             'REHEARSAL_INTENT stage=5 operation=connect',deferred,'REHEARSAL_SUPPORTED stage=5 status=0',
             deferred,'REHEARSAL_RETURN stage=5 status=0','REHEARSAL_STAGE_VERIFIED stage=5 edges=9',
             'REHEARSAL_INTENT stage=6 operation=connect','REHEARSAL_SUPPORTED stage=6 status=0',
             'REHEARSAL_RETURN stage=6 status=0','REHEARSAL_STAGE_VERIFIED stage=6 edges=11',
             'target location: status=0',bdf,'CHECK stage=restored-config status=0']
    rows += bar+['CHECK stage=restored-bar0 status=0']+cold+['CHECK stage=restored-cold-state status=0',
             'REHEARSAL_RESTORED: display ownership and PCI/cold state verified.',FINAL]
    return ('\n'.join(rows)+'\n').encode()


class StagedProfileTests(unittest.TestCase):
    def setUp(self):
        base = parse_capture(*fixture())
        self.profile = bind_restoration_report(base,restoration_fixture(base))
        self.report = success_fixture(self.profile)

    def bind(self,data=None,confirmed=True):
        return bind_rehearsal_success(self.profile,self.report if data is None else data,confirmed)

    def test_complete_success_records_only_prior_rehearsal_and_observed_boot(self):
        bound = self.bind(); evidence = bound['staged_unlock_evidence']
        self.assertNotIn('staged_unlock_evidence',self.profile)
        self.assertIs(bound['unlock_authorized'],False)
        self.assertIs(bound['rehearsal_hardware_tested'],False)
        self.assertEqual(evidence['report_sha256'],hashlib.sha256(self.report).hexdigest())
        self.assertEqual(evidence['restoration_report_sha256'],self.profile['restoration_evidence']['report_sha256'])
        self.assertEqual(evidence['stage_edges'],[9,8,5,8,9,11])
        self.assertEqual(evidence['windows_handoff'],'pending')
        self.assertIs(evidence['windows_boot_user_confirmed'],True)
        self.assertIs(evidence['unlock_attempted'],False)
        self.assertEqual(evidence['windows_preload'],{'bytes':4096,'sha256':'77'*32})
        with self.assertRaises(ValueError): bind_rehearsal_success(bound,self.report,True)

    def test_user_boot_confirmation_is_separate_and_strict(self):
        for value in (False,None,0,1,'true','True',[],{}):
            with self.subTest(value=value),self.assertRaisesRegex(ValueError,'confirmed separately'):
                self.bind(confirmed=value)

    def test_no_rehearsal_or_changed_prior_profile_cannot_bind_success(self):
        for change in ('missing','hash','baseline','authorization','purpose'):
            profile = copy.deepcopy(self.profile)
            if change == 'missing': del profile['restoration_evidence']
            if change == 'hash': profile['board']['report_sha256'] = 'ff'*32
            if change == 'baseline': profile['restoration_evidence']['baseline']['framebuffer_base'] += 4096
            if change == 'authorization': profile['unlock_authorized'] = True
            if change == 'purpose': profile['purpose'] = 'unlock-authorized'
            with self.subTest(change=change),self.assertRaises(ValueError):
                bind_rehearsal_success(profile,self.report,True)

    def test_every_record_boundary_truncation_and_duplicate_is_refused(self):
        records = self.report.splitlines(keepends=True)
        offset = 0
        for index,line in enumerate(records):
            with self.subTest(duplicate=index),self.assertRaises(ValueError):
                self.bind(b''.join(records[:index]+[line]+records[index:]))
            offset += len(line)
            if index < len(records)-1:
                with self.subTest(truncated=index),self.assertRaises(ValueError):
                    self.bind(self.report[:offset])

    def test_unknown_extra_malformed_and_reordered_records_are_refused(self):
        inputs = [self.report[:-1],b'PREAMBLE\n'+self.report,self.report+b'PCI enumeration: status=0 handles=30\n',
                  self.report+b'EXTRA x=1\n',b'\x00'+self.report,self.report+b'\xff',
                  self.report.replace(b'CHECK stage=arming status=0',b'UNKNOWN stage=arming status=0'),
                  self.report.replace(b'REHEARSAL_SUPPORTED stage=5 status=0\n',b''),
                  self.report.replace(b'stage=4 accepted=1 mode=3 max=2',b'stage=4 accepted=1 mode=3 max=2 max=2',1),
                  self.report.replace(b'CHECK stage=arming status=0\nCHECK stage=watchdog status=0',
                                      b'CHECK stage=watchdog status=0\nCHECK stage=arming status=0')]
        for data in inputs:
            with self.subTest(data=data[-100:]),self.assertRaises(ValueError): self.bind(data)

    def test_failures_and_wrong_execution_paths_cannot_become_success(self):
        changes = [(b'0.5.2-rehearsal',b'0.5.1-rehearsal'),
                   (b'bytes=1 value=0',b'bytes=1 value=1'),
                   (b'CHECK stage=arming status=0',b'CHECK stage=arming status=0x8000000000000012'),
                   (b'CHECK stage=armed-cold status=0',b'CHECK stage=armed-cold status=1'),
                   (b'REHEARSAL_RETURN stage=1 status=0',b'REHEARSAL_RETURN stage=1 status=1'),
                   (b'REHEARSAL_SUPPORTED stage=4 status=0',b'REHEARSAL_SUPPORTED stage=4 status=1'),
                   (b'REHEARSAL_RETURN stage=5 status=0',b'REHEARSAL_RETURN stage=5 status=1'),
                   (b'CHECK stage=restored-config status=0',b'CHECK stage=restored-config status=1'),
                   (b'CHECK stage=restored-bar0 status=0',b'CHECK stage=restored-bar0 status=1'),
                   (b'CHECK stage=restored-cold-state status=0',b'CHECK stage=restored-cold-state status=1'),
                   (b'app_mmio_writes=0',b'app_mmio_writes=1'),(b'unlock_attempted=0',b'unlock_attempted=1'),
                   (b'windows_handoff=pending',b'windows_handoff=started')]
        for before,after in changes:
            with self.subTest(before=before),self.assertRaises(ValueError):
                self.assertIn(before,self.report); self.bind(self.report.replace(before,after,1))

    def test_exact_stage_deltas_and_deferred_mode_are_required(self):
        changes = [(b'edges=11 drivers',b'edges=10 drivers'),
                   (b'stage=1 edges=9',b'stage=1 edges=8'),(b'stage=2 edges=8',b'stage=2 edges=9'),
                   (b'stage=3 edges=5',b'stage=3 edges=6'),(b'stage=4 edges=8',b'stage=4 edges=9'),
                   (b'stage=5 edges=9',b'stage=5 edges=8'),(b'stage=6 edges=11',b'stage=6 edges=10'),
                   (b'stage=4 accepted=1',b'stage=4 accepted=0'),(b'mode=3 max=2',b'mode=2 max=2'),
                   (b'mode=3 max=2',b'mode=3 max=3'),
                   (b'framebuffer=0 framebuffer_bytes=0',b'framebuffer=1 framebuffer_bytes=0')]
        for before,after in changes:
            with self.subTest(before=before),self.assertRaises(ValueError):
                self.bind(self.report.replace(before,after,1))

    def test_board_display_and_boot_preload_identity_are_checked(self):
        changes = [(b'id=270510de',b'id=270510df'),(b'270510de 00000003',b'270510de 00000007'),
                   (b'bar_base=83000000',b'bar_base=84000000'),
                   (b'expected=0x200',b'expected=0x201'),(b'mode_readable=1',b'mode_readable=0'),
                   (b'query=0x2200260',b'query=0'),(b'framebuffer_bytes=1920000',b'framebuffer_bytes=1920001'),
                   (b'width=800',b'width=801'),(b'info_bytes=36',b'info_bytes=35'),
                   (b'candidates=1',b'candidates=2'),(b'same_boot_pin=1',b'same_boot_pin=0'),
                   (b'bytes=4096 sha256=',b'bytes=32 sha256='),
                   (b'sha256='+b'77'*32,b'sha256='+b'0'*64)]
        for before,after in changes:
            with self.subTest(before=before),self.assertRaises(ValueError):
                self.assertIn(before,self.report); self.bind(self.report.replace(before,after,1))

    def test_cold_state_must_be_unchanged_when_armed_and_restored(self):
        before = b'SS1=0x7'
        for index in (1,2):
            rows = self.report.split(before)
            changed = before.join(rows[:index+1])+b'SS1=0x8'+before.join(rows[index+1:])
            with self.subTest(index=index),self.assertRaises(ValueError): self.bind(changed)

    def test_serialized_scope_and_provenance_cannot_change_header_policy(self):
        original = self.bind()
        mutations = [lambda e:e.update(schema_version=True),lambda e:e.update(source_version='0.6.0-unlock'),
                     lambda e:e.update(unlock_attempted=True),lambda e:e.update(app_mmio_writes=1),
                     lambda e:e.update(windows_boot_user_confirmed=False),lambda e:e.update(windows_handoff='started'),
                     lambda e:e.update(restoration_verified=False),lambda e:e.update(deferred_observations=2),
                     lambda e:e.update(stage_edges=[9,8,5,8,9,10]),lambda e:e.update(extra=1),
                     lambda e:e.update(report_sha256=None),lambda e:e.update(report_sha256='invalid'),
                     lambda e:e.update(restoration_report_sha256='ab'*32),lambda e:e.update(profile_tag='ab'*32),
                     lambda e:e.update(topology_sha256='ab'*32),lambda e:e['windows_preload'].update(bytes=True),
                     lambda e:e['windows_preload'].update(bytes=1),lambda e:e['windows_preload'].update(extra=1)]
        for mutate in mutations:
            profile = copy.deepcopy(original); mutate(profile['staged_unlock_evidence'])
            with self.subTest(e=profile['staged_unlock_evidence']),self.assertRaises(ValueError): staged_header(profile)

    def test_crlf_provenance_binds_original_bytes(self):
        data = self.report.replace(b'\n',b'\r\n')
        e = self.bind(data)['staged_unlock_evidence']
        self.assertEqual(e['report_sha256'],hashlib.sha256(data).hexdigest())
        self.assertEqual(e['stage_edges'],[9,8,5,8,9,11])

    def test_generated_header_compiles_without_enabling_unlock(self):
        profile = self.bind(); generated = staged_header(profile)
        self.assertTrue(generated.startswith(rehearsal_header(profile)))
        self.assertNotIn('GU_ENABLE_UNLOCK',generated)
        self.assertIn('GU_STAGED_WINDOWS_BOOT_USER_CONFIRMED 1',generated)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory); (root/'staged_profile.h').write_text(generated)
            (root/'check.c').write_text('#include "staged_profile.h"\nint main(void){return GU_STAGED_PROFILE_VERSION!=1 || GU_STAGED_WINDOWS_PRELOAD_BYTES!=4096;}\n')
            p = subprocess.run([os.environ.get('CC','cc'),'-std=c11','-fshort-wchar',
                                '-I'+str(WIN/'deps/edk2'),'-I'+str(WIN/'deps/edk2/X64'),
                                str(root/'check.c'),'-o',str(root/'check')],capture_output=True,text=True)
            self.assertEqual(p.returncode,0,p.stdout+p.stderr)
            self.assertEqual(subprocess.run([str(root/'check')]).returncode,0)


if __name__ == '__main__': unittest.main()
