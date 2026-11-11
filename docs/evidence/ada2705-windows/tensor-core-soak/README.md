# RTX 4070 Ti SUPER (10de:2705): stock vs unlocked, 5-minute sustained runs (Windows)

Same card, same boot session type (cold start via full power-off), same
script, same binaries; the only difference is whether the unlock was applied
before the OS booted. One physical card (MSI Z790 host), one run per state.
This does not establish support for other AD103 boards.

## Method

- `soak.ps1 -Seconds 300 -Label <stock|unlocked>` loops, for 300 s,
  `mma_bench.exe 20000 5` then `cublas_gemm.exe 8192 40 3`, while
  `nvidia-smi` samples power, SM clock, temperature, utilization and throttle
  reasons every 0.5 s (`smi.csv`). Raw tool output is `runs.txt`.
- `mma_bench.cu` is this repository's `diagnostics/bench/mma_bench.cu`.
  `cublas_gemm.cu` is from the third-party fork
  `A-XIANGQAQ/rtx4070tis-ad103-tensor-unlock` (not copied here).
  Built with CUDA 13.5 (`nvcc -O3 -arch=sm_89 -std=c++17`).
  SHA-256: mma_bench.exe `cc534b4f...8069d`, cublas_gemm.exe `77fd6224...4d226b`.
- Driver 621.14, VBIOS 95.03.45.80.07, power limit 285 W (stock VBIOS limit).
- Stock was captured after a full power-off (`shutdown /s /full`), no unlock
  applied. Unlocked used the resident auto-unlock path.
- The stock soak ran 20 loops and the unlocked soak 25, because stock
  kernels are slower per loop. Values below are medians of the per-loop best.

## Results (TF/s, higher is better)

| Mode | Stock | Unlocked | Change |
|---|---|---|---|
| mma f16/f32 | 88.8 | 195.7 | 2.20x |
| mma bf16/f32 | 90.4 | 195.4 | 2.16x |
| mma fp8 e4m3 | 182.1 | 392.2 | 2.15x |
| mma f16/f16 (control) | 182.6 | 195.4 | 1.07x |
| mma s8/s32 (control) | 368.6 | 393.6 | 1.07x |
| mma dp4a (control) | 92.5 | 97.8 | 1.06x |
| mma ffma (control) | 45.2 | 47.2 | 1.04x |
| cuBLAS fp16, fp32 accumulate | 84.5 | 135.2 | 1.60x |
| cuBLAS bf16, fp32 accumulate | 84.5 | 143.9 | 1.70x |
| cuBLAS fp16, fp16 accumulate (control) | 121.7 | 126.6 | 1.04x |
| cuBLAS fp32 (control) | 26.6 | 27.5 | 1.03x |

Decision metric, f16/f32 divided by f16/f16 (median per loop): stock 0.493,
unlocked 0.999.

The controls move 3-7%. That is clock and power-limit drift between the two
runs, not the unlock. The stock run averaged a higher SM clock while busy
(2725 vs 2542 MHz) because the slower f32 path draws less power, so the 2x
figures if anything slightly understate the per-clock effect.

## Power, clocks, thermals (busy samples, utilization > 90%)

| | Stock | Unlocked |
|---|---|---|
| Peak board power | 277.1 W | 278.1 W |
| Average board power | 212.1 W | 231.5 W |
| SM clock avg / min / max | 2725 / 2055 / 2940 MHz | 2542 / 2055 / 2940 MHz |
| Peak temperature | 75 C | 73 C |
| Samples with SW power cap (0x4) | 266 of 598 | 394 of 594 |

Both states reach the 285 W limit region under sustained load; the unlocked
card is power-capped more often. Short benchmark bursts draw far less
(about 126 W average), so a short run does not exercise the limit.

## Files

- `stock/`, `unlocked/`: `runs.txt` (raw output), `smi.csv` (0.5 s samples:
  timestamp, W, SM MHz, C, util %, throttle bitmask), `summary.txt`.
- `soak.ps1`: the exact script used.
- `../SHA256SUMS` (parent folder): hashes of every file.
