#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# cuBLAS matrix multiply throughput per tensor mode through PyTorch, in TFLOPS.
# fp16 and bf16 accumulate in FP32 (the half-rate path on GeForce Ada);
# fp16-acc16 accumulates in FP16 and fp32 uses no tensor cores (controls).
import statistics

import torch

dev = "cuda"


def tflops(fn, n, iters=30, repeats=3):
    for _ in range(5):
        fn()
    torch.cuda.synchronize()
    runs = []
    for _ in range(repeats):
        start, end = torch.cuda.Event(enable_timing=True), torch.cuda.Event(enable_timing=True)
        start.record()
        for _ in range(iters):
            fn()
        end.record()
        torch.cuda.synchronize()
        runs.append(2 * n**3 * iters / (start.elapsed_time(end) / 1000) / 1e12)
    return statistics.median(runs)


def modes(n):
    a16, b16 = (torch.randn(n, n, device=dev, dtype=torch.float16) for _ in range(2))
    ab, bb = a16.bfloat16(), b16.bfloat16()
    a32, b32 = a16.float(), b16.float()
    a8, b8 = (torch.randint(-64, 64, (n, n), device=dev, dtype=torch.int8) for _ in range(2))
    af8, bf8 = a16.to(torch.float8_e4m3fn), b16.to(torch.float8_e4m3fn).t().contiguous().t()
    one = torch.tensor(1.0, device=dev)

    def matmul_flags(tf32=False, acc16=False):
        torch.backends.cuda.matmul.allow_tf32 = tf32
        torch.backends.cuda.matmul.allow_fp16_accumulation = acc16

    yield "fp16", lambda: matmul_flags(), lambda: a16 @ b16
    yield "fp16-acc16", lambda: matmul_flags(acc16=True), lambda: a16 @ b16
    yield "bf16", lambda: matmul_flags(), lambda: ab @ bb
    yield "tf32", lambda: matmul_flags(tf32=True), lambda: a32 @ b32
    yield "fp32", lambda: matmul_flags(), lambda: a32 @ b32
    yield "int8", lambda: matmul_flags(), lambda: torch._int_mm(a8, b8)
    yield "fp8", lambda: matmul_flags(), lambda: torch._scaled_mm(af8, bf8, one, one, out_dtype=torch.bfloat16)


print(f"torch {torch.__version__} cuda {torch.version.cuda} gpu {torch.cuda.get_device_name()}")
for n in (4096, 8192):
    for name, setup, fn in modes(n):
        setup()
        try:
            print(f"gemm {name:>10} n={n}: {tflops(fn, n):7.2f} TFLOPS")
        except RuntimeError as err:
            print(f"gemm {name:>10} n={n}: failed ({str(err).splitlines()[0]})")
    torch.cuda.empty_cache()
