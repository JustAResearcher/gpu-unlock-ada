# SPDX-License-Identifier: GPL-2.0-only
"""Entry-point control flow with real PCI/ROM checks and USB logger."""
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest
import test_diagnostic as fixture_module
WIN, ROOT = fixture_module.WIN, fixture_module.ROOT

sys.path.insert(0,str(WIN))
from board_profile import header, HEX

class RehearsalEntryTests(unittest.TestCase):
    def test_arming_cancellation_partial_failure_and_handoff(self):
        # Derive the exact *synthetic* ROM pins using the existing diagnostic
        # fixture. No uploaded capture or firmware binary is a test input.
        fixture_module.DiagnosticTests.setUpClass()
        try:
            fixture=fixture_module.DiagnosticTests.out/'entry-fixture.txt'
            subprocess.run([str(fixture_module.DiagnosticTests.exe),'success',str(fixture)],check=True,capture_output=True)
            board={}
            data=fixture.read_bytes()
            for line in data.decode().splitlines():
                if line.startswith('PROFILE '):
                    for pair in line[8:].split():
                        k,v=pair.split('=');board[k]=v if k.endswith('sha256') else int(v,16 if k in HEX else 10)
            import hashlib
            board['report_sha256']=hashlib.sha256(data).hexdigest()
        finally:
            fixture_module.DiagnosticTests.tearDownClass()
        with tempfile.TemporaryDirectory() as temp:
            out=Path(temp);(out/'board_profile.h').write_text(header(board))
            sources=[WIN/'tests/test_rehearsal_entry.c']
            sources += [WIN/'src'/(x+'.c') for x in ('app','app_io','ownership','format','sha256','checks','port','compat','probe')]
            sources += [ROOT/'kernel/gpu_unlock'/(x+'.c') for x in ('vbios_610','gfw_eval')]
            cmd=[os.environ.get('CC','cc'),'-std=c11','-O1','-g','-fshort-wchar',
                 '-ffunction-sections','-fdata-sections','-fsanitize=address,undefined','-fno-omit-frame-pointer',
                 '-D__KERNEL__','-DGU_UEFI_PORT','-DGU_ENABLE_UNLOCK=0','-DGU_REHEARSAL=1','-DGU_SESSION_HELPER=0',
                 '-I'+str(out),'-I'+str(WIN/'src'),'-I'+str(WIN/'src/compat'),
                 '-I'+str(ROOT/'kernel/gpu_unlock'),'-I'+str(WIN/'deps/edk2'),'-I'+str(WIN/'deps/edk2/X64')]
            result=subprocess.run(cmd+list(map(str,sources))+['-Wl,--gc-sections','-o',str(out/'entry')],capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stdout+result.stderr)
            env=os.environ.copy()
            env['ASAN_OPTIONS']=env.get('ASAN_OPTIONS','')+':detect_leaks=0'
            env['UBSAN_OPTIONS']=env.get('UBSAN_OPTIONS','')+':halt_on_error=1'
            for name in ('start-return','start-error','secure-on','secure-warning','wrong-subsystem','wrong-rom',
                         'preload-fail','prepare-fail','cancel-error','escape','timeout','wrong-key','watchdog-warning',
                         'intent-log-fail','run-before-mutation','run-partial','run-warning','run-fault',
                         'restore-fail','handoff-log-fail','close-error'):
                with self.subTest(name=name):
                    result=subprocess.run([str(out/'entry'),name],capture_output=True,text=True,timeout=15,env=env)
                    self.assertEqual(result.returncode,0,result.stdout+result.stderr)
                    self.assertIn('direct_mutations=0',result.stdout)

if __name__=='__main__':unittest.main()
