# RTX 4060 (10de:2882) Ada canary

On October 26, 2026, the AD102-family four-PLM payload ran on one desktop
RTX 4060 (`10de:2882`, AD107, 24 SMs, 8 GB) before the unmodified NVIDIA
`615.78.08` open kernel driver. Every gate the module enforces passed and the
NVIDIA RM getter changed FMLA32, IMLA2 and IMLA4 from half to full rate. The
card unlocked on a cold boot after a full power-off and again on a later warm
reboot. A same-card stock and unlocked cuBLAS comparison through PyTorch
measured FP16 and BF16 GEMMs with FP32 accumulation 41 to 58% faster, while
the FP16-accumulate, FP32 and INT8 controls stayed level at n=8192. This is
one physical card. It does not establish support for other AD107 boards, and
the laptop AD107 IDs are not covered by this target.
The [evidence directory](evidence/ada2882/README.md) contains the selected boot
messages, rate readbacks and benchmark stdout with the GPU UUID replaced.

The host ran Fedora 44 with kernel `7.2.11-200.fc44.x86_64`, an AMD Ryzen 7
5800X and the NVIDIA open kernel modules from RPM Fusion's `akmod-nvidia`. The
card is a PNY board (subsystem `196e:13e6`, VBIOS `95.07.31.40.39`) in a
CPU-attached PCIe 4.0 slot at x8, its full width, at the default 115 W cap.
The module and firmware were built with `make rpm` from commit `57c128e`,
which adds the target; later commits change only comments, a parameter
description and documentation. They were installed as `gpu-unlock-dkms` and
`gpu-unlock-firmware`. DKMS signed the module with its own key; Secure Boot
was disabled, so the `.ko` hash is host-specific and not recorded here. The
packaged dracut drop-in put the module and firmware into the initramfs, which
grew to 222 MB per kernel with the GSP firmware included; a small `/boot`
needs room for that, and dracut writes the new image beside the old one.
The boot used the **single exact PCI target** and `ad107_experimental=1`.

## Firmware and driver checks

The module loaded the pinned NVIDIA `610.43.03` stock AD102 Booter/GSP assets
and the generated 64,000-byte payload. On the cold boot the kernel log
recorded:

- entry: `SS0=0x3374074`, `SS1=0x5`, WPR2 high `0x0`, chip `0x197000a1`;
- payload marker `0x67b00001` (expected `0x67b00001`), SEC2 halted;
- the three required permission writes: `WPR_CFG=0xfffff0ff`,
  `WPR=0xffffffff`, `FEAT=0xffffffff`. The observational FBPA field stayed
  `0xffffff0f`, as on the 2705 and 2805;
- selector writes read back `SS0=0x88888888` and `SS1=0x8`, and the
  effective readout changed from fields `0 0 2 0 0 2 0 2 0` to all zero;
- after PCIe FLR the selectors were still `SS0=88888888 SS1=00000008`;
- `gpu_unlock: unlocked`.

After a warm reboot the entry readback was `SS0=0x0`, `SS1=0x0`, WPR2 high
`0x0`. The module ran the whole sequence again with the same results, line for
line apart from the SS0/SS1 values read before the selector writes. In both
boots the stock driver then loaded, CUDA work completed and the kernel log
contained no NVRM Xid or GPU-fallen-off message.

The read-only RM getter reported:

| Field | Before | After |
| --- | ---: | ---: |
| FMLA32 | 1 (half) | 0 (full) |
| IMLA2 | 1 (half) | 0 (full) |
| IMLA4 | 1 (half) | 0 (full) |
| IMLA0, FMLA16, DP, FFMA, IMLA1, IMLA3 | 0 | 0 |

The "before" values were read on the same card and driver before the module
was installed; the "after" values after the warm reboot.

## Stock and unlocked cuBLAS GEMM

`torch-gemm.py` in the evidence directory times square cuBLAS GEMMs through
PyTorch `2.14.1+cu130` (median of three repetitions of 30 calls). It ran in a
stock boot and in the cold unlocked boot with the same card, driver, power cap
and PyTorch build. FP16 and BF16 use FP32 accumulation; `fp16-acc16` allows
FP16 accumulation; FP8 is `torch._scaled_mm` with e4m3 inputs, unit scales and
BF16 output. Rates in TF/s (TOP/s for INT8):

