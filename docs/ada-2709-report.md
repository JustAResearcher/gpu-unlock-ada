# RTX 4070 on AD103 (10de:2709) canary

On October 25, 2026, the AD102-family four-PLM payload ran on one RTX 4070
built on the AD103 die (`10de:2709`, 46 SMs, VBIOS `95.03.3C.40.0C`) before the
unmodified NVIDIA `595.99.02` open kernel driver. Every gate the module
enforces passed, the NVIDIA RM getter changed FMLA32, IMLA2 and IMLA4 from half
to full rate, and matched instruction rates doubled. This is one physical card
and one unlocked boot. It does not establish support for other 2709 boards or
for the AD104 RTX 4070 (`10de:2786`), which is a different chip.
The [evidence directory](evidence/ada2709/README.md) contains the transaction
excerpt, rate readbacks and unedited benchmark output with the GPU UUID
replaced.

The host ran Ubuntu 24.04.5 with the HWE kernel `7.0.0-38-generic`, Secure
Boot disabled, and the card in a PCIe 4.0 x4 slot. A second, non-NVIDIA GPU was
present and was not touched. The module was built by DKMS from this branch
(the 2709 admission on top of 1.1.1) through `make deb`. The boot used the
**single exact PCI target** and `ad103_experimental=1`.

## Warm reboot, not a cold start

The first and only attempt was an ordinary `systemctl reboot` from a running
stock system that had used the card for CUDA work. The module's entry readback
reported WPR2 high `0x0`, so the card returned to a fresh enough state, as the
2705 did on its warm reboots. No AC power cut was needed on this board. Other
boards may retain WPR2; the module would then refuse, as designed.

## Firmware and driver checks

The module loaded the pinned NVIDIA `610.43.03` stock AD102 Booter/GSP assets
and the generated payload. The kernel log recorded:

- entry: `SS0=0x57072002`, `SS1=0x3`, WPR2 high `0x0`, chip `0x193000a1`;
- payload marker `0x67b00001` (expected `0x67b00001`), SEC2 halted;
- the three required permission writes: `WPR_CFG=0xfffff0ff`,
  `WPR=0xffffffff`, `FEAT=0xffffffff`. The observational FBPA field stayed
  `0xffffff0f`, as on the 2705 and 2805;
- selector writes read back `SS0=0x88888888` and `SS1=0x8`, and the
  effective readout changed to all-zero fields;
- after PCIe FLR the selectors were still `SS0=88888888 SS1=00000008`;
- `gpu_unlock: unlocked`.

The stock driver then loaded and CUDA kernels completed. The current-boot
kernel log contained no NVRM Xid or GPU-fallen-off message.

The read-only RM getter reported:

| Field | Stock | Unlocked |
| --- | ---: | ---: |
| FMLA32 | 1 (half) | 0 (full) |
| IMLA2 | 1 (half) | 0 (full) |
| IMLA4 | 1 (half) | 0 (full) |
| IMLA0, FMLA16, DP, FFMA, IMLA1, IMLA3 | 0 | 0 |

## Matched measurements

Stock was measured in the preceding boot, before the package was installed;
the stock 1.1.1 module does not accept `10de:2709`. Both states used the same
card, kernel, driver, 200 W default power cap, binaries and cubin (CUDA 12.8,
`sm_89`, cubin SHA-256
`b7e23c0d7922311c4319352640255e84caad02642fcdc901270c97a0cee4eac1`). Only
`/proc/cmdline` differed.

`diagnostics/bench/mma_bench.cu`, 20,000 iterations, best of five:

| Mode | Stock TF/s | Unlocked TF/s |
| --- | ---: | ---: |
| FP16 → FP16 accumulate | 132.6 | 132.2 |
| FP16 → FP32 accumulate | 66.3 | 132.2 |
| BF16 → FP32 accumulate | 66.3 | 132.0 |
| INT8 → INT32 | 265.1 | 263.7 |
| dp4a | 65.8 | 65.7 |
| scalar FFMA | 31.7 | 31.7 |

`tensor_modes_issue` through the driver runner (0 mismatches in every mode):

| Mode | Stock | Unlocked |
| --- | ---: | ---: |
| FP16 → FP32 accumulate | 66.3 TF/s | 131.6 TF/s |
| TF32 → FP32 accumulate | 33.1 TF/s | 65.8 TF/s |
| Unsigned INT4 → INT32 | 527.2 TOP/s | 525.7 TOP/s |

cuBLAS GEMM median (`docs/evidence/rtx4090-pr1-stock/gemm_bench.cu`):

| Mode | n | Stock TF/s | Unlocked TF/s | Change |
| --- | ---: | ---: | ---: | ---: |
| FP16/FP32 | 1024 | 49.06 | 70.14 | +43% |
| BF16/FP32 | 1024 | 49.00 | 69.91 | +43% |
| FP16/FP32 | 4096 | 64.04 | 96.33 | +50% |
| BF16/FP32 | 4096 | 63.92 | 95.05 | +49% |
| FP16/FP32 | 8192 | 64.42 | 93.63 | +45% |
| BF16/FP32 | 8192 | 64.16 | 92.56 | +44% |

`validate_gemm` passed every FP16/FP32 and BF16/FP32 case with
`failures=0, max_abs=0` in both states, and `gemm_bench` reported zero failed
cells. The large GEMMs at the 200 W cap are likely power-limited, which may
explain why they gain less than 2×.

## Not yet done

- a cold-boot (AC cut) attempt and a repeat warm reboot;
- an extended stability run and application workloads such as training;
- other 2709 boards. The AD104 `10de:2786` RTX 4070 is not admitted by this
  change.
