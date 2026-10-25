# SPDX-License-Identifier: GPL-2.0-only
"""Run the production firmware-metadata capture against synthetic EFI services."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

WIN = Path(__file__).resolve().parents[1]


class FirmwareCaptureTests(unittest.TestCase):
    def test_graph_driver_identity_and_failures_never_mutate(self):
        with tempfile.TemporaryDirectory() as temporary:
            executable = Path(temporary) / 'capture'
            command = [os.environ.get('CC', 'cc'), '-std=c11', '-O1', '-g',
                       '-fshort-wchar', '-DGU_ENABLE_UNLOCK=0',
                       '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                       '-I'+str(WIN/'src'), '-I'+str(WIN/'deps/edk2'),
                       '-I'+str(WIN/'deps/edk2/X64')]
            command += [str(WIN/'tests/test_capture.c')]
            command += [str(WIN/'src'/(name+'.c')) for name in
                        ('capture', 'capture_pe', 'sha256', 'format')]
            compiled = subprocess.run(command+['-o', str(executable)], capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stdout+compiled.stderr)
            env = os.environ.copy()
            env['ASAN_OPTIONS'] = env.get('ASAN_OPTIONS', '')+':detect_leaks=0'
            env['UBSAN_OPTIONS'] = env.get('UBSAN_OPTIONS', '')+':halt_on_error=1'
            result = subprocess.run([str(executable)], capture_output=True, text=True, env=env, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
            self.assertIn('pools balanced, source preserved, mutations=0: PASS', result.stdout)


if __name__ == '__main__':
    unittest.main()
