"""0.6.4 upgrade tests reuse existing synthetic assets; Windows storage is mocked."""
import os
from pathlib import Path
import shutil
import subprocess
import unittest

WIN = Path(__file__).resolve().parents[1]


class StagedUsbTests(unittest.TestCase):
    def test_actual_unlock_usb_update_guards(self):
        pwsh = os.environ.get('PWSH') or shutil.which('pwsh')
        if not pwsh:
            self.skipTest('PowerShell 7 is required for mocked USB tests')
        result = subprocess.run([pwsh, '-NoProfile', '-File',
                                 str(WIN / 'tests/Test-StagedUsb.ps1')],
                                text=True, capture_output=True, timeout=90)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('Staged USB checks passed:', result.stdout)
        self.assertIn('Windows storage mocked', result.stdout)


if __name__ == '__main__':
    unittest.main()
