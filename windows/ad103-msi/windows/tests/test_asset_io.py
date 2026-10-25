# SPDX-License-Identifier: GPL-2.0-only
"""Real app_io asset loading with actual pinned files and fake EFI file services."""
from __future__ import annotations
import hashlib
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

WIN = Path(__file__).resolve().parents[1]
PINS = {
    'booter_load_ad102.bin': (57672, '6ee05b20e1dc244c08c052f36ef28469b8d09f4955d1fe33e48a179e11a7fd49'),
    'v67-ad102-610.43.03-four-plm.bin': (64000, '44c36b2d6b3424e53e18f3e55db85c833655de071ce66ce42749588a4a7dc8cb'),
    'gsp_ga10x.bin': (84277400, '572373620a37418f24dc16b5031c39338778c3257e48e8408de9a57291b24f3a'),
    'gsp_boot_ad102.bin': (36864, '45d6335a8185d6ff13e8d1484cea73f68d3d13aa1c19dc080ed1c3482dd5bac0'),
    'gsp_boot_desc_ad102.bin': (84, '83f8f3f1f5f9496e0ea8fc7103c2beb64d0b6de2f5ffe725f13514de5a95fa42'),
}


class AssetIoTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        location = os.environ.get('GPU_UNLOCK_ASSETS')
        if not location:
            raise unittest.SkipTest('asset I/O with pinned firmware NOT RUN; set GPU_UNLOCK_ASSETS')
        cls.assets = Path(location).resolve()
        for name, (size, digest) in PINS.items():
            path = cls.assets / name
            if not path.is_file() or path.stat().st_size != size:
                raise AssertionError(f'missing/wrong-size pinned asset: {path}')
            hasher = hashlib.sha256()
            with path.open('rb') as asset:
                for block in iter(lambda: asset.read(1024 * 1024), b''):
                    hasher.update(block)
            if hasher.hexdigest() != digest:
                raise AssertionError(f'wrong pinned asset hash: {path}')
        cls.temp = tempfile.TemporaryDirectory()
        directory = Path(cls.temp.name)
        (directory / 'board_profile.h').write_text('''#pragma once
#define GU_PROFILE_VERSION 1
#define GU_PROFILE_ID 0x270510deU
#define GU_PROFILE_TAG "synthetic-asset-io-tests"
''')
        cls.executable = directory / 'asset_io'
        command = [os.environ.get('CC', 'cc'), '-std=c11', '-O1', '-g', '-fshort-wchar',
                   '-DGU_ENABLE_UNLOCK=1', '-DGU_SESSION_HELPER=0',
                   '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                   '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                   '-I' + str(directory), '-I' + str(WIN / 'src'),
                   '-I' + str(WIN / 'deps/edk2'), '-I' + str(WIN / 'deps/edk2/X64'),
                   str(WIN / 'tests/test_asset_io.c'), str(WIN / 'src/sha256.c'),
                   str(WIN / 'src/format.c'), '-o', str(cls.executable)]
        compiled = subprocess.run(command, capture_output=True, text=True)
        if compiled.returncode:
            cls.temp.cleanup()
            raise AssertionError(compiled.stdout + compiled.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def cases(self, names, index=0):
        env = os.environ.copy()
        # The hosted executor cannot enumerate /proc threads for LeakSanitizer;
        # the C fixture independently accounts for every allocation and handle.
        env['ASAN_OPTIONS'] = env.get('ASAN_OPTIONS', '') + ':detect_leaks=0'
        env['UBSAN_OPTIONS'] = env.get('UBSAN_OPTIONS', '') + ':halt_on_error=1'
        for name in names:
            with self.subTest(case=name, asset=index):
                result = subprocess.run([str(self.executable), name, str(self.assets), str(index)],
                                        capture_output=True, text=True, env=env, timeout=30)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn('PASS', result.stdout)

    def test_real_full_assets_hash_eof_metadata_and_readonly_access(self):
        self.cases(['readonly-success'])
        print('real app_io pinned-asset loader executed: 5 assets; 84277400-byte GSP; SHA256 and EOF verified', flush=True)

    def test_failed_open_always_closes_any_returned_handle(self):
        self.cases(['missing-file', 'open-warning-null', 'open-success-null',
                    'open-warning-handle', 'open-error-handle'])

    def test_metadata_error_warning_and_malformed_file_info(self):
        self.cases(['info-error', 'info-warning', 'info-small-result', 'info-large-result',
                    'info-small-size', 'info-large-size', 'wrong-size', 'directory'])

    def test_reads_hash_and_post_read_metadata_fail_closed(self):
        self.cases(['read-error', 'read-warning', 'short-read', 'oversized-result',
                    'hash-mismatch', 'trailing-byte', 'eof-error', 'eof-warning',
                    'late-info-error', 'late-info-warning', 'file-grew', 'file-shrank',
                    'became-directory', 'late-info-small-size', 'close-error', 'close-warning',
                    'allocation-failure'])

    def test_report_write_and_flush_errors_refuse_after_asset_verification(self):
        self.cases(['log-write-error', 'log-write-warning', 'log-short-write',
                    'log-flush-error', 'log-flush-warning'])

    def test_prior_verified_assets_are_released_after_later_asset_failure(self):
        self.cases(['read-warning', 'hash-mismatch', 'close-warning',
                    'log-flush-warning', 'allocation-failure'], index=1)
        self.cases(['info-warning'], index=2)


if __name__ == '__main__':
    unittest.main()
