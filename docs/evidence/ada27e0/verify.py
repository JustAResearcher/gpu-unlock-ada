#!/usr/bin/env python3
"""Offline only: verify hashes and recompute summaries from supplied observations.
--write-summary is a maintainer aid, and skips hash validation while replacing
summary.json. Default mode never writes files or accesses a GPU/network.
"""
import csv
import hashlib
import json
import math
from pathlib import Path
import re
import statistics
import sys

BASE = Path(__file__).resolve().parent

def load(name):
    return json.loads((BASE / name).read_text())

def stats(values):
    assert values and all(math.isfinite(v) for v in values)
    return {'samples': len(values), 'median': statistics.median(values),
            'min': min(values), 'max': max(values)}

def rounds(name):
    parts = re.split(r'^=== round ([123]): [^\n]+ ===\n',
                     (BASE / name).read_text(), flags=re.M)
    assert parts[0] == '' and parts[1::2] == ['1', '2', '3'], name
    return parts[2::2]

def recompute():
    result = {'schema': 1, 'kind': 'offline-derived-summary', 'states': {}}
    for state in ['stock', 'unlocked']:
        mma, tensor, gemm = {}, {}, {}
        for chunk in rounds(f'mma-{state}.txt'):
            matches = re.findall(r'^(\S+)\s+[\d.]+\s+([\d./]+)$', chunk, re.M)
            assert len(matches) == 7
            for mode, raw in matches:
                values = list(map(float, raw.split('/')))
                assert len(values) == 5
                mma.setdefault(mode, []).extend(values)
        for chunk in rounds(f'tensor-{state}.txt'):
            record = json.loads(chunk)
            assert record['reps'] == 3 and record['iters'] == 20000
            assert len(record['modes']) == 3
            for mode, data in record['modes'].items():
                assert data['mismatches'] == 0 and data['cells_checked'] == 270336
                assert len(data['rates_Top_per_s']) == 3
                tensor.setdefault(mode, []).extend(data['rates_Top_per_s'])
        for chunk in rounds(f'gemm-{state}.txt'):
            records = [json.loads(line) for line in chunk.splitlines() if line.strip()]
            assert len(records) == 6
            for record in records:
                assert record['failures'] == 0 and record['cells_checked'] == record['n']**2
                assert record['warmups'] == 10 and record['iterations'] == 20
                assert record['reps'] == 5 and len(record['times_ms']) == 5
                key = (record['mode'], record['n'])
                gemm.setdefault(key, []).extend(2*record['n']**3/(ms*1e9) for ms in record['times_ms'])
        assert len(mma) == 7 and all(len(v) == 15 for v in mma.values())
        assert len(tensor) == 3 and all(len(v) == 9 for v in tensor.values())
        assert len(gemm) == 6 and all(len(v) == 15 for v in gemm.values())
        numerical = (BASE / f'numerical-{state}.txt').read_text()
        cases = re.findall(r'^(FP16/FP32|BF16/FP32): .*failures=(\d+), max_abs=([\d.]+)$', numerical, re.M)
        assert len(cases) == 6 and all(int(f) == 0 and float(e) == 0 for _,f,e in cases)
        rates = load(f'sm-rates-{state}.json')
        for phase in ['before', 'after']:
            assert rates[phase]['hardware_changes'] is False
            assert rates[phase]['rates']['fmla32'] == (1 if state == 'stock' else 0)
        data = {'mma': {k: stats(v) for k,v in mma.items()},
                'checked_tensor': {k: stats(v) for k,v in tensor.items()},
                'gemm': [{'mode':k[0], 'n':k[1], **stats(v)} for k,v in sorted(gemm.items())],
                'seeded_cases': len(cases), 'seeded_failures': 0,
                'FP8_numerical_validation': False}
        for family in ['telemetry', 'power-limits']:
            with (BASE/f'{family}-{state}.csv').open() as stream:
                rows = [{k.strip():v.strip() for k,v in row.items()} for row in csv.DictReader(stream)]
            ts = [float(r['relative_seconds']) for r in rows]
            assert ts[0] == 0 and all(b > a for a,b in zip(ts,ts[1:]))
            data[family] = {'samples':len(rows), 'duration_seconds':ts[-1]}
            for field in rows[0]:
                if field == 'relative_seconds':
                    continue
                values = [r[field] for r in rows]
                if field.startswith('clocks_event_reasons.'):
                    data[family][field] = {'active_samples': values.count('Active')}
                elif all(re.fullmatch(r'-?\d+(?:\.\d+)?',v) for v in values):
                    data[family][field] = stats(list(map(float,values)))
            if family == 'power-limits':
                assert all(r['query_exit'] == '0' for r in rows)
        result['states'][state] = data
    records = load('run-records.json')
    for state in ['stock', 'unlocked']:
        r = records[state]
        assert r['complete'] and len(r['runs']) == 9 and all(x['exit'] == 0 for x in r['runs'])
        assert r['desktop_active'] and r['power_cap_fixed'] is False
    manifest = load('manifest.json')
    assert len(manifest['sha256']) == 18
    for digest in manifest['sha256'].values():
        assert re.fullmatch('[0-9a-f]{64}', digest)
    result['matched_manifest_entries'] = 18
    comparison = []
    for family in ['mma','checked_tensor']:
        for mode, before in result['states']['stock'][family].items():
            after = result['states']['unlocked'][family][mode]
            comparison.append({'family':family, 'mode':mode, 'before':before['median'],
                               'after':after['median'], 'change_percent':(after['median']/before['median']-1)*100})
    for before in result['states']['stock']['gemm']:
        after = next(v for v in result['states']['unlocked']['gemm'] if (v['mode'],v['n']) == (before['mode'],before['n']))
        comparison.append({'family':'gemm','mode':before['mode'],'n':before['n'],'before':before['median'],
                           'after':after['median'],'change_percent':(after['median']/before['median']-1)*100})
    result['comparison'] = comparison
    return result

