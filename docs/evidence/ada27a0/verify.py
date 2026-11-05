#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Verify published bytes, correctness fields, request identity and summaries offline."""
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent

def require(value, message):
    if not value:
        raise SystemExit(message)

expected = {}
for line in (ROOT / 'SHA256SUMS').read_text().splitlines():
    digest, name = line.split('  ', 1)
    require(name not in expected and not Path(name).is_absolute() and '..' not in Path(name).parts,
            'Invalid or duplicate manifest entry')
    expected[name] = digest
actual = {p.relative_to(ROOT).as_posix() for p in ROOT.rglob('*')
          if p.is_file() and p.name != 'SHA256SUMS' and '__pycache__' not in p.parts}
require(actual == set(expected), 'Manifest does not cover exactly the evidence files')
for name, digest in expected.items():
    require(hashlib.sha256((ROOT / name).read_bytes()).hexdigest() == digest, f'Hash mismatch: {name}')
requests = []
for phase, rate in [('stock', 1), ('unlocked', 0), ('recovered', 1)]:
    folder = ROOT / phase
    rates = json.loads((folder / 'sm-rates.json').read_text())['rates']
    require(all(rates[k] == rate for k in ('fmla32', 'imla2', 'imla4')), f'{phase}: rate state')
    numerical = (folder / 'numerical.txt').read_text()
    require(len(re.findall(r'failures=0\b', numerical)) == 6 and len(re.findall(r'failures=', numerical)) == 6,
            f'{phase}: seeded correctness')
    for rep in range(3):
        rows = [json.loads(x) for x in (folder / f'gemm-{rep}.jsonl').read_text().splitlines()]
        require(len(rows) == 6 and {(r['mode'], r['n']) for r in rows} ==
                {(m, n) for m in ('FP16/FP32', 'BF16/FP32') for n in (1024, 4096, 8192)}, 'GEMM shape coverage')
        require(all(r['failures'] == 0 and r['cells_checked'] == r['n']**2 and len(r['times_ms']) == 5
                    and all(t > 0 for t in r['times_ms']) for r in rows), f'{phase}: GEMM correctness/samples')
        modes = json.loads((folder / f'tensor-{rep}.json').read_text())['modes']
        require(set(modes) == {'f16_f32', 'tf32', 'u4_s32'}, 'Tensor mode coverage')
        require(all(v['mismatches'] == 0 and v['cells_checked'] > 0 and len(v['rates_Top_per_s']) == 3
                    for v in modes.values()), f'{phase}: tensor correctness/samples')
    strata = [json.loads(x) for x in (folder / 'strata/results.jsonl').read_text().splitlines()]
    require(len(strata) == 10 and sum(r['excluded_warmup'] for r in strata) == 1, 'Strata coverage')
    require(all(r['done'] and r['timings']['cache_n'] == 0 and r['usage']['completion_tokens'] == 256
                for r in strata), f'{phase}: Strata completion/cache')
    requests.append([(r['case'], r['rep'], r['request_sha256']) for r in strata])
    if phase != 'stock':
        soak = json.loads((folder / 'soak/results.json').read_text())
        require(soak[-1]['elapsed_s'] >= 300 and soak[-1]['gemms'] ==
                (216400 if phase == 'unlocked' else 136400), f'{phase}: soak coverage')
        require(all(r['mismatches'] == 0 for r in soak), f'{phase}: soak mismatches')
require(requests[0] == requests[1] == requests[2], 'Request identities differ')
with tempfile.TemporaryDirectory(prefix='ada27a0-verify-') as temp:
    copy = Path(temp) / 'evidence'
    shutil.copytree(ROOT, copy)
    subprocess.run([sys.executable, str(copy / 'summarize.py')], check=True)
    for name in ('summary.json', 'comparison.csv'):
        require((copy / name).read_bytes() == (ROOT / name).read_bytes(), f'Recomputed {name} differs')
print('PASS: hashes, phase/rate states, numerical results, matched requests and all published medians')
