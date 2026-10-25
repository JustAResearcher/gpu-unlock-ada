# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the real bounded boot-manager handoff using synthetic EFI services."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

WIN = Path(__file__).resolve().parents[1]
CASES = '''success reentry cancel allocation-failure map-error map-version unmapped ambiguous
    duplicate-handle enumeration-error missing-partition missing-file oversize tiny-file
    directory bad-info-size file-grew info-error short-read extra-byte read-error close-error
    wrong-device wrong-parent wrong-table wrong-code-type wrong-data-type wrong-image-size
    wrong-loaded-path load-options null-load-image load-security load-error log-failure
    usb removable no-media not-logical bad-block-size partition-size not-esp not-gpt
    bad-partition-revision bad-path-length path-multi-instance path-guid pe-machine pe-subsystem
    pe-entry pe-raw-overrun pe-virtual-overrun pe-zero-sections pe-header-truncated pe-image-overflow
    late-graph late-media late-loaded-code late-loaded-path late-volume-path late-fault
    before-mutation log-still-open unload-error invalid-root invalid-file pe-entry-bss
    pe-directory-overrun pe-certificate-overrun pe-overlap'''.split()

class RehearsalHandoffTests(unittest.TestCase):
    def test_preload_and_one_shot_handoff(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            (directory/'board_profile.h').write_text(
                '#define GU_PROFILE_VERSION 1\n#define GU_PROFILE_ID 0x270510deU\n')
            exe = directory/'handoff'
            command = [os.environ.get('CC', 'cc'), '-std=c11', '-O1', '-g', '-fshort-wchar',
                       '-DGU_REHEARSAL=1', '-DGU_ENABLE_UNLOCK=0',
                       '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                       '-I'+str(directory), '-I'+str(WIN/'src'), '-I'+str(WIN/'deps/edk2'),
                       '-I'+str(WIN/'deps/edk2/X64'), str(WIN/'tests/test_rehearsal_handoff.c'),
                       str(WIN/'src/sha256.c'), '-o', str(exe)]
            compiled = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stdout+compiled.stderr)
            env = os.environ.copy()
            env['ASAN_OPTIONS'] = env.get('ASAN_OPTIONS', '')+':detect_leaks=0'
            env['UBSAN_OPTIONS'] = env.get('UBSAN_OPTIONS', '')+':halt_on_error=1'
            for case in CASES:
                with self.subTest(case=case):
                    result = subprocess.run([str(exe), case], capture_output=True, text=True,
                                            env=env, timeout=10)
                    self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
                    self.assertIn('PASS', result.stdout)

if __name__ == '__main__':
    unittest.main()
