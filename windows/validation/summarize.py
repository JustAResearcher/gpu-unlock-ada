# SPDX-License-Identifier: GPL-2.0-only
"""Compare private matched GEMM reports, emitting only allowlisted numeric data."""
import argparse
import json
import math
from pathlib import Path
import re
import statistics

FORMATS = ('fp16', 'bf16', 'tf32', 'fp32_control', 'fp8_e4m3')
MATCH = ('identity', 'torch_version', 'cuda_version', 'device_capability', 'script_sha256')
CASE_MATCH = ('n', 'input_dtype', 'output_dtype', 'kernel', 'quantization_timed',
              'reference', 'sample_count', 'atol', 'rtol', 'repetitions', 'batches')

def require(condition, message):
    if not condition:
        raise ValueError(message)

def number(value, positive=False):
    require(type(value) in (int, float) and math.isfinite(value), 'Invalid numeric value')
    require(value > 0 if positive else value >= 0, 'Invalid numeric range')
    return value

def validate(run):
    require(run['complete'] is True and run['hardware_settings_changed'] is False,
            'Incomplete run or changed hardware settings')
    require(isinstance(run['identity'], list) and len(run['identity']) == 5
            and all(isinstance(x, str) and x.strip() for x in run['identity']),
            'Missing device identity')
    require(all(isinstance(run[key], str) and run[key].strip()
                for key in ('torch_version', 'cuda_version')), 'Missing software identity')
    capability = run['device_capability']
    require(isinstance(capability, list) and len(capability) == 2
            and all(type(x) is int and 0 <= x <= 20 for x in capability),
            'Missing device capability')
    require(re.fullmatch('[0-9a-f]{64}', run['script_sha256']) is not None,
            'Missing collector identity')
    rows = run['results']
    require(len(rows) == len(FORMATS) and [x['name'] for x in rows] == list(FORMATS),
            'Missing, duplicate, or reordered formats')
    for row in rows:
        require(row['correctness_passed'] is True and row['finite'] is True,
                'Numerical validation failed')
        require(type(row['n']) is int and 128 <= row['n'] <= 16384, 'Invalid matrix size')
        require(type(row['batches']) is int and row['batches'] == 3, 'Expected three batches')
        require(type(row['repetitions']) is int and row['repetitions'] > 0, 'Invalid repetitions')
        require(type(row['sample_count']) is int and 0 < row['sample_count'] <= row['n']**2,
                'Invalid correctness sample count')
        for key in ('max_abs_error', 'rms_error', 'atol', 'rtol'):
            number(row[key])
        samples = row['ms_per_matmul']
        require(len(samples) == row['batches'], 'Timing batch count mismatch')
        for value in samples:
            number(value, positive=True)
        median = statistics.median(samples)
        require(math.isclose(number(row['median_ms'], True), median, rel_tol=1e-9),
                'Reported median does not match timing samples')
        expected = 2 * row['n']**3 / (median * 1e9)
        require(math.isclose(number(row['dense_tflops'], True), expected, rel_tol=1e-9),
                'Reported rate does not match timing samples')
    return rows

def summarize(stock, trial):
    before, after = validate(stock), validate(trial)
    require(stock['label'] in ('stock', 'restored') and trial['label'] == 'trial',
            'Expected stock/restored and trial labels')
    for field in MATCH:
        require(stock[field] == trial[field], 'Unmatched private field: ' + field)
    rows = []
    for name, left, right in zip(FORMATS, before, after):
        for field in CASE_MATCH:
            require(left[field] == right[field], 'Unmatched case field: ' + field)
        # Construct a new record; never copy dictionaries, paths, telemetry,
        # timestamps, device identities, hashes, or arbitrary strings from inputs.
        rows.append(dict(format=name, n=left['n'], stock_tflops=round(left['dense_tflops'], 4),
                         trial_tflops=round(right['dense_tflops'], 4),
                         gain_percent=round((right['dense_tflops']/left['dense_tflops']-1)*100, 2),
                         stock_batch_ms=[round(x, 6) for x in left['ms_per_matmul']],
                         trial_batch_ms=[round(x, 6) for x in right['ms_per_matmul']],
                         numerical_checks_passed=True))
    return dict(schema='windows-gemm-public-summary-v1', rows=rows,
                boot_or_unlock_success_inferred=False,
                scope='One matched process pair; dense GEMM proxy, sampled FP64 correctness. '
                      'Review boot evidence and clocks/thermals privately. Not application speed or long-term reliability.')

def load(path):
    require(path.stat().st_size <= 4 * 1024 * 1024, 'Input exceeds size bound')
    return json.loads(path.read_text(encoding='utf-8-sig'),
                      parse_constant=lambda _: (_ for _ in ()).throw(ValueError('Nonfinite JSON value')))

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--stock', type=Path, required=True)
    parser.add_argument('--trial', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    try:
        result = summarize(load(args.stock), load(args.trial))
        with args.output.open('x', encoding='utf-8') as handle:
            handle.write(json.dumps(result, indent=2, allow_nan=False) + '\n')
    except (KeyError, TypeError, ValueError, OverflowError, OSError):
        # Do not echo malformed input values or private filesystem paths.
        parser.exit(1, 'Summary refused: invalid/unmatched inputs or unavailable output. Raw data was not printed.\n')
    print('Numeric-only summary written. Review it before publishing; raw inputs remain private.')

if __name__ == '__main__':
    main()
