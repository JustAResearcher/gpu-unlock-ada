"""Mocked PowerShell launcher checks; no interpreter/GPU execution."""
import os
from pathlib import Path
import shutil
import subprocess
import unittest

WIN = Path(__file__).resolve().parents[1]


class BenchmarkLauncherTests(unittest.TestCase):
    @unittest.skipIf(os.name == 'nt', 'Do not run the actual Windows launcher in a host unit test')
    def test_actual_launcher_rejects_non_windows_before_process_launch(self):
        pwsh = os.environ.get('PWSH') or shutil.which('pwsh')
        if not pwsh:
            self.skipTest('PowerShell 7 is required for the non-Windows refusal check')
        result = subprocess.run([pwsh, '-NoProfile', '-File',
                                 str(WIN / 'usb-staged/Run-Benchmark.ps1'), '-Mode', 'stock'],
                                text=True, capture_output=True, timeout=15)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Run the benchmark launcher in Windows.', result.stdout + result.stderr)

    def test_process_selection_and_failure_propagation(self):
        pwsh = os.environ.get('PWSH') or shutil.which('pwsh')
        if not pwsh:
            self.skipTest('PowerShell 7 is required for mocked launcher tests')
        result = subprocess.run([pwsh, '-NoProfile', '-File',
                                 str(WIN / 'tests/Test-BenchmarkLauncher.ps1')],
                                text=True, capture_output=True, timeout=90)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('Benchmark launcher checks passed:', result.stdout)


if __name__ == '__main__':
    unittest.main()
