# SPDX-License-Identifier: GPL-2.0-only
"""Synthetic host tests only. These are not Windows or GPU execution evidence."""
import copy
import ctypes as C
import importlib.util
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location('native_benchmark', Path(__file__).resolve().parents[1] / 'benchmark' / 'run_benchmark.py')
bench = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(bench)
FIXTURE_UUID = 'GPU-11111111-2222-3333-4444-555555555555'
OTHER_UUID = 'GPU-aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee'


def telemetry():
    return bench.parse_telemetry(FIXTURE_UUID + ', Synthetic fixture, 00000000:01:00.0, 621.01, fixture-vbios, 285, 2400, 10501, 50, 0, P0, 20', FIXTURE_UUID)


def report(label='stock'):
    data = {'schema_version': bench.SCHEMA, 'protocol': bench.PROTOCOL, 'label': label,
            'status': 'complete', 'hardware_unlock_verified': False,
            'environment': {'python': '3.12.0 fixture', 'windows': 'Windows fixture'},
            'telemetry': {'before': telemetry(), 'after': telemetry()}, 'results': {},
            'contract': {'gpu_uuid': FIXTURE_UUID, 'gpu_name': 'Synthetic fixture; not hardware evidence',
                         'pci_bus_id': '0000:01:00.0', 'compute_capability': '8.9', 'multiprocessors': 66,
                         'cuda_driver_api': 12090, 'driver_version': '621.01', 'vbios_version': 'fixture-vbios',
                         'power_limit_watts': 285.0, 'runner_sha256': 'a' * 64, 'ptx_sha256': 'b' * 64,
                         'settings': bench.settings(264)}}
    for mode in bench.MODES:
        data['results'][mode] = {'times_ms': [1.0] * bench.SAMPLES, 'median_ms': 1.0,
                                'tflops': bench.operation_count(mode, 264) / 1e9,
                                'all_outputs_correct': True,
                                'output_cells_per_launch': 264 * bench.THREADS * (8 if mode == 'scalar_fp32' else 32),
                                'checked_launches': len(bench.PROBES) + bench.WARMUPS + bench.SAMPLES}
    return data


