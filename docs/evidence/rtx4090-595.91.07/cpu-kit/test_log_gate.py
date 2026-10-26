#!/usr/bin/env python3
"""CPU replay only. Does not import CUDA, load modules, or change the machine."""
from pathlib import Path
import subprocess
from tempfile import TemporaryDirectory
import unittest

ROOT = Path(__file__).resolve().parent
GOOD = (ROOT / 'synthetic-pass.log').read_text()


def accept(log, bdf='0000:03:00.0'):
  with TemporaryDirectory() as tmp:
    p = Path(tmp) / 'transaction.log'; p.write_text(log)
    result = subprocess.run(['sh', '-c', 'GU_BDF=$1; . "$2"; gu_validate "$3"',
      'cpu-test', bdf, str(ROOT / 'ad102-log-gate.sh'), str(p)], capture_output=True)
    return result.returncode == 0


class TestLogGate(unittest.TestCase):
  def test_zero_spellings(self):
    self.assertTrue(accept(GOOD))
    self.assertTrue(accept(GOOD.replace('readout1=0 fields=', 'readout1=0x0 fields=')))

  def test_missing_duplicate_contradictory(self):
    lines = GOOD.splitlines()
    for i in range(len(lines)):
      self.assertFalse(accept('\n'.join(lines[:i] + lines[i+1:])))
      self.assertFalse(accept(GOOD + lines[i] + '\n'))
    payload = next(line for line in lines if 'AD102 payload result:' in line)
    self.assertFalse(accept(GOOD.replace(payload, payload + ' marker_ok=0')))
    self.assertFalse(accept(GOOD.replace(payload, payload + ' marker_ok=1')))
    permission = next(line for line in lines if 'permission after-payload: WPR_CFG' in line)
    self.assertFalse(accept(GOOD.replace(permission, permission + ' valid=0')))
    for old, new in [('marker_ok=1', 'marker_ok=0'), ('readout1=0 fields=', 'readout1=0x410080 fields='),
      ('BME=0 pending=0', 'BME=1 pending=0'), ('plm=0x8b8f', 'plm=0'), ('full=1', 'full=0')]:
      self.assertFalse(accept(GOOD.replace(old, new)))
    self.assertFalse(accept(GOOD, '0000:04:00.0'))
    self.assertFalse(accept(GOOD, ''))
    self.assertFalse(accept(GOOD + 'firmware did not stop; keeping its DMA buffers\n'))


if __name__ == '__main__':
  unittest.main()
