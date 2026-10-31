# SPDX-License-Identifier: GPL-2.0-only
"""Verify the sanitized evidence using files and the standard library only."""
import copy
import hashlib
import json
import math
from pathlib import Path
import re
import statistics

import compare

ROOT = Path(__file__).resolve().parent
UUID_ALIAS = 'GPU-REDACTED-SAME-DEVICE'
EXECUTABLE_ALIAS = '<benchmark-venv>/Scripts/python.exe'
MODES = [('fp32_pedantic', 0, 69), ('fp16_fp32', 2, 68), ('bf16_fp32', 14, 68)]
SOURCES = {
    'benchmark.py': 'ba10ce79168965564c23f08560185000542769afd5f721c45932478f822c2c95',
    'compare.py': 'ccff1f2a636192b35bbb9b4a4db8443e698ba919c68f3821b5097b29f78bfd86',
    'run_stock.py': '1808110045e31ae5308b1a5187cff9bad3c1f33629a76119d57b0cb0d481d5d3',
}
OUTPUT_HASH = '0e3b4c1a854bc95d06fb91f688b1f4f739fe7e76972c889fa1bbeb042ac7c1bc'
EXPECTED_FILES = {'.gitattributes', 'README.md', 'LICENSE', 'verify.py', 'RUN0003.sanitized.txt', *SOURCES}
EXPECTED_FILES |= {f'{phase}/{name}' for phase in ('stock', 'after')
                   for name in ('summary.json', 'run-1.json', 'run-2.json', 'run-3.json')}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def close(actual, expected, label):
    require(isinstance(actual, (int, float)) and math.isfinite(actual) and
            math.isclose(actual, expected, rel_tol=1e-12, abs_tol=1e-12), 'Recalculation differs: ' + label)


def verify_checksums():
    entries = {}
    for line in (ROOT / 'SHA256SUMS').read_text(encoding='ascii').splitlines():
        match = re.fullmatch(r'([0-9a-f]{64})  ([A-Za-z0-9._/-]+)', line)
        require(match is not None, 'Malformed checksum line')
        digest, name = match.groups()
        require(name in EXPECTED_FILES and name not in entries, 'Unexpected/duplicate checksum path: ' + name)
        path = ROOT / name
        require(not path.is_symlink() and path.resolve().is_relative_to(ROOT) and path.is_file(), 'Missing/unsafe file: ' + name)
        require(sha(path) == digest, 'Checksum mismatch: ' + name)
        entries[name] = digest
    require(set(entries) == EXPECTED_FILES, 'Incomplete checksum inventory')
    actual = {p.relative_to(ROOT).as_posix() for p in ROOT.rglob('*') if p.is_file() and '__pycache__' not in p.parts}
    require(actual == EXPECTED_FILES | {'SHA256SUMS'}, 'Unexpected/missing evidence files')
    for name, digest in SOURCES.items():
        require(entries[name] == digest, 'Measurement source is not the exact recorded source: ' + name)
    return len(entries)


