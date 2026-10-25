# SPDX-License-Identifier: GPL-2.0-only
"""Exercise read-only loaded-PE parsing against synthetic images."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

WIN = Path(__file__).resolve().parents[1]


class PeCaptureTests(unittest.TestCase):
    def test_bounded_normalization_and_malformed_images(self):
        with tempfile.TemporaryDirectory() as temporary:
            executable = Path(temporary) / 'capture-pe'
            command = [os.environ.get('CC', 'cc'), '-std=c11', '-O1', '-g',
                       '-Wall', '-Wextra', '-Werror', '-fshort-wchar',
                       '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                       '-I'+str(WIN/'src'), '-I'+str(WIN/'deps/edk2'),
                       '-I'+str(WIN/'deps/edk2/X64'),
                       str(WIN/'tests/test_capture_pe.c'), str(WIN/'src/capture_pe.c'),
                       str(WIN/'src/sha256.c'), '-o', str(executable)]
            compiled = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            env = os.environ.copy()
            # This pure parser allocates nothing; LeakSanitizer's /proc scan is
            # unavailable in some test containers. ASan bounds checks remain on.
            env['ASAN_OPTIONS'] = env.get('ASAN_OPTIONS', '') + ':detect_leaks=0'
            env['UBSAN_OPTIONS'] = env.get('UBSAN_OPTIONS', '') + ':halt_on_error=1'
            result = subprocess.run([str(executable)], capture_output=True,
                                    text=True, timeout=30, env=env)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('source preservation: PASS', result.stdout)


if __name__ == '__main__':
    unittest.main()
