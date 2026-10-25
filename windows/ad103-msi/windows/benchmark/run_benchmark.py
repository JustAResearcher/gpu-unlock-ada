#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""User-mode, driver-JIT CUDA microbenchmark. Never performs an unlock."""
from __future__ import annotations

import argparse
import csv
import ctypes as C
from datetime import datetime, timezone
import hashlib
import io
import json
import math
from pathlib import Path
import platform
import re
import statistics
import subprocess
import sys
import time
import uuid

PROTOCOL = 'ada-sm89-driver-ptx-v1'
SCHEMA = 1
THREADS = 128
BLOCKS_PER_SM = 4
ITERATIONS = 4096
PROBES = (16, 64, 256, 1024)
WARMUPS = 3
SAMPLES = 9
MAX_LAUNCH_MS = 100.0
MAX_REPORT_BYTES = 1024 * 1024
MODES = ('fp16_fp32', 'bf16_fp32', 'scalar_fp32')
INPUT_BITS = {'fp16_fp32': 0x3c003c00, 'bf16_fp32': 0x3f803f80, 'scalar_fp32': 0x3f800000}
FIELDS = ('uuid', 'name', 'pci.bus_id', 'driver_version', 'vbios_version',
          'power.limit', 'clocks.current.sm', 'clocks.current.memory',
          'temperature.gpu', 'utilization.gpu', 'pstate', 'power.draw')
NOTICE = ('Stock/unlocked are user-supplied labels, not hardware attestation. '
          'This measures synthetic instruction throughput, not game or application performance. '
          'Clock, temperature and background load differences can change results.')


def utc_now():
    return datetime.now(timezone.utc).isoformat()


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def numeric(value, field, positive=False):
    try:
        valid = not isinstance(value, bool) and isinstance(value, (int, float)) and math.isfinite(value)
    except OverflowError:
        valid = False
    if not valid:
        raise ValueError(field + ' must be finite numeric data.')
    if value < 0 or (positive and value == 0):
        raise ValueError(field + ' is outside the allowed range.')
    return value


def normalized_gpu_uuid(value):
    if not isinstance(value, str) or not re.fullmatch(
            r'GPU-[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}', value, re.IGNORECASE):
        raise ValueError('Provide the full GPU-... UUID of the intended GPU from nvidia-smi -L; indices and partial UUIDs are not accepted.')
    return 'GPU-' + value[4:].lower()


def parse_telemetry(text, gpu_uuid):
    gpu_uuid = normalized_gpu_uuid(gpu_uuid)
    rows = list(csv.reader(io.StringIO(text.strip())))
    if len(rows) != 1 or len(rows[0]) != len(FIELDS):
        raise ValueError('nvidia-smi did not return exactly one complete GPU record.')
    result = dict(zip(FIELDS, (x.strip() for x in rows[0])))
    if normalized_gpu_uuid(result['uuid']) != gpu_uuid:
        raise ValueError('nvidia-smi returned a different GPU UUID; refusing to run.')
    result['uuid'] = gpu_uuid
    for field in ('name', 'pci.bus_id', 'driver_version', 'vbios_version'):
        if not result[field] or result[field].lower() in ('n/a', '[n/a]', 'not supported'):
            raise ValueError('Required telemetry unavailable: ' + field)
    for field in ('power.limit', 'clocks.current.sm', 'clocks.current.memory',
                  'temperature.gpu', 'utilization.gpu', 'power.draw'):
        try:
            value = float(result[field])
            numeric(value, field, positive=field == 'power.limit')
        except (ValueError, TypeError):
            if field == 'power.limit':
                raise ValueError('A readable configured power limit is required.') from None
            value = None
        result[field] = value
    result['captured_utc'] = utc_now()
    return result


def system_directory():
    if sys.platform != 'win32' or C.sizeof(C.c_void_p) != 8:
        raise RuntimeError('Run this benchmark with 64-bit Python on Windows. No GPU operation was attempted.')
    kernel = C.WinDLL('kernel32.dll', winmode=0x00000800)
    fn = kernel.GetSystemDirectoryW
    fn.argtypes = [C.c_wchar_p, C.c_uint]
    fn.restype = C.c_uint
    buffer = C.create_unicode_buffer(32768)
    size = fn(buffer, len(buffer))
    if not 0 < size < len(buffer):
        raise RuntimeError('Cannot resolve the Windows system directory.')
    return Path(buffer.value)


