"""Compare matched JSON results; reject incompatible software/configuration."""
import argparse
import json
from pathlib import Path
import statistics

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--before', nargs='+', required=True, type=Path)
p.add_argument('--after', nargs='+', required=True, type=Path)
a = p.parse_args()
groups = [[json.loads(x.read_text(encoding='utf-8-sig')) for x in paths]
          for paths in (a.before, a.after)]
if min(map(len, groups)) < 3:
    p.error('At least three fresh-process measurements per phase are required')
all_runs = groups[0] + groups[1]
reference = all_runs[0]
for run in all_runs:
    for key in ('schema', 'python', 'torch', 'torch_cuda', 'gpu', 'capability', 'benchmark_sha256',
                'cublas_sha256', 'cublasLt_sha256', 'cublas_version', 'math_mode', 'seed', 'warmup_seconds'):
        if run[key] != reference[key]:
            p.error('Incompatible field: ' + key)
    if run['initial_telemetry']['returncode'] != 0:
        p.error('Missing initial telemetry')
    # Query columns: name,bus,id,driver,vbios,power limit,...
    current = run['initial_telemetry']['csv'].split(',')
    initial = reference['initial_telemetry']['csv'].split(',')
    if current[:6] != initial[:6]:
        p.error('GPU identity, driver, VBIOS or power limit changed')
    if [r['mode'] for r in run['results']] != [r['mode'] for r in reference['results']]:
        p.error('Mode list changed')
    for result, original in zip(run['results'], reference['results']):
        for key in ('n', 'samples', 'batch', 'compute_type', 'input_type', 'output_type', 'output_sha256'):
            if result[key] != original[key]:
                p.error('Incompatible mode field: ' + key)
        if not result['full_256_exact'] or not result['sampled_large_exact']:
            p.error('Numerical validation missing')
print('| Mode | Before TFLOP/s | After TFLOP/s | Change |')
print('|---|---:|---:|---:|')
for idx, original in enumerate(reference['results']):
    med = [statistics.median(run['results'][idx]['median_tflops'] for run in group) for group in groups]
    print(f'| {original["mode"]} | {med[0]:.2f} | {med[1]:.2f} | {(med[1]/med[0]-1)*100:+.1f}% |')
print('\nCompare clocks, temperatures and workload conditions separately. GEMM gain alone does not prove an unlock.')