def validate_measurements(groups):
    require(len(groups) == 2 and all(len(group) == 3 for group in groups), 'Exactly three runs per phase required')
    compare.matched(groups)
    for phase, runs in zip(('stock', 'unlocked'), groups):
        for run in runs:
            require(run['schema'] == 2 and run['label'] == phase and run['completed'] is True, 'Wrong phase/schema/completion')
            require(run['benchmark_sha256'] == SOURCES['benchmark.py'], 'Wrong measurement source hash')
            require(run['executable'] == EXECUTABLE_ALIAS and
                    run['process_environment']['CUDA_VISIBLE_DEVICES'] == UUID_ALIAS, 'Personal fields not consistently redacted')
            require(run['math_mode'] == 16 and run['seed'] == 20261024 and run['warmup_seconds'] == 0.75,
                    'Unexpected fixed measurement controls')
            initial = run['initial_telemetry']['devices'][0]
            require(float(initial['utilization.gpu']) <= 3 and float(initial['memory.used']) <= 128 and
                    float(initial['temperature.gpu']) <= 60, 'Recorded initial idle gate failed')
            telemetry = [run['initial_telemetry']]
            for result, (mode, input_type, compute_type) in zip(run['results'], MODES):
                require(result['mode'] == mode and result['input_type'] == input_type and
                        result['compute_type'] == compute_type and result['output_type'] == 'FP32', 'Wrong precision mode')
                require(result['n'] == 4096 and result['samples'] == 15 and result['batch'] == 5,
                        'Unexpected matrix/timing workload')
                times = result['milliseconds']
                require(len(times) == 15 and all(math.isfinite(t) and t > 0 for t in times), 'Invalid timing sample')
                median = statistics.median(times)
                close(result['median_ms'], median, mode + ' median_ms')
                close(result['median_tflops'], 2 * 4096 ** 3 / (median * 1e9), mode + ' median_tflops')
                close(result['min_ms'], min(times), mode + ' min_ms')
                close(result['max_ms'], max(times), mode + ' max_ms')
                close(result['sample_cv'], statistics.pstdev(times) / statistics.mean(times), mode + ' sample_cv')
                require(result['full_256_exact'] is True and result['sampled_large_exact'] is True and
                        result['full_256_max_abs_error'] == 0 and result['sampled_large_max_abs_error'] == 0 and
                        result['sampled_large_elements'] == 1024 and result['output_sha256'] == OUTPUT_HASH,
                        'Numerical/output checks differ')
                require(0 < result['warmup_batches'] <= 2000, 'Warmup bounds differ')
                telemetry.extend((result['telemetry_before'], result['telemetry_after']))
            for sample in telemetry:
                require(sample['returncode'] == 0 and len(sample['devices']) == 1 and sample['error'] == '', 'Telemetry incomplete')
                device = sample['devices'][0]
                require(device['uuid'] == UUID_ALIAS, 'Device alias differs')
                for key in ('name', 'pci.bus_id', 'pci.device_id', 'driver_version', 'vbios_version', 'power.limit'):
                    require(device[key] == initial[key], 'Telemetry identity/control drift: ' + key)
    result = {}
    for index, (mode, _, _) in enumerate(MODES):
        values = [[run['results'][index]['median_tflops'] for run in group] for group in groups]
        medians = [statistics.median(v) for v in values]
        result[mode] = dict(stock_median_tflops=medians[0], after_median_tflops=medians[1],
                            change_percent=(medians[1] / medians[0] - 1) * 100,
                            stock_runs_tflops=values[0], after_runs_tflops=values[1],
                            after_run_cv_percent=100 * statistics.pstdev(values[1]) / statistics.mean(values[1]))
    return result


def rejection_checks(groups):
    """Small in-memory negative checks; no evidence files are changed."""
    changes = [lambda g: g[1][0].update(benchmark_sha256='0' * 64),
               lambda g: g[1][0]['results'][0].update(median_tflops=1),
               lambda g: g[1][0]['results'][1].update(output_sha256='0' * 64),
               lambda g: g[1][0]['results'][2].update(sampled_large_exact=False),
               lambda g: g[1][0]['initial_telemetry']['devices'][0].update(driver_version='different'),
               lambda g: g[1][0]['results'][0]['telemetry_after']['devices'][0].update(uuid='different')]
    for change in changes:
        modified = copy.deepcopy(groups)
        change(modified)
        try:
            validate_measurements(modified)
        except ValueError:
            continue
        raise ValueError('Verifier accepted an altered measurement')
    return len(changes)


def main():
    count = verify_checksums()
    groups = [compare.load(ROOT / phase) for phase in ('stock', 'after')]
    result = validate_measurements(groups)
    negative_count = rejection_checks(groups)
    print(json.dumps(dict(verified_files=count, verified_runs=6, verified_mode_measurements=18,
                          negative_checks=negative_count, offline_only=True, results=result), indent=2))


if __name__ == '__main__':
    main()