def telemetry(directory, gpu_uuid):
    gpu_uuid = normalized_gpu_uuid(gpu_uuid)
    # Never search PATH or the package directory for an executable.
    executable = directory / 'nvidia-smi.exe'
    if not executable.is_file():
        raise RuntimeError('nvidia-smi.exe was not found in Windows System32. Keep the error report for diagnosis; do not change drivers just for this test.')
    result = subprocess.run([str(executable), '--id=' + gpu_uuid,
                             '--query-gpu=' + ','.join(FIELDS), '--format=csv,noheader,nounits'],
                            capture_output=True, text=True, timeout=20, check=True)
    return parse_telemetry(result.stdout, gpu_uuid)


def select_device(devices, gpu_uuid):
    gpu_uuid = normalized_gpu_uuid(gpu_uuid)
    matched = [device for device in devices if device['uuid'] == gpu_uuid]
    if len(matched) != 1:
        raise ValueError('The intended GPU UUID is not visible exactly once in CUDA; refusing to use another GPU.')
    device = matched[0]
    if device['compute_capability'] != '8.9':
        raise ValueError('This benchmark requires the intended SM 8.9 GPU.')
    if not 1 <= device['multiprocessors'] <= 256:
        raise ValueError('Invalid multiprocessor count.')
    return device


def normalized_bus(value):
    match = re.fullmatch(r'([0-9a-fA-F]+):([0-9a-fA-F]{2}):([0-9a-fA-F]{2})\.([0-7])', value)
    if not match:
        raise ValueError('Invalid PCI bus identifier.')
    return tuple(int(x, 16) for x in match.groups())


