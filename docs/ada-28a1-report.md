# RTX 4050 Laptop (10de:28a1, HP Victus) Ada canary — transactional unlock, no rate change

On October 31, 2026, the AD107-family four-PLM payload ran on an HP Victus
15-fb3xxx RTX 4050 Laptop (`10de:28a1`, AD107M, 20 SMs, rev a1, subsystem
`103c:8dd0`) before the unmodified NVIDIA `580.178.04` open kernel driver.
Every gate the module enforced passed, and the NVIDIA RM getter changed
FMLA32, IMLA2 and IMLA4 from half to full rate in every unlock boot.
However, **matched SM89 CUDA tensor benchmarks showed no issue-rate
change**: FP16/FP32 accumulation stayed at 25.3–25.5 TF/s in both states,
i.e. 1.00× instead of the 2.00× measured on the other AD107 boards
(`10de:2705`, `10de:2709`, `10de:28e1`).

This report separates the two verdicts that the upstream reports combine:

| Claim | Verdict | Basis |
| :--- | :--- | :--- |
| **Supported transactionally** (payload installs, gates pass, driver binds, system stable) | **DEMONSTRATED** | 3 unlock boots (1 warm, 2 cold EC-reset cycles), zero Xid errors, 18/18 GEMM cases correct |
| **Selector getter flips half→full** | **DEMONSTRATED** | read-only RM getter: `1 1 1` → `0 0 0` on 2 independent cold boots |
| **Performance unlocked (2.00× issue rate)** | **CONTRADICTED on this board/config** | matched pairs: Δ ≤ +0.6% on every tensor mode; per-GHz throughput identical |

This is one physical machine and one driver version. It does not
establish support for, or the absence of a rate change on, other HP
models, other `10de:28a1` boards, or desktop AD107 cards.

> [!WARNING]
> **OEM Laptop Architectural Variance Warning (now measured, not
> hypothetical):** the same AD107M silicon with the same payload yields
> 2.00× on the ASUS TUF A16 (`10de:28e1`, see [ada-28e1](ada-28e1-report.md))
> and 1.00× on this HP Victus (`10de:28a1`). At least one board-, VBIOS-,
> EC- or driver-dependent gate exists between "selector reads full rate"
> and "SMs issue at full rate". Treat the getter flip as a necessary but
> not sufficient condition until the gate is identified.

The [evidence directory](evidence/ada28a1/README.md) contains the
selected boot messages, selector readbacks, benchmark outputs and
telemetry with the GPU UUID replaced.

## Host system specifications

- **Model:** Victus by HP Gaming Laptop 15-fb3xxx, BIOS F.15
- **Processor:** AMD Ryzen 5 8645HS w/ Radeon 760M Graphics (6 cores / 12 threads)
- **dGPU:** NVIDIA Corporation AD107M [GeForce RTX 4050 Max-Q / Mobile]
  (`10de:28a1`, rev a1, subsystem `103c:8dd0` Hewlett-Packard, 20 SMs,
  6141 MiB GDDR6, VBIOS 95.07.40.40.FA)
- **Display routing:** AMD iGPU hybrid mode (no hardware MUX on this chassis)
- **OS / Distribution:** Ubuntu 24.04 LTS (x86_64)
- **Kernel:** `7.0.0-42-generic`
- **NVIDIA Driver:** `nvidia-driver-580-open` / `nvidia-dkms-580-open`
  (`580.178.04`, open kernel module, license `Dual MIT/GPL` verified in
  the packaged `nvidia.ko.zst`)
- **Secure Boot:** disabled

The module was built with the distribution `dh-dkms` packaging
(`gpu-unlock-dkms_1.1.1_all.deb`), installed through DKMS for
`7.0.0-42-generic`, and bundled into the Ubuntu initramfs. The initramfs
copy was verified to contain the patched device table entry
(`10de:28a1`) by decompressing and grepping the image before testing.

