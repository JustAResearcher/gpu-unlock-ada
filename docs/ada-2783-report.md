# RTX 4070 SUPER (10de:2783) Ada canary

The AD102-family four-PLM payload ran on one RTX 4070 SUPER (`10de:2783`,
AD104, 56 SMs, 12 GB) before the unmodified NVIDIA `595.99.02` driver.
Every gate the module enforces passed, the NVIDIA RM getter changed FMLA32,
IMLA2 and IMLA4 from half to full rate, and matched stock/unlocked
measurements show roughly 2x FP16/BF16 tensor instruction throughput with FP32
accumulation. This is **one card**; it does not establish support for other
AD104 boards, and no extended stability test was run.

The [evidence directory](evidence/ada2783/README.md) holds the sanitized
outputs. GPU UUID, PCI location, host name and boot identifiers were removed.

## Test system

Ubuntu 24.04.5, initramfs-tools, **Secure Boot enabled**. The module was built
and installed from the `.deb` packages in this repository (DKMS) and signed with
the machine's own enrolled MOK key. Kernel `6.8.0-147-generic` for the unlocked
runs. The module compiles on 6.8 without warnings. NVIDIA `595.99.02`
(prebuilt Canonical modules), power limit 242 W, nothing else using the GPU
during the measurements (the host's inference service was stopped for the GEMM
and llama.cpp runs). Whether a desktop session was active was not recorded.

The stock runs used kernel `6.8.0-146-generic` of the same installation, whose
initramfs does not contain `gpu_unlock`, after an ordinary reboot. The driver
and all benchmark binaries (verified by SHA-256) were identical; the kernel
build differs between the two states, so this is not a same-kernel A/B.

## Boot and handoff

Options used: `gpu_unlock.targets=<exact BDF> gpu_unlock.ad104_experimental=1`.

First boot after enabling the option (after a full power-off; removal of AC
power was not verified), see [transaction excerpt](evidence/ada2783/transaction-excerpt.txt):

- entry: `SS0=0x44013055`, `SS1=0x0`, WPR2 high `0x0`, chip `0x194000a1`;
- payload marker `0x67b00001` (expected `0x67b00001`), SEC2 halted;
- permission writes: `WPR_CFG=0xfffff0ff`, `WPR=0xffffffff`, `FEAT=0xffffffff`;
  the observational FBPA field stayed `0xffffff0f`;
- selector writes read back `SS0=0x88888888` and `SS1=0x8`; the effective
  readout changed from fields `0 0 2 0 0 2 0 2 0` to all zero;
- after PCIe FLR the selectors were still `88888888`/`8`, then
  `gpu_unlock: unlocked`. The stock driver loaded; no `NVRM: Xid` was logged.

A later **ordinary warm reboot** into the same kernel and options entered with
`SS0=0x0` and unlocked again with the same readbacks
([excerpt](evidence/ada2783/warm-reboot-excerpt.txt)). A warm reboot into the
kernel without the module returned the card to stock (FMLA32/IMLA2/IMLA4 = 1),
so the unlock must be re-applied on every boot.

| RM getter | Stock | Unlocked | Unlocked after warm reboot |
| --- | ---: | ---: | ---: |
| FMLA32, IMLA2, IMLA4 | 1 (half) | 0 (full) | 0 (full) |
| IMLA0, FMLA16, DP, FFMA, IMLA1, IMLA3 | 0 | 0 | 0 |

## Instruction rates (`mma_bench`, 20000 iterations, 5 repetitions)

| Mode | Stock TF/s | Unlocked TF/s |
| --- | ---: | ---: |
| f16/f16 | 153.4 | 153.6 |
| f16/f32 | 76.7 | 153.6 |
| bf16/f32 | 76.7 | 153.6 |
| s8/s32 | 306.9 | 307.3 |
| dp4a | 76.4 | 76.5 |
| ffma | 36.6 | 36.7 |

The SM89 tensor-mode probe (cubin SHA-256 `fc0efe68…c599d4`, exact UUID and
power-cap checks) measured f16/f32 76.7 → 152.7, TF32 38.4 → 76.4 and
unsigned INT4 610 → 611 TOP/s, with zero output mismatches in both states.

## cuBLAS GEMM (`gemm_bench`, medians of 5 repetitions)

| Mode | n | Stock TF/s | Unlocked TF/s | Ratio |
| --- | ---: | ---: | ---: | ---: |
| FP16/FP32 | 1024 | 49.3 | 67.5 | 1.37 |
| FP16/FP32 | 4096 | 73.6 | 122.9 | 1.67 |
| FP16/FP32 | 8192 | 75.7 | 144.4 | 1.91 |
| BF16/FP32 | 1024 | 49.3 | 67.8 | 1.37 |
| BF16/FP32 | 4096 | 73.6 | 115.7 | 1.57 |
| BF16/FP32 | 8192 | 75.7 | 145.2 | 1.92 |

`validate_gemm` (six seeded FP16/BF16 cases) reported zero failures in the
stock, unlocked and warm-rebooted states. Peak telemetry during the stock
batch was 141.6 W / 51 C and during the unlocked GEMMs 151.5 W / 53 C
([summary](evidence/ada2783/telemetry-summary.json)); this describes these short
runs only. Small shapes gain less because they are not tensor-bound.

## Application check (llama.cpp): no change

`llama-bench` (build 11552, `-ngl 99`, 3 repetitions) on a 27B model quantized
to IQ2_XS:

| Test | Stock t/s | Unlocked t/s |
| --- | ---: | ---: |
| pp512 | 1407.12 ± 31.49 | 1407.03 ± 32.12 |
| pp2048 | 1420.12 ± 3.41 | 1423.85 ± 3.79 |
| tg128 | 45.93 ± 0.05 | 45.93 ± 0.05 |

Quantized llama.cpp kernels use INT8/dp4a and memory bandwidth, neither of which
the unlock changes (INT8 above is unchanged). Workloads that use FP16/BF16 tensor
GEMMs with FP32 accumulation should benefit; this one does not.

## Limits

- One RTX 4070 SUPER, one board and VBIOS; first-boot AC removal not verified.
- Stock and unlocked runs used different kernel builds of the same install.
- Instruction rates and GEMMs are synthetic. No training, diffusion or
  FP16/BF16 inference workload was measured.
- No long-duration stability, thermal or power test was run.
- Linux only. Nothing here changes the Windows contribution.