class Cuda:
    """Explicit Windows CUDA Driver API bindings; no third-party Python module."""
    def __init__(self, directory):
        path = directory / 'nvcuda.dll'
        if not path.is_file():
            raise RuntimeError('Windows System32 nvcuda.dll is missing. Keep the error report for diagnosis; do not change drivers just for this test.')
        self.lib = C.WinDLL(str(path), winmode=0x00000800)
        self.context, self.module = C.c_void_p(), C.c_void_p()
        self.memory = C.c_uint64()
        self.events = []
        p = C.POINTER
        signatures = {
            'cuInit': [C.c_uint], 'cuDriverGetVersion': [p(C.c_int)],
            'cuDeviceGetCount': [p(C.c_int)], 'cuDeviceGet': [p(C.c_int), C.c_int],
            'cuDeviceGetUuid': [C.c_void_p, C.c_int],
            'cuDeviceGetName': [C.c_void_p, C.c_int, C.c_int],
            'cuDeviceGetPCIBusId': [C.c_void_p, C.c_int, C.c_int],
            'cuDeviceGetAttribute': [p(C.c_int), C.c_int, C.c_int],
            'cuCtxCreate_v2': [p(C.c_void_p), C.c_uint, C.c_int],
            'cuCtxDestroy_v2': [C.c_void_p],
            'cuModuleLoadDataEx': [p(C.c_void_p), C.c_void_p, C.c_uint, p(C.c_int), p(C.c_void_p)],
            'cuModuleUnload': [C.c_void_p],
            'cuModuleGetFunction': [p(C.c_void_p), C.c_void_p, C.c_char_p],
            'cuMemAlloc_v2': [p(C.c_uint64), C.c_size_t], 'cuMemFree_v2': [C.c_uint64],
            'cuMemsetD32_v2': [C.c_uint64, C.c_uint, C.c_size_t],
            'cuMemcpyDtoH_v2': [C.c_void_p, C.c_uint64, C.c_size_t],
            'cuEventCreate': [p(C.c_void_p), C.c_uint],
            'cuEventRecord': [C.c_void_p, C.c_void_p],
            'cuEventSynchronize': [C.c_void_p],
            'cuEventElapsedTime': [p(C.c_float), C.c_void_p, C.c_void_p],
            'cuEventDestroy_v2': [C.c_void_p],
            'cuGetErrorString': [C.c_int, p(C.c_char_p)],
            'cuLaunchKernel': [C.c_void_p, C.c_uint, C.c_uint, C.c_uint,
                               C.c_uint, C.c_uint, C.c_uint, C.c_uint,
                               C.c_void_p, p(C.c_void_p), C.c_void_p],
        }
        for name, args in signatures.items():
            fn = getattr(self.lib, name)
            fn.argtypes, fn.restype = args, C.c_int
        self.call('cuInit', 0)

    def call(self, name, *args):
        status = getattr(self.lib, name)(*args)
        if status:
            message = C.c_char_p()
            self.lib.cuGetErrorString(status, C.byref(message))
            detail = message.value.decode('utf-8', 'replace') if message.value else 'unknown error'
            raise RuntimeError(f'{name}: CUDA {status}: {detail}')

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, traceback):
        failures = []
        resources = [('cuEventDestroy_v2', event) for event in reversed(self.events)]
        resources += [('cuMemFree_v2', self.memory.value), ('cuModuleUnload', self.module),
                      ('cuCtxDestroy_v2', self.context)]
        for name, handle in resources:
            if handle:
                try:
                    self.call(name, handle)
                except RuntimeError as error:
                    failures.append(str(error))
        if failures:
            if exc is None:
                raise RuntimeError('CUDA cleanup failed: ' + '; '.join(failures))
            print('CUDA cleanup warning: ' + '; '.join(failures), file=sys.stderr)

    def devices(self):
        count = C.c_int()
        self.call('cuDeviceGetCount', C.byref(count))
        result = []
        for ordinal in range(count.value):
            device = C.c_int()
            self.call('cuDeviceGet', C.byref(device), ordinal)
            raw_uuid = (C.c_ubyte * 16)()
            self.call('cuDeviceGetUuid', raw_uuid, device.value)
            name, bus = C.create_string_buffer(256), C.create_string_buffer(32)
            self.call('cuDeviceGetName', name, len(name), device.value)
            self.call('cuDeviceGetPCIBusId', bus, len(bus), device.value)
            attributes = []
            for attribute in (75, 76, 16):  # major, minor, multiprocessor count
                output = C.c_int()
                self.call('cuDeviceGetAttribute', C.byref(output), attribute, device.value)
                attributes.append(output.value)
            result.append({'handle': device.value, 'uuid': 'GPU-' + str(uuid.UUID(bytes=bytes(raw_uuid))),
                           'name': name.value.decode('utf-8', 'replace'),
                           'pci_bus_id': bus.value.decode('ascii'),
                           'compute_capability': f'{attributes[0]}.{attributes[1]}',
                           'multiprocessors': attributes[2]})
        return result

    def prepare(self, device, ptx, cells):
        self.call('cuCtxCreate_v2', C.byref(self.context), 0, device['handle'])
        image, error_log = C.create_string_buffer(ptx), C.create_string_buffer(8192)
        options = (C.c_int * 2)(5, 6)  # CU_JIT_ERROR_LOG_BUFFER, *_SIZE_BYTES
        values = (C.c_void_p * 2)(C.cast(error_log, C.c_void_p), len(error_log))
        try:
            self.call('cuModuleLoadDataEx', C.byref(self.module), image, 2, options, values)
        except RuntimeError as error:
            raise RuntimeError(str(error) + '\nPTX JIT: ' + error_log.value.decode('utf-8', 'replace')) from error
        self.call('cuMemAlloc_v2', C.byref(self.memory), cells * 32 * 4)
        for _ in range(2):
            event = C.c_void_p()
            self.call('cuEventCreate', C.byref(event), 0)
            self.events.append(event)
        self.functions = {}
        for mode in MODES:
            function = C.c_void_p()
            self.call('cuModuleGetFunction', C.byref(function), self.module, ('bench_' + mode).encode('ascii'))
            self.functions[mode] = function

    def launch(self, mode, iterations, blocks):
        if mode not in MODES or type(iterations) is not int or not 1 <= iterations <= ITERATIONS:
            raise ValueError('Invalid or unbounded kernel launch.')
        if type(blocks) is not int or not 1 <= blocks <= 256 * BLOCKS_PER_SM:
            raise ValueError('Invalid launch grid.')
        cells = blocks * THREADS
        count = cells * (8 if mode == 'scalar_fp32' else 32)
        # NaN sentinel means omitted stores cannot accidentally pass validation.
        self.call('cuMemsetD32_v2', self.memory.value, 0x7fc00000, count)
        iterations_value, input_value = C.c_uint(iterations), C.c_uint(INPUT_BITS[mode])
        params = (C.c_void_p * 3)(C.cast(C.byref(iterations_value), C.c_void_p),
                                  C.cast(C.byref(self.memory), C.c_void_p),
                                  C.cast(C.byref(input_value), C.c_void_p))
        self.call('cuEventRecord', self.events[0], None)
        self.call('cuLaunchKernel', self.functions[mode], blocks, 1, 1, THREADS, 1, 1, 0, None, params, None)
        self.call('cuEventRecord', self.events[1], None)
        self.call('cuEventSynchronize', self.events[1])
        elapsed = C.c_float()
        self.call('cuEventElapsedTime', C.byref(elapsed), self.events[0], self.events[1])
        check_time(elapsed.value)
        host = (C.c_float * count)()
        self.call('cuMemcpyDtoH_v2', host, self.memory.value, count * 4)
        check_outputs(host, mode, iterations, cells)
        return float(elapsed.value)


