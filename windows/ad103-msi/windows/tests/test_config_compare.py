# SPDX-License-Identifier: GPL-2.0-only
"""CPU-only exhaustive PCI status transition comparison in all build modes."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_staged_transaction import PROFILE

WIN = Path(__file__).resolve().parents[1]


class ConfigCompareTests(unittest.TestCase):
    def test_exact_comparison_except_narrow_clean_staged_status_clear(self):
        with tempfile.TemporaryDirectory(prefix='gu-config-compare-') as temporary:
            root = Path(temporary)
            (root/'board_profile.h').write_text(PROFILE)
            env = os.environ.copy()
            env['ASAN_OPTIONS'] = env.get('ASAN_OPTIONS', '')+':detect_leaks=0'
            env['UBSAN_OPTIONS'] = env.get('UBSAN_OPTIONS', '')+':halt_on_error=1'
            for mode, staged, rehearsal, unlock in [('default', 0, 0, 0), ('rehearsal', 0, 1, 0), ('staged', 1, 0, 1)]:
                with self.subTest(mode=mode):
                    exe = root/mode
                    command = [os.environ.get('CC', 'cc'), '-std=c11', '-O1', '-g', '-fshort-wchar',
                               '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                               f'-DGU_STAGED_UNLOCK={staged}', f'-DGU_REHEARSAL={rehearsal}', f'-DGU_ENABLE_UNLOCK={unlock}',
                               '-I'+str(root), '-I'+str(WIN/'src'), '-I'+str(WIN/'deps/edk2'), '-I'+str(WIN/'deps/edk2/X64'),
                               str(WIN/'tests/test_config_compare.c'), str(WIN/'src/checks.c'), '-o', str(exe)]
                    compiled = subprocess.run(command, capture_output=True, text=True)
                    self.assertEqual(compiled.returncode, 0, compiled.stdout+compiled.stderr)
                    result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=20, env=env)
                    self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
                    self.assertIn('config compare PASS staged='+str(staged), result.stdout)


if __name__ == '__main__':
    unittest.main()
