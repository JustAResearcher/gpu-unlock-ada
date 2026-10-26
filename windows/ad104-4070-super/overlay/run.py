"""Run one unchanged-math benchmark in a guarded, low-priority child process."""
import argparse
import csv
import json
import os
from pathlib import Path
import subprocess
import sys
import time
from datetime import datetime, timezone


ROOT = Path(__file__).resolve().parent
FIELDS = ('name,pci.device_id,driver_version,vbios_version,power.limit,'
          'temperature.gpu,utilization.gpu,memory.used,memory.free,clocks.sm')


def telemetry():
    result = subprocess.run(
        ['nvidia-smi', '--query-gpu=' + FIELDS, '--format=csv,noheader,nounits'],
        capture_output=True, text=True, check=True, timeout=10,
        creationflags=subprocess.CREATE_NO_WINDOW)
    rows = list(csv.reader(result.stdout.splitlines(), skipinitialspace=True))
    if len(rows) != 1:
        raise RuntimeError('Expected exactly one GPU')
    data = dict(zip(FIELDS.split(','), rows[0], strict=True))
    for key in ('temperature.gpu', 'utilization.gpu', 'memory.used', 'memory.free', 'clocks.sm'):
        data[key] = int(data[key])
    data['utc'] = datetime.now(timezone.utc).isoformat()
    return data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--name', required=True)
    parser.add_argument('--label', default='stock')
    parser.add_argument('--smoke', action='store_true')
    args = parser.parse_args()
    if not args.name or any(c not in 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-' for c in args.name):
        parser.error('--name accepts letters, digits, underscores and hyphens only')
    paths = {suffix: ROOT / (args.name + suffix) for suffix in
             ('.json', '.stdout.txt', '.stderr.txt', '.execution.json')}
    if any(path.exists() for path in paths.values()):
        raise FileExistsError('Refusing to overwrite existing run artifacts: ' + args.name)
    before = telemetry()
    if before['pci.device_id'] != '0x278310DE':
        raise RuntimeError('Unexpected GPU device ID')
    # Conservative task-local admission limits; no GPU settings are changed.
    if before['temperature.gpu'] >= 60 or before['utilization.gpu'] > 10 or before['memory.free'] < 2048:
        raise RuntimeError('GPU is not sufficiently idle/cool/free: ' + str(before))
    env = os.environ.copy()
    thread_env = {name: '4' for name in
                  ('OMP_NUM_THREADS', 'OPENBLAS_NUM_THREADS', 'MKL_NUM_THREADS',
                   'NUMEXPR_NUM_THREADS', 'BLIS_NUM_THREADS')}
    env.update(thread_env)
    configuration = ['--size', '256', '--samples', '3', '--batch', '1', '--warmup', '0.05'] if args.smoke else [
        '--size', '4096', '--samples', '15', '--batch', '5', '--warmup', '1']
    command = [sys.executable, str(ROOT / 'benchmark.py'), '--out', str(paths['.json']),
               '--label', args.label] + configuration
    metadata = {'command': command, 'thread_environment': thread_env,
                'priority': 'BELOW_NORMAL_PRIORITY_CLASS', 'preflight': before,
                'monitored_telemetry': [], 'started_utc': datetime.now(timezone.utc).isoformat()}
    started = time.monotonic()
    process = None
    try:
        with paths['.stdout.txt'].open('x', encoding='utf-8') as stdout, paths['.stderr.txt'].open('x', encoding='utf-8') as stderr:
            process = subprocess.Popen(command, cwd=ROOT, env=env, stdout=stdout, stderr=stderr,
                                       creationflags=subprocess.BELOW_NORMAL_PRIORITY_CLASS | subprocess.CREATE_NO_WINDOW)
            metadata['pid'] = process.pid
            while process.poll() is None:
                sample = telemetry()
                metadata['monitored_telemetry'].append(sample)
                if sample['temperature.gpu'] >= 75 or sample['memory.free'] < 1024:
                    raise RuntimeError('Safety monitor threshold reached: ' + str(sample))
                if time.monotonic() - started > 60:
                    raise TimeoutError('Benchmark exceeded 60 seconds')
                time.sleep(0.5)
            metadata['exit_code'] = process.returncode
            if process.returncode != 0:
                raise RuntimeError('Benchmark failed; inspect ' + str(paths['.stderr.txt']))
        metadata['completed'] = True
    except Exception as exc:
        metadata['completed'] = False
        metadata['error'] = str(exc)
        raise
    finally:
        if process is not None and process.poll() is None:
            process.terminate()
            process.wait(timeout=10)
        metadata['elapsed_seconds'] = time.monotonic() - started
        metadata['finished_utc'] = datetime.now(timezone.utc).isoformat()
        paths['.execution.json'].write_text(json.dumps(metadata, indent=2), encoding='utf-8')
    print(paths['.stdout.txt'].read_text(encoding='utf-8'), end='')
    print('Execution evidence:', paths['.execution.json'])


if __name__ == '__main__':
    main()