class BenchmarkTests(unittest.TestCase):
    def test_exact_target_selected_even_when_second(self):
        other = {'uuid': OTHER_UUID, 'compute_capability': '8.9', 'multiprocessors': 40, 'handle': 0}
        target = {'uuid': FIXTURE_UUID, 'compute_capability': '8.9', 'multiprocessors': 66, 'handle': 1}
        self.assertIs(bench.select_device([other, target], FIXTURE_UUID), target)
        self.assertIs(bench.select_device([other, target], OTHER_UUID.upper()), other)
        for devices in ([other], [target, target], [dict(target, compute_capability='9.0')]):
            with self.assertRaises(ValueError):
                bench.select_device(devices, FIXTURE_UUID)

    def test_uuid_must_be_explicit_complete_and_canonicalized(self):
        self.assertEqual(bench.normalized_gpu_uuid(OTHER_UUID.upper()), OTHER_UUID)
        for value in ('', '0', 'GPU-11111111', FIXTURE_UUID + '\n', ' ' + FIXTURE_UUID,
                      '--query-gpu=name', None, 0, 'MIG-' + FIXTURE_UUID[4:]):
            with self.subTest(value=value), self.assertRaises(ValueError):
                bench.normalized_gpu_uuid(value)
        with patch.object(bench, 'system_directory') as directory:
            with self.assertRaises(ValueError):
                bench.run_benchmark('stock', '')
            directory.assert_not_called()

    def test_cli_refuses_missing_or_malformed_uuid_before_measurement(self):
        with patch.object(bench, 'run_benchmark') as run, patch('sys.stderr', new=io.StringIO()):
            for options in ([], ['--gpu-uuid', '0'], ['--gpu-uuid', 'GPU-11111111']):
                with self.assertRaises(SystemExit) as stopped:
                    bench.main(['run', '--label', 'stock', *options])
                self.assertEqual(stopped.exception.code, 2)
            run.assert_not_called()

    def test_telemetry_exact_uuid_and_complete_row(self):
        self.assertEqual(telemetry()['power.limit'], 285)
        for text in ('GPU-other, other', '\n'.join(['x'] * 2), ''):
            with self.assertRaises(ValueError):
                bench.parse_telemetry(text, FIXTURE_UUID)
        source = ', '.join(str(telemetry()[field]) for field in bench.FIELDS)
        with self.assertRaises(ValueError):
            bench.parse_telemetry(source.replace(FIXTURE_UUID, OTHER_UUID), FIXTURE_UUID)
        with self.assertRaises(ValueError):
            bench.parse_telemetry(source.replace(', 285.0,', ', N/A,'), FIXTURE_UUID)

    def test_optional_telemetry_unavailable_is_explicit(self):
        source = ', '.join(str(telemetry()[field]) for field in bench.FIELDS)
        row = bench.parse_telemetry(source.replace(', 2400.0,', ', N/A,'), FIXTURE_UUID)
        self.assertIsNone(row['clocks.current.sm'])

    def test_bus_normalization(self):
        self.assertEqual(bench.normalized_bus('00000000:01:00.0'), bench.normalized_bus('0000:01:00.0'))
        with self.assertRaises(ValueError):
            bench.normalized_bus('index 0')

    def test_exact_all_accumulator_outputs(self):
        for mode in bench.MODES:
            planes = 8 if mode == 'scalar_fp32' else 32
            expected = 7 if mode == 'scalar_fp32' else 7 * 16
            values = [float(expected + plane) for plane in range(planes) for _ in range(4)]
            bench.check_outputs(values, mode, 7, 4)
            for index in (0, len(values) // 2, len(values) - 1):
                for bad in (float('nan'), float('inf'), -1, values[index] + 1):
                    changed = list(values)
                    changed[index] = bad
                    with self.assertRaises(ValueError):
                        bench.check_outputs(changed, mode, 7, 4)
            with self.assertRaises(ValueError):
                bench.check_outputs(values[:-1], mode, 7, 4)

    def test_launch_time_and_prediction_guards(self):
        for value in (0, -1, True, float('nan'), float('inf'), 100.001, 10 ** 1000):
            with self.assertRaises(ValueError):
                bench.check_time(value)
        bench.check_projected_time(1, 16, 64)
        with self.assertRaises(ValueError):
            bench.check_projected_time(20, 16, 64)

    def test_valid_comparison_is_not_unlock_proof(self):
        output = bench.compare_reports(report(), report('unlocked'))
        self.assertFalse(output['hardware_unlock_verified'])
        self.assertEqual(output['warnings'], [])
        for row in output['results'].values():
            self.assertEqual(row['throughput_ratio'], 1)
            self.assertEqual(row['stock_tflops'], row['unlocked_tflops'])
        self.assertIn('TFLOP/s', bench.comparison_text(output))

    def test_independent_control_ratio(self):
        after = report('unlocked')
        for mode in ('fp16_fp32', 'bf16_fp32'):
            after['results'][mode]['times_ms'] = [0.5] * bench.SAMPLES
            after['results'][mode]['median_ms'] = 0.5
            after['results'][mode]['tflops'] *= 2
        result = bench.compare_reports(report(), after)
        self.assertEqual(result['results']['fp16_fp32']['ratio_divided_by_scalar_control'], 2)
        self.assertEqual(result['results']['scalar_fp32']['throughput_ratio'], 1)

    def test_wrong_labels_refused(self):
        for old, new in ((report('unlocked'), report()), (report(), report())):
            with self.assertRaises(ValueError):
                bench.compare_reports(old, new)

    def test_contract_differences_refused(self):
        for field, value in report()['contract'].items():
            current = report('unlocked')
            if isinstance(value, dict):
                current['contract'][field] = dict(value, iterations=128)
            elif isinstance(value, str):
                current['contract'][field] = 'c' * 64 if field.endswith('sha256') else value + '-changed'
            else:
                current['contract'][field] = value + 1
            with self.subTest(field=field), self.assertRaises(ValueError):
                bench.compare_reports(report(), current)

    def test_valid_other_gpu_report_refused_when_pair_does_not_match(self):
        current = report('unlocked')
        current['contract']['gpu_uuid'] = OTHER_UUID
        for stage in ('before', 'after'):
            current['telemetry'][stage]['uuid'] = OTHER_UUID
        bench.validate_report(current)
        with self.assertRaisesRegex(ValueError, 'metadata differs: gpu_uuid'):
            bench.compare_reports(report(), current)

    def test_environment_changes_refused(self):
        for field in ('python', 'windows'):
            current = report('unlocked')
            current['environment'][field] += ' changed'
            with self.assertRaises(ValueError):
                bench.compare_reports(report(), current)

    def test_incomplete_or_forged_results_refused(self):
        mutations = [('median_ms', 2), ('tflops', 7), ('all_outputs_correct', False),
                     ('output_cells_per_launch', 1), ('checked_launches', 1), ('times_ms', [1.0]),
                     ('times_ms', [float('nan')] * bench.SAMPLES)]
        for field, value in mutations:
            current = report()
            current['results']['fp16_fp32'][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                bench.validate_report(current)
        for value in (True, 2, '1'):
            current = report()
            current['schema_version'] = value
            with self.assertRaises(ValueError):
                bench.validate_report(current)

    def test_telemetry_mismatch_refused(self):
        for field, value in (('uuid', 'GPU-other'), ('power.limit', 300), ('driver_version', 'other'),
                             ('vbios_version', 'other'), ('pci.bus_id', '0000:02:00.0')):
            current = report()
            current['telemetry']['after'][field] = value
            with self.assertRaises(ValueError):
                bench.validate_report(current)

    def test_high_variation_or_control_change_warns(self):
        current = report('unlocked')
        row = current['results']['scalar_fp32']
        row['times_ms'] = [0.5] * bench.SAMPLES
        row['median_ms'] = 0.5
        row['tflops'] *= 2
        current['results']['fp16_fp32']['times_ms'][0] = 1.5
        warnings = bench.compare_reports(report(), current)['warnings']
        self.assertTrue(any('Scalar control' in line for line in warnings))
        self.assertTrue(any('timing range' in line for line in warnings))

    def test_unique_reports_and_strict_json_read(self):
        with tempfile.TemporaryDirectory() as directory:
            a = bench.save_report(directory, 'stock', report())
            b = bench.save_report(directory, 'stock', report())
            self.assertNotEqual(a, b)
            self.assertEqual(bench.load_report(a)['label'], 'stock')
            for text in ('{"a":1,"a":2}', '{"a":NaN}', ' ' * (bench.MAX_REPORT_BYTES + 1)):
                a.write_text(text)
                with self.assertRaises(ValueError):
                    bench.load_report(a)

    def test_nonwindows_refuses_before_driver_load(self):
        with patch.object(bench.sys, 'platform', 'linux'):
            with self.assertRaises(RuntimeError):
                bench.system_directory()

    def test_smi_uses_absolute_system_path_and_exact_uuid(self):
        with tempfile.TemporaryDirectory() as folder:
            directory = Path(folder)
            (directory / 'nvidia-smi.exe').touch()
            source = ', '.join(str(telemetry()[field]) for field in bench.FIELDS)
            with patch.object(bench.subprocess, 'run') as run:
                run.return_value.stdout = source
                bench.telemetry(directory, FIXTURE_UUID)
                args = run.call_args.args[0]
                self.assertEqual(args[0], str(directory / 'nvidia-smi.exe'))
                self.assertIn('--id=' + FIXTURE_UUID, args)
                self.assertNotIn('shell', run.call_args.kwargs)

    def test_mock_launch_parameter_layout_and_check_every_output(self):
        cuda = bench.Cuda.__new__(bench.Cuda)
        cuda.memory = C.c_uint64(0x100000)
        cuda.events = [C.c_void_p(1), C.c_void_p(2)]
        cuda.functions = {mode: C.c_void_p(3) for mode in bench.MODES}
        calls = []
        def call(name, *args):
            calls.append(name)
            if name == 'cuLaunchKernel':
                self.assertEqual(args[1:8], (1, 1, 1, bench.THREADS, 1, 1, 0))
                params = args[9]
                self.assertEqual(C.cast(params[0], C.POINTER(C.c_uint)).contents.value, 16)
                self.assertEqual(C.cast(params[1], C.POINTER(C.c_uint64)).contents.value, 0x100000)
                self.assertEqual(C.cast(params[2], C.POINTER(C.c_uint)).contents.value, bench.INPUT_BITS['fp16_fp32'])
            elif name == 'cuEventElapsedTime':
                C.cast(args[0], C.POINTER(C.c_float)).contents.value = 1.0
            elif name == 'cuMemcpyDtoH_v2':
                values = args[0]
                for index in range(len(values)):
                    values[index] = 16 * 16 + index // bench.THREADS
        cuda.call = call
        self.assertEqual(cuda.launch('fp16_fp32', 16, 1), 1.0)
        self.assertEqual(calls, ['cuMemsetD32_v2', 'cuEventRecord', 'cuLaunchKernel', 'cuEventRecord',
                                 'cuEventSynchronize', 'cuEventElapsedTime', 'cuMemcpyDtoH_v2'])
        for iterations in (0, bench.ITERATIONS + 1, True):
            with self.assertRaises(ValueError):
                cuda.launch('fp16_fp32', iterations, 1)

    def test_mock_cleanup_order_and_exception_preservation(self):
        cuda = bench.Cuda.__new__(bench.Cuda)
        cuda.memory, cuda.context, cuda.module = C.c_uint64(4), C.c_void_p(5), C.c_void_p(6)
        cuda.events = [C.c_void_p(1), C.c_void_p(2)]
        calls = []
        cuda.call = lambda name, *args: calls.append(name)
        cuda.__exit__(None, None, None)
        self.assertEqual(calls, ['cuEventDestroy_v2', 'cuEventDestroy_v2', 'cuMemFree_v2',
                                 'cuModuleUnload', 'cuCtxDestroy_v2'])

    def test_failed_run_never_writes_stock_report(self):
        with tempfile.TemporaryDirectory() as directory:
            with patch.object(bench, 'run_benchmark', side_effect=RuntimeError('synthetic failure')):
                with patch('sys.stdout', new=io.StringIO()), patch('sys.stderr', new=io.StringIO()):
                    status = bench.main(['run', '--label', 'stock', '--gpu-uuid', FIXTURE_UUID, '--output-dir', directory])
            self.assertEqual(status, 1)
            files = list(Path(directory).glob('*.json'))
            self.assertEqual(len(files), 1)
            self.assertTrue(files[0].name.startswith('error-stock-'))
            self.assertEqual(json.loads(files[0].read_text())['status'], 'failed')


if __name__ == '__main__':
    unittest.main()
