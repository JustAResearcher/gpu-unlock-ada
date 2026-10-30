# SPDX-License-Identifier: GPL-2.0-only
"""Keep exact AD104 desktop and experimental AD104M options aligned."""
import contextlib
import importlib.util
import io
from pathlib import Path
import subprocess
import unittest
from unittest import mock

spec = importlib.util.spec_from_file_location(
    "boot_option", Path(__file__).resolve().parents[1] / "tools/print_boot_option.py")
boot = importlib.util.module_from_spec(spec)
spec.loader.exec_module(boot)


class AD104BootOptionTests(unittest.TestCase):
    def test_exact_id_only(self):
        output = ("0000:01:00.0 VGA [10de:2786]\n"
                  "0000:02:00.0 VGA [10de:1b80]\n"
                  "0000:11:00.0 VGA [1002:164e]")
        self.assertEqual(boot.candidates(output), [("0000:01:00.0", "2786")])

    def test_prints_the_hardware_tested_exact_target_and_opt_in(self):
        result = subprocess.CompletedProcess([], 0, "0000:01:00.0 VGA [10de:2786]\n")
        output = io.StringIO()
        with mock.patch.object(boot.subprocess, "run", return_value=result), \
                contextlib.redirect_stdout(output):
            self.assertEqual(boot.main(), 0)
        lines = output.getvalue().splitlines()
        self.assertIn("RTX 4070 (experimental) at 0000:01:00.0 [10de:2786]:", lines)
        self.assertIn("gpu_unlock.targets=0000:01:00.0 gpu_unlock.ad104_experimental=1", lines)

    def test_4080_laptop_exact_id_and_address_only(self):
        output = ("0000:03:00.0 VGA [10DE:27E0] (rev a1)\n"
                  "0000:04:00.0 VGA [10de:27a0]\n"
                  "03:00.0 VGA [10de:27e0]\n"
                  "0000:05:00.0 VGA [1002:27e0]\n")
        self.assertEqual(boot.candidates(output), [("0000:03:00.0", "27e0")])

    def test_prints_4080_laptop_experimental_exact_option(self):
        result = subprocess.CompletedProcess([], 0, "0000:03:00.0 VGA [10de:27e0]\n")
        output = io.StringIO()
        with mock.patch.object(boot.subprocess, "run", return_value=result), \
                contextlib.redirect_stdout(output):
            self.assertEqual(boot.main(), 0)
        self.assertEqual(output.getvalue(),
            "RTX 4080 Laptop GPU (experimental) at "
            "0000:03:00.0 [10de:27e0]:\n"
            "gpu_unlock.targets=0000:03:00.0 gpu_unlock.ad104m_experimental=1\n\n")

    def test_unrecognized_27a0_does_not_print_boot_option(self):
        result = subprocess.CompletedProcess([], 0, "0000:03:00.0 VGA [10de:27a0]\n")
        output, errors = io.StringIO(), io.StringIO()
        with mock.patch.object(boot.subprocess, "run", return_value=result), \
                contextlib.redirect_stdout(output), contextlib.redirect_stderr(errors):
            self.assertEqual(boot.main(), 2)
        self.assertEqual(output.getvalue(), "")
        self.assertNotIn("27a0", boot.TARGETS)
        self.assertIn("No supported card was found", errors.getvalue())
