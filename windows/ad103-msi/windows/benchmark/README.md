# Windows instruction benchmark

This optional benchmark measures the intended RTX 4070 Ti SUPER using the installed NVIDIA driver. It does **not** perform an unlock, install a driver, access firmware/registers, change NVIDIA or Windows settings, or need administrator access. It needs **64-bit Python 3.9 or newer**, `nvcuda.dll` and `nvidia-smi.exe` from Windows System32. No PyTorch, CUDA Toolkit, package downloads, or Python dependencies are needed. The NVIDIA driver compiles the included text PTX at runtime; its normal JIT cache may be written by the driver.

You must explicitly select the intended GPU by its full `GPU-...` UUID from `nvidia-smi -L`; no GPU is chosen by default. The selected GPU must have compute capability 8.9, and CUDA and `nvidia-smi` must agree on its UUID and PCI address. Other GPUs are never used for the kernels. Selecting a GPU for this measurement does not make it a supported unlock target: the UEFI package retains its separate board and firmware gates.

## Run

The package's `Benchmark-Stock.cmd`, `Benchmark-Unlocked.cmd` and `Compare-Benchmarks.cmd` launchers provide the normal workflow and save files in `benchmark-results`. The two measurement launchers prompt for the GPU UUID. Use the same UUID for both. To avoid the prompt, pass `-GpuUuid` to `Run-Benchmark.ps1`. Direct Python equivalents from the extracted package directory are:

```powershell
nvidia-smi -L
py -3 benchmark\run_benchmark.py run --label stock --gpu-uuid GPU-<full-UUID-from-nvidia-smi> --output-dir benchmark-results
py -3 benchmark\run_benchmark.py run --label unlocked --gpu-uuid GPU-<same-full-UUID> --output-dir benchmark-results
py -3 benchmark\run_benchmark.py compare benchmark-results\stock-<timestamp>-<id>.json benchmark-results\unlocked-<timestamp>-<id>.json --output-dir benchmark-results
```

Replace the UUID placeholders with the complete selected UUID, and use actual report filenames in the final command. Run Stock after a cold shutdown/start with the test USB removed. Run Unlocked immediately after a successful UEFI unlock and Windows handoff, without restarting. Follow START-HERE for the USB workflow. Close games, AI tools and other GPU-heavy applications for both runs, and keep the same driver and configured power limit. The benchmark does not change either setting.

Reports have unique timestamps and IDs and are created without overwriting existing files: `stock-*.json`, `unlocked-*.json`, `comparison-*.json` and `comparison-*.txt`. A failed run produces `error-*.json`, never a successful stock/unlocked report. Keep both measurement reports and the matching USB report. The labels record what the user believes was booted; the benchmark cannot attest the unlock state.

## What is measured

The PTX has three register-resident instruction probes:

- FP16 inputs with FP32 accumulation: `mma.sync.aligned.m16n8k16.row.col.f32.f16.f16.f32`.
- BF16 inputs with FP32 accumulation: the corresponding BF16 tensor instruction.
- Scalar FP32 `fma.rn.f32` as a control that does not use tensor instructions.

Each warp uses eight independent tensor accumulator chains; scalar threads use eight independent FMA chains. Inputs are exactly one and the accumulators start at distinct small integers. Every accumulator is stored, and every output cell is checked for the exact finite expected result after **every** probe, warmup and timed launch. Output buffers are filled with NaNs before each launch to detect missing stores. All outputs remain exactly representable in FP32 for the fixed workload.

The grid is four blocks per multiprocessor, 128 threads per block. Probes use 16, 64, 256 and 1,024 loop iterations. Before increasing work, measured time must predict at most 50 ms for the next launch. The main workload uses 4,096 iterations, three warmups and nine measured repetitions per mode. CUDA events measure each kernel including its output stores; host checking and buffer initialization are outside the event interval. Modes rotate between repetitions. The report contains raw timings and medians, plus operation-count-derived dense TFLOP/s. This is a synthetic issue-rate probe, not a GEMM library or game benchmark.

Each launch completes before the next is submitted. Any observed launch over 100 ms stops the run. The bounded work and probes reduce watchdog exposure, but cannot promise immunity to a driver/OS hang or guarantee a deadline on malfunctioning hardware. No Windows TDR setting is changed.

Comparison requires matching GPU UUID, PCI address, SM count, CUDA driver API, NVIDIA driver, VBIOS, configured power limit, Python/Windows versions, full workload protocol, and SHA-256 of both the Python runner and PTX. Missing/invalid results or incomplete correctness checks are rejected. Read-only telemetry before/after includes clocks, temperature, utilization and power draw where available. These snapshots do not measure clocks continuously during kernels.

The comparison shows throughput ratios, the scalar control ratio, and target ratios divided by the scalar ratio. It flags large timing spread, changed control throughput and substantially different telemetry. Even a clean comparison does **not prove an unlock**. Power management, scheduling, driver JIT changes, thermal conditions and background activity can affect throughput. Scalar normalization is only a diagnostic aid and does not remove all such effects.

## Validation scope and references

Host tests exercise parsing, explicit device selection, report validation, numerical coverage, ctypes launch sequencing with mocks and comparison rejection. Offline `ptxas` compilation and SASS inspection are separate build checks. Neither is a physical Windows/GPU execution test. The [2026-10-25 hardware result](../HARDWARE-RESULT-2026-10-25.md) records one completed stock/unlocked pair with the earlier machine-bound runner, including its timing warnings and limits; it does not establish prolonged stability or application performance. That historical runner hash remains unchanged in the evidence. This public runner has a different hash because it requires explicit UUID selection; obtain a new matching stock/unlocked pair when testing it.

Primary API references:

- [NVIDIA CUDA Driver API](https://docs.nvidia.com/cuda/cuda-driver-api/index.html)
- [PTX warp-level matrix instructions](https://docs.nvidia.com/cuda/parallel-thread-execution/index.html#warp-level-matrix-instructions-mma)
- [NVIDIA System Management Interface](https://docs.nvidia.com/deploy/nvidia-smi/index.html)
- [Microsoft WDDM timeout detection and recovery](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/timeout-detection-and-recovery)
