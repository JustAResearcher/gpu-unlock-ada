# SPDX-License-Identifier: GPL-2.0-only
"""Production firmware-release rehearsal against synthetic EFI services."""
from __future__ import annotations
import hashlib
import os
from pathlib import Path
import subprocess
import struct
import tempfile
import unittest

WIN = Path(__file__).resolve().parents[1]


def fixture_header() -> str:
    def path(index: int) -> str:
        return '{1,1,6,0,%d,0,127,255,4,0}' % index

    code = bytearray(b'Z' * 512)
    for i, offset in enumerate((0x220, 0x230, 0x240)):
        code[offset-0x200:offset-0x200+6] = b'\xff\x25'+struct.pack('<I', 0x400+8*i-offset-6)
    code_hash = hashlib.sha256(code).hexdigest()
    empty_hash = hashlib.sha256(b'').hexdigest()
    drivers = []
    for _ in range(3):
        drivers.append('{' + ','.join([
            '.version=0x10', '.image_bytes=4096', '.supported_offset=0x240',
            '.start_offset=0x230', '.stop_offset=0x220', '.code_rva=0x200',
            '.code_bytes=0x200', '.file_path='+path(3), '.file_path_bytes=10',
            '.code_sha256="'+code_hash+'"', '.section_count=2', '.headers_bytes=0x200',
            '.entry_rva=0x220', '.relocation_rva=0', '.relocation_bytes=0',
            '.relocation_count=0', '.relocation_sha256="'+empty_hash+'"',
        ]) + '}')
    return '\n'.join([
        '#pragma once',
        '#include <Protocol/GraphicsOutput.h>',
        '''typedef struct {
  UINT32 version;
  UINTN image_bytes,supported_offset,start_offset,stop_offset,code_rva,code_bytes;
  UINT8 file_path[32]; UINTN file_path_bytes; const char *code_sha256;
  UINTN section_count,headers_bytes,entry_rva,relocation_rva,relocation_bytes,relocation_count;
  const char *relocation_sha256;
} GU_REHEARSAL_DRIVER_PIN;''',
        'static const UINT8 GuRehearsalPciPath[]='+path(0)+';',
        'static const UINT8 GuRehearsalGopPath[]='+path(1)+';',
        'static const UINT8 GuRehearsalEdidPath[]='+path(2)+';',
        'static const EFI_GUID GuRehearsalPciProtocols[]={EFI_PCI_IO_PROTOCOL_GUID,EFI_DEVICE_PATH_PROTOCOL_GUID};',
        'static const EFI_GUID GuRehearsalGopProtocols[]={EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID,EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL_GUID,EFI_DEVICE_PATH_PROTOCOL_GUID,'
        '{0x12345679,0x1234,0x5678,{0x90,0xab,0xcd,0xef,0x01,0x23,0x45,0x67}},'
        '{0x1234567a,0x1234,0x5678,{0x90,0xab,0xcd,0xef,0x01,0x23,0x45,0x67}}};',
        'static const EFI_GUID GuRehearsalEdidProtocols[]={{0x1c0c34f6,0xd380,0x41fa,{0xa0,0x49,0x8a,0xd0,0x6c,0x1a,0x66,0xaa}},EFI_DEVICE_PATH_PROTOCOL_GUID};',
        'static const EFI_GUID GuRehearsalConsoleProtocols[]={EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL_GUID,EFI_SIMPLE_TEXT_INPUT_PROTOCOL_GUID,EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID};',
        'static const GU_REHEARSAL_DRIVER_PIN GuRehearsalDrivers[3]={'+','.join(drivers)+'};',
        'static const UINTN GuRehearsalConsoleOffsets[5]={0x220,0x220,0x220,0x220,0x220};',
        'static const BOOLEAN GuRehearsalDeferredEnabled=TRUE;',
        'static const UINT32 GuRehearsalDeferredMaxMode=1,GuRehearsalDeferredActiveMode=0,GuRehearsalDeferredMode=2;',
        'static const UINTN GuRehearsalDeferredInfoBytes=sizeof(EFI_GRAPHICS_OUTPUT_MODE_INFORMATION);',
        'static const UINT64 GuRehearsalDeferredFrameBufferBase=0x6800000000ULL;',
        'static const UINTN GuRehearsalDeferredFrameBufferBytes=2560*1440*4;',
        'static const EFI_GRAPHICS_OUTPUT_MODE_INFORMATION GuRehearsalDeferredInfo={0,2560,1440,PixelBlueGreenRedReserved8BitPerColor,{0,0,0,0},2560};',

    ]) + '\n'


class RehearsalTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory()
        cls.root = Path(cls.temporary.name)
        (cls.root/'rehearsal_profile.h').write_text(fixture_header())
        (cls.root/'board_profile.h').write_text('''#pragma once
#define GU_PROFILE_VERSION 1
#define GU_PROFILE_ID 0x270510deU
#define GU_BOARD_SUBSYSTEM 0xe1301462U
#define GU_BOARD_CHIP 0x193000a1U
#define GU_PROFILE_SEGMENT 0U
#define GU_PROFILE_BUS 1U
#define GU_PROFILE_DEVICE 0U
#define GU_PROFILE_FUNCTION 0U
#define GU_PROFILE_BAR_BASE 0x83000000ULL
#define GU_PROFILE_BAR_SIZE 0x1000000ULL
#define GU_PROFILE_ROM_SIZE 150528U
#define GU_PROFILE_VRAM 16376U
#define GU_PROFILE_PROM_HASH ""
#define GU_PROFILE_ROM_HASH ""
#define GU_PROFILE_TAG "synthetic-rehearsal-tests"
''')
        cls.executable = cls.root/'rehearsal'
        command = [os.environ.get('CC', 'cc'), '-std=c11', '-O1', '-g', '-fshort-wchar',
                   '-DGU_REHEARSAL=1', '-DGU_ENABLE_UNLOCK=0',
                   '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                   '-I'+str(cls.root), '-I'+str(WIN/'src'),
                   '-I'+str(WIN/'deps/edk2'), '-I'+str(WIN/'deps/edk2/X64'),
                   str(WIN/'tests/test_rehearsal.c')]
        command += [str(WIN/'src'/(name+'.c')) for name in ('rehearsal', 'capture_pe', 'sha256')]
        result = subprocess.run(command+['-o', str(cls.executable)], capture_output=True, text=True)
        if result.returncode:
            cls.temporary.cleanup()
            raise AssertionError(result.stdout+result.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def run_cases(self, cases, diagnostics=None, absent=None, stop_before_stage5=False):
        env = os.environ.copy()
        env['ASAN_OPTIONS'] = env.get('ASAN_OPTIONS', '')+':detect_leaks=0'
        env['UBSAN_OPTIONS'] = env.get('UBSAN_OPTIONS', '')+':halt_on_error=1'
        for case in cases:
            with self.subTest(case=case):
                result = subprocess.run([str(self.executable), case], capture_output=True,
                                        text=True, timeout=20, env=env)
                self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
                self.assertIn(': PASS', result.stdout)
                if diagnostics:
                    for expected in diagnostics[case]:
                        self.assertIn(expected, result.stderr, result.stdout+result.stderr)
                if stop_before_stage5:
                    self.assertNotIn('REHEARSAL_INTENT stage=5', result.stderr)
                    self.assertNotIn('REHEARSAL_STAGE_VERIFIED stage=4', result.stderr)
                if absent:
                    for forbidden in absent.get(case, []):
                        self.assertNotIn(forbidden, result.stderr, result.stdout+result.stderr)

    def test_report10_semantic_leaf_replacement_boundaries(self):
        self.run_cases(['usb-replace-preflight-six', 'usb-replace-preflight-reversed', 'usb-replace-preflight-partial', 'usb-replace-preflight-retirement', 'usb-replace-stage4-six', 'usb-replace-final-six', 'usb-replace-final-refuse', 'usb-replace-generation-two'])

    def test_report10_invalid_replacements_refuse_before_any_mutation(self):
        self.run_cases(['usb-replace-refuse-'+name for name in ['extra', 'duplicate', 'path', 'agent', 'attributes', 'count', 'wrong-host', 'reuse-child', 'reuse-other', 'reuse-idle', 'reuse-permuted', 'old-alias', 'protocol', 'active', 'image-device', 'image-parent', 'host-path', 'host-interface', 'host-owner', 'path-unreadable', 'interface-unreadable', 'descendant', 'alias']])

    def test_replacement_diagnostics_cannot_hide_protected_changes(self):
        self.run_cases(['usb-replace-log-keyboard', 'usb-replace-stage4-refuse'])

    def test_usb_leaf_retirement_does_not_block_restoration(self):
        cases = ['usb-retire', 'usb-retire-absent', 'usb-retire-final', 'usb-final-extra-missing', 'usb-multi-unrelated']
        self.run_cases(cases, diagnostics={
            case: ['REHEARSAL_USB_LEAF_BASELINE'] +
            ([] if case in ('usb-retire-final', 'usb-final-extra-missing') else ['REHEARSAL_GRAPH_USB_LEAF_RETIRED'])
            for case in cases
        })

    def test_usb_leaf_exception_preserves_other_ownership(self):
        self.run_cases(['usb-active-child', 'usb-storage-child', 'usb-keyboard-child', 'usb-loaded-image-child',
                        'usb-host-owner-lost', 'usb-host-interface-drift', 'usb-host-path-drift',
                        'usb-child-path-drift', 'usb-child-interface-drift', 'usb-child-protocol-added', 'usb-child-active-now',
                        'usb-extra-missing', 'usb-log-extra-missing', 'usb-log-added-edge', 'usb-added-edge', 'usb-added-child', 'usb-agent', 'usb-count',
                        'usb-host-path-bad', 'usb-child-path-bad', 'usb-child-path-not-usb',
                        'usb-child-path-prefix', 'usb-child-path-multi-instance',
                        'usb-host-interface-unreadable', 'usb-child-path-unreadable',
                        'usb-child-protocol-extra', 'usb-ancestor-storage', 'usb-ancestor-keyboard',
                        'usb-ancestor-late', 'usb-ancestor-alias', 'usb-multi-descendant'])

    def test_snapshot_requires_tpl_services(self):
        self.run_cases(['no-raise-tpl', 'no-restore-tpl'])

    def test_release_restore_and_late_recheck(self):
        self.run_cases(['success', 'success-reused-handles', 'late-refusal', 'late-restored-drift'])

    def test_every_callback_error_partial_effect_and_noop_stops(self):
        self.run_cases([f'{callback}-{effect}-{i}' for callback in ('detach', 'connect')
                        for effect in ('error', 'partial', 'unchanged', 'warning') for i in (1, 2, 3)])

    def test_supported_cannot_mutate_or_return_warning(self):
        self.run_cases([f'supported-{effect}-{i}' for effect in ('error', 'warning', 'mutation', 'identity', 'code-xp')
                        for i in (1, 2, 3)])
        self.run_cases(['supported-decode-1'])

    def test_unrelated_console_and_hardware_state_remain_protected(self):
        self.run_cases(['keyboard-child-removed', 'keyboard-count-drift', 'new-consumer',
                        'config-drift', 'bme-set', 'console-callback-drift', 'key-callback-drift',
                        'image-drift', 'gop-path-drift', 'restore-config-drift',
                        'restore-attributes-drift', 'restore-path-drift', 'log-before-first',
                        'restore-cross-role', 'restore-unrelated-reuse', 'restore-driver-reuse',
                        'late-code-xp', 'late-nv-code-xp'])

    def test_restored_display_refusals_stop_before_consumers(self):
        unsupported = ['fb-base', 'fb-size', 'query-null', 'set-null', 'blt-null',
                       'unreadable-info', 'null-info', 'small-info', 'zero-width',
                       'zero-height', 'pixel-blt', 'pixel-invalid', 'short-stride',
                       'short-buffer', 'foreign', 'duplicate', 'virtual-fb']
        compromised = ['interface-error', 'unreadable-interface', 'unreadable-mode']
        changed = ['mode-drift', 'max-mode-drift', 'fb-drift', 'info-drift', 'callback-drift']
        reasons = {
            'fb-base': 'zero-framebuffer-base', 'fb-size': 'zero-framebuffer-size',
            'query-null': 'missing-callback', 'set-null': 'missing-callback',
            'blt-null': 'missing-callback', 'unreadable-info': 'info-unreadable',
            'null-info': 'info-unreadable', 'small-info': 'info-size',
            'zero-width': 'zero-resolution', 'zero-height': 'zero-resolution',
            'pixel-blt': 'pixel-format', 'pixel-invalid': 'pixel-format',
            'short-stride': 'scanline', 'short-buffer': 'framebuffer-capacity',
            'foreign': 'unexpected-handle', 'duplicate': 'duplicate-physical',
            'virtual-fb': 'unexpected-handle', 'interface-error': 'interface',
            'unreadable-interface': 'protocol-unreadable',
            'unreadable-mode': 'mode-unreadable',
            'mode-drift': 'baseline-mismatch', 'max-mode-drift': 'baseline-mismatch',
            'fb-drift': 'baseline-mismatch', 'info-drift': 'baseline-mismatch',
            'callback-drift': 'baseline-mismatch',
        }
        diagnostics = {}
        for names, status in ((unsupported, 3), (compromised, 33), (changed, 6)):
            for name in names:
                diagnostics['restore-display-'+name] = [
                    'REHEARSAL_RETURN stage=4 status=0',
                    'REHEARSAL_DISPLAY_REFUSED stage=4 reason='+reasons[name],
                    'REHEARSAL_GOP_OBSERVATION stage=4 reason='+reasons[name],
                    f'REHEARSAL_VERIFY_FAILED stage=4 check=display status=0x{(1<<63)|status:x}',
                ]
        # Invalid pointers must remain observations, not trigger a read through
        # them or a GOP callback while the program is already stopping.
        absent = {}
        for name in compromised:
            absent['restore-display-'+name] = ['REHEARSAL_GOP_MODE stage=4',
                                              'REHEARSAL_GOP_INFO stage=4']
        for name in ['unreadable-info', 'null-info', 'small-info']:
            absent['restore-display-'+name] = ['REHEARSAL_GOP_INFO stage=4']
        diagnostics['restore-display-zero-width'].append(
            'REHEARSAL_GOP_INFO stage=4 version=0 width=0 height=1440 format=1 stride=2560')
        diagnostics['restore-display-pixel-blt'].append(
            'REHEARSAL_GOP_INFO stage=4 version=0 width=2560 height=1440 format=3 stride=2560')
        self.run_cases(diagnostics, diagnostics, absent, stop_before_stage5=True)

    def test_evidence_baseline_mismatch_refuses_before_release(self):
        cases = ['baseline-pin-'+name for name in ('mode', 'max', 'framebuffer',
                                                  'bytes', 'info-size', 'info')]
        diagnostics = {case: ['REHEARSAL_DISPLAY_REFUSED stage=0 reason=evidence-baseline-mismatch']
                       for case in cases}
        absent = {case: ['REHEARSAL_INTENT stage=1'] for case in cases}
        self.run_cases(cases, diagnostics, absent)

    def test_exact_deferred_mode_initializes_only_through_existing_graphics_driver(self):
        cases = ['deferred-success', 'deferred-success-reused-handles']
        diagnostics = {case: ['REHEARSAL_GOP_DEFERRED stage=4 accepted=1',
                              'REHEARSAL_STAGE_VERIFIED stage=4',
                              'REHEARSAL_STAGE_VERIFIED stage=5',
                              'REHEARSAL_STAGE_VERIFIED stage=6'] for case in cases}
        self.run_cases(cases, diagnostics)

    def test_other_dormant_display_states_still_refuse(self):
        cases = ['deferred-'+name for name in (
            'mode-valid', 'mode-other', 'max-mode', 'nonzero-base', 'nonzero-size',
            'info-drift', 'width-drift', 'pixel-drift', 'stride-drift', 'info-size',
            'unreadable-info', 'callback-drift')]
        diagnostics = {case: ['REHEARSAL_RETURN stage=4 status=0',
                              'REHEARSAL_VERIFY_FAILED stage=4 check=display'] for case in cases}
        self.run_cases(cases, diagnostics, stop_before_stage5=True)

    def test_deferred_mode_does_not_skip_other_ownership_guards(self):
        cases = ['deferred-'+name for name in (
            'driver-code', 'path-drift', 'bme', 'config-drift', 'console-drift',
            'keyboard-edge', 'new-consumer')]
        self.run_cases(cases, stop_before_stage5=True)

    def test_console_protocol_lifetimes_and_final_inventory(self):
        # Console-produced baseline tags need not exist just after NVIDIA Start.
        # Unowned intermediate protocols do not establish their producer; exact
        # full inventory is mandatory once all three consumers are restored.
        success = 'deferred-success-transient-tag'
        self.run_cases([success], {success: ['REHEARSAL_STAGE_VERIFIED stage=4',
                                            'REHEARSAL_STAGE_VERIFIED stage=5',
                                            'REHEARSAL_STAGE_VERIFIED stage=6']})
        cases = ['deferred-stage6-'+name for name in (
            'missing-console-tag', 'missing-vendor-tag', 'protocol-extra', 'missing-text')]
        diagnostics = {case: ['REHEARSAL_STAGE_VERIFIED stage=4',
                              'REHEARSAL_STAGE_VERIFIED stage=5',
                              'REHEARSAL_INVENTORY_REFUSED stage=6',
                              'REHEARSAL_VERIFY_FAILED stage=6 check=endpoint-identities'] for case in cases}
        absent = {case: ['REHEARSAL_STAGE_VERIFIED stage=6'] for case in cases}
        self.run_cases(cases, diagnostics, absent)

    def test_graphics_supported_cannot_change_deferred_state(self):
        cases = ['deferred-supported-active', 'active-supported-deferred',
                 'deferred-supported-mode-pointer', 'deferred-supported-info-pointer',
                 'deferred-supported-interface-pointer']
        diagnostics = {case: ['REHEARSAL_DISPLAY_REFUSED stage=4 reason=stage4-state-changed']
                       for case in cases}
        absent = {case: ['REHEARSAL_RETURN stage=5', 'REHEARSAL_INTENT stage=6'] for case in cases}
        self.run_cases(cases, diagnostics, absent)

    def test_graphics_start_must_restore_complete_active_display(self):
        cases = ['deferred-still-uninitialized', 'deferred-stage5-mode-drift',
                 'deferred-stage5-info-drift']
        diagnostics = {case: ['REHEARSAL_STAGE_VERIFIED stage=4',
                              'REHEARSAL_RETURN stage=5 status=0',
                              'REHEARSAL_VERIFY_FAILED stage=5 check=display'] for case in cases}
        absent = {case: ['REHEARSAL_STAGE_VERIFIED stage=5', 'REHEARSAL_INTENT stage=6']
                  for case in cases}
        self.run_cases(cases, diagnostics, absent)

    def test_deferred_mode_never_survives_final_restoration(self):
        cases = ['deferred-stage6-dormant', 'deferred-final-dormant']
        diagnostics = {case: ['REHEARSAL_STAGE_VERIFIED stage=5',
                              'REHEARSAL_RETURN stage=6 status=0',
                              'REHEARSAL_VERIFY_FAILED stage=6 check=display'] for case in cases}
        absent = {'deferred-stage6-dormant': ['REHEARSAL_STAGE_VERIFIED stage=6']}
        self.run_cases(cases, diagnostics, absent)

    def test_invalid_metadata_refused_without_mutation(self):
        self.run_cases(['locate-error', 'null-handles', 'too-many-handles', 'duplicate-handle',
                        'null-handle', 'protocol-error', 'null-protocols', 'too-many-protocols',
                        'duplicate-protocol', 'null-protocol', 'information-error', 'null-opens',
                        'too-many-opens', 'zero-open-count', 'unknown-consumer', 'unknown-protocol',
                        'unknown-attributes', 'duplicate-edge', 'null-agent', 'second-physical-gop',
                        'root-parent-missing', 'root-driver-missing', 'root-parent-duplicate', 'unknown-incoming',
                        'wrong-parent', 'parent-error', 'config-read-error', 'wrong-bdf',
                        'wrong-subsystem', 'attributes-error', 'bar-drift', 'allocation-first',
                        'free-error', 'no-log', 'owned', 'armed', 'started', 'no-disconnect',
                        ])

    def test_firmware_warning_statuses_fail_closed(self):
        self.run_cases(['map-warning', 'locate-warning', 'protocol-warning', 'information-warning',
                        'interface-warning', 'config-read-warning', 'attributes-warning', 'bar-warning',
                        'free-warning'])

    def test_driver_identity_and_unreadable_memory_refused(self):
        self.run_cases(['binding-error', 'image-error', 'wrong-driver-version', 'wrong-image-size',
                        'wrong-stop', 'wrong-start', 'wrong-supported', 'wrong-image-source',
                        'wrong-driver-path', 'wrong-code-hash', 'bad-pe', 'unreadable-image',
                        'wrong-console-code',
                        'unreadable-binding', 'unreadable-image-interface', 'unreadable-path',
                        'bad-path', 'no-memory-map', 'map-error', 'read-protected',
                        'map-conventional', 'overlapping-map', 'interior-overlap', 'code-interior-overlap',
                        'code-in-data-map', 'code-xp-1', 'code-xp-2', 'code-xp-3',
                        'bad-map-stride', 'bad-map-version'])


if __name__ == '__main__':
    unittest.main()
