# RTX 4090 reproduction on NVIDIA 610.57.04

An independent owner-controlled RTX 4090 (`10de:2684`) completed the reviewed
module's transaction and stock NVIDIA driver handoff on 2026-10-25 UTC.
The executed module source was revision `4e7fefcc4923f9e083d5324dc75a7d2f906af77f`.
This evidence commit does not change the module or firmware payload.

Both measurements used Linux `7.2.5-3-omarchy`, NVIDIA `610.57.04`, CUDA `13.3`,
the same exact GPU and benchmark binaries, and the existing 450 W power cap.
The Hyprland compositor and panel remained active. Clocks and power policy were
not changed. This is **one matched stock/unlocked pair**, with five internal
timing repetitions per mode.

This submission is the **first documented reproduction on NVIDIA driver 610.57.04**.
Prior matched evidence (the `rtx4090-pr1-matched` directory) used driver `615.78.08`.

## New evidence in this submission

1. **Driver 610.57.04 compatibility.** The module, transaction and NVIDIA handoff
   worked identically on this older driver. This broadens the validated driver
   range from a single version.

2. **Matched BAR0 `gpu_peek` register dumps.** Both stock and unlocked states
   were read with the diagnostic `gpu_peek` kernel module, providing register-level
   confirmation independent of the RM getter:

   | Register | Stock | Unlocked |
   |---|---|---|
   | PLM `0x00823804` | `0xffffff8f` | `0xffffffff` |
   | SS0 `0x0082381c` | `0x47131055` | `0x88888888` |
   | SS1 `0x00823820` | `0x00000001` | `0x00000008` |

   Fuse registers (`FUSE_SS_FMLA32`, `FUSE_SS_IMLA2`, `FUSE_SS_IMLA4`) remained
   at `0x00000001` in both states, confirming the unlock overrides firmware-written
   values, not fuses.

3. **Power and thermal impact of the unlock.** Unlocked peak power reached 346 W
   versus 313 W stock. SW Power Cap throttle (`0x4`) triggered during both MMA
   and GEMM passes (stock: GEMM only). Minimum clock under load dropped to
   2505 MHz (stock: 2655 MHz). Already-full-rate modes (f16/f16, s8/s32)
   regressed slightly, consistent with less thermal headroom from the
   now-active FP32-accumulate paths.

## Boot and correctness evidence

The test used a UKI containing the reviewed module and all nine pinned firmware
assets, loaded before NVIDIA via a `mkinitcpio.conf.d` drop-in. Module and
firmware hashes are in `metadata.json`.

The selected card reported `gpu_unlock: unlocked` before NVIDIA initialization
([excerpt](unlocked-boot-excerpt.txt)). After the driver initialized, the RM
getter changed from `fmla32=1, imla2=1, imla4=1` (half rate) to
`fmla32=0, imla2=0, imla4=0` (full rate); both sanitized readbacks are included.
No `NVRM: Xid` errors were observed before or after the benchmark suite.

Both states passed all six seeded FP16/BF16 numerical cases, with zero mismatches
and `max_abs=0`: 256×256×256, 64×96×128, and 127×65×95. Every output cell in each
large cuBLAS GEMM and each tensor-mode control also passed its exact-reference check.

## Instruction throughput

`mma_bench` used 20,000 iterations, 1,056 blocks, eight warps per block and the
best of five timing repetitions.

| Mode | Stock | Unlocked | Ratio |
|---|---:|---:|---:|
| FP16 / FP32 accumulate | 162.7 TF/s | 327.6 TF/s | 2.013× |
| BF16 / FP32 accumulate | 162.9 TF/s | 323.0 TF/s | 1.983× |
| FP16 / FP16 accumulate | 332.8 TF/s | 312.7 TF/s | 0.940× |
| INT8 / INT32 accumulate | 661.6 TOP/s | 645.2 TOP/s | 0.975× |
| `dp4a` | 164.6 TOP/s | 164.6 TOP/s | 1.000× |
| scalar FP32 `ffma` | 79.3 TF/s | 79.3 TF/s | 1.000× |

The separate same-cubin tensor-mode probe measured TF32/FP32
**81.8 → 159.7 TF/s** (1.952×). Its FP16/FP32 control was
163.6 → 320.8 TF/s (1.961×), and unsigned INT4/INT32 was
1318.4 → 1271.2 TOP/s (0.964×).

The f16/f16 regression (0.940×) is consistent with higher power draw from the
unlocked FP32-accumulate paths reducing thermal headroom for the subsequent
f16/f16 pass. This was not observed in the `rtx4090-pr1-matched` evidence
(0.999×), where peak power was 308 W and the card did not reach the SW Power
Cap during the MMA passes.

## cuBLAS GEMM throughput

Square dense GEMMs with FP32 accumulation used ten warmups, twenty GEMMs per
timing, and five timing repetitions. The table reports median throughput.

| N | Input type | Stock TF/s | Unlocked TF/s | Ratio |
|---|---|---:|---:|---:|
| 1024 | FP16 | 114.0 | 134.9 | 1.183× |
| 1024 | BF16 | 132.8 | 162.1 | 1.220× |
| 4096 | FP16 | 159.6 | 202.1 | 1.266× |
| 4096 | BF16 | 152.0 | 147.9 | 0.973× |
| 8192 | FP16 | 169.1 | 182.1 | 1.077× |
| 8192 | BF16 | 165.0 | 181.6 | 1.100× |

The 4096³ BF16 regression (0.973×) shows monotonically increasing rep times
(0.87 → 1.03 ms), consistent with thermal throttling after the preceding
4096³ FP16 run. This parallels the unchanged 4096³ BF16 result (0.995×) in
the `rtx4090-pr1-matched` evidence.

## Telemetry and limits

Among all samples (100 ms sampling interval):

| Sampled measurement | Stock | Unlocked |
|---|---:|---:|
| Clock range (active) | 2655–2775 MHz | 2505–2775 MHz |
| GPU temperature range | 35–53 °C | 34–50 °C |
| Largest observed power sample | 313.17 W | 345.66 W |
| SW Power Cap throttle | GEMM only | MMA + GEMM |

The 450 W hardware power cap was not reached in either state.

Desktop activity, temperature and clock variation can influence timing. Stock
had Steam processes active (~193 MiB VRAM); unlocked did not. Neither was
compute-active during benchmarks. There was no extended stability test or
representative inference/training workload in this run. Physical AC removal
was not independently observed.

## Reproduce and verify

From this directory, check every included evidence file with:

```sh
sha256sum -c SHA256SUMS
```
