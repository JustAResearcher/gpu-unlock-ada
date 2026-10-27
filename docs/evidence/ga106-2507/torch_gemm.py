#!/usr/bin/env python3
"""PyTorch AMP GEMM probe: FP16 and BF16 with FP32 accumulation.

For each shape and dtype: fixed-seed inputs, 10 warm-up matmuls, then 3 passes
of 50 individually event-timed matmuls; reports the median per pass and the
median over all passes. Each output is checked against an FP32 reference
computed on the GPU with TF32 disabled, from the same (rounded) inputs.
Prints one JSON object per line.
"""
import json
import statistics

import torch

SHAPES = [(2048, 2048, 2048), (4096, 4096, 4096), (8192, 2048, 4096)]  # (M, N, K)
DTYPES = {"fp16": torch.float16, "bf16": torch.bfloat16}
WARMUP, ITERS, PASSES = 10, 50, 3

torch.backends.cuda.matmul.allow_tf32 = False
torch.backends.cudnn.allow_tf32 = False
# Keep FP32 accumulation end to end (no reduced-precision split-K reductions).
torch.backends.cuda.matmul.allow_fp16_reduced_precision_reduction = False
torch.backends.cuda.matmul.allow_bf16_reduced_precision_reduction = False

dev = torch.device("cuda")
print(json.dumps({"torch": torch.__version__, "cuda": torch.version.cuda,
                  "cublas_lt": torch.backends.cuda.preferred_blas_library().name,
                  "device": torch.cuda.get_device_name(0),
                  "capability": torch.cuda.get_device_capability(0)}))

for m, n, k in SHAPES:
    gen = torch.Generator(device=dev).manual_seed(m * 31 + n * 7 + k)
    a32 = torch.randn(m, k, device=dev, generator=gen)
    b32 = torch.randn(k, n, device=dev, generator=gen)
    for name, dt in DTYPES.items():
        a, b = a32.to(dt), b32.to(dt)
        ref = a.float() @ b.float()
        with torch.autocast("cuda", dtype=dt):
            for _ in range(WARMUP):
                out = a @ b
            torch.cuda.synchronize()
            passes = []
            for _ in range(PASSES):
                times = []
                for _ in range(ITERS):
                    s, e = torch.cuda.Event(enable_timing=True), torch.cuda.Event(enable_timing=True)
                    s.record()
                    out = a @ b
                    e.record()
                    e.synchronize()
                    times.append(s.elapsed_time(e))
                passes.append(times)
        flop = 2.0 * m * n * k
        med_pass = [statistics.median(t) for t in passes]
        med_all = statistics.median([x for t in passes for x in t])
        diff = (out.float() - ref)
        rel_fro = (diff.norm() / ref.norm()).item()
        max_abs = diff.abs().max().item()
        # Tolerance: output rounding to fp16/bf16 dominates (eps 2^-11 / 2^-8).
        tol = {"fp16": 2e-3, "bf16": 1.6e-2}[name]
        print(json.dumps({
            "shape_mnk": [m, n, k], "dtype": name, "out_dtype": str(out.dtype),
            "median_ms_per_pass": [round(x, 5) for x in med_pass],
            "median_ms": round(med_all, 5),
            "median_TF_s": round(flop / (med_all * 1e9), 3),
            "TF_s_per_pass": [round(flop / (x * 1e9), 3) for x in med_pass],
            "rel_fro_err": rel_fro, "max_abs_err": max_abs,
            "has_nan_inf": bool(~torch.isfinite(out).all()),
            "numerics_ok": rel_fro < tol and bool(torch.isfinite(out).all()),
        }))
        del a, b, ref, out, diff
    del a32, b32
    torch.cuda.empty_cache()
