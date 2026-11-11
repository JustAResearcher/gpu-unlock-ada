# RTX 4070 Ti SUPER (10de:2705): Windows UEFI unlock, combined evidence

Two independent measurements of the same card with and without the unlock
from [`windows/ad103-z790`](../../../windows/ad103-z790/README.md). One
physical card (MSI MPG Z790 EDGE WIFI, VBIOS 95.03.45.80.07, 285 W limit,
driver 621.14); this does not establish support for other AD103 boards.

| Folder | What it measures | Headline |
|---|---|---|
| [`tensor-core-soak/`](tensor-core-soak/README.md) | Raw tensor-core and GEMM throughput, 5-minute sustained runs, stock vs unlocked on the same boot type, with power/clock/thermal telemetry | FP16/BF16 with FP32 accumulate ~2.2x (about 89 to 196 TF/s); controls move 3-7% |
| [`comfyui-workloads/`](comfyui-workloads/README.md) | Real ComfyUI jobs (Wan, FLUX, Qwen-Edit, MiniMax, LTX), default vs unlocked, plus the reproducible harness | +14% to +43% per job; outputs bit-identical |

How they fit together: the soak shows the hardware-level effect (the FP32-
accumulate tensor path runs at the FP16-accumulate rate, ratio 0.49 to 1.00).
The workload numbers show how much of that reaches whole applications, which
is smaller because real jobs are not purely tensor-core bound.

## Read before quoting

- The two sets were not taken in one session. The soak pair (stock, unlocked)
  was run with identical binaries and script; the workload "default" and
  "unlocked" sets differ in ComfyUI core version (disclosed in
  `comfyui-workloads/BENCHMARKS.md`), so those percentages carry that
  uncertainty. Use the soak for hardware claims and the workloads for
  indicative application gains.
- Each state has one run, no repetition across cold boots.
- The stock soak ran 20 loops against 25 unlocked; values are medians of the
  per-loop best.
- The start image used for the workload runs is included
  (`comfyui-workloads/assets/bench_start.png`).
- No host identifiers, drive serials or network details are included.

`SHA256SUMS` covers every file in this folder.
