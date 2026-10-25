# SPDX-License-Identifier: GPL-2.0-only
"""Production capture I/O with fake file handles; no Windows or EFI execution."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

WIN = Path(__file__).resolve().parents[1]
ROOT = WIN.parent


class CaptureIoTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory()
        cls.directory = Path(cls.temporary.name)
        # Only compiler admission for denied entry-point tests, never a hardware profile.
        (cls.directory / 'board_profile.h').write_text(
            '#define GU_PROFILE_VERSION 1\n#define GU_PROFILE_ID 0x270510deU\n'
            '#define GU_PROFILE_TAG "synthetic-denial-test"\n')
        cls.executables = {}
        for mode in (0, 1):
            output = cls.directory / ('capture-' + str(mode))
            command = [os.environ.get('CC', 'cc'), '-std=c11', '-O1', '-g',
                       '-ffunction-sections', '-fdata-sections', '-fshort-wchar',
                       '-D__KERNEL__', '-DGU_UEFI_PORT', '-DGU_SESSION_HELPER=0',
                       '-DGU_ENABLE_UNLOCK='+str(mode), '-I'+str(cls.directory),
                       '-I'+str(WIN/'src/compat'), '-I'+str(ROOT/'kernel/gpu_unlock'),
                       '-I'+str(WIN/'deps/edk2'), '-I'+str(WIN/'deps/edk2/X64'),
                       str(WIN/'tests/test_capture_io.c'), '-Wl,--gc-sections', '-o', str(output)]
            subprocess.run(command, check=True, capture_output=True, text=True)
            cls.executables[mode] = output

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def run_case(self, name):
        result = subprocess.run([str(self.executables[0]), name], check=True,
                                capture_output=True, text=True, timeout=5)
        self.assertIn('console callbacks=0', result.stdout)

    def test_buffering_and_capture_limits(self):
        for name in ('buffering', 'chunk-limit', 'total-limit', 'exact-limit', 'null-chunk'):
            with self.subTest(name=name):
                self.run_case(name)

    def test_existing_and_failed_creation_never_overwrite(self):
        for name in ('existing', 'late-nonempty', 'create-error-handle', 'duplicate-open', 'missing-run'):
            with self.subTest(name=name):
                self.run_case(name)

    def test_write_flush_close_errors_preserve_report_and_first_error(self):
        for name in ('short-write', 'write-error', 'flush-error', 'close-error'):
            with self.subTest(name=name):
                self.run_case(name)

    def test_fallback_always_closes_capture_and_report(self):
        for name in ('fallback-close', 'fallback-error'):
            with self.subTest(name=name):
                self.run_case(name)

    def test_experimental_entry_points_are_denied(self):
        subprocess.run([str(self.executables[1])], check=True, capture_output=True, timeout=5)


if __name__ == '__main__':
    unittest.main()
