# FP16 accumulation uncap (GeForce)

A ~30-line Python import hook that enables full-rate FP16 accumulation in
PyTorch on GeForce cards. On consumer GPUs the driver runs FP16 matmuls at
half rate when accumulation is FP32; PyTorch exposes the full-rate path
through its public toggle `torch.backends.cuda.matmul.allow_fp16_accumulation`
(it is simply off by default). This hook flips it to `True` automatically in
every Python environment where it is installed — no driver, firmware, or
kernel changes involved.

Measured on an RTX 5090 (GB202, driver 615.71.09): ~325 TFLOPS FP16 matmul
with this hook vs ~217 TFLOPS without (fp16_accumulation defaulting to the
reduced-rate path). YMMV by card and clock.

## What this is not

This does not touch, patch, or re-sign any NVIDIA component. It calls a
public PyTorch option at import time. It does not change FP32-accumulate
rate (the GeForce FP32-accum cap lives in signed firmware and is a
separate, unsolved problem — see the GB202 notes in this repo).

## Install (per venv)

```bash
VENV=/path/to/venv
cp _nvidia_fp16_uncap.py _nvidia_fp16_uncap.pth "$VENV/lib/python3.*/site-packages/"
```

To uninstall, delete both files.

## Verify

```bash
python - <<'EOF'
import torch
assert torch.backends.cuda.matmul.allow_fp16_accumulation
print("fp16 accumulation: full rate")
EOF
```

## Trade-offs

FP16 accumulation adds rounding error to long reductions; numerically
sensitive training may want it off (that is why the default exists).
For inference and most fine-tuning it is indistinguishable in practice.
