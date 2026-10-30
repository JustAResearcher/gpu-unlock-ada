# RTX 4080 Laptop (AD104M, 10de:27e0): one HP result

One HP OMEN 17-ck2xxx completed the firmware transaction, stock NVIDIA driver
handoff and three rounds of checked compute probes. Median FP16/FP32 and
BF16/FP32 issue throughput was **53.9 -> 108.3 TFLOP/s (2.01x)**. cuBLAS
GEMM improved **50.7–63.2%** by shape/type. This is one board and one tested
unlock boot, not qualification of all 27e0 laptops or a whole-model speedup.

Both states used the same policy and hash-checked programs/libraries, but the
laptop's dynamic current-limit median differed: **114.18 W stock / 119.42 W
unlocked**. This is **not a fixed-power comparison**.

## Hardware and environment

| Item | Recorded value |
|---|---|
| Laptop | HP OMEN by HP Laptop 17-ck2xxx (OMEN 9 Plus), board 8BAD |
| GPU | AD104M A1, 10de:27e0, HP subsystem 103c:8bad, chip 0x194000a1 |
| Resources | 58 SMs, 12 GiB installed, 12282 MiB visible |
| BIOS / VBIOS | F.08 / 95.04.3C.00.79 |
| OS / kernel | Ubuntu 24.04.1 / 6.8.0-51-generic |
| Driver | NVIDIA proprietary 590.48.01, unchanged across the pair |
| Secure Boot | Enabled; tested DKMS module signature verified against enrolled local MOK before boot; no signing material published |
| Display | Active Xorg/GNOME; NVIDIA DRM exposed connected internal eDP and external DP outputs; BIOS MUX setting not independently recorded |
| Policy | AC online, battery full; OEM platform profile and power-profiles-daemon performance; nvidia-powerd active in both measured states |
| CUDA | nvcc 12.8.93, sm_89, private cudart 12.8.90; actually loaded cuBLAS/cuBLASLt 12.8.3.14 |
| Firmware | Existing pinned 610.43.03 inputs and unchanged AD102-family assets |

## Target and source provenance

The new default-off `ad104m_experimental` flag admits only `27e0`, requires an
exact PCI address in `targets` (alone or in an explicit list), and excludes
`targets=all`. Other SKU flags do not authorize this ID; `27a0` stays unsupported.
The helper prints the detected address. With a placeholder, the options are:

```text
gpu_unlock.targets=<exact-PCI-address> gpu_unlock.ad104m_experimental=1
```

The patch reuses the existing AD102-family asset and transaction routing.
ROM/hash/signature/HALT/marker/permission/selector/FLR/DMA checks and the
upstream 1.1.1 package version are unchanged.

The hardware-tested research tree was `568d915074e4cd9fc90bf3452903738baf6c9dfb`,
with adaptation `7a9c81f23f397baa223d9913b282e8244ce06f7d`, based on upstream
`d28c485418c3d9d034cd4bac19200bc33dd3453b`. These identify the original
collection, not a fresh boot of this later PR. Compared with that tested
production source, the PR changes only the new ID's comment, parameter
description and diagnostic label from preparation wording to experimental
wording; `unlock_610.c` is byte-identical. Executable transaction behavior is
unchanged. Research-only setup/Gitea docs and the macOS test-shim change are
excluded from this contribution.

Code/test commit `2c5e14ca2a24f242d59ff51b739b23353c21e785` was separately
validated on Linux: `make test-full` with pinned source/assets **36/36 passed,
zero skips**; `make -C kernel/gpu_unlock` built for `6.8.0-51-generic`.
The rebuilt module was not installed or used for another firmware transaction.
Later docs/evidence commits do not change this code. The new host fixture uses
measured AD104 A1 chip/visible-FB/VGA/MMU/selector inputs; ROM and register
transitions remain synthetic, not another silicon test.

## Boot transaction and driver handoff

Before boot, the signed module and all nine firmware assets in the initramfs
were checked. This image had no NVIDIA/nouveau dGPU driver modules;
`gpu_unlock` in `/conf/modules` ran before NVIDIA loaded from the root
filesystem. This does not establish ordering for other initramfs layouts.
The owner used one-time GRUB editing after shutdown/power-reset preparation
and retained the normal entry. Entry WPR2 high was zero; this does not prove
every HP shutdown/restart path clears firmware state.

The [sanitized transaction excerpt](evidence/ada27e0/transaction-excerpt.txt)
retains the ordered observations:

- Entry PLM=0xffffff8f, SS0=0x10763075, SS1=0x4, wpr2_hi=0.
- A 1 MiB PROM read with 0xaa55 signature; parsed FWSEC descriptor/signature
  selection and successful FWSEC result.
- Payload boot=0, halt=0, halted=1; marker 0x67b00001, marker_ok=1,
  required_permissions=1. Optional FBPA stayed 0xffffff0f.
- SS0=0x88888888 and SS1=0x8, matched selectors and full effective readout.
- One FLR, result=0; BAR recovery/GFW check/SEC2 reset release, GSP halted,
  BME=0, pending=0, disabled WPR2 and post-FLR full effective readout.