def check_time(value):
    numeric(value, 'CUDA event time', positive=True)
    if value > MAX_LAUNCH_MS:
        raise ValueError(f'Observed kernel time {value:.3f} ms exceeds {MAX_LAUNCH_MS:g} ms; stopping further launches.')


def check_projected_time(previous_ms, previous_iterations, next_iterations):
    check_time(previous_ms)
    if previous_ms * next_iterations / previous_iterations > MAX_LAUNCH_MS / 2:
        raise ValueError('Probe predicts a launch longer than 50 ms; stopping before increasing the workload.')


def check_outputs(values, mode, iterations, cells):
    if mode not in MODES or not 1 <= iterations <= ITERATIONS or cells <= 0:
        raise ValueError('Invalid correctness-check dimensions.')
    planes = 8 if mode == 'scalar_fp32' else 32
    if len(values) != cells * planes:
        raise ValueError('Output length mismatch.')
    increment = iterations if mode == 'scalar_fp32' else 16 * iterations
    for index, value in enumerate(values):
        expected = increment + index // cells
        if not math.isfinite(value) or value != expected:
            raise ValueError(f'{mode}: incorrect output at {index}: {value!r}, expected {expected}. Stopping.')


def operation_count(mode, blocks):
    if mode == 'scalar_fp32':
        return blocks * THREADS * ITERATIONS * 8 * 2
    return blocks * THREADS // 32 * ITERATIONS * 8 * (16 * 8 * 16 * 2)


def settings(blocks):
    return {'threads': THREADS, 'blocks': blocks, 'iterations': ITERATIONS, 'probes': list(PROBES),
            'warmups': WARMUPS, 'samples': SAMPLES, 'max_launch_ms': MAX_LAUNCH_MS,
            'modes': list(MODES), 'accumulator_chains': 8, 'all_accumulators_checked': True}


