# SM89 tensor issue-rate probe

`tensor_modes_issue.cu` measures register-resident FP16/FP32, TF32/FP32, and
unsigned INT4/INT32 `mma.sync` instruction issue rates. Each warp keeps eight
independent accumulator chains in flight. `tensor_modes_driver_runner.py`
times 20,000 iterations three times, then checks all 270,336 output cells
per mode against the expected all-ones result. It requires the exact GPU UUID,
power cap, and cubin SHA-256 so a different card or binary cannot silently
enter a matched comparison.

Build on Linux with the CUDA toolkit for an SM89 card:

```sh
nvcc -ptx -arch=sm_89 -O3 -std=c++17 \
  -o tensor_modes_issue_sm89.ptx diagnostics/bench/tensor_modes_issue.cu
ptxas -arch=sm_89 -O3 -o tensor_modes_issue_sm89.cubin \
  tensor_modes_issue_sm89.ptx
sha256sum tensor_modes_issue_sm89.cubin
python3 diagnostics/bench/tensor_modes_driver_runner.py \
  tensor_modes_issue_sm89.cubin \
  --sha256 PASTE-THE-CUBIN-SHA256 \
  --expect-uuid GPU-PASTE-YOUR-EXACT-UUID \
  --expect-power-limit 240
```

Set `--expect-power-limit` to the cap actually used on your card. Laptops
with Dynamic Boost report `[N/A]` for `power.limit`; pass `N/A` there. Run only
when the GPU is free of customer and owner workloads. For a stock/unlocked
comparison, use the **same card, cubin, power cap, driver, launch settings,
and clock policy** in both states. Record live clocks and restore the original
workload afterward.

This is an instruction-throughput probe, not a GEMM or inference benchmark.
All-one inputs check the exercised arithmetic path but do not validate general
numeric accuracy or predict application speedup. The
[RTX 4070 Ti SUPER report](../../docs/ada-2705-report.md) records one matched
240 W comparison and its limits.

`validate_gemm.cu` is a separate correctness check using cuBLAS and a CPU
reference. It uses three seeded FP16/BF16 input sets with square, rectangular
and non-tile-aligned shapes. Inputs vary across both coordinates and use exact
dyadic values. Run the same built binary on the same GPU in stock and unlocked
states; these small checks do not establish general application accuracy.

```sh
nvcc -O3 -arch=sm_89 -std=c++17 -o validate_gemm \
  diagnostics/bench/validate_gemm.cu -lcublas
```

`mma_bench.cu` accepts positive iteration and repetition counts. Repetition
results are stored dynamically, so longer runs do not use a fixed text buffer.

The `fp8/e4m3` mode drives `mma.sync.aligned.m16n8k32.row.col.f32.e4m3.e4m3.f32`
from registers, eight independent accumulator chains per warp, and counts
16*8*32*2 = 8192 FLOP per instruction -- the same product as the 16x16x16 mma
shapes, so the printed TF/s is directly comparable with `f16/f32`. Every input
is 1.0 in e4m3, so each accumulator reaches 1.0 * k = 32 per instruction. The
mode is compiled out below compute capability 8.9, and such a build prints
`n/a` for it instead of timing an empty loop. It is an issue rate: an
application FP8 GEMM will land lower when the power limit, not the issue rate,
sets the pace (an unlocked RTX 4090 on driver 610.57.04 peaked at 346 W against
313 W stock and throttled on SW Power Cap during both MMA and GEMM passes).

The [testing guide](../../docs/testing.md) gives a stock/unlocked collection
workflow, including the separate large cuBLAS GEMM probe. The
[matched RTX 4090 evidence](../../docs/evidence/rtx4090-pr1-matched/README.md)
records both improved and unchanged cases, numerical checks and telemetry.
