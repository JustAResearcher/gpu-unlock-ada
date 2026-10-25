# SPDX-License-Identifier: GPL-2.0-only
"""Production diagnostic core/USB logger, with a mocked capture boundary.

test_capture and test_capture_io exercise the real capture components.
"""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

WIN = Path(__file__).resolve().parents[1]
ROOT = WIN.parent
sys.path.insert(0, str(WIN))
from board_profile import parse_report


class DiagnosticTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.out = Path(cls.temp.name)
        cls.exe = cls.out / 'diagnostic'
        sources = [WIN / 'tests/test_diagnostic.c']
        sources += [WIN / 'src' / (x + '.c') for x in (
            'app', 'app_io', 'ownership', 'format', 'sha256', 'checks',
            'port', 'compat', 'probe')]
        sources += [ROOT / 'kernel/gpu_unlock' / (x + '.c') for x in ('vbios_610', 'gfw_eval')]
        cmd = [os.environ.get('CC', 'cc'), '-std=c11', '-O1', '-g',
               '-ffunction-sections', '-fdata-sections', '-fshort-wchar',
               '-D__KERNEL__', '-DGU_UEFI_PORT', '-DGU_ENABLE_UNLOCK=0', '-DGU_SESSION_HELPER=0',
               '-I'+str(WIN/'src'), '-I'+str(WIN/'src/compat'),
               '-I'+str(ROOT/'kernel/gpu_unlock'), '-I'+str(WIN/'deps/edk2'),
               '-I'+str(WIN/'deps/edk2/X64')]
        subprocess.run(cmd + list(map(str, sources)) + ['-Wl,--gc-sections', '-o', str(cls.exe)],
                       check=True, capture_output=True, text=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_case(self, name):
        report = self.out / (name+'.txt')
        result = subprocess.run([str(self.exe), name, str(report)], check=True,
                                capture_output=True, text=True, timeout=10)
        self.assertIn('mutations=0', result.stdout)
        return report.read_bytes()

    def test_finished_diagnostic_is_observational_and_not_a_profile(self):
        data = self.run_case('success')
        self.assertIn(b'DIAGNOSTIC_FINISHED eligible=0', data)
        self.assertIn(b'CHECK stage=firmware-capture status=0', data)
        with self.assertRaises(ValueError):
            parse_report(data)

    def test_hardware_refusals_never_mutate(self):
        for name, stage in (
            ('no-target', 'target'), ('duplicate-target', 'target'),
            ('bad-config', 'target'), ('bad-bar', 'bar0'), ('read-error', 'cold-state'),
            ('warm-gpu', 'cold-state'), ('no-rom', 'prom'), ('bad-prom', 'prom'),
            ('multiple-displays', 'display'), ('bad-owner', 'owner-observe'),
            ('capture-error', 'firmware-capture'),
        ):
            with self.subTest(name=name):
                data = self.run_case(name)
                self.assertIn(('stage='+stage+' gpu_writes=0').encode(), data)
                self.assertNotIn(b'DIAGNOSTIC_COMPLETE', data)
                with self.assertRaises(ValueError):
                    parse_report(data)

    def test_usb_rejections_and_log_faults_never_reach_gpu(self):
        for name in ('non-usb', 'missing-log-directory', 'write-error', 'short-write', 'flush-error'):
            with self.subTest(name=name):
                data = self.run_case(name)
                with self.assertRaises(ValueError):
                    parse_report(data)

    def test_final_log_and_close_errors_are_not_success(self):
        for name in ('complete-flush-error', 'final-flush-error', 'close-error'):
            with self.subTest(name=name):
                self.run_case(name)

    def test_second_child_rejections_log_reason_without_relaxing_policy(self):
        for name, reason, status in (
            ('child-no-gop', 'gop-protocol', '3'),
            ('child-no-mode', 'gop-mode', '3'),
            ('child-no-info', 'gop-geometry', '3'),
            ('child-short-info', 'gop-geometry', '3'),
            ('child-zero-framebuffer', 'gop-geometry', '3'),
            ('child-bad-geometry', 'gop-geometry', '3'),
            ('child-no-path', 'device-path', '3'),
            ('child-path-length', 'path-length', '21'),
            ('child-path-end', 'path-end-node', '3'),
            ('child-path-limit', 'path-node-limit', '21'),
            ('child-wrong-parent', 'pci-parent', 'f'),
            ('child-parent-error', 'pci-parent', 'f'),
            ('child-record-policy', 'record-policy', 'f'),
            ('child-duplicate-record', 'duplicate-child', '21'),
        ):
            with self.subTest(name=name):
                data = self.run_case(name)
                self.assertIn(('reason='+reason+' status=0x80000000000000'+status.zfill(2)).encode(), data)
                self.assertIn(b'stage=owner-observe gpu_writes=0', data)
                self.assertIn(b'observational_only=1', data)
                self.assertIn(b'09576e91-6d3f-11d2-8e39-00a0c969723b', data)
                self.assertIn(b'child-open[0:0] attributes=0x2 count=1 agent=0x5 controller=0x4', data)
                self.assertNotIn(b'DIAGNOSTIC_COMPLETE', data)
                with self.assertRaises(ValueError):
                    parse_report(data)

    def test_optional_child_metadata_failures_preserve_original_refusal(self):
        for name, marker in (
            ('child-protocol-error', b'protocol_status=0x8000000000000007'),
            ('child-protocol-overflow', b'protocols_invalid=bounded-count-or-null'),
            ('child-open-info-error', b'child-open protocol_index=0 status=0x8000000000000007'),
            ('child-open-info-overflow', b'opens_invalid=bounded-record-limit-or-null'),
        ):
            with self.subTest(name=name):
                data = self.run_case(name)
                self.assertIn(marker, data)
                self.assertIn(b'reason=gop-protocol status=0x8000000000000003', data)
                self.assertNotIn(b'DIAGNOSTIC_COMPLETE', data)

    def test_quiet_owner_recheck_does_not_query_additional_child_metadata(self):
        self.assertEqual(self.run_case('quiet-child-no-gop'), b'')


if __name__ == '__main__':
    unittest.main()
