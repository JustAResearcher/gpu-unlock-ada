# Gigabyte RTX 4070: first native Linux unlock benchmark

2026-10-24, CachyOS kernel 7.3.0-5-cachyos / NVIDIA open 615.78.08, 10de:2786. The user reported full shutdown and manually selected the dedicated test entry. This is the first measured successful Linux cold-boot unlock on this card in this session.

| Mode | Stock | After unlock | Change |
|---|---:|---:|---:|
| f16/f16 | 133.9 | 133.9 | +0.0% |
| f16/f32 | 67.0 | 133.8 | +99.7% |
| bf16/f32 | 66.9 | 133.7 | +99.9% |
| s8/s32 | 267.4 | 266.7 | -0.3% |
| dp4a | 66.6 | 66.4 | -0.3% |
| ffma | 32.1 | 32.1 | +0.0% |

Throughput is TF/s for floating-point modes and TOP/s for integer modes. Same unchanged sm_89 benchmark binary (SHA-256 `7405a0d6c40f603983fd44ece26e861cda858cec874061395e5490ab474da127`), 20,000 iterations, three repetitions. The benchmark reports the best timed run. Both targeted FP32-accumulate tensor modes approximately doubled; FP16-accumulate, INT8, DP4A and scalar FP32 controls were essentially unchanged.

Independent RM rates changed fmla32, imla2 and imla4 from 1 (half) to 0 (full). All other RM rate fields remained 0.

The boot log confirms cold WPR2 disabled, successful Booter/FWSEC version-1 selection, payload marker 0x67b00001, halted SEC2, all required post-payload permissions, SS0=0x88888888 and SS1=0x8 after FLR, zero effective readout after FLR, and gpu_unlock: unlocked. FBPA remained the observational 0xffffff0f, as in prior Ada evidence; it is not counted as an unlocked permission. WPR_CFG/WPR permissions returned to their initial values after FLR while selectors persisted, matching the logged transaction design.

No NVRM Xid, fallen-off-GPU or retained-DMA error appears in the current-boot log captured after the benchmark. nvidia-smi reports the expected card and driver. Pre-benchmark point samples were 216 W limit, 2100 MHz graphics, 10501 MHz memory and 34 C; stock rate-query point samples had the same configured power limit and clocks. These are not continuous measurement traces.

The kernel logged an out-of-tree module taint and a signature-verification warning. The module was signed and its signature independently verified with the existing DKMS certificate, but that certificate was not accepted by the running kernel's trust policy. Secure Boot was already off and remains off; no enforcement, EFI loader or enrolled key was changed. Secure-Boot-enabled operation is not validated.

This establishes one successful cold transaction and the targeted instruction-throughput gain on this specific board. It does not establish whole-application speedups, repeat-boot reliability, long-duration stability or other-board support. Keep the stock GRUB entry and retain the optional test entry until further validation.

Evidence is retained alongside this comparison: [inventory](README.md), [validation](cold-boot-validation.json), and [GPU transaction excerpt](cold-boot-unlock.log). Full kernel logs remain private.
