# SPDX-License-Identifier: GPL-2.0-only
"""Compile/run the session permit against fake EFI/FAT; never access a GPU."""
import os
from pathlib import Path
import subprocess
import shutil
import sys
import tempfile
import unittest

WIN = Path(__file__).resolve().parents[1]
ROOT = WIN.parent
sys.path.insert(0, str(WIN))
from board_profile import header, parse_report
from test_windows import REPORT


class SessionTests(unittest.TestCase):
    def build_harness(self, directory):
        (directory / "board_profile.h").write_text(header(parse_report(REPORT)))
        exe = directory / "session-tests"
        command = [os.environ.get("CC", "cc"), "-std=c11", "-O1", "-g", "-fshort-wchar",
                       "-Wall", "-Wextra", "-Werror", "-Wno-misleading-indentation",
                       "-D__KERNEL__", "-DGU_UEFI_PORT", "-DGU_ENABLE_UNLOCK=1", "-DGU_SESSION_HELPER=1",
                       "-I" + str(directory), "-I" + str(WIN / "src/compat"),
                       "-I" + str(ROOT / "kernel/gpu_unlock"),
                       "-I" + str(WIN / "deps/edk2"), "-I" + str(WIN / "deps/edk2/X64"),
                       str(WIN / "tests/test_session.c"), "-o", str(exe)]
        result = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        return exe

    def test_permit_protocol_and_failures(self):
        with tempfile.TemporaryDirectory() as td:
            exe = self.build_harness(Path(td))
            subprocess.run([str(exe)], check=True)

    def test_synthetic_powershell_packets_are_accepted_by_c_helper(self):
        pwsh = os.environ.get("PWSH") or shutil.which("pwsh")
        if not pwsh:
            self.skipTest("Set PWSH or install pwsh for cross-language integration")
        with tempfile.TemporaryDirectory(prefix="gu-session-wire-test-") as td:
            directory = Path(td)
            exe = self.build_harness(directory)
            subprocess.run([pwsh, "-NoProfile", "-File", str(WIN / "tests/session_packet_fixture.ps1"),
                            "-OutputDirectory", str(directory),
                            "-ProfileSha256", parse_report(REPORT)["report_sha256"]], check=True)
            subprocess.run([str(exe), str(directory / "boot-option.bin"),
                            str(directory / "request.bin")], check=True)

    def test_session_cannot_be_a_diagnostic_build(self):
        result = subprocess.run([sys.executable, str(WIN / "build.py"), "--session-helper"],
                                capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("requires --experimental and --profile", result.stderr)


if __name__ == "__main__":
    unittest.main()
