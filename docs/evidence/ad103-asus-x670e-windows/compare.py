"""Compare two completed three-process benchmark phases with matching provenance."""
import argparse
import json
import math
from pathlib import Path
import statistics
from benchmark import sha256


def load(folder):
    summary=json.loads((folder/'summary.json').read_text(encoding='utf-8'))
    if not summary['completed'] or len(summary['runs'])!=3:
        raise ValueError('A completed three-process phase is required')
    files=[folder/r['path'] for r in summary['runs']]
    if len({p.resolve() for p in files})!=3 or any(p.resolve().parent!=folder.resolve() for p in files):
        raise ValueError('Invalid or duplicate result paths')
    for p,r in zip(files,summary['runs']):
        if sha256(p)!=r['sha256']: raise ValueError('Measurement integrity mismatch: '+str(p))
    return [json.loads(p.read_text(encoding='utf-8')) for p in files]


def matched(groups):
    reference=groups[0][0]
    keys=('schema','python','numpy','torch','torch_cuda','gpu','capability','benchmark_sha256',
          'cublas_sha256','cublasLt_sha256','cublas_version','math_mode','seed','warmup_seconds','process_environment')
    identity=('name','uuid','pci.bus_id','pci.device_id','driver_version','vbios_version','power.limit')
    for run in groups[0]+groups[1]:
        if not run.get('completed') or len(run['results'])!=3: raise ValueError('Incomplete measurement')
        for key in keys:
            if run[key]!=reference[key]: raise ValueError('Incompatible field: '+key)
        t=run['initial_telemetry']; original=reference['initial_telemetry']
        if t['returncode'] or len(t['devices'])!=1: raise ValueError('Missing GPU telemetry')
        for key in identity:
            if t['devices'][0][key]!=original['devices'][0][key]: raise ValueError('GPU identity/settings mismatch: '+key)
        for result,first in zip(run['results'],reference['results']):
            for key in ('mode','n','samples','batch','compute_type','input_type','output_type','output_sha256'):
                if result[key]!=first[key]: raise ValueError('Incompatible workload: '+key)
            if not result['full_256_exact'] or not result['sampled_large_exact']:
                raise ValueError('Numerical validation failed')
            if not math.isfinite(result['median_tflops']) or result['median_tflops']<=0:
                raise ValueError('Invalid measurement')


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--before',type=Path,required=True)
    p.add_argument('--after',type=Path,required=True)
    args=p.parse_args()
    if args.before.resolve()==args.after.resolve(): p.error('Choose distinct phases')
    groups=[load(args.before),load(args.after)]; matched(groups)
    print('| Mode | Before TFLOP/s | After TFLOP/s | Change |')
    print('|---|---:|---:|---:|')
    for i,result in enumerate(groups[0][0]['results']):
        med=[statistics.median(r['results'][i]['median_tflops'] for r in group) for group in groups]
        print(f'| {result["mode"]} | {med[0]:.2f} | {med[1]:.2f} | {(med[1]/med[0]-1)*100:+.1f}% |')
    print('Review clocks, temperature and spread; GEMM throughput alone does not prove an unlock.')


if __name__=='__main__': main()
