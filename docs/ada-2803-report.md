# RTX 4060 Ti 16GB (10de:2803) Ada canary

On October 25, 2026, the AD102-family four-PLM payload ran on one
RTX 4060 Ti 16GB (`10de:2803`, AD106, 34 SMs) before the unmodified NVIDIA
`615.78.08` driver. Every gate the module enforces passed and the NVIDIA RM
getter changed FMLA32, IMLA2 and IMLA4 from half to full rate. This is one
physical card and one observed unlock boot. Other AD106 boards and the 8 GB
variant remain unverified. The [evidence directory](evidence/ada2803/README.md)
contains the selected boot messages, rate readbacks and benchmark output.

The host ran Arch Linux with kernel `6.18.58-1-lts`. The installed DKMS module
source matched commit `41818ac`. The unlocked boot used the
**single exact PCI target** and `ad106_experimental=1`. The owner reported a
reboot; a cold start was not independently confirmed. The module's entry
readback reported WPR2 high `0x0`.

## Firmware and driver checks

The module loaded the pinned NVIDIA `610.43.03` stock AD102 Booter/GSP assets
and the unchanged four-PLM payload, using the same path as `10de:2805`.
The kernel log recorded:

- entry: `SS0=0x0`, `SS1=0x0`, WPR2 high `0x0`, chip `0x196000a1`;
- payload marker `0x67b00001`, SEC2 halted;
- required permissions: `WPR_CFG=0xfffff0ff`, `WPR=0xffffffff`,
  `FEAT=0xffffffff`; observational FBPA stayed `0xffffff0f`;
- selectors `SS0=0x88888888`, `SS1=0x8`, with all nine effective readout
  fields zero before and after PCIe FLR;
- `gpu_unlock: unlocked`.

The stock driver loaded and CUDA kernels completed. The current-boot kernel
log contained no NVRM Xid, GPU-fallen-off or GSP failure/timeout messages.
The read-only RM getter reported:

| Field | Stock | Unlocked |
| --- | ---: | ---: |
| FMLA32 | 1 (half) | 0 (full) |
| IMLA2 | 1 (half) | 0 (full) |
| IMLA4 | 1 (half) | 0 (full) |
| IMLA0, FMLA16, DP, FFMA, IMLA1, IMLA3 | 0 | 0 |

## Unlocked instruction rates

One matched pair used the same card, kernel `6.18.58-1-lts`, initramfs,
installed module, NVIDIA `615.78.08`, CUDA `13.4.92`, 165 W cap, clock policy
and benchmark binaries. Kernel, initramfs, module, firmware and binary hashes
matched. The unlocked run came first, followed by a stock boot without
`gpu_unlock.*` options. RM rate fields stayed unchanged through the workloads
in each state. A Wayland desktop was active in both runs.

`diagnostics/bench/tensor_modes_issue.cu` was built for `sm_89` and run with
20,000 iterations and three repetitions. Best rates and checks of all
270,336 output cells per mode were:

| Mode | Stock | Unlocked | Mismatches |
| --- | ---: | ---: | ---: |
| FP16 → FP32 accumulate | 46.4 TF/s | 92.3 TF/s | 0 |
| TF32 → FP32 accumulate | 23.2 TF/s | 46.2 TF/s | 0 |
| Unsigned INT4 → INT32 | 368.7 TOP/s | 368.7 TOP/s | 0 |

FP16/FP32 and TF32/FP32 instruction rates increased by 1.99×. The separate
`mma_bench 20000 5` run measured FP16/FP32 and BF16/FP32 at 46.4 TF/s stock,
versus 92.8 and 92.7 TF/s unlocked, respectively. FP16/FP16, INT8, `dp4a`
and scalar FP32 stayed near their stock rates. All six seeded FP16/BF16
numerical cases passed with zero mismatches in both states.

## Not yet done

- repeated matched stock/unlocked pairs;
- an independently confirmed cold start and reboot repeats;
- application workloads and extended stability testing;
- other 2803 boards and the 8 GB variant.
