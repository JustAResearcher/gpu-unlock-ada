# RTX 3050 8GB on GA106 (10de:2507)

On October 27, 2026, the existing GA102 Booter path and v67 payload ran on one
GeForce RTX 3050 8GB (`10de:2507`, GA106, 20 SMs, MSI `1462:c978`, VBIOS
`94.06.37.00.86`) before the unmodified NVIDIA `615.71.09` open kernel driver.
Every gate the module enforces passed, the NVIDIA RM getter changed FMLA32 and
IMLA4 from half to full rate, and matched FP16/BF16/TF32 tensor instruction
rates with FP32 accumulation doubled. `0x2507` was already in the GA106 list
of `supported_ids` but had not been run on hardware; this is one physical card
and one unlocked boot. The [evidence directory](evidence/ga106-2507/README.md)
contains the transaction excerpt, rate readbacks and unedited benchmark output
with the GPU UUID replaced.

## Host and sequence

The host ran Arch Linux with kernel `7.2.6-arch2-1`, an AMD Ryzen 7 5700,
Secure Boot disabled, and the card on a PCIe 3.0 x8 link as the only GPU, with
no display attached. The module was built from `d28c485` with `make arch`
(the build ran as an unprivileged user, so `make install-arch` was replaced by
`make arch` and `pacman -U` of the two packages) and loaded from the
mkinitcpio image through the packaged drop-in.

The unlocked boot used a one-shot GRUB entry (`grub-reboot`) identical to the
default entry plus `gpu_unlock.targets=0000:01:00.0`. No experimental flag is
needed for `0x2507`. That boot followed an ordinary warm reboot, not an AC
power cut: the journal shows 37 s between the previous boot's last entry and
the new one. The previous boot had run the stock driver and CUDA work. The
entry readback was nevertheless `wpr2_hi=0x0` and the module did not refuse the
card. This is a single attempt and does not show that a warm reboot always
leaves this card in a usable state.

## Firmware and driver checks

The kernel log recorded:

- entry: `plm=0xffffff8f`, `SS0=0x10410064`, `SS1=0x2`, WPR2 high `0x0`,
  chip `0xb76000a1`;
- layout: 8 GiB FB (`fb_mb=0x2000`), all reservations above the
  pre-scrubbed top 256 MiB (`nonwpr=0x1f5300000`, floor `0x1f0000000`);
- Booter signature `fuse_raw=0x1 version=1`, FWSEC fuse version 2;
- FWSEC-FRTS set WPR2 at the expected page (`low=0x1ffe000 high=0x1ffee00`);
- selector writes read back `SS0=0x88888888` and `SS1=0x8`;
- the PCIe FLR returned `0` after 103 ms; afterwards the selectors were still
  `SS0=88888888 SS1=00000008`, GFW completed (`error=0`), SEC2 was reset, GSP
  halted and WPR2 high was `0`;
- `gpu_unlock: unlocked`, about 1.06 s after the first asset request.

The only timeouts were the two Falcon pre-reset polls the module tolerates per
HW bug 3419321, as on the 2584. The stock driver then loaded, `nvidia-drm`
initialized, CUDA work completed and the kernel log had no NVRM Xid, before or
after the workloads.

The read-only RM getter reported:

| Field | Stock | Unlocked |
| --- | ---: | ---: |
| FMLA32 | 1 (half) | 0 (full) |
| IMLA4 | 1 (half) | 0 (full) |
| IMLA0–3, FMLA16, DP, FFMA | 0 | 0 |

The next warm reboot into the default entry, without the option, read FMLA32
and IMLA4 at `1` again before any workload: the unlock did not persist across
that reboot.

## Matched measurements

