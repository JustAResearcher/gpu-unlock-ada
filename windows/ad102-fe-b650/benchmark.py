"""Windows cuBLAS baseline. Ordinary CUDA compute only; no GPU/boot modifications."""
import argparse
import ctypes as ct
import hashlib
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess
import time
from datetime import datetime, timezone

import numpy as np
import torch


def sha256(path):
    with open(path, 'rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def telemetry():
    fields = 'name,pci.bus_id,pci.device_id,driver_version,vbios_version,power.limit,power.draw,temperature.gpu,clocks.sm,clocks.mem,utilization.gpu,memory.used'
    p = subprocess.run(['nvidia-smi', '--query-gpu=' + fields,
                        '--format=csv,noheader,nounits'], capture_output=True, text=True)
    return {'fields': fields.split(','), 'csv': p.stdout.strip(),
            'returncode': p.returncode, 'error': p.stderr.strip()}


class Blas:
    def __init__(self):
        self.path = Path(torch.__file__).parent / 'lib' / 'cublas64_12.dll'
        if not self.path.is_file():
            raise RuntimeError('Requires the installed PyTorch CUDA 12 cuBLAS DLL: ' + str(self.path))
        self.dll_dir = os.add_dll_directory(str(self.path.parent))
        self.lib = ct.WinDLL(str(self.path))
        p, i = ct.c_void_p, ct.c_int
        signatures = {
            'cublasCreate_v2': [ct.POINTER(p)],
            'cublasDestroy_v2': [p],
            'cublasSetStream_v2': [p, p],
            'cublasSetMathMode': [p, i],
            'cublasGetVersion_v2': [p, ct.POINTER(i)],
            'cublasGemmEx': [p, i, i, i, i, i, p, p, i, i, p, i, i, p, p, i, i, i, i],
        }
        for name, args in signatures.items():
            f = getattr(self.lib, name)
            f.argtypes, f.restype = args, i
        self.handle = p()
        self.check(self.lib.cublasCreate_v2(ct.byref(self.handle)))
        self.check(self.lib.cublasSetStream_v2(self.handle, torch.cuda.current_stream().cuda_stream))
        # CUBLAS_MATH_DISALLOW_REDUCED_PRECISION_REDUCTION.
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
        alpha, beta = ct.c_float(1.0), ct.c_float(0.0)
        # cuBLAS column-major: C_row=A_row@B_row is C_col=B_col@A_col.
        self.check(self.lib.cublasGemmEx(
            self.handle, 0, 0, n, n, n, ct.byref(alpha),
            b.data_ptr(), input_type, n, a.data_ptr(), input_type, n,
            ct.byref(beta), c.data_ptr(), 0, n, compute, -1))

    def close(self):
        self.check(self.lib.cublasDestroy_v2(self.handle))
        self.dll_dir.close()


def matrices(n, dtype, seed):
    rng = np.random.default_rng(seed)
    # Dyadic inputs are exactly representable in FP16, BF16 and FP32.
    # Their products/sums are exact in FP32 for the supported test sizes.
    a = rng.integers(-16, 17, size=(n, n), dtype=np.int16).astype(np.float32) / 32
    b = rng.integers(-16, 17, size=(n, n), dtype=np.int16).astype(np.float32) / 32
    return a, b, torch.from_numpy(a).to('cuda', dtype), torch.from_numpy(b).to('cuda', dtype)


def correctness(blas, dtype, input_type, compute):
    ah, bh, a, b = matrices(256, dtype, 20261024)
    c = torch.empty((256, 256), device='cuda', dtype=torch.float32)
    blas.gemm(a, b, c, input_type, compute)
    torch.cuda.synchronize()
    actual = c.cpu().numpy()
    expected = ah.astype(np.float64) @ bh.astype(np.float64)
    err = float(np.max(np.abs(actual - expected)))
    if not np.array_equal(actual, expected):
        raise RuntimeError(f'Full 256x256 correctness failure, maximum error {err}')
    return {'full_256_exact': True, 'full_256_max_abs_error': err}


def run_mode(blas, mode, n, samples, batch, warmup):
    name, dtype, input_type, compute = mode
    result = correctness(blas, dtype, input_type, compute)
    ah, bh, a, b = matrices(n, dtype, 20261024)
    c = torch.empty((n, n), device='cuda', dtype=torch.float32)
    torch.cuda.synchronize()
    deadline = time.perf_counter() + warmup
    while time.perf_counter() < deadline:
        for _ in range(batch):
            blas.gemm(a, b, c, input_type, compute)
        torch.cuda.synchronize()
    before = telemetry()
    times = []
    for _ in range(samples):
        start, end = torch.cuda.Event(enable_timing=True), torch.cuda.Event(enable_timing=True)
        start.record()
        for _ in range(batch):
            blas.gemm(a, b, c, input_type, compute)
        end.record()
        end.synchronize()
        times.append(start.elapsed_time(end) / batch)
    after = telemetry()
    if not bool(torch.isfinite(c).all().item()):
        raise RuntimeError(name + ': nonfinite output')
    idx = np.unique(np.linspace(0, n - 1, min(32, n), dtype=np.int64))
    expected = ah[idx, :].astype(np.float64) @ bh[:, idx].astype(np.float64)
    actual = c[torch.as_tensor(idx, device='cuda')[:, None],
               torch.as_tensor(idx, device='cuda')[None, :]].cpu().numpy()
    err = float(np.max(np.abs(actual - expected)))
    if not np.array_equal(actual, expected):
        raise RuntimeError(name + f': sampled {n}x{n} correctness failure, error {err}')
    median = statistics.median(times)
    result.update(mode=name, n=n, samples=samples, batch=batch, milliseconds=times,
                  median_ms=median, median_tflops=2 * n ** 3 / (median * 1e9),
                  min_ms=min(times), max_ms=max(times), compute_type=compute,
                  input_type=input_type, output_type='FP32',
                  sampled_large_exact=True, sampled_large_elements=int(len(idx) ** 2),
                  sampled_large_max_abs_error=err,
                  output_sha256=hashlib.sha256(c.cpu().numpy().tobytes()).hexdigest(),
                  telemetry_before=before, telemetry_after=after)
    print(f'{name}: {result["median_tflops"]:.2f} TFLOP/s; numerical checks passed', flush=True)
    return result


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--label', default='stock')
    ap.add_argument('--size', type=int, default=4096, choices=[256, 1024, 2048, 4096, 8192])
    ap.add_argument('--samples', type=int, default=15)
    ap.add_argument('--batch', type=int, default=5)
    ap.add_argument('--warmup', type=float, default=1.0)
    args = ap.parse_args()
    if args.samples < 3 or args.batch < 1 or args.warmup < 0:
        ap.error('samples >= 3, batch >= 1, warmup >= 0 required')
    if os.name != 'nt' or not torch.cuda.is_available():
        raise RuntimeError('Requires Windows with the existing CUDA-enabled PyTorch install')
    if args.out.exists():
        raise FileExistsError('Refusing to replace an existing measurement: ' + str(args.out))
    torch.cuda.set_device(0)
    torch.set_num_threads(4)
    blas = Blas()
    try:
        data = dict(schema=1, label=args.label, utc=datetime.now(timezone.utc).isoformat(),
                    python=platform.python_version(), torch=torch.__version__,
                    torch_cuda=torch.version.cuda, gpu=torch.cuda.get_device_name(0),
                    capability=list(torch.cuda.get_device_capability(0)),
                    cublas_version=blas.version, cublas_sha256=sha256(blas.path),
                    cublasLt_sha256=sha256(blas.path.parent / 'cublasLt64_12.dll'),
                    benchmark_sha256=sha256(__file__),
                    math_mode=16, seed=20261024, warmup_seconds=args.warmup,
                    note='cuBLAS GEMM throughput, not register-resident instruction rate or proof of unlock',
                    initial_telemetry=telemetry(), results=[])
        modes = [('fp32_pedantic', torch.float32, 0, 69),
                 ('fp16_fp32', torch.float16, 2, 68),
                 ('bf16_fp32', torch.bfloat16, 14, 68)]
        for mode in modes:
            data['results'].append(run_mode(blas, mode, args.size, args.samples, args.batch, args.warmup))
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(json.dumps(data, indent=2), encoding='utf-8')
        print('Saved:', args.out)
    finally:
        blas.close()


if __name__ == '__main__':
    main()
