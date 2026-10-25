# SPDX-License-Identifier: GPL-2.0-only
"""Staged entry control flow with real PCI/ROM checks, logger and post-state checks."""
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import test_diagnostic as fixture_module

WIN, ROOT = fixture_module.WIN, fixture_module.ROOT
sys.path.insert(0, str(WIN))
from board_profile import header, HEX


class StagedEntryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        fixture_module.DiagnosticTests.setUpClass()
        try:
            fixture = fixture_module.DiagnosticTests.out / 'staged-entry-fixture.txt'
            subprocess.run([str(fixture_module.DiagnosticTests.exe), 'success', str(fixture)],
                           check=True, capture_output=True)
            data = fixture.read_bytes()
            board = {}
            for line in data.decode().splitlines():
                if line.startswith('PROFILE '):
                    for pair in line[8:].split():
                        key, value = pair.split('=')
                        board[key] = value if key.endswith('sha256') else int(value, 16 if key in HEX else 10)
            board['report_sha256'] = hashlib.sha256(data).hexdigest()
        finally:
            fixture_module.DiagnosticTests.tearDownClass()
        cls.temp = tempfile.TemporaryDirectory(prefix='gu-staged-entry-')
        cls.addClassCleanup(cls.temp.cleanup)
        out = Path(cls.temp.name)
        (out / 'board_profile.h').write_text(header(board))
        cls.exe = out / 'entry'
        sources = [WIN / 'tests/test_staged_entry.c']
        sources += [WIN / 'src' / (name + '.c') for name in
                    ('app', 'app_io', 'ownership', 'format', 'sha256', 'checks', 'port', 'compat', 'probe', 'staged_state')]
        sources += [ROOT / 'kernel/gpu_unlock' / (name + '.c') for name in ('vbios_610', 'gfw_eval')]
        command = [os.environ.get('CC', 'cc'), '-std=c11', '-O1', '-g', '-fshort-wchar',
                   '-ffunction-sections', '-fdata-sections', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                   '-D__KERNEL__', '-DGU_UEFI_PORT', '-DGU_ENABLE_UNLOCK=1', '-DGU_STAGED_UNLOCK=1',
                   '-DGU_REHEARSAL=0', '-DGU_SESSION_HELPER=0',
                   '-I' + str(out), '-I' + str(WIN / 'src'), '-I' + str(WIN / 'src/compat'),
                   '-I' + str(ROOT / 'kernel/gpu_unlock'), '-I' + str(WIN / 'deps/edk2'), '-I' + str(WIN / 'deps/edk2/X64')]
        result = subprocess.run(command + list(map(str, sources)) +
                                ['-Wl,--gc-sections,--wrap=GuLoadAssets,--wrap=GuReleaseAssets', '-o', str(cls.exe)],
                                capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def cases(self, names):
        env = os.environ.copy()
        env['ASAN_OPTIONS'] = env.get('ASAN_OPTIONS', '') + ':detect_leaks=0'
        env['UBSAN_OPTIONS'] = env.get('UBSAN_OPTIONS', '') + ':halt_on_error=1'
        for name in names:
            with self.subTest(scenario=name):
                result = subprocess.run([str(self.exe), name], capture_output=True, text=True, timeout=15, env=env)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn('staged entry ' + name + ':', result.stdout)

    def test_only_u_arms_and_every_return_from_windows_halts(self):
        self.cases(('start-return', 'start-error', 'start-warning', 'lowercase-u', 'escape', 'timeout', 'r-key',
                    'key-warning', 'input-reset-warning', 'stall-warning', 'watchdog-warning',
                    'intent-log-fail', 'handoff-log-fail', 'close-error'))

    def test_preflight_failure_and_cancellation(self):
        self.cases(('secure-on', 'secure-warning', 'wrong-subsystem', 'wrong-rom', 'assets-error', 'assets-warning',
                    'preload-error', 'preload-warning', 'prepare-error', 'prepare-warning', 'cancel-error', 'cancel-warning',
                    'release-before-mutation', 'asset-release-retained'))

    def test_each_adapter_failure_blocks_transaction_or_handoff(self):
        self.cases(tuple(stage + '-' + status for stage in ('release', 'claim', 'complete', 'restore', 'restoration-check')
                         for status in ('error', 'warning')) +
                   ('release-fault', 'restore-fault', 'claim-cleanup-failed', 'claimed-config', 'claimed-rom', 'restored-config'))

    def test_core_failure_or_incomplete_lifetime_never_restores_or_hands_off(self):
        self.cases(('core-load-error', 'core-before-start-error', 'core-error', 'core-positive-result', 'core-fault',
                    'core-retained', 'core-not-started', 'core-not-quiesced', 'core-not-saved', 'core-no-flr',
                    'core-no-writes', 'core-dma-left'))

    def test_real_post_state_checks_block_bad_handoff(self):
        self.cases(('post-selectors', 'post-readout', 'post-mmio-warning', 'post-config-warning',
                    'restored-selectors', 'asset-release-fault'))

    def test_emergency_refresh_rejects_warnings_even_with_valid_outputs(self):
        self.cases(('refresh-protocol-warning', 'refresh-location-warning', 'refresh-config-warning'))

    def test_application_uses_same_narrow_post_flr_status_clear_policy(self):
        self.cases(('status-clear-reported', 'status-clear-correctable', 'status-clear-unsupported',
                    'status-partial-correctable', 'status-partial-unsupported',
                    'status-new-nonfatal', 'status-new-fatal', 'status-new-pending',
                    'status-control', 'status-other-dword', 'status-stale-cap', 'status-stale-version',
                    'status-prearm-clear', 'status-preclaim-clear', 'status-reassert-restored'))


if __name__ == '__main__':
    unittest.main()
