# Windows GEMM validation and private evidence summaries

These tools measure ordinary CUDA computation. They do not unlock a GPU, write
registers, install a driver, change clocks/power, or modify startup/security settings.
The collector currently requires exactly one NVIDIA GPU and one CUDA device,
without `CUDA_VISIBLE_DEVICES` remapping. Multi-GPU selection is not implemented.

Use an existing PyTorch CUDA environment. The measurement implementation derives
from the PyTorch 2.6.0 / CUDA 12.4 collector used in the
[AD106M Windows report](../../docs/ad106m-windows-secure-boot.md).
Keep the same collector, environment, driver, GPU mode, displays, AC power and
background load for every phase. The generic collector has no board-specific
admission claim; its checks do not establish that a firmware experiment is safe.

```powershell
python windows/validation/gemm_probe.py --label stock
# After an independently reviewed, validated same-boot handoff:
python windows/validation/gemm_probe.py --label trial
# After a full stock/OFF startup:
python windows/validation/gemm_probe.py --label restored
```

Outputs and a concurrent-run lock are under the ignored `local/` directory.
Raw reports contain device identity, software details, telemetry and timestamps:
keep them private. A stale lock requires checking that the previous process has
ended before removing it. The collector refuses a busy, warm or memory-constrained
GPU and does not stop other applications. It uses five 4096-square GEMMs, ten
warmups and three batches of forty repetitions per format, timed by CUDA events.

FP16/BF16 reduced-precision reductions are disabled. Full FP16 accumulation is
also explicitly disabled when the PyTorch API exists. FP8 uses quantized E4M3
inputs, FP32 unit scales, FP16 output and `use_fast_accum=False`. Input generation
and quantization are outside timing. Every format checks all outputs for finite
values and a 32-by-32 selection against an FP64 product of the actual inputs.
This is sampled accuracy, not exhaustive equality or a standalone MMA issue-rate
benchmark. The bounded runtime check occurs between cases; it cannot interrupt
a stuck CUDA call. Stop further load if the desktop or driver is unhealthy.

Review boot evidence, errors, timing spread, clocks and temperatures locally.
Then emit an allowlisted numeric summary:

```powershell
python windows/validation/summarize.py `
  --stock windows/validation/local/STOCK.json `
  --trial windows/validation/local/TRIAL.json `
  --output comparison-public.json
python -m unittest discover -s windows/validation -p "test_*.py"
```

Replace the example filenames with the actual generated files. The summary
requires matching private GPU/software/collector identities and matching case
parameters. It rejects incomplete/numerically failed reports, nonfinite timing,
inconsistent medians/rates, and missing/reordered formats. It constructs a new
record containing only numeric measurements and fixed format names; paths,
identifiers, timestamps, raw telemetry and hashes are not copied. Existing
output files are not overwritten. Review even the reduced summary before sharing.

The summary never infers unlock or boot success from speed. A correct stock-speed
result and a failed firmware handoff must not be relabeled as an unlock pass.
Record unsuccessful attempts, uncertainty, test counts and untested conditions
alongside any shared numbers. Full FP16 accumulation is a different numerical
mode, not a substitute for measuring the FP32-accumulate path.