- `gpu_unlock: unlocked`, stock NVIDIA binding, normal display/NVML and no
  retained-DMA error. Post-FLR permissions are kept as actually logged,
  not all claimed to stay at their payload-stage values.

The two pre-reset timeouts explicitly tolerated by the existing
HW-bug-3419321 path were followed by successful checks, not hidden as
unhandled firmware failures. The complete private boot log and post-workload
query had no NVRM Xid, GPU-loss or PCIe/AER error. A network device's XID
identifier was not classified as an NVIDIA Xid. Boot IDs and monotonic kernel
timestamps were used internally because the early wall clock was corrected;
identifying host/calendar fields are omitted from public excerpts.

## Compute results and numerical checks

The stock side was collected before package installation, with powerd already
active; an earlier stock session without powerd is excluded. Both sides used
the same GPU/kernel/driver/policy/desktop and all **18 source/binary/library
hashes**. No manual clock or power settings were changed between the pair.
RM fmla32 was 1 before/after stock workloads and 0 before/after unlocked
workloads; this readback alone was not treated as performance proof.

[All observations and provenance](evidence/ada27e0/README.md) are retained.
`python3 docs/evidence/ada27e0/verify.py` verifies hashes and recomputes pooled
medians offline without operating the GPU.

MMA used 20000 iterations, five observations per mode per round, three rounds.
Pool all **15** observations. Floating rows are TFLOP/s, INT8/dp4a TOP/s:

| Input / accumulation or control | Stock | Unlocked | Change |
|---|---:|---:|---:|
| FP16 / FP16 | 106.100 | 107.000 | +0.85% |
| FP16 / FP32 | 53.900 | 108.300 | +100.93% |
| BF16 / FP32 | 53.900 | 108.300 | +100.93% |
| FP8 E4M3 / FP32 | 107.000 | 214.800 | +100.75% |
| INT8 / INT32 | 216.900 | 216.700 | -0.09% |
| dp4a | 54.600 | 52.900 | -3.11% |
| scalar FP32 | 25.600 | 25.400 | -0.78% |

FP8 is issue-throughput only: **no FP8 numerical or FP8 application GEMM
validation is claimed**.

The independent CUDA-driver probe has three observations per mode per round,
**nine** timing samples total and zero output mismatches. FP16/TF32 use
TFLOP/s; unsigned INT4 uses TOP/s:

| Checked mode | Stock | Unlocked | Change |
|---|---:|---:|---:|
| f16_f32 | 53.767 | 107.000 | +99.01% |
| tf32 | 26.804 | 53.769 | +100.60% |
| u4_s32 | 427.207 | 424.781 | -0.57% |

cuBLAS used ten warmups then five batches of 20 GEMMs per size/type per round.
The table pools all **15** batch rates, not rounded per-round medians. All
large-matrix output checks had zero failures. TFLOP/s:

| Shape and input / accumulation | Stock | Unlocked | Change |
|---|---:|---:|---:|
| 1024 cubed, FP16/FP32 | 46.349 | 71.575 | +54.43% |
| 1024 cubed, BF16/FP32 | 46.864 | 70.611 | +50.67% |
| 4096 cubed, FP16/FP32 | 49.818 | 76.395 | +53.35% |
| 4096 cubed, BF16/FP32 | 48.991 | 74.458 | +51.98% |
| 8192 cubed, FP16/FP32 | 49.727 | 81.143 | +63.18% |
| 8192 cubed, BF16/FP32 | 49.036 | 79.081 | +61.27% |

All six seeded FP16/BF16 validator cases per state had failures=0 and
max_abs=0, including non-square and odd-sized cases. These checks do not
establish correctness for arbitrary models or value ranges.

## Power, telemetry and limits

| Whole-session sample metric, including phase gaps | Stock | Unlocked |
|---|---:|---:|
| Detailed current-limit median | 114.18 W | 119.42 W |
| Detailed current-limit range | 113.01–118.35 W | 118.07–133.73 W |
| Reported average-draw peak, 200 ms | 115.57 W | 121.78 W |
| Instantaneous-draw peak, about 0.5 s | 118.99 W | 132.46 W |
| GPU temperature range | 50–69 C | 54–77 C |
| SM clock range, including gaps | 885–2445 MHz | 840–2445 MHz |
| SW Power Cap active | 92/239 samples | 59/162 samples |
| HW Thermal Slowdown active | 0 samples | 0 samples |

Peak samples are not physical maxima. Dynamic budgets, temperature and desktop
activity are confounders; library gains are not strict equal-power gains.
Similar controls and mode-specific issue-rate changes support the intended
path taking effect, not a whole-application speedup.

Not established: second cold boot, warm-reboot repeatability, A/B/A stock
restoration, suspend/resume/runtime-power recovery, long-duration load, other
HP/OEM/VBIOS variants, Windows, or end-to-end training/inference. No automatic
retry, re-arm or forced unload was used. Keep a normal boot/recovery path;
persistent enablement was not tested.
