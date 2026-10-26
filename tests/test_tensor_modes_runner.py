# SPDX-License-Identifier: GPL-2.0-only
"""The tensor probe runner pins the power cap, including a laptop's [N/A]."""
import hashlib
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

spec = importlib.util.spec_from_file_location(
    "runner", Path(__file__).resolve().parents[1] / "diagnostics/bench/tensor_modes_driver_runner.py")
runner = importlib.util.module_from_spec(spec)
spec.loader.exec_module(runner)


class Checked(Exception):
    """Raised in place of loading libcuda: every GPU identity check passed."""


class PowerLimitTests(unittest.TestCase):
    def run_checks(self, reported, expected):
        with tempfile.NamedTemporaryFile() as cubin:
            smi = subprocess.CompletedProcess(
                [], 0, f"GPU-test, NVIDIA GeForce, 00000000:01:00.0, 580.178.04, {reported}\n")
            argv = ["runner", cubin.name, "--sha256", hashlib.sha256().hexdigest(),
                    "--expect-uuid", "GPU-test", "--expect-power-limit", expected]
            with mock.patch.object(sys, "argv", argv), \
                    mock.patch.object(runner.subprocess, "run", return_value=smi), \
                    mock.patch.object(runner.C, "CDLL", side_effect=Checked):
                runner.main()

    def test_desktop_cap_must_match(self):
        with self.assertRaises(Checked):
            self.run_checks("240.00", "240")
        for reported in ("250.00", "[N/A]"):
            with self.subTest(reported=reported), self.assertRaises(AssertionError):
                self.run_checks(reported, "240")

    def test_laptop_without_a_cap_must_say_so(self):
        with self.assertRaises(Checked):
            self.run_checks("[N/A]", "N/A")
        with self.assertRaises(AssertionError):
            self.run_checks("80.00", "N/A")