The patch adding `10de:28a1` under the existing `ad107_experimental` flag
touches three kernel sites (`arch_610.h`, `main.c` device table,
`unlock_610.c`), the boot tool and three test files; the full test
battery passed (33/33 with `GPU_UNLOCK_ASSETS` populated, 30/30 without,
3 asset-dependent skips). A dedicated GRUB entry (`/etc/grub.d/40_custom`)
carries the exact PCI target and the experimental flag; the stock entry
is preserved and boots by default:

```text
gpu_unlock.targets=0000:01:00.0 gpu_unlock.ad107_experimental=1
```

Cold cycles followed the full discharge protocol: shutdown, AC unplug,
15–25 second power-button hold (HP EC reset), AC reconnect, boot. The
stock baseline was collected after a warm reboot on the stock kernel
command line (no `gpu_unlock` parameters).

## Firmware and driver checks

Three unlock boots are recorded (see `evidence/ada28a1/boots.txt`):

| Boot | Cycle | Transaction result | Selector getter |
| :--- | :--- | :--- | :--- |
| `1ff2699e…` (01:58) | warm reboot | FLR `result=0`, `gpu_unlock: unlocked` | full (`0 0 0`) |
| `267d0cf7…` (03:26) | cold, EC reset | FLR `result=0`, post-FLR GFW `error=0 pci=0x28a110de`, `gpu_unlock: unlocked` | full (`0 0 0`) |
| `7569fe75…` (03:35) | cold, EC reset | FLR `result=0`, post-FLR GFW `error=0 pci=0x28a110de`, `gpu_unlock: unlocked` | full (`0 0 0`) |

In both cold boots the post-FLR register block was identical:
`plm=0x8b8f boot=0x3ff cpuctl=0x10 budget_us=2050000`. The BAR0 VBIOS
aperture at `+0x300000` was read directly from the module without any
MUX or display-mode manipulation — the first proof that aperture
exposure does **not** require an ASUS-style hardware MUX (this chassis
has none). The NVIDIA open module then initialized `nvidia-drm`; the
kernel log contained zero NVRM Xid errors and no PCIe drop-offs in any
boot, stock or unlocked.

The read-only RM getter (`tools/read_sm_rates.py`) reported:

| Field | Stock boots | Unlocked boots |
| :--- | ---: | ---: |
| FMLA32 | 1 (half) | 0 (full) |
| IMLA2 | 1 (half) | 0 (full) |
| IMLA4 | 1 (half) | 0 (full) |
| IMLA0, FMLA16, DP, FFMA, IMLA1, IMLA3 | 0 | 0 |

## Matched benchmark methodology

To make stock and unlocked measurements comparable, every run fixes:

- **Binaries:** `mma_bench`, `validate_gemm`, `gemm_bench` and the
  `tensor_modes_issue_sm89.cubin` driver binary, all SHA-256 hashed per
  run (`binaries_sha256.txt`); the cubin rebuild is byte-identical
  (`33aa6db3…`).
- **Software state:** same kernel, same `580.178.04` open driver, same
  module parameters, git tree `9d42480` clean in every collect.
- **Machine state:** same GPU UUID, same PCI address, stock collects on
  the stock command line, unlock collects on the canary entry; unlock
  collects on cold EC-reset cycles; a same-boot repeat quantifies
  run-to-run variance (< 0.02%).
- **Receipts:** each collect (`build/matched-bench/parity/collect.sh`)
  records environment, cmdline, classifier, selector state, binary
  hashes, numerical validation, benchmarks, telemetry and a verdict
  into `parity/{stock,unlock}/runN/`; negative and contaminated runs
  are preserved, never overwritten.

Excluded from the canonical pair but retained: `stock/run1` (legacy
harness era, incomplete receipts) and `unlock/run1` (warm-boot legacy
collect, used below only as a cross-check).

## Core results

### Matched issue-rate comparison

Best-of-reps, identical binaries and geometry:

