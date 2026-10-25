# SPDX-License-Identifier: GPL-2.0-only
"""Actual unlock postconditions through read-only EFI PCI/MMIO observations."""
from __future__ import annotations
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

WIN = Path(__file__).resolve().parents[1]


class StagedStateTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory()
        cls.root = Path(cls.temporary.name)
        (cls.root / 'board_profile.h').write_text('''#pragma once
#define GU_PROFILE_VERSION 1
#define GU_PROFILE_ID 0x270510deU
#define GU_BOARD_SUBSYSTEM 0xe1301462U
#define GU_BOARD_CHIP 0x193000a1U
#define GU_PROFILE_SEGMENT 0U
#define GU_PROFILE_BUS 1U
#define GU_PROFILE_DEVICE 0U
#define GU_PROFILE_FUNCTION 0U
#define GU_PROFILE_BAR_BASE 0x83000000ULL
#define GU_PROFILE_BAR_SIZE 0x1000000ULL
#define GU_PROFILE_ROM_SIZE 150528U
#define GU_PROFILE_VRAM 16376U
#define GU_PROFILE_PROM_HASH ""
#define GU_PROFILE_ROM_HASH ""
#define GU_PROFILE_TAG "synthetic-staged-state-tests"
''')
        cls.executable = cls.root / 'staged_state'
        command = [os.environ.get('CC', 'cc'), '-std=c11', '-O1', '-g',
                   '-fshort-wchar', '-Wall', '-Wextra', '-Werror', '-DGU_ENABLE_UNLOCK=1',
                   '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                   '-I' + str(cls.root), '-I' + str(WIN / 'src'),
                   '-I' + str(WIN.parent / 'kernel/gpu_unlock'),
                   '-I' + str(WIN / 'deps/edk2'), '-I' + str(WIN / 'deps/edk2/X64'),
                   str(WIN / 'tests/test_staged_state.c'), str(WIN / 'src/staged_state.c'),
                   '-o', str(cls.executable)]
        result = subprocess.run(command, capture_output=True, text=True)
        if result.returncode:
            cls.temporary.cleanup()
            raise AssertionError(result.stdout + result.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def cases(self, names):
        env = os.environ.copy()
        env['ASAN_OPTIONS'] = env.get('ASAN_OPTIONS', '') + ':detect_leaks=0'
        env['UBSAN_OPTIONS'] = env.get('UBSAN_OPTIONS', '') + ':halt_on_error=1'
        for name in names:
            with self.subTest(case=name):
                result = subprocess.run([str(self.executable), name], capture_output=True,
                                        text=True, env=env, timeout=10)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn(': PASS', result.stdout)

    def test_completed_unlock_in_each_ownership_and_logging_state(self):
        self.cases(['owned-success', 'unowned-success', 'silent-closed-success',
                    'silent-open-success', 'payload-permissions-success',
                    'reserved-bits-success', 'silent-failure'])

    def test_lifecycle_and_interface_refuse_before_hardware_access(self):
        self.cases(['not-started', 'not-quiesced', 'not-saved', 'no-flr', 'two-flrs',
                    'no-writes', 'fault', 'retained', 'master-pending', 'dma-count',
                    'allocations', 'master-attribute-error', 'master-hardware-warning',
                    'null-pci', 'null-config-read', 'null-mem-read', 'bar-base',
                    'bar-size', 'pcie-missing', 'pcie-unaligned', 'pcie-overflow', 'log-closed'])

    def test_independent_before_after_pci_and_chip_checks(self):
        self.cases(['wrong-device', 'wrong-subsystem', 'bme', 'decode-disabled',
                    'command-inaccessible', 'pending', 'pending-inaccessible',
                    'late-device', 'late-bme', 'late-pending', 'late-chip',
                    'late-lifecycle', 'callback-fault'])

    def test_every_transport_error_and_warning_stops_immediately(self):
        self.cases([f'read-{kind}-{index}' for kind in ('error', 'warning')
                    for index in range(1, 24)])

    def test_each_log_failure_stops_without_hardware_writes(self):
        self.cases([f'log-failure-{index}' for index in range(1, 15)])
        self.cases([f'log-disappears-{index}' for index in (1, 6, 18, 23)])

    def test_inaccessible_registers_cannot_masquerade_as_unlock(self):
        self.cases([f'register-{index}-badf1234' for index in range(14)])
        # All ones is legitimate only in the four permission registers.
        self.cases([f'register-{index}-ffffffff' for index in range(14)
                    if index not in (2, 3, 4, 5)])

    def test_actual_unlock_values_and_quiescence_are_required(self):
        self.cases(['register-0-193000a2', 'register-1-4000', 'register-5-ffffff8f',
                    'register-6-88888880', 'register-7-7', 'register-9-0',
                    'register-10-fe', 'register-11-0', 'register-13-10'])
        self.cases([f'register-8-{1 << bit:x}' for bit in range(27)])


if __name__ == '__main__':
    unittest.main()
