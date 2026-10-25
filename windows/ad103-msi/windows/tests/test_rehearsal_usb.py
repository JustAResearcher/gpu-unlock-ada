# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the active-rehearsal updater using mocked storage and temporary files."""
import os
from pathlib import Path
import shutil
import subprocess
import unittest

WIN = Path(__file__).resolve().parents[1]


class RehearsalUsbTests(unittest.TestCase):
    def test_updater_guards_and_preservation(self):
        pwsh = os.environ.get('PWSH') or shutil.which('pwsh')
        if not pwsh:
            self.skipTest('PowerShell 7 is required for the mocked USB tests')
        result = subprocess.run([pwsh, '-NoProfile', '-File',
                                 str(WIN/'tests/Test-RehearsalUsb.ps1')],
                                text=True, capture_output=True, timeout=90)
        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
        self.assertIn('Rehearsal USB checks passed:', result.stdout)
        self.assertIn('Windows storage mocked', result.stdout)


if __name__ == '__main__':
    unittest.main()