def main():
    summary = recompute()
    if sys.argv[1:] == ['--write-summary']:
        (BASE/'summary.json').write_text(json.dumps(summary,indent=2)+'\n')
        return
    assert not sys.argv[1:], 'Usage: python3 verify.py [--write-summary]'
    listed = {}
    for line in (BASE/'SHA256SUMS').read_text().splitlines():
        digest, name = line.split('  ',1)
        assert '/' not in name and name != 'SHA256SUMS' and name not in listed
        assert hashlib.sha256((BASE/name).read_bytes()).hexdigest() == digest, name
        listed[name] = digest
    actual = {p.name for p in BASE.iterdir() if p.is_file() and p.name != 'SHA256SUMS'}
    assert set(listed) == actual, 'SHA256SUMS must cover every other file'
    assert load('summary.json') == summary, 'summary differs from raw observations'
    sensitive = [r'GPU-[0-9a-fA-F-]{36}', r'\b[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}\b',
                 r'\b(?:[0-9a-fA-F]{4,8}:)[0-9a-fA-F]{2}:[0-9a-fA-F]{2}\.[0-7]\b',
                 r'/(?:home|Users)/[^/\s]+', r'(?<![\w.])(?:\d{1,3}\.){3}\d{1,3}(?![\w.])', chr(45)*5 + r'BEGIN [A-Z ]*PRIVATE KEY' + chr(45)*5,
                 r'\b[\w.+-]+@[\w.-]+\.[A-Za-z]{2,}\b']
    for name in actual:
        content = (BASE/name).read_text()
        for pattern in sensitive:
            assert not re.search(pattern,content), f'Privacy scan: {name}: {pattern}'
    print(f'PASS: {len(listed)} file hashes; both states: 7 MMA x15, 3 tensor x9, 6 GEMM x15; 6 seeded cases each; zero checked errors; all medians recomputed; privacy patterns clear.')

if __name__ == '__main__':
    main()
