"""Three bounded fresh-process baseline runs. Does not close apps or change settings."""
import argparse
import importlib.metadata
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess
import sys
import time
from datetime import datetime, timezone
from benchmark import telemetry, require_idle, sha256

ROOT = Path(__file__).resolve().parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--label', choices=['stock','unlocked','stock-return'], default='stock')
    args = parser.parse_args()
    if Path(sys.prefix).resolve() != (ROOT/'.venv').resolve():
        raise RuntimeError('Run with benchmark/.venv/Scripts/python.exe')
    stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ')
    folder = ROOT/'results'/(args.label+'-'+stamp)
    folder.mkdir(parents=True, exist_ok=False)
    report = dict(schema=1, label=args.label, utc=stamp, completed=False, runs=[], idle_checks=[],
                  python=sys.version, executable=sys.executable, platform=platform.platform(),
                  workload=dict(size=4096,samples=15,batch=5,warmup_seconds=0.75,runs=3,process_timeout_seconds=180),
                  scripts={p.name:sha256(p) for p in ROOT.glob('*.py')},
                  setup_files={p.name:sha256(p) for p in ROOT.iterdir() if p.is_file() and p.suffix in ('.ps1','.txt','.json','.md')},
                  packages={d.metadata['Name']:d.version for d in importlib.metadata.distributions()},
                  sources=['https://pytorch.org/get-started/previous-versions/',
                           'https://download.pytorch.org/whl/cu128/torch/',
                           'https://docs.nvidia.com/cuda/archive/12.8.1/cublas/index.html'],
                  controls='Same software/data/modes and stock clocks/power. FP32 pedantic control. No modification of GPU settings.',
                  limitations='Exact small GEMM plus 1024 sampled large outputs; finite check covers the full large output. Three short processes do not establish long-duration stability or unlock.')
    try:
        freeze = subprocess.run([sys.executable,'-m','pip','freeze','--all'],capture_output=True,text=True,check=True,timeout=30)
        (folder/'pip-freeze.txt').write_text(freeze.stdout,encoding='utf-8')
        for index in range(1,4):
            for check in range(3):
                sample=telemetry(); report['idle_checks'].append(sample); require_idle(sample)
                if check < 2: time.sleep(1)
            output=folder/('run-%d.json'%index)
            cmd=[sys.executable,str(ROOT/'benchmark.py'),'--out',str(output),'--label',args.label]
            print('Starting short process',index,'of 3',flush=True)
            result=subprocess.run(cmd,capture_output=True,text=True,timeout=180)
            (folder/('run-%d.log'%index)).write_text(result.stdout+'\n'+result.stderr,encoding='utf-8')
            print(result.stdout,end='',flush=True)
            if result.returncode:
                raise RuntimeError('Measurement failed: '+result.stderr)
            run=json.loads(output.read_text(encoding='utf-8')); assert run['completed'] and len(run['results'])==3
            report['runs'].append(dict(path=output.name,sha256=sha256(output)))
            time.sleep(2)
        runs=[json.loads((folder/r['path']).read_text(encoding='utf-8')) for r in report['runs']]
        from compare import matched
        matched([runs,runs])
        report['summary']={}
        for i,first in enumerate(runs[0]['results']):
            values=[r['results'][i]['median_tflops'] for r in runs]
            report['summary'][first['mode']]=dict(median_tflops=statistics.median(values),
                min_tflops=min(values),max_tflops=max(values),runs_tflops=values,
                run_cv=statistics.pstdev(values)/statistics.mean(values),
                correctness_all_exact=all(r['results'][i]['full_256_exact'] and r['results'][i]['sampled_large_exact'] for r in runs))
        report['completed']=True
        report['warnings']=[name+': run-to-run CV exceeds 5%' for name,value in report['summary'].items() if value['run_cv']>0.05]
    except Exception as error:
        report['failure']=repr(error)
        raise
    finally:
        (folder/'summary.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
        print('Evidence folder:',folder,flush=True)


if __name__=='__main__':
    main()
