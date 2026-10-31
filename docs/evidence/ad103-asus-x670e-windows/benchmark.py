"""Short Windows cuBLAS baseline; ordinary CUDA compute, no settings changes.

Adapted from the preserved windows/ad103-2705/benchmark.py (GPL-2.0-only).
Run through run_stock.py for three fresh-process measurements and idle checks.
"""
import argparse
import csv
import ctypes as ct
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import statistics
import subprocess
import time
from datetime import datetime, timezone

ROOT = Path(__file__).resolve().parent
FIELDS = 'name,uuid,pci.bus_id,pci.device_id,driver_version,vbios_version,power.limit,power.draw,temperature.gpu,clocks.sm,clocks.mem,utilization.gpu,memory.used'
SEED = 20261024


def sha256(path):
    with open(path, 'rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def telemetry():
    p = subprocess.run(['nvidia-smi', '--query-gpu=' + FIELDS,
                        '--format=csv,noheader,nounits'], capture_output=True, text=True, timeout=15)
    rows = list(csv.reader(p.stdout.strip().splitlines(), skipinitialspace=True)) if p.stdout.strip() else []
    devices = [dict(zip(FIELDS.split(','), (v.strip() for v in row))) for row in rows]
    return dict(utc=datetime.now(timezone.utc).isoformat(), returncode=p.returncode,
                devices=devices, error=p.stderr.strip())


def require_idle(sample):
    if sample['returncode'] or len(sample['devices']) != 1:
        raise RuntimeError('A single NVIDIA GPU with readable telemetry is required')
    gpu = sample['devices'][0]
    if gpu['pci.device_id'].lower() != '0x270510de' or not gpu['pci.bus_id'].lower().endswith(':01:00.0'):
        raise RuntimeError('Unexpected GPU identity/location')
    if float(gpu['utilization.gpu']) > 3 or float(gpu['memory.used']) > 128 or float(gpu['temperature.gpu']) > 60:
        raise RuntimeError('GPU is busy or warm; leave applications/settings unchanged and retry when idle')
    return gpu


class Blas:
    def __init__(self):
        self.path = Path(torch.__file__).parent / 'lib' / 'cublas64_12.dll'
        if not self.path.is_file():
            raise RuntimeError('The pinned CUDA 12 cuBLAS DLL is missing')
        self.dll_dir = os.add_dll_directory(str(self.path.parent))
        self.lib = ct.WinDLL(str(self.path))
        p, i = ct.c_void_p, ct.c_int
        signatures = {
            'cublasCreate_v2': [ct.POINTER(p)], 'cublasDestroy_v2': [p],
            'cublasSetStream_v2': [p, p], 'cublasSetMathMode': [p, i],
            'cublasGetVersion_v2': [p, ct.POINTER(i)],
            'cublasGemmEx': [p, i, i, i, i, i, p, p, i, i, p, i, i, p, p, i, i, i, i],
        }
        for name, args in signatures.items():
            function = getattr(self.lib, name)
            function.argtypes, function.restype = args, i
        self.handle = p()
        self.check(self.lib.cublasCreate_v2(ct.byref(self.handle)))
        self.check(self.lib.cublasSetStream_v2(self.handle, torch.cuda.current_stream().cuda_stream))
        # CUBLAS_MATH_DISALLOW_REDUCED_PRECISION_REDUCTION; FP32 accumulator/output.
        self.check(self.lib.cublasSetMathMode(self.handle, 16))
        version = i()
        self.check(self.lib.cublasGetVersion_v2(self.handle, ct.byref(version)))
        self.version = version.value

    @staticmethod
    def check(status):
        if status != 0:
            raise RuntimeError(f'cuBLAS status {status}')

    def gemm(self, a, b, c, input_type, compute):
        n = a.shape[0]
        alpha, beta = ct.c_float(1), ct.c_float(0)
        # Column-major B*A is the same storage as row-major A*B.
        self.check(self.lib.cublasGemmEx(
            self.handle, 0, 0, n, n, n, ct.byref(alpha),
            b.data_ptr(), input_type, n, a.data_ptr(), input_type, n,
            ct.byref(beta), c.data_ptr(), 0, n, compute, -1))

    def close(self):
        self.check(self.lib.cublasDestroy_v2(self.handle))
        self.dll_dir.close()


def matrices(n, dtype, seed):
    rng = np.random.default_rng(seed)
    # Exact dyadic values. Even the sum of absolute products fits FP32 exactly
    # for n<=4096 (integer numerator <=4096*256, denominator 1024).
    a = rng.integers(-16, 17, size=(n, n), dtype=np.int16).astype(np.float32) / 32
    b = rng.integers(-16, 17, size=(n, n), dtype=np.int16).astype(np.float32) / 32
    return a, b, torch.from_numpy(a).to('cuda', dtype), torch.from_numpy(b).to('cuda', dtype)


def correctness(blas, dtype, input_type, compute):
    ah, bh, a, b = matrices(256, dtype, SEED)
    c = torch.empty((256, 256), device='cuda', dtype=torch.float32)
    blas.gemm(a, b, c, input_type, compute)
    torch.cuda.synchronize()
    actual = c.cpu().numpy()
    expected = ah.astype(np.float64) @ bh.astype(np.float64)
    error = float(np.max(np.abs(actual - expected)))
    if not np.array_equal(actual, expected):
        raise RuntimeError(f'Full 256x256 correctness failure: maximum absolute error {error}')
    return dict(full_256_exact=True, full_256_max_abs_error=error)


def run_mode(blas, mode, n, samples, batch, warmup):
    name, dtype, input_type, compute = mode
    result = correctness(blas, dtype, input_type, compute)
    ah, bh, a, b = matrices(n, dtype, SEED)
    c = torch.empty((n, n), device='cuda', dtype=torch.float32)
    torch.cuda.synchronize()
    deadline = time.perf_counter() + warmup
    warmup_batches = 0
    while time.perf_counter() < deadline and warmup_batches < 2000:
        for _ in range(batch):
            blas.gemm(a, b, c, input_type, compute)
        torch.cuda.synchronize()
        warmup_batches += 1
    before = telemetry()
    times = []
    # Events measure GPU execution on the same stream as cuBLAS, excluding
    # matrix allocation, transfers, CPU reference work, and telemetry calls.
    for _ in range(samples):
        start, end = torch.cuda.Event(enable_timing=True), torch.cuda.Event(enable_timing=True)
        start.record()
        for _ in range(batch):
            blas.gemm(a, b, c, input_type, compute)
        end.record()
        end.synchronize()
        elapsed = start.elapsed_time(end) / batch
        if not math.isfinite(elapsed) or elapsed <= 0:
            raise RuntimeError('Invalid CUDA event time')
        times.append(elapsed)
    after = telemetry()
    for sample in (before, after):
        if sample['returncode'] or len(sample['devices']) != 1:
            raise RuntimeError('Missing telemetry during measurement')
    if not bool(torch.isfinite(c).all().item()):
        raise RuntimeError(name + ': nonfinite output')
    indices = np.unique(np.linspace(0, n - 1, min(32, n), dtype=np.int64))
    expected = ah[indices, :].astype(np.float64) @ bh[:, indices].astype(np.float64)
    ix = torch.as_tensor(indices, device='cuda')
    actual = c[ix[:, None], ix[None, :]].cpu().numpy()
    error = float(np.max(np.abs(actual - expected)))
    if not np.array_equal(actual, expected):
        raise RuntimeError(name + f': sampled {n}x{n} correctness failure: {error}')
    median = statistics.median(times)
    result.update(mode=name, n=n, samples=samples, batch=batch, milliseconds=times,
                  median_ms=median, median_tflops=2 * n ** 3 / (median * 1e9),
                  min_ms=min(times), max_ms=max(times), sample_cv=statistics.pstdev(times)/statistics.mean(times),
                  compute_type=compute, input_type=input_type, output_type='FP32',
                  sampled_large_exact=True, sampled_large_elements=int(len(indices) ** 2),
                  sampled_large_max_abs_error=error, warmup_batches=warmup_batches,
                  output_sha256=hashlib.sha256(c.cpu().numpy().tobytes()).hexdigest(),
                  telemetry_before=before, telemetry_after=after)
    print(f'{name}: {result["median_tflops"]:.2f} TFLOP/s; exact numerical checks passed', flush=True)
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--label', default='stock')
    ap.add_argument('--size', type=int, default=4096, choices=[256, 1024, 2048, 4096])
    ap.add_argument('--samples', type=int, default=15)
    ap.add_argument('--batch', type=int, default=5)
    ap.add_argument('--warmup', type=float, default=0.75)
    args = ap.parse_args()
    if not (3 <= args.samples <= 30 and 1 <= args.batch <= 10 and 0.1 <= args.warmup <= 2):
        ap.error('Bounded workload requires samples3..30, batch1..10, warmup0.1..2 seconds')
    output = args.out.resolve()
    if not output.is_relative_to(ROOT / 'results'):
        ap.error('Measurements must be under this benchmark/results directory')
    if output.exists():
        raise FileExistsError('Refusing to overwrite an existing measurement')
    initial = telemetry()
    gpu = require_idle(initial)
    os.environ['CUDA_VISIBLE_DEVICES'] = gpu['uuid']
    global np, torch
    import numpy as np
    import torch
    if os.name != 'nt' or not torch.cuda.is_available():
        raise RuntimeError('Windows and the isolated CUDA-enabled PyTorch environment are required')
    if torch.__version__ != '2.11.0+cu128' or np.__version__ != '2.3.5':
        raise RuntimeError('Unexpected package versions; use the pinned project environment')
    torch.cuda.set_device(0)
    torch.set_num_threads(4)
    blas = Blas()
    data = dict(schema=2, label=args.label, utc=datetime.now(timezone.utc).isoformat(),
                python=platform.python_version(), executable=os.sys.executable,
                numpy=np.__version__, torch=torch.__version__, torch_cuda=torch.version.cuda,
                gpu=torch.cuda.get_device_name(0), capability=list(torch.cuda.get_device_capability(0)),
                cublas_version=blas.version, cublas_sha256=sha256(blas.path),
                cublasLt_sha256=sha256(blas.path.parent/'cublasLt64_12.dll'),
                benchmark_sha256=sha256(__file__), math_mode=16, seed=SEED,
                warmup_seconds=args.warmup, initial_telemetry=initial, results=[], completed=False,
                process_environment={k: os.environ.get(k) for k in ('CUDA_VISIBLE_DEVICES','CUDA_LAUNCH_BLOCKING','CUBLAS_WORKSPACE_CONFIG','NVIDIA_TF32_OVERRIDE')},
                note='cuBLAS GEMM baseline, not a register instruction-rate measurement or proof of unlock')
    began = time.monotonic()
    try:
        modes = [('fp32_pedantic', torch.float32, 0, 69),
                 ('fp16_fp32', torch.float16, 2, 68), ('bf16_fp32', torch.bfloat16, 14, 68)]
        for mode in modes:
            data['results'].append(run_mode(blas, mode, args.size, args.samples, args.batch, args.warmup))
        data['completed'] = True
    except Exception as error:
        data['failure'] = repr(error)
        raise
    finally:
        data['elapsed_seconds'] = time.monotonic()-began
        output.parent.mkdir(parents=True, exist_ok=True)
        with output.open('x', encoding='utf-8') as f:
            json.dump(data, f, indent=2)
        blas.close()
        print('Saved:', output, flush=True)


if __name__ == '__main__':
    main()