def run_benchmark(label, gpu_uuid):
    if label not in ('stock', 'unlocked'):
        raise ValueError('Label must be stock or unlocked.')
    gpu_uuid = normalized_gpu_uuid(gpu_uuid)
    directory = system_directory()
    before = telemetry(directory, gpu_uuid)
    base = Path(__file__).resolve().parent
    ptx_path = base / 'kernels.ptx'
    ptx = ptx_path.read_bytes()
    report = {'schema_version': SCHEMA, 'protocol': PROTOCOL, 'label': label, 'started_utc': utc_now(),
              'status': 'complete', 'hardware_unlock_verified': False, 'notice': NOTICE,
              'environment': {'python': platform.python_version(), 'windows': platform.platform()},
              'telemetry': {'before': before}, 'results': {}}
    with Cuda(directory) as cuda:
        device = select_device(cuda.devices(), gpu_uuid)
        if normalized_bus(device['pci_bus_id']) != normalized_bus(before['pci.bus_id']):
            raise ValueError('CUDA and nvidia-smi PCI identities disagree.')
        blocks = device['multiprocessors'] * BLOCKS_PER_SM
        cells = blocks * THREADS
        version = C.c_int()
        cuda.call('cuDriverGetVersion', C.byref(version))
        report['contract'] = {'gpu_uuid': gpu_uuid, 'gpu_name': device['name'],
                              'pci_bus_id': device['pci_bus_id'], 'compute_capability': '8.9',
                              'multiprocessors': device['multiprocessors'], 'cuda_driver_api': version.value,
                              'driver_version': before['driver_version'], 'vbios_version': before['vbios_version'],
                              'power_limit_watts': before['power.limit'],
                              'runner_sha256': sha256(Path(__file__)), 'ptx_sha256': sha256(ptx_path),
                              'settings': settings(blocks)}
        cuda.prepare(device, ptx, cells)
        # Each probe completes and passes correctness before any longer launch.
        previous = {}
        for iterations in PROBES:
            for mode in MODES:
                if mode in previous:
                    check_projected_time(previous[mode][0], previous[mode][1], iterations)
                print(f'Probe {mode}: {iterations} iterations', flush=True)
                elapsed = cuda.launch(mode, iterations, blocks)
                previous[mode] = (elapsed, iterations)
        for mode in MODES:
            check_projected_time(previous[mode][0], previous[mode][1], ITERATIONS)
        for warmup in range(WARMUPS):
            for mode in MODES:
                print(f'Warmup {warmup + 1}/{WARMUPS}: {mode}', flush=True)
                cuda.launch(mode, ITERATIONS, blocks)
        measured = {mode: [] for mode in MODES}
        # Rotate modes between samples to distribute changing clocks/temperature.
        for sample in range(SAMPLES):
            for offset in range(len(MODES)):
                mode = MODES[(sample + offset) % len(MODES)]
                print(f'Sample {sample + 1}/{SAMPLES}: {mode}', flush=True)
                measured[mode].append(cuda.launch(mode, ITERATIONS, blocks))
        for mode in MODES:
            times = measured[mode]
            median = statistics.median(times)
            planes = 8 if mode == 'scalar_fp32' else 32
            report['results'][mode] = {'times_ms': times, 'median_ms': median,
                                       'tflops': operation_count(mode, blocks) / median / 1e9,
                                       'all_outputs_correct': True, 'output_cells_per_launch': cells * planes,
                                       'checked_launches': len(PROBES) + WARMUPS + SAMPLES}
    after = telemetry(directory, gpu_uuid)
    report['telemetry']['after'] = after
    for field in ('uuid', 'driver_version', 'vbios_version', 'power.limit', 'pci.bus_id'):
        if before[field] != after[field]:
            raise ValueError('GPU identity or settings changed during the run: ' + field)
    report['finished_utc'] = utc_now()
    validate_report(report)
    return report