| Mode | n=4096 stock | unlocked | n=8192 stock | unlocked |
| --- | ---: | ---: | ---: | ---: |
| FP16, FP32 accumulate | 29.97 | 46.49 (+55%) | 31.38 | 44.23 (+41%) |
| BF16, FP32 accumulate | 31.46 | 49.57 (+58%) | 31.59 | 45.37 (+44%) |
| TF32, FP32 accumulate | 15.86 | 25.86 (+63%) | 15.89 | 25.30 (+59%) |
| FP8 (`torch._scaled_mm`) | 63.97 | 96.19 (+50%) | 63.49 | 100.43 (+58%) |
| FP16, FP16 accumulate | 46.75 | 52.48 (+12%) | 49.47 | 49.19 (-1%) |
| FP32 (no tensor cores) | 7.85 | 7.60 (-3%) | 7.79 | 7.78 (0%) |
| INT8 (`torch._int_mm`) | 28.10 | 28.27 (+1%) | 26.62 | 26.62 (0%) |

The n=8192 controls are level, so the gains there are not a clock effect.
The FP16-accumulate control moved 12% at n=4096, so the smaller size carries
more run-to-run variation. `torch._int_mm` does not reach this card's INT8
tensor peak, so its row only shows that the path did not change. The FP8 gain
differs from the one matched FP8 GEMM on the 2705, which stayed level; the
kernel cuBLAS chose for this FP8 call was not recorded.

## Numerical check

`torch-gemm-check.py` ran in the cold unlocked boot. Relative error of a
4096 x 4096 GEMM against a float64 reference:

| Mode | Error |
| --- | ---: |
| FP16 input rounding floor | 2.94e-04 |
| FP16, FP32 accumulate | 3.60e-04 |
| FP16, FP16 accumulate | 3.33e-03 |
| BF16 input rounding floor | 2.35e-03 |
| BF16, FP32 accumulate | 2.88e-03 |
| TF32 | 2.94e-04 |
| FP32 | 1.15e-06 |

FP16 stays close to its input-rounding floor and about nine times below the
FP16-accumulate path, so the unlocked rate still accumulates in FP32. The same
FP16 and BF16 GEMMs then ran for 180 seconds: 57,074 results, none differing
bitwise from the first. No stock run of this check was made.

## Unlocked instruction rates

`diagnostics/bench/tensor_modes_issue.cu` was built with CUDA 13.4.92 for
`sm_89` (cubin SHA-256
`33aa6db385aa9eb8a1f73bb2f74891aea84794326b7c40992bc642f93a3c614d`, the same
cubin as the 2805 report) and run after the warm reboot at the 115 W cap with
20,000 iterations and three repetitions:

| Mode | Best rate | Mismatches |
| --- | ---: | ---: |
| FP16, FP32 accumulate | 65.5 TF/s | 0 / 270,336 |
| TF32, FP32 accumulate | 34.0 TF/s | 0 / 270,336 |
| Unsigned INT4, INT32 | 271.2 TOP/s | 0 / 270,336 |

INT4 did not change between stock and unlocked on the 2705, so it serves as a
clock reference. The TF32-to-INT4 ratio is 0.125, the full-rate ratio of 512
to 4,096 operations per clock per SM; half rate would give 0.0625. The
FP16/FP32-to-INT4 ratio is 0.241, against 0.252 unlocked and 0.126 stock on
the 2705 and 0.243 on the 2805. FP16 runs first and its first two repetitions
were slower while clocks ramped, as on the 2805.

## Applications

Measured in the same stock boot and cold unlocked boot:

- llama.cpp `llama-bench` (release b11379, CUDA 12.8 build) on Qwen3.5-4B
  Q8_0 (`-ngl 99 -fa on -p 2048 -n 128 -r 3`): prompt processing 3,276.87 to 3,290.00 tokens/s and
  generation 50.42 to 50.48 tokens/s, both unchanged. Its quantized matmuls
  run on INT8 paths, which did not change in the GEMM table either.
- a ComfyUI Z-Image Turbo text-to-image workflow at 1024 x 1024 with INT8
  model weights: 9.39 to 8.25 seconds per image, median of five (-12%). The
  gain presumably comes from the parts that still run in BF16, such as
  attention.

## Not yet done

- a stock boot with the tensor-mode probe and `validate_gemm.cu`;
- a second cold boot;
- suspend and resume;
- other 2882 boards.
