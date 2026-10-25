# SPDX-License-Identifier: GPL-2.0-only
import os
import hashlib
import io
from pathlib import Path
import subprocess
import sys
import tempfile
import tarfile
import unittest

WIN=Path(__file__).resolve().parents[1]
ROOT=WIN.parent
sys.path.insert(0,str(WIN))
from board_profile import parse_report,header
from prepare_assets import selected_members

# Synthetic test input. Never package an unlock EFI built with these values.
REPORT=b'''SYNTHETIC UNIT TEST ONLY - NOT A HARDWARE REPORT
PROFILE version=1 id=270510de subsystem=123410de segment=0 bus=3 device=0 function=0
PROFILE bar_base=90000000 bar_size=1000000
PROFILE chip=193000a1 vram_mib=16384
PROFILE prom_size=1048576 prom_sha256=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
PROFILE rom_size=1048576 rom_sha256=bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb
PROFILE fwsec_parsed=1
DIAGNOSTIC_COMPLETE eligible=1 gpu_writes=0
FINAL mode=diagnostic status=0 stage=complete gpu_writes=0
'''

class ProfileTests(unittest.TestCase):
 def test_complete_report_and_crlf(self):
  v=parse_report(REPORT);self.assertEqual(v['bus'],3)
  self.assertEqual(parse_report(REPORT.replace(b'\n',b'\r\n'))['chip'],0x193000a1)
  self.assertIn('#define GU_PROFILE_BUS 0x3U',header(v))
 def test_reject_incomplete_failed_or_written(self):
  for changed in [REPORT[:REPORT.index(b'FINAL')],REPORT.replace(b'status=0',b'status=8000000000000003'),
                  REPORT.replace(b'gpu_writes=0',b'gpu_writes=1'),REPORT.replace(b'fwsec_parsed=1',b'fwsec_parsed=0'),
                  REPORT+REPORT,REPORT+b'PROFILE bus=3\n',REPORT+b'PROFILE unknown=1\n']:
   with self.subTest(changed=changed[-90:]),self.assertRaises(ValueError):parse_report(changed)
 def test_reject_wrong_board_or_layout(self):
  for old,new in [(b'id=270510de',b'id=270410de'),(b'chip=193000a1',b'chip=192000a1'),
                  (b'vram_mib=16384',b'vram_mib=32768'),(b'bus=3',b'bus=256'),
                  (b'function=0',b'function=1'),(b'bar_base=90000000',b'bar_base=90000001'),
                  (b'bar_size=1000000',b'bar_size=2000000'),(b'subsystem=123410de',b'subsystem=100000000'),
                  (b'rom_size=1048576',b'rom_size=0'),(b'bus=3',b'bus=3;evil'),
                  (b'chip=193000a1',b'chip=1193000a1')]:
   with self.subTest(new=new),self.assertRaises(ValueError):parse_report(REPORT.replace(old,new))
 def test_reject_bad_hash_or_oversize(self):
  for value in [b'0'*64,b'a'*63,b'G'*64,b'";bad']:
   with self.assertRaises(ValueError):parse_report(REPORT.replace(b'a'*64,value))
  with self.assertRaises(ValueError):parse_report(b'x'*131073)

class ArchiveTests(unittest.TestCase):
 def read(self,entries,digest=None):
  data=io.BytesIO()
  with tarfile.open(fileobj=data,mode='w') as tar:
   for name,kind,content in entries:
    info=tarfile.TarInfo(name);info.type=kind
    info.size=len(content) if kind==tarfile.REGTYPE else 0
    info.linkname='../outside'
    tar.addfile(info,io.BytesIO(content) if info.size else None)
  data.seek(0)
  with tarfile.open(fileobj=data,mode='r:') as tar:
   return selected_members(tar,{'wanted':digest})
 def test_reads_only_named_regular_member(self):
  self.assertEqual(self.read([('../ignored',tarfile.REGTYPE,b'ignored'),
                             ('wanted',tarfile.REGTYPE,b'ok')],hashlib.sha256(b'ok').hexdigest()),
                   {'wanted':b'ok'})
 def test_reject_links_duplicates_missing_and_bad_hash(self):
  for entries in [[('wanted',tarfile.SYMTYPE,b'')],[],
                  [('wanted',tarfile.REGTYPE,b'a'),('wanted',tarfile.REGTYPE,b'b')]]:
   with self.assertRaises(ValueError):self.read(entries)
  with self.assertRaises(ValueError):self.read([('wanted',tarfile.REGTYPE,b'a')],'0'*64)

class UefiGuardTests(unittest.TestCase):
 def test_real_adapters_and_handoff_checks(self):
  with tempfile.TemporaryDirectory() as td:
   d=Path(td);(d/'board_profile.h').write_text(header(parse_report(REPORT)))
   for mode in (0,1):
    exe=d/f'guards-{mode}'
    cmd=[os.environ.get('CC','cc'),'-std=c11','-O1','-g','-ffunction-sections','-fdata-sections',
         '-fshort-wchar','-D__KERNEL__','-DGU_UEFI_PORT',f'-DGU_ENABLE_UNLOCK={mode}',
         '-I'+str(d),'-I'+str(WIN/'src/compat'),'-I'+str(ROOT/'kernel/gpu_unlock'),
         '-I'+str(WIN/'deps/edk2'),'-I'+str(WIN/'deps/edk2/X64'),
         str(WIN/'tests/test_port.c'),str(WIN/'src/port.c'),'-Wl,--gc-sections','-o',str(exe)]
    subprocess.run(cmd,check=True,capture_output=True,text=True)
    subprocess.run([str(exe)],check=True)

if __name__=='__main__':unittest.main()