def validate_report(report):
    if not isinstance(report, dict) or type(report.get('schema_version')) is not int or report.get('schema_version') != SCHEMA or report.get('protocol') != PROTOCOL:
        raise ValueError('Unsupported benchmark report schema/protocol.')
    if report.get('status') != 'complete' or report.get('hardware_unlock_verified') is not False:
        raise ValueError('Report is not a completed measurement-only benchmark.')
    if report.get('label') not in ('stock', 'unlocked'):
        raise ValueError('Invalid report label.')
    environment = report.get('environment')
    if (not isinstance(environment, dict) or set(environment) != {'python', 'windows'} or
            any(not isinstance(value, str) or not value for value in environment.values())):
        raise ValueError('Missing Python/Windows environment metadata.')
    contract = report.get('contract')
    required = {'gpu_uuid', 'gpu_name', 'pci_bus_id', 'compute_capability', 'multiprocessors',
                'cuda_driver_api', 'driver_version', 'vbios_version', 'power_limit_watts',
                'runner_sha256', 'ptx_sha256', 'settings'}
    if not isinstance(contract, dict) or set(contract) != required:
        raise ValueError('Missing or unknown comparison metadata.')
    if normalized_gpu_uuid(contract['gpu_uuid']) != contract['gpu_uuid'] or contract['compute_capability'] != '8.9':
        raise ValueError('Report requires a canonical GPU UUID and SM 8.9 device.')
    if type(contract['multiprocessors']) is not int or not 1 <= contract['multiprocessors'] <= 256:
        raise ValueError('Invalid multiprocessor count.')
    blocks = contract['multiprocessors'] * BLOCKS_PER_SM
    if contract['settings'] != settings(blocks):
        raise ValueError('Benchmark workload/protocol mismatch.')
    for field in ('runner_sha256', 'ptx_sha256'):
        if not isinstance(contract[field], str) or not re.fullmatch('[0-9a-f]{64}', contract[field]):
            raise ValueError('Missing benchmark hash: ' + field)
    for field in ('gpu_name', 'pci_bus_id', 'driver_version', 'vbios_version'):
        if not isinstance(contract[field], str) or not contract[field]:
            raise ValueError('Missing comparison metadata: ' + field)
    if type(contract['cuda_driver_api']) is not int or contract['cuda_driver_api'] < 11080:
        raise ValueError('Invalid CUDA driver API version.')
    numeric(contract['power_limit_watts'], 'Power limit', positive=True)
    normalized_bus(contract['pci_bus_id'])
    telemetry_data = report.get('telemetry')
    if not isinstance(telemetry_data, dict) or set(telemetry_data) != {'before', 'after'}:
        raise ValueError('Missing before/after telemetry.')
    for stage in ('before', 'after'):
        row = telemetry_data[stage]
        if not isinstance(row, dict):
            raise ValueError('Invalid telemetry.')
        expected = {'uuid': contract['gpu_uuid'], 'driver_version': contract['driver_version'],
                    'vbios_version': contract['vbios_version'], 'power.limit': contract['power_limit_watts']}
        if any(row.get(key) != value for key, value in expected.items()):
            raise ValueError('Telemetry identity/driver/power mismatch.')
        if normalized_bus(row.get('pci.bus_id', '')) != normalized_bus(contract['pci_bus_id']):
            raise ValueError('Telemetry PCI identity mismatch.')
        for key in ('clocks.current.sm', 'clocks.current.memory', 'temperature.gpu', 'utilization.gpu', 'power.draw'):
            if row.get(key) is not None:
                numeric(row[key], key)
    results = report.get('results')
    if not isinstance(results, dict) or set(results) != set(MODES):
        raise ValueError('Incomplete benchmark results.')
    for mode, result in results.items():
        if not isinstance(result, dict):
            raise ValueError('Invalid mode result.')
        times = result.get('times_ms')
        if not isinstance(times, list) or len(times) != SAMPLES:
            raise ValueError('Incomplete timing samples.')
        for value in times:
            check_time(value)
        median = statistics.median(times)
        numeric(result.get('median_ms'), 'Median', positive=True)
        numeric(result.get('tflops'), 'Throughput', positive=True)
        if not math.isclose(result['median_ms'], median, rel_tol=1e-10):
            raise ValueError('Reported median differs from samples.')
        if not math.isclose(result['tflops'], operation_count(mode, blocks) / median / 1e9, rel_tol=1e-10):
            raise ValueError('Reported throughput differs from workload and timing.')
        if result.get('all_outputs_correct') is not True:
            raise ValueError('Output correctness was not verified.')
        planes = 8 if mode == 'scalar_fp32' else 32
        if result.get('output_cells_per_launch') != blocks * THREADS * planes:
            raise ValueError('Incomplete output correctness coverage.')
        if result.get('checked_launches') != len(PROBES) + WARMUPS + SAMPLES:
            raise ValueError('Some launches were not checked.')
    return report


def load_report(path):
    with Path(path).open('rb') as stream:
        data = stream.read(MAX_REPORT_BYTES + 1)
    if len(data) > MAX_REPORT_BYTES:
        raise ValueError('Report exceeds 1 MiB.')
    def pairs(items):
        result = {}
        for key, value in items:
            if key in result:
                raise ValueError('Duplicate JSON key: ' + key)
            result[key] = value
        return result
    def nonfinite(value):
        raise ValueError('Non-finite JSON value: ' + value)
    return validate_report(json.loads(data.decode('utf-8-sig'), object_pairs_hook=pairs, parse_constant=nonfinite))


