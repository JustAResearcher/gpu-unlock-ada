# SPDX-License-Identifier: GPL-2.0-only
"""Pinned Windows preload plus actual silent staged poststate at StartImage."""
from __future__ import annotations
import hashlib
import os
from pathlib import Path
import re
import struct
import subprocess
import tempfile
import unittest

WIN = Path(__file__).resolve().parents[1]


def disk_fixture() -> bytes:
    disk = bytearray(1024)
    def word(at, value): struct.pack_into('<H', disk, at, value)
    def dword(at, value): struct.pack_into('<I', disk, at, value)
    disk[:2] = b'MZ'; dword(60, 0x80); disk[0x80:0x84] = b'PE\0\0'
    word(0x84, 0x8664); word(0x86, 1); word(0x94, 240); word(0x96, 2)
    opt = 0x98
    word(opt, 0x20b); dword(opt + 16, 0x1000); dword(opt + 32, 0x1000)
    dword(opt + 36, 512); dword(opt + 56, 8192); dword(opt + 60, 512)
    word(opt + 68, 10); dword(opt + 108, 16)
    sec = opt + 240
    disk[sec:sec + 5] = b'.text'; dword(sec + 8, 512); dword(sec + 12, 0x1000)
    dword(sec + 16, 512); dword(sec + 20, 512); dword(sec + 36, 0x60000020)
    disk[512] = 0xc3
    return bytes(disk)


class StagedHandoffTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.directory = Path(cls.temp.name)
        fixture = (WIN / 'tests/test_rehearsal_handoff.c').read_text()
        fixture = fixture.replace('#include "../src/rehearsal_handoff.c"', '')
        for function in ('GuLog', 'GuLogReady', 'GuRehearsalRestored'):
            fixture, count = re.subn(r'^(?:VOID|BOOLEAN|EFI_STATUS) ' + function +
                                    r'\([^\n]*\)\{[^\n]*\}\n', '', fixture, flags=re.M)
            if count != 1:
                raise AssertionError(f'fixture definition changed: {function}')
        fixture = fixture.replace('int main(int argc,char **argv)',
                                  'int RehearsalFixtureMain(int argc,char **argv)')
        (cls.directory / 'handoff_fixture.h').write_text(fixture)
        (cls.directory / 'board_profile.h').write_text('''#pragma once
#define GU_PROFILE_VERSION 1
#define GU_PROFILE_ID 0x270510deU
#define GU_BOARD_SUBSYSTEM 0xe1301462U
#define GU_BOARD_CHIP 0x193000a1U
#define GU_PROFILE_BAR_BASE 0x83000000ULL
#define GU_PROFILE_BAR_SIZE 0x1000000ULL
#define GU_PROFILE_VRAM 16376U
''')
        cls.executables = {}
        for mode in ('good', 'bytes', 'hash'):
            pin = cls.directory / mode
            pin.mkdir()
            digest = hashlib.sha256(disk_fixture()).hexdigest() if mode != 'hash' else '0' * 64
            size = 1024 if mode != 'bytes' else 1023
            (pin / 'rehearsal_profile.h').write_text(
                '#define GU_STAGED_PROFILE_VERSION 1\n' +
                f'#define GU_STAGED_WINDOWS_PRELOAD_BYTES {size}U\n' +
                f'#define GU_STAGED_WINDOWS_PRELOAD_SHA256 "{digest}"\n')
            executable = pin / 'handoff'
            command = [os.environ.get('CC', 'cc'), '-std=c11', '-O1', '-g', '-fshort-wchar',
                       '-DGU_STAGED_UNLOCK=1', '-DGU_ENABLE_UNLOCK=1', '-DGU_REHEARSAL=0',
                       '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                       '-I' + str(pin), '-I' + str(cls.directory), '-I' + str(WIN / 'src'),
                       '-I' + str(WIN / 'deps/edk2'), '-I' + str(WIN / 'deps/edk2/X64'),
                       '-I' + str(WIN.parent / 'kernel/gpu_unlock'),
                       str(WIN / 'tests/test_staged_handoff.c'), str(WIN / 'src/staged_state.c'),
                       str(WIN / 'src/sha256.c'), '-o', str(executable)]
            compiled = subprocess.run(command, capture_output=True, text=True)
            if compiled.returncode:
                cls.temp.cleanup()
                raise AssertionError(compiled.stdout + compiled.stderr)
            cls.executables[mode] = executable

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def cases(self, cases, mode='good'):
        env = os.environ.copy()
        env['ASAN_OPTIONS'] = env.get('ASAN_OPTIONS', '') + ':detect_leaks=0'
        env['UBSAN_OPTIONS'] = env.get('UBSAN_OPTIONS', '') + ':halt_on_error=1'
        for case in cases:
            with self.subTest(case=case, pin=mode):
                result = subprocess.run([str(self.executables[mode]), case], capture_output=True,
                                        text=True, env=env, timeout=10)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn('PASS', result.stdout)

    def test_success_and_stable_same_boot_loaded_image(self):
        self.cases(['success', 'cancel', 'late-file-replaced', 'start-warning'])

    def test_stale_windows_file_and_wrong_profile_pins(self):
        self.cases(['stale-windows'])
        self.cases(['pin-bytes'], 'bytes')
        self.cases(['pin-hash'], 'hash')

    def test_all_preload_warning_returns_refuse_before_mutation(self):
        self.cases(['prepare-warning-' + name for name in
                    ('map', 'handle', 'enumeration', 'volume', 'open', 'info',
                     'read', 'close', 'free', 'load')])

    def test_silent_late_loaded_image_and_volume_checks(self):
        self.cases(['late-loaded-code', 'late-loaded-path', 'late-media',
                    'late-log-reopened', 'late-fault', 'start-warning-map',
                    'start-warning-handle'])

    def test_fresh_completed_unlock_guard_after_usb_close(self):
        self.cases(['late-graph', 'late-guard-warning', 'late-bme', 'late-feat',
                    'late-selector', 'late-effective', 'late-gfw', 'late-wpr2',
                    'late-retained', 'late-dma', 'late-pci-warning', 'late-mmio-warning'])


if __name__ == '__main__':
    unittest.main()