The stock reference was collected in the boot after the unlocked one, so that
both states share the same kernel, driver, 130 W cap (the board's default and
maximum), binaries and cubin (CUDA 13.4, `sm_86`, cubin SHA-256
`431f0489c702d5713592bfe0d6949b7da7e725dadf7cc264a37d324a44386092`), and the
same CPU power regime (power-profiles-daemon `performance`; see
[the inference section](#quantized-inference) for why that matters). A first
stock baseline taken before installation, under a different CPU regime,
agrees with this reference within 1% on every GPU-bound probe below; its files
are kept as `*-baseline.*`. All GPU clients were stopped before each state.

`diagnostics/bench/mma_bench.cu`, 20,000 iterations, best of five per pass,
best of three passes:

| Mode | Stock TF/s | Unlocked TF/s |
| --- | ---: | ---: |
| FP16 → FP16 accumulate | 39.8 | 39.8 |
| FP16 → FP32 accumulate | 19.9 | 39.8 |
| BF16 → FP32 accumulate | 19.9 | 39.8 |
| INT8 → INT32 | 79.5 | 79.5 |
| dp4a | 19.8 | 19.8 |
| scalar FFMA | 9.5 | 9.5 |

`tensor_modes_issue` through the driver runner, best of three passes
(0 mismatches in every mode and pass):

| Mode | Stock | Unlocked |
| --- | ---: | ---: |
| FP16 → FP32 accumulate | 19.89 TF/s | 39.62 TF/s |
| TF32 → FP32 accumulate | 9.95 TF/s | 19.81 TF/s |
| Unsigned INT4 → INT32 | 158.15 TOP/s | 158.16 TOP/s |

cuBLAS GEMM median (`docs/evidence/rtx4090-pr1-stock/gemm_bench.cu`):

| Mode | n | Stock TF/s | Unlocked TF/s | Change |
| --- | ---: | ---: | ---: | ---: |
| FP16/FP32 | 1024 | 12.79 | 22.49 | +76% |
| BF16/FP32 | 1024 | 13.22 | 16.74 | +27% |
| FP16/FP32 | 4096 | 18.98 | 29.47 | +55% |
| BF16/FP32 | 4096 | 18.92 | 35.47 | +88% |
| FP16/FP32 | 8192 | 19.41 | 25.87 | +33% |
| BF16/FP32 | 8192 | 19.41 | 25.83 | +33% |

PyTorch 2.14.1 matmul under AMP with FP32 accumulation
(`evidence/ga106-2507/torch_gemm.py`; FP16/BF16 output, reduced-precision
reductions disabled), median of 150 event-timed calls after warm-up:

| Shape (M×N×K) | Type | Stock TF/s | Unlocked TF/s | Change |
| --- | --- | ---: | ---: | ---: |
| 2048³ | FP16 | 16.10 | 30.34 | +88% |
| 2048³ | BF16 | 17.20 | 31.39 | +82% |
| 4096³ | FP16 | 19.01 | 33.38 | +76% |
| 4096³ | BF16 | 18.93 | 34.56 | +83% |
| 8192×2048×4096 | FP16 | 19.01 | 33.17 | +75% |
| 8192×2048×4096 | BF16 | 18.96 | 34.56 | +82% |

`validate_gemm` passed every FP16/FP32 and BF16/FP32 case with
`failures=0, max_abs=0` in both states, and `gemm_bench` reported zero failed
cells. The PyTorch outputs, compared with an FP32 reference without TF32, had
identical relative errors in both states (FP16 about 2.1e-4, BF16 about 1.7e-3
relative Frobenius) and no NaN or Inf.

Telemetry sampled every 200 ms (samples with utilization ≥ 50%):

| Probe | Stock W / SM MHz / SW power cap | Unlocked W / SM MHz / SW power cap |
| --- | --- | --- |
| mma_bench | 53 / 1957 / 0% | 60–62 / 1942–1957 / 0% |
| gemm_bench | 96 / 1935 / 0% | 112 / 1920 / 0% |
| PyTorch GEMM | 109 / 1927 / 0% | 129 / 1770 / 100% |

Unlocked, the PyTorch GEMMs ran at the 130 W cap with SW Power Cap active in
every sample and the SM clock about 8% lower, so they gain less than the 2×
issue rate.

## Quantized inference

Ollama 0.34.2 with `llama3.2:3b` Q4_K_M (all 29 layers on the GPU), a
2,020-token prompt with a per-request nonce, temperature 0, seed 42, one
warm-up and five measured requests (medians):

| State | CPU regime | Prefill tok/s | Generation tok/s | GPU util. |
| --- | --- | ---: | ---: | ---: |
| Stock | power-profiles-daemon | 3312 | 81.9 | 98% |
| Unlocked | power-profiles-daemon | 3362 | 81.8 | 98% |

At the same CPU regime, generation is unchanged (1.00×) and prefill is 1.5%
higher, about the run-to-run spread. The generated token counts were identical
in every run. This matches the README's observation that quantized matmuls do
not use the FP32-accumulate tensor path.

**Methodological caution for other testers.** The first stock baseline,
collected in an earlier boot, measured 45.0 tok/s generation, so the first
comparison suggested a +82% gain. That difference came only from the CPU
power daemon: both `tuned` and `power-profiles-daemon` are enabled on this
host, and the baseline boot had started `tuned` (`realtime-virtual-host`)
instead. With the stock card in the same boot, switching only the daemon gave:

| State | CPU regime | Prefill tok/s | Generation tok/s | GPU util. |
| --- | --- | ---: | ---: | ---: |
| Stock | tuned `realtime-virtual-host` | 3141 | 45.6 | 61% |
| Stock | power-profiles-daemon | 3312 | 81.9 | 98% |

The cpufreq governor, EPP and frequency limits were the same under both
daemons; tuned also set `sched_rt_runtime_us=-1`, `timer_migration=0` and
`vm.stat_interval=10`. Which setting slows the host side of generation was not
isolated. Record the CPU power daemon and its profile with every
stock/unlocked pair, and keep it identical.

Ollama also added `--load-mode none` to its `llama-server` command line in
both runs of the later boot (power-profiles-daemon and tuned), but not in the
first baseline or the unlocked boot, with the same version and configuration;
the cause was not identified. It does not change the conclusion: the
tuned/power-profiles-daemon gap is the same with the flag (45.6 → 81.9) as
without it (45.0 → 81.8), and the unlocked run without the flag matches the
stock run with it (81.8 and 81.9).

## Open points

- IMLA4 changed from 1 to 0, but none of the integer modes measured here moved
  (INT8 mma, unsigned INT4 mma and dp4a are within 1%). Which instructions
  IMLA4 governs is not established by these probes.
- At 4096³ unlocked, PyTorch FP16 reached 33.4 TF/s and `gemm_bench` FP16
  29.5 TF/s, while `gemm_bench` BF16 reached 35.5. The calls differ:
  `gemm_bench` uses `cublasGemmEx` (NN, FP32 output,
  `CUBLAS_GEMM_DEFAULT_TENSOR_OP`) and PyTorch writes FP16 output with its own
  layout and cuBLAS heuristics. `gemm_bench` FP16 was not power-capped
  (112 W). The gap is probably due to kernel selection; it was not profiled.

## Limits

- One card, one unlocked boot, reached by a warm reboot; no AC power-cut boot
  was tried.
- No extended stability run; the workloads are short probes and one small
  quantized model.
- Other `10de:2507` boards, VBIOS versions and laptop variants are untested.
- `0x2507` is admitted through the ordinary GA106 list, so it needs no opt-in
  flag and `gpu_unlock.targets=all` selects it, unlike the experimental Ada
  IDs, which require their own flag and, except for the RTX 4090 D, an exact
  address. This report does not change that admission.

## Not yet done

- a cold-boot (AC cut) attempt;
- a second warm reboot with the module option, to see whether WPR2 is cleared
  again;
- an extended stability run and application workloads such as training.