| Probe | Stock run2 | Stock run3 | Unlock run2 | Unlock run3 | Δ (unlock − stock) |
| :--- | ---: | ---: | ---: | ---: | ---: |
| tensor `f16_f32` (Top/s) | 25.320 | 25.317 | 25.468 | 25.390 | +0.3 … +0.6% |
| tensor `tf32` (Top/s) | 12.660 | 12.659 | 12.733 | 12.694 | +0.3 … +0.6% |
| tensor `u4_s32` (Top/s) | 201.30 | 201.32 | 202.23 | 201.88 | +0.3 … +0.5% |
| `mma` f16/f32 (TF/s) | 25.5 | 25.3 | 25.5 | 25.5 | ~0% |
| `validate_gemm` failures | 0 (6/6) | 0 (6/6) | 0 (6/6) | 0 (6/6) | — |
| tensor mismatches | 0 | 0 | 0 | 0 | — |

If the full-rate selector state translated into execution rate, these
rows would read ≈50.7 Top/s (2.00×) like `10de:28e1`. They do not. The
sub-percent differences sit inside inter-boot noise (same-boot repeats
differ by < 0.02%, boot-to-boot by up to ~0.6%).

### Clock normalization (rules out power/clock as the explanation)

| Run | tensor `f16_f32` (Top/s) | max SM clock | **Top/s per GHz** |
| :--- | ---: | ---: | ---: |
| stock run2 | 25.320 | 2490 MHz | **10.169** |
| stock run3 | 25.317 | 2490 MHz | **10.168** |
| unlock run2 | 25.468 | 2505 MHz | **10.167** |
| unlock run3 | 25.390 | 2505 MHz | **10.136** |

Throughput per GHz is identical (Δ ≤ 0.3%) and the throttle-reason
histograms match between states (`telemetry_summary.txt`: GPU_IDLE
dominant, a few SW_POWER_CAP samples in both). 20 SMs × 512 FLOP/clk ×
~2.5 GHz = ~25.6 TF/s, exactly the measured value in **both** states:
the card executes the half-rate-per-clock profile whether the getter
reads half or full. The divergence from the other boards is therefore a
real issue-rate gate, not a clock, power or thermal artifact.

### Full instruction fingerprint

`mma_bench` best TF/s per mode:

| Mode | Stock run2 | Stock run3 | Unlock run2 | Unlock run3 | Unlock run1 (warm boot, legacy) |
| :--- | ---: | ---: | ---: | ---: | ---: |
| f16/f16 | 50.9 | 50.9 | 50.9 | 50.9 | 51.2 |
| f16/f32 | 25.5 | 25.3 | 25.5 | 25.5 | 25.6 |
| bf16/f32 | 25.5 | 25.3 | 25.5 | 25.4 | 25.6 |
| s8/s32 | 101.8 | 101.2 | 96.8 | 95.6 | 102.3 |
| fp8/e4m3 | 50.9 | 50.6 | 50.9 | 50.9 | 50.9 |
| dp4a | 25.3 | 24.8 | 22.3 | 22.1 | 25.3 |
| ffma | 11.8 | 11.6 | 10.5 | 10.3 | 12.1 |

The tensor modes that upstream reports identify as rate-limited
(FP16/FP32, TF32) show zero change here, in both directions: the unlock
neither helps nor damages them. The dips in `s8/s32`, `dp4a` and `ffma`
in the first collect after each **cold** unlock boot are a separate,
secondary observation:

- same-boot repetition restored `s8/s32` to 102.3 (≥ stock) and
  partially restored `dp4a` (23.1–23.3) and `ffma` (10.8);
- the **warm** unlock boot (`unlock/run1`) measured stock-level values
  for all three modes (102.3 / 25.3 / 12.1).

They are therefore classified `CORRELATION_ONLY` — a cold-boot
transient of this laptop's power management, **refuted as an
unlock-state effect** (N=2 cold unlock boots vs 1 warm unlock boot and
2 stock collects, with the state-prediction contradicted by run1).

## Comparison with the other AD107 board (`10de:28e1`)

