# SPDX-License-Identifier: GPL-2.0-only
"""Rehearsal packages must bind exact mode, image, pins and check evidence.

All files below are synthetic. The structural PE fixture is never executed or
distributed; validation uses the production PE and packaging checks.
"""
from __future__ import annotations

import copy
from pathlib import Path
import struct
import sys
import tempfile
import unittest

WIN = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(WIN))
from board_profile import header as board_header
from rehearsal_profile import parse_capture, rehearsal_header, bind_restoration_report
from package_diagnostic import HOST_REQUIRED_SOURCES
from package_rehearsal import SOURCES, CASES, check_inputs, digest, validation_source_hashes
from test_rehearsal_profile import fixture, restoration_fixture


class RehearsalPackageTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        image = bytearray(1024)
        image[:2] = b'MZ'
        struct.pack_into('<I', image, 60, 128)
        image[128:132] = b'PE\0\0'
        struct.pack_into('<HH', image, 132, 0x8664, 1)
        struct.pack_into('<H', image, 148, 240)
        opt = 152
        struct.pack_into('<H', image, opt, 0x20b)
        struct.pack_into('<I', image, opt+16, 0x1000)
        struct.pack_into('<HH', image, opt+68, 10, 0x140)
        struct.pack_into('<I', image, opt+112+5*8+4, 12)
        section = opt+240
        struct.pack_into('<II', image, section+8, 512, 0x1000)
        struct.pack_into('<I', image, section+36, 0x20000000)
        marker = b'REHEARSAL_ONLY: mode=firmware-rehearsal'
        image[512:512+len(marker)] = marker
        self.image = bytes(image)
        profile = parse_capture(*fixture())
        profile = bind_restoration_report(profile,restoration_fixture(profile))
        self.generated = {'board_profile.h': board_header(profile['board']).encode(),
                          'rehearsal_profile.h': rehearsal_header(profile).encode()}
        names = SOURCES | HOST_REQUIRED_SOURCES | {
            'windows/build.py', 'windows/rehearsal_profile.py', 'windows/board_profile.py',
            'windows/package_rehearsal.py', 'windows/validate_rehearsal.py',
            'windows/tests/test_fixture.py', 'windows/rehearsal/Update-USB.ps1',
            'windows/rehearsal/README.md', 'windows/rehearsal/RehearsalUsb.psm1'}
        for name in names:
            path = self.root/name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('synthetic validation input: '+name+'\n')
        build_inputs = SOURCES | {'windows/build.py', 'windows/rehearsal_profile.py', 'windows/board_profile.py'}
        self.result = dict(mode='firmware-rehearsal', version='0.5.2-rehearsal', hardware_tested=False,
                           unlock_enabled=False, helper_protocol=None, sha256=digest(self.image), size=len(self.image),
                           compiled_sources=sorted(SOURCES), capture_profile=profile, board_profile=profile['board'],
                           generated_header_sha256={name: digest(data) for name, data in self.generated.items()},
                           source_sha256={name: digest((self.root/name).read_bytes()) for name in build_inputs})
        self.validation = dict(all_passed=True, mode='firmware-rehearsal', hardware_tested=False,
                               windows_runtime_tested=False, image_sha256=digest(self.image),
                               tested_source_sha256=validation_source_hashes(self.root),
                               checks=[dict(name=name, exit_code=0) for name in ('build', 'host-tests', 'usb-tests', 'vm-tests')])
        self.vm = dict(passed=True, mode='firmware-rehearsal', image_sha256=digest(self.image),
                       hardware_tested=False, windows_handoff_tested=False, secure_boot_tested=False, bitlocker_tested=False,
                       cases=[dict(name=name, passed=True, returned_to_firmware=True) for name in sorted(CASES)])

    def check(self):
        check_inputs(self.image, self.result, self.validation, self.vm, self.generated, self.root)

    def test_complete_consistent_record_is_accepted(self):
        self.check()

    def test_mode_payload_and_hardware_claims_are_bound(self):
        baseline = copy.deepcopy(self.result)
        for key, value in (('mode', 'diagnostic'), ('mode', 'experimental-unlock'), ('version', '0.4.2-diagnostic'),
                           ('version', '0.5.0-rehearsal'), ('version', '0.5.1-rehearsal'),
                           ('hardware_tested', True), ('unlock_enabled', True), ('helper_protocol', 'gu-session-v1')):
            with self.subTest(key=key, value=value):
                self.result = copy.deepcopy(baseline)
                self.result[key] = value
                with self.assertRaises(ValueError):
                    self.check()

    def test_changed_efi_and_invalid_pe_are_rejected(self):
        self.image += b'changed after testing'
        with self.assertRaisesRegex(ValueError, 'EFI/build mismatch'):
            self.check()
        self.image = b'XX'+self.image[2:]
        self.result.update(sha256=digest(self.image), size=len(self.image))
        with self.assertRaisesRegex(ValueError, 'PE'):
            self.check()

    def test_markers_cannot_disguise_different_payload(self):
        baseline = self.image
        for forbidden in (b'DIAGNOSTIC_ONLY:', b'Press U once', b'booter_load_ad102.bin', b'GUSSN001', b'UNLOCK_VERIFIED:'):
            with self.subTest(marker=forbidden):
                self.image = baseline+forbidden
                self.result.update(sha256=digest(self.image), size=len(self.image))
                with self.assertRaisesRegex(ValueError, 'Unexpected mode/payload'):
                    self.check()
        self.image = baseline.replace(b'REHEARSAL_ONLY:', b'UNRECOGNIZED!!')
        self.result.update(sha256=digest(self.image), size=len(self.image))
        with self.assertRaisesRegex(ValueError, 'entry marker'):
            self.check()

    def test_compiled_set_cannot_gain_unlock_or_omit_adapter(self):
        baseline = self.result['compiled_sources'][:]
        for changed in (baseline+['kernel/gpu_unlock/unlock_610.c'], baseline[:-1], baseline+[baseline[0]],
                        [name if name != 'windows/src/rehearsal.c' else 'windows/src/capture.c' for name in baseline]):
            with self.subTest(sources=changed):
                self.result['compiled_sources'] = changed
                with self.assertRaisesRegex(ValueError, 'compiled source set'):
                    self.check()

    def test_both_generated_headers_are_exactly_bound(self):
        baseline = copy.deepcopy(self.generated)
        for name in baseline:
            with self.subTest(name=name):
                self.generated = copy.deepcopy(baseline)
                self.generated[name] += b'\n#define UNREVIEWED_CHANGE 1\n'
                with self.assertRaisesRegex(ValueError, 'header mismatch'):
                    self.check()
                # Updating only its digest must not bypass regeneration from
                # the captured profile used by the actual build.
                self.result['generated_header_sha256'][name] = digest(self.generated[name])
                with self.assertRaisesRegex(ValueError, 'header mismatch'):
                    self.check()
                self.result['generated_header_sha256'][name] = digest(baseline[name])
        self.generated = baseline
        self.generated['unreviewed.h'] = b'extra'
        with self.assertRaisesRegex(ValueError, 'header mismatch'):
            self.check()

    def test_profile_changes_invalidate_generated_driver_pins(self):
        baseline = copy.deepcopy(self.result)
        for change in ('permission', 'board', 'code', 'path', 'callbacks', 'hardware_claim', 'evidence', 'deferred', 'evidence_hash'):
            with self.subTest(change=change):
                self.result = copy.deepcopy(baseline)
                profile = self.result['capture_profile']
                if change == 'permission': profile['unlock_authorized'] = True
                if change == 'board': self.result['board_profile'] = {}
                if change == 'code': profile['drivers'][0]['code_sha256'] = 'ab'*32
                if change == 'path': profile['endpoints']['gop']['path'] = '7fff0400'
                if change == 'callbacks': profile['console_offsets'][0] += 1
                if change == 'hardware_claim': profile['rehearsal_hardware_tested'] = True
                if change == 'evidence': del profile['restoration_evidence']
                if change == 'deferred': profile['restoration_evidence']['deferred']['mode'] = 0
                if change == 'evidence_hash': profile['restoration_evidence']['report_sha256'] = 'ab'*32
                with self.assertRaises(ValueError):
                    self.check()

    def test_build_sources_are_present_unchanged_and_unlinked(self):
        name = 'windows/src/rehearsal.c'
        path = self.root/name
        original = path.read_bytes()
        path.write_bytes(original+b'changed')
        with self.assertRaisesRegex(ValueError, 'Source changed'):
            self.check()
        path.unlink()
        with self.assertRaisesRegex(ValueError, 'Source changed'):
            self.check()
        target = self.root/'copy.c'
        target.write_bytes(original)
        path.symlink_to(target)
        with self.assertRaisesRegex(ValueError, 'Source changed'):
            self.check()

    def test_missing_or_external_source_provenance_is_rejected(self):
        del self.result['source_sha256']['windows/rehearsal_profile.py']
        with self.assertRaisesRegex(ValueError, 'Incomplete build provenance'):
            self.check()
        self.result['source_sha256']['windows/rehearsal_profile.py'] = digest((self.root/'windows/rehearsal_profile.py').read_bytes())
        for name in ('../outside.c', '/outside.c'):
            with self.subTest(name=name):
                self.result['source_sha256'][name] = 'ab'*32
                with self.assertRaisesRegex(ValueError, 'Source changed'):
                    self.check()
                del self.result['source_sha256'][name]

    def test_changes_after_validation_cannot_be_packaged(self):
        for name in ('windows/rehearsal/Update-USB.ps1', 'windows/rehearsal/README.md',
                     'windows/rehearsal/RehearsalUsb.psm1', 'windows/package_rehearsal.py',
                     'windows/validate_rehearsal.py', 'windows/tests/test_fixture.py'):
            with self.subTest(name=name):
                path = self.root/name
                original = path.read_bytes()
                path.write_bytes(original+b'changed')
                with self.assertRaisesRegex(ValueError, 'stale validation'):
                    self.check()
                path.write_bytes(original)
        added = self.root/'windows/tests/test_added_after_checks.py'
        added.write_text('unvalidated test dependency')
        with self.assertRaisesRegex(ValueError, 'stale validation'):
            self.check()

    def test_host_validation_requires_success_and_named_checks(self):
        baseline = copy.deepcopy(self.validation)
        for key, value in (('all_passed', False), ('mode', 'diagnostic'), ('hardware_tested', True),
                           ('windows_runtime_tested', True), ('image_sha256', 'ab'*32), ('tested_source_sha256', {}),
                           ('checks', []), ('checks', baseline['checks'][:-1]), ('checks', [baseline['checks'][0]]*4),
                           ('checks', [dict(name=entry['name'], exit_code=1 if index == 2 else 0)
                                       for index, entry in enumerate(baseline['checks'])])):
            with self.subTest(key=key):
                self.validation = copy.deepcopy(baseline)
                self.validation[key] = value
                with self.assertRaises(ValueError):
                    self.check()

    def test_vm_cannot_claim_hardware_or_omit_refusal_return(self):
        baseline = copy.deepcopy(self.vm)
        changes = [('passed', False), ('mode', 'diagnostic'), ('image_sha256', 'ab'*32), ('cases', []),
                   ('cases', baseline['cases'][:-1]), ('cases', [baseline['cases'][0]]*3)]
        changes += [(key, True) for key in ('hardware_tested', 'windows_handoff_tested', 'secure_boot_tested', 'bitlocker_tested')]
        for index in range(3):
            for key in ('passed', 'returned_to_firmware'):
                cases = copy.deepcopy(baseline['cases'])
                cases[index][key] = False
                changes.append(('cases', cases))
        for key, value in changes:
            with self.subTest(key=key, value=value):
                self.vm = copy.deepcopy(baseline)
                self.vm[key] = value
                with self.assertRaises(ValueError):
                    self.check()


if __name__ == '__main__':
    unittest.main()
