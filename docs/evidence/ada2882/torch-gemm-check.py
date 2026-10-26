#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# torch-gemm-check.py [SECONDS]: GEMM error against a float64 reference per mode,
# then the same fp16 and bf16 GEMMs repeated for SECONDS (default 180); every
# result must be bitwise identical to the first.
import sys
import time

import torch

dev = "cuda"
n = 4096
torch.manual_seed(0)
a64, b64 = torch.randn(n, n, dtype=torch.float64), torch.randn(n, n, dtype=torch.float64)
ref = a64 @ b64


def error(out):
    return ((out.double().cpu() - ref).norm() / ref.norm()).item()


def flags(acc16=False, tf32=False):
    torch.backends.cuda.matmul.allow_fp16_accumulation = acc16
    torch.backends.cuda.matmul.allow_tf32 = tf32


a16, b16 = a64.half().to(dev), b64.half().to(dev)
ab, bb = a64.bfloat16().to(dev), b64.bfloat16().to(dev)
a32, b32 = a64.float().to(dev), b64.float().to(dev)
# The inputs' own rounding sets a floor: the exact product of the rounded inputs.
print(f"input rounding floor fp16 {error(a16.double().cpu() @ b16.double().cpu()):.2e}"
      f"  bf16 {error(ab.double().cpu() @ bb.double().cpu()):.2e}")
flags(); print(f"error fp16       {error(a16 @ b16):.2e}")
flags(acc16=True); print(f"error fp16-acc16 {error(a16 @ b16):.2e}")
flags(); print(f"error bf16       {error(ab @ bb):.2e}")
flags(tf32=True); print(f"error tf32       {error(a32 @ b32):.2e}")
flags(); print(f"error fp32       {error(a32 @ b32):.2e}")

seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 180
first = {"fp16": a16 @ b16, "bf16": ab @ bb}
runs = mismatches = 0
end = time.time() + seconds
while time.time() < end:
    for name, out in (("fp16", a16 @ b16), ("bf16", ab @ bb)):
        runs += 1
        if not torch.equal(out, first[name]):
            mismatches += 1
torch.cuda.synchronize()
print(f"stability: {runs} GEMMs in {seconds:.0f} s, {mismatches} differed from the first result")