def compare_reports(stock, unlocked):
    validate_report(stock)
    validate_report(unlocked)
    if stock['label'] != 'stock' or unlocked['label'] != 'unlocked':
        raise ValueError('Comparison requires stock first and unlocked second.')
    if stock['environment'] != unlocked['environment']:
        raise ValueError('Comparison refused; Python/Windows environment differs.')
    differing = [key for key in stock['contract'] if stock['contract'][key] != unlocked['contract'][key]]
    if differing:
        raise ValueError('Comparison refused; metadata differs: ' + ', '.join(sorted(differing)))
    rows = {}
    warnings = []
    for mode in MODES:
        a, b = stock['results'][mode], unlocked['results'][mode]
        ratio = a['median_ms'] / b['median_ms']
        rows[mode] = {'stock_median_ms': a['median_ms'], 'unlocked_median_ms': b['median_ms'],
                      'stock_tflops': a['tflops'], 'unlocked_tflops': b['tflops'],
                      'throughput_ratio': ratio, 'percent_change': (ratio - 1) * 100}
        for label, result in (('stock', a), ('unlocked', b)):
            spread = (max(result['times_ms']) - min(result['times_ms'])) / result['median_ms']
            if spread > 0.2:
                warnings.append(f'{label} {mode}: timing range exceeds 20% of median; background load/clocks may distort results.')
    control = rows['scalar_fp32']['throughput_ratio']
    if not 0.9 <= control <= 1.1:
        warnings.append('Scalar control changed by more than 10%; do not attribute target changes to an unlock.')
    for mode in ('fp16_fp32', 'bf16_fp32'):
        rows[mode]['ratio_divided_by_scalar_control'] = rows[mode]['throughput_ratio'] / control
    for field in ('clocks.current.sm', 'clocks.current.memory', 'temperature.gpu'):
        a = stock['telemetry']['after'].get(field)
        b = unlocked['telemetry']['after'].get(field)
        if a is None or b is None:
            warnings.append('Telemetry unavailable: ' + field)
        elif (field == 'temperature.gpu' and abs(a - b) > 10) or (field != 'temperature.gpu' and a > 0 and abs(b / a - 1) > 0.1):
            warnings.append('Different end-of-run telemetry may affect comparison: ' + field)
    return {'schema_version': SCHEMA, 'protocol': PROTOCOL, 'created_utc': utc_now(),
            'status': 'matched-measurements', 'hardware_unlock_verified': False, 'notice': NOTICE,
            'contract': stock['contract'], 'results': rows, 'warnings': warnings}


def save_report(directory, prefix, report):
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    target = directory / (prefix + '-' + stamp + '-' + uuid.uuid4().hex[:8] + '.json')
    with target.open('x', encoding='utf-8', newline='\n') as stream:
        json.dump(report, stream, indent=2, allow_nan=False)
        stream.write('\n')
    return target


def comparison_text(report):
    lines = ['GPU benchmark comparison', NOTICE, '']
    for mode, row in report['results'].items():
        lines.append(f"{mode}: stock {row['stock_tflops']:.3f} TFLOP/s, unlocked {row['unlocked_tflops']:.3f} TFLOP/s; "
                     f"{row['throughput_ratio']:.3f}x throughput ({row['percent_change']:+.1f}%)")
    lines += ['', *('WARNING: ' + warning for warning in report['warnings'])]
    lines += ['The scalar control is a comparison aid; it cannot prove or disprove the unlock.']
    return '\n'.join(lines) + '\n'


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    run = commands.add_parser('run')
    run.add_argument('--label', choices=('stock', 'unlocked'), required=True)
    run.add_argument('--gpu-uuid', required=True, type=normalized_gpu_uuid,
                     help='Full GPU-... UUID copied from nvidia-smi -L; no default GPU is selected.')
    compare = commands.add_parser('compare')
    compare.add_argument('stock', type=Path)
    compare.add_argument('unlocked', type=Path)
    for command in (run, compare):
        command.add_argument('--output-dir', type=Path, default=Path(__file__).resolve().parent / 'results')
    args = parser.parse_args(argv)
    started = time.monotonic()
    try:
        if args.command == 'run':
            print('Measuring only ' + args.gpu_uuid + '. Close GPU-heavy applications. No settings will be changed.', flush=True)
            report = run_benchmark(args.label, args.gpu_uuid)
            prefix = args.label
        else:
            report = compare_reports(load_report(args.stock), load_report(args.unlocked))
            report['source_reports'] = {'stock': {'path': str(args.stock.resolve()), 'sha256': sha256(args.stock)},
                                        'unlocked': {'path': str(args.unlocked.resolve()), 'sha256': sha256(args.unlocked)}}
            prefix = 'comparison'
        report['wall_seconds'] = round(time.monotonic() - started, 3)
        target = save_report(args.output_dir, prefix, report)
        if args.command == 'compare':
            text = comparison_text(report)
            with target.with_suffix('.txt').open('x', encoding='utf-8', newline='\n') as stream:
                stream.write(text)
            print(text)
        print('Saved: ' + str(target))
        return 0
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError, AttributeError) as error:
        print('Stopped: ' + str(error), file=sys.stderr)
        failure = {'schema_version': SCHEMA, 'protocol': PROTOCOL, 'status': 'failed',
                   'created_utc': utc_now(), 'hardware_unlock_verified': False, 'error': str(error)}
        try:
            target = save_report(args.output_dir, 'error-' + getattr(args, 'label', 'comparison'), failure)
            print('Failure details: ' + str(target), file=sys.stderr)
        except OSError:
            pass
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
