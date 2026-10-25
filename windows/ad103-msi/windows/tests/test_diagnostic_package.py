# SPDX-License-Identifier: GPL-2.0-only
"""Packaging must not turn a different/untested payload into a diagnostic."""
import copy
from pathlib import Path
import struct
import sys
import tempfile
import unittest

WIN = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(WIN))
from package_diagnostic import (DIAGNOSTIC_SOURCES, VM_CASES, EXPECTED_CHECKS,
                               HOST_REQUIRED_SOURCES, check_inputs, digest,
                               validation_source_hashes)


class PackageTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        # A structural PE fixture, never executable and never distributed.
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
        image[512:529] = b'DIAGNOSTIC_ONLY:xx'
        self.image = bytes(image)
        hashes = {}
        for name in DIAGNOSTIC_SOURCES | {'windows/build.py'}:
            path = self.root/name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('synthetic test source\n')
            hashes[name] = digest(path.read_bytes())
        self.result = {'mode': 'diagnostic', 'hardware_tested': False, 'board_profile': None,
                       'helper_protocol': None, 'sha256': digest(self.image), 'size': len(self.image),
                       'compiled_sources': sorted(DIAGNOSTIC_SOURCES), 'source_sha256': hashes}
        for name in HOST_REQUIRED_SOURCES | {'windows/tests/test_fixture.py'}:
            path = self.root/name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('synthetic host-test input\n')
        self.validation = {'all_passed': True, 'hardware_tested': False, 'windows_runtime_tested': False,
                           'image_sha256': digest(self.image),
                           'tested_source_sha256': validation_source_hashes(self.root),
                           'checks': [{'name': name, 'exit_code': 0} for name in sorted(EXPECTED_CHECKS)]}
        self.vm = {'passed': True, 'image_sha256': digest(self.image), 'hardware_tested': False,
                   'windows_handoff_tested': False, 'secure_boot_tested': False, 'bitlocker_tested': False,
                   'cases': [{'name': name, 'passed': True, 'returned_to_firmware': True} for name in sorted(VM_CASES)]}

    def check(self):
        check_inputs(self.image, self.result, self.validation, self.vm, self.root)

    def test_accept_complete_consistent_record(self):
        self.check()

    def test_reject_experimental_or_claimed_hardware_validation(self):
        original = copy.deepcopy(self.result)
        for key, value in (('mode', 'experimental-unlock'), ('hardware_tested', True),
                           ('board_profile', {}), ('helper_protocol', 'gu-session-v1')):
            with self.subTest(key=key):
                self.result = copy.deepcopy(original)
                self.result[key] = value
                with self.assertRaises(ValueError):
                    self.check()

    def test_reject_changed_image_and_invalid_pe(self):
        self.image += b'x'
        with self.assertRaises(ValueError):
            self.check()
        self.result.update(sha256=digest(self.image), size=len(self.image))
        self.image = b'XX' + self.image[2:]
        self.result['sha256'] = digest(self.image)
        with self.assertRaises(ValueError):
            self.check()

    def test_reject_changed_build_sources(self):
        (self.root/'windows/src/app.c').write_text('changed\n')
        with self.assertRaisesRegex(ValueError, 'Source changed'):
            self.check()

    def test_reject_preparer_or_tests_changed_after_validation(self):
        for name in ('windows/UsbPreparation.psm1', 'windows/Prepare-USB.ps1',
                     'windows/tests/test_fixture.py', 'windows/package_diagnostic.py',
                     'tests/vbios_fixture.h', 'windows/analyze_diagnostic.py'):
            with self.subTest(name=name):
                path = self.root/name
                original = path.read_bytes()
                path.write_text('changed after host tests\n')
                with self.assertRaisesRegex(ValueError, 'sources changed since validation'):
                    self.check()
                path.write_bytes(original)

    def test_reject_new_or_missing_host_test_dependency(self):
        path = self.root/'windows/tests/test_added.py'
        path.write_text('new test after validation\n')
        with self.assertRaisesRegex(ValueError, 'sources changed since validation'):
            self.check()
        path.unlink()
        (self.root/'tests/vbios_fixture.h').unlink()
        with self.assertRaisesRegex(ValueError, 'Missing or linked validation source'):
            self.check()

    def test_reject_transaction_code_and_missing_provenance(self):
        self.result['compiled_sources'].append('kernel/gpu_unlock/unlock_610.c')
        with self.assertRaises(ValueError):
            self.check()
        self.result['compiled_sources'].pop()
        del self.result['source_sha256']['windows/build.py']
        with self.assertRaises(ValueError):
            self.check()

    def test_reject_embedded_unlock_marker(self):
        self.image += b'UNLOCK_SUCCESS'
        self.result.update(sha256=digest(self.image), size=len(self.image))
        with self.assertRaisesRegex(ValueError, 'experimental routine'):
            self.check()

    def test_reject_failed_or_different_host_test_record(self):
        original = copy.deepcopy(self.validation)
        for key, value in (('all_passed', False), ('image_sha256', '0'*64),
                           ('windows_runtime_tested', True), ('checks', []),
                           ('checks', [{'exit_code': 1}]), ('checks', [{'exit_code': 0}]),
                           ('checks', [self.validation['checks'][0]]*4),
                           ('tested_source_sha256', {})):
            with self.subTest(key=key):
                self.validation = copy.deepcopy(original)
                self.validation[key] = value
                with self.assertRaises(ValueError):
                    self.check()

    def test_reject_incomplete_or_different_vm_record(self):
        original = copy.deepcopy(self.vm)
        for key, value in (('passed', False), ('image_sha256', '0'*64),
                           ('hardware_tested', True), ('cases', []),
                           ('cases', [self.vm['cases'][0]]*3)):
            with self.subTest(key=key):
                self.vm = copy.deepcopy(original)
                self.vm[key] = value
                with self.assertRaises(ValueError):
                    self.check()


if __name__ == '__main__':
    unittest.main()