| | `10de:28e1` (ASUS TUF A16) | `10de:28a1` (HP Victus, this report) |
| :--- | :--- | :--- |
| Silicon | AD107M, 20 SMs | AD107M, 20 SMs (same die) |
| Display routing | hardware MUX, dGPU-only mode | no MUX, AMD hybrid |
| Driver | open `615.78.08` | open `580.178.04` |
| OS / kernel | CachyOS, `7.2.10-1-cachyos` | Ubuntu 24.04, `7.0.0-42-generic` |
| BAR0 aperture | exposed after MUX switch + EC reset | exposed with no MUX manipulation |
| Getter half→full | yes | yes |
| FP16/FP32 rate | 26.48 → 52.97 Top/s (**2.00×**) | 25.32 → 25.47 Top/s (**1.00×**) |
| TF32 rate | 13.21 → 26.42 Top/s (**2.00×**) | 12.66 → 12.73 Top/s (**1.00×**) |

The differing variables are: board/OEM (EC behavior, power routing),
VBIOS vendor contents (`Hewlett-Packard` vs `ASUS` subsystem images),
driver version, distribution/kernel, and MUX presence. Which of them
carries the second gate is **NOT_DEMONSTRATED**; the candidates and
their status are the next section's ledger.

## Hypothesis ledger

| ID | Hypothesis | Status |
| :--- | :--- | :--- |
| H1 | VBIOS image differences (vendor-specific GSP/boot config in the HP image) | `NOT_DEMONSTRATED` — read-only VBIOS dump comparison planned |
| H2 | A second EC/firmware gate (volatile state not cleared by the HP EC reset, or an HP-specific fuse/scratch register) | `NOT_DEMONSTRATED` — cold vs warm unlock boots show identical getter and throughput, which argues against a volatile-only gate but does not exclude a sticky one |
| H3 | Driver-version dependence (580 vs the 615 open module used on all rate-change reports) | `NOT_DEMONSTRATED` — testable by installing the 615 open module on this machine |
| H4 | GSP firmware interaction (`NVreg_EnableGpuFirmware=0` was set in both states; no toggle was tested) | `NOT_DEMONSTRATED` |
| H5 | Power/clock/thermal limitation of the rate | **REFUTED** — per-GHz throughput and throttle profiles identical between states |
| H6 | MUX/display routing gates the unlock | **REFUTED for aperture exposure** (worked without a MUX); as an explanation for the missing rate change: `NOT_DEMONSTRATED` |
| H7 | Measurement artifact / wrong cubin / stale driver | **REFUTED** — byte-identical cubin, hashed binaries, identical driver, 0 mismatches, three independent boots |
| H8 | Cold-boot transient explains the main result | **REFUTED** — the 1.00× tensor result reproduces on cold and warm unlock boots alike |
| H9 | `s8/dp4a/ffma` dips are an unlock-state effect | `CORRELATION_ONLY` — contradicted by the warm unlock boot at stock-level values |
| H10 | Board-level fuse config prevents full-rate issue regardless of the PLM payload | `NOT_DEMONSTRATED` |

## NOT_DEMONSTRATED

- The root cause of the missing 2.00× rate change on this board (H1–H4,
  H6, H10 above).
- Sustained-load stability of the unlocked state on this machine
  (long-duration tensor stress, VRAM saturation, NVENC concurrency) —
  planned, not yet run.
- Behavior on other `10de:28a1` systems, other HP models, or desktop
  AD107 cards.
- Whether a future driver (≥ 615) restores the rate change here.

## Reproduction

1. Install the patched DKMS package; verify the initramfs contains the
   device table entry.
2. Add the canary GRUB entry with
   `gpu_unlock.targets=0000:01:00.0 gpu_unlock.ad107_experimental=1`.
3. Cold cycle (AC unplug, 15–25 s power-button hold) and boot the canary
   entry; confirm `gpu_unlock: unlocked` and getter `0 0 0`.
4. Run `build/matched-bench/parity/collect.sh unlock runN`; repeat on a
   stock boot for the matched baseline.
