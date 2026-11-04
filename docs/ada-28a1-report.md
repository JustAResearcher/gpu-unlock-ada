# RTX 4050 Laptop (10de:28a1, HP Victus) Ada canary — 2.00× tensor unlock and the Optimus D3cold power-rail trap

On October 31 – November 1, 2026, the AD102-family four-PLM payload was tested
on an HP Victus 15-fb3xxx RTX 4050 Laptop GPU (`10de:28a1`, AD107M, 20 SMs,
rev a1, subsystem `103c:8dd0`) with the unmodified NVIDIA `580.178.04` open
kernel driver on Ubuntu 24.04 (`7.0.0-42-generic`).

The investigation proceeded in two phases that together establish both the
**2.00× SM89 tensor issue-rate unlock** on `10de:28a1` and the root cause of
a **silent `D3cold` hardware reset + false-positive RM getter (`0 0 0`) trap**
that affects hybrid/Optimus laptops when runtime power management cuts the
dGPU power rail after boot:

| Claim | Verdict | Basis |
| :--- | :--- | :--- |
| **Supported transactionally** (payload installs, gates pass, driver binds, system stable) | **DEMONSTRATED** | 3 boot unlock cycles + 1 live `D3cold`→`D0` cycle, zero Xid errors, 24/24 `validate_gemm` cases + all `gemm_bench` / tensor checks exact (`failures=0`) |
| **Optimus `D3cold` (`NVreg_DynamicPowerManagement=0x02`) wipes hardware unlock while RM getter still reports `0 0 0` (full rate)** | **DEMONSTRATED** | `gpu_peek` BAR0 readback (`gpu_peek_stock_run4.txt`): after `D3cold` (`LNXPOWER:00=0`, `Video Memory: Off`), `SS0` resets to `0x53123046` and `FEAT_READOUT_1` (`0x00823818`) resets to `0x00410080` (`0 0 2 0 0 2 0 2 0`), while `ioctl 0x20801230` returns cached `0 0 0` (`NVreg_PreserveVideoMemoryAllocations=1`) and benchmarks measure `1.00×` (`run2`, `run3`) |
| **Performance unlocked (2.00× issue rate) when `D3cold` is inhibited (`run4_nod3cold`)** | **DEMONSTRATED** | With `power/control=on` + `NVreg_DynamicPowerManagement=0x00`: `SS0=0x88888888` and `FEAT_READOUT_1=0x00000000` persist during active CUDA (`dmesg_unlock_run4_nod3cold.txt`); tensor `f16_f32` **25.320 → 50.682 Top/s (2.002×)**, `tf32` **12.660 → 25.281 Top/s (1.997×)**, `mma_bench` `f16/f32` & `bf16/f32` **25.5 → 50.9 TF/s (1.996×)**, `fp8/e4m3` **50.9 → 101.4 TF/s (1.992×)**, cuBLAS $4096^3$ GEMM median **+35.9% FP16 / +48.3% BF16** (best-to-best +36.2% / +61.6%) |
| **Live no-reboot cold reset via unbound `D3cold` → `D0` transition** | **DEMONSTRATED** | Cycling `0000:01:00.0` through ACPI `D3cold` (`LNXPOWER:00` 0→1) while unbound clears `WPR2_HI` to `0x00000000`, allowing `gpu_unlock` to run in a live session without rebooting or EC reset |

> [!IMPORTANT]
> **Optimus / Hybrid Laptop `D3cold` Warning (now measured at the BAR0 register
> level):** On laptops where the desktop session runs on the iGPU (`amdgpu` or
> `i915`/`xe` with `nvidia_drm.modeset=0`), `NVreg_DynamicPowerManagement=0x02`
> places the idle NVIDIA dGPU into runtime `D3cold` (`Video Memory: Off`) a few
> seconds after `nvidia.ko` finishes probing. `D3cold` removes the physical GPU
> power rail, wiping the volatile unlock registers (`0x00823804`, `0x0082381c`,
> `0x00823820`) back to factory half-rate fuses. Because
> `NVreg_PreserveVideoMemoryAllocations=1` restores GSP-RM's cached `OBJGR`
> software state on resume without re-sampling `NV_PFUSE_FEATURE_READOUT_1`
> (`0x00823818`), `tools/read_sm_rates.py` (`0x20801230`) continues reporting
> `0 0 0` (full rate) while the hardware executes at half rate (`1.00×`).
> Keeping the dGPU out of `D3cold` (`NVreg_DynamicPowerManagement=0x00` or
> `0x01`, or locking `/sys/bus/pci/devices/<bdf>/power/control` to `on`) is
> required to retain the unlock across idle periods. This same mechanism likely
> explains the `0 0 0` getter / `1.00×` throughput split previously observed on
> the hybrid-mode Clevo RTX 4090 Laptop (`10de:2757`, see
> [ada-2757-report.md](ada-2757-report.md)).

The [evidence directory](evidence/ada28a1/README.md) contains both the
`D3cold`-wiped negative-control runs (`run2`, `run3`) and the `D3cold`-inhibited
`2.00×` unlocked run (`run4_nod3cold`), including full 63-register BAR0
`gpu_peek` dumps before, during, and after CUDA execution.

## Host system specifications

- **Model:** Victus by HP Gaming Laptop 15-fb3xxx, BIOS F.15
- **Processor:** AMD Ryzen 5 8645HS w/ Radeon 760M Graphics (6 cores / 12 threads)
- **dGPU:** NVIDIA Corporation AD107M [GeForce RTX 4050 Max-Q / Mobile]
  (`10de:28a1`, rev a1, subsystem `103c:8dd0` Hewlett-Packard, 20 SMs,
  6141 MiB GDDR6, VBIOS 95.07.40.40.FA, chip ID `0x197000a1`)
- **Display routing:** AMD iGPU (`0000:06:00.0`) drives the display in hybrid
  mode (no hardware MUX on this chassis; `nvidia_drm.modeset=0`)
- **ACPI dGPU power resource:** `/sys/bus/pci/devices/0000:00:01.1/firmware_node/power_resources_D0/LNXPOWER:00`
- **OS / Distribution:** Ubuntu 24.04 LTS (x86_64)
- **Kernel:** `7.0.0-42-generic`
- **NVIDIA Driver:** `nvidia-driver-580-open` / `nvidia-dkms-580-open`
  (`580.178.04`, open kernel module, license `Dual MIT/GPL`)
- **Default distro modprobe config (`/etc/modprobe.d/nvidia-kms.conf`):**
  `NVreg_DynamicPowerManagement=0x02`, `NVreg_PreserveVideoMemoryAllocations=1`,
  `NVreg_EnableGpuFirmware=0` (ignored by the open kernel module, which always
  runs GSP-RM)
- **Secure Boot:** disabled

The patch admitting `10de:28a1` under `ad107_experimental` touches
`kernel/gpu_unlock/arch_610.h`, `kernel/gpu_unlock/main.c`,
`kernel/gpu_unlock/unlock_610.c`, `tools/print_boot_option.py`, and the three C
test suites (`33/33` tests pass with `GPU_UNLOCK_ASSETS` populated).

## Root cause of the Phase 1 (`1.00×`) negative control: `D3cold` vs cached GSP-RM state

In Phase 1 (`unlock/run2` and `unlock/run3`, see `boots.txt`), the machine
booted with `gpu_unlock.targets=0000:01:00.0 gpu_unlock.ad107_experimental=1`
in the initramfs. Every firmware and permission gate succeeded:

| Cycle | Transaction result | Post-boot RM getter (`0x20801230`) | Benchmarks (`run2`, `run3`) |
| :--- | :--- | :--- | :--- |
| `1ff2699e…` (warm boot) | FLR `result=0`, `gpu_unlock: unlocked` | `0 0 0` (full) | `25.6 TF/s` (`1.00×`) |
| `267d0cf7…` (cold, EC reset) | FLR `result=0`, post-FLR GFW `error=0 pci=0x28a110de`, `gpu_unlock: unlocked` | `0 0 0` (full) | `25.468 Top/s` (`1.00×`) |
| `7569fe75…` (cold, EC reset) | FLR `result=0`, post-FLR GFW `error=0 pci=0x28a110de`, `gpu_unlock: unlocked` | `0 0 0` (full) | `25.390 Top/s` (`1.00×`) |

To determine why `tools/read_sm_rates.py` reported `0 0 0` while the SMs issued
at half rate, an extended read-only `gpu_peek.ko` was loaded alongside
`nvidia.ko` (`gpu_peek_stock_run4.txt`, `dmesg_unlock_run4_nod3cold.txt`) and
correlated with `/proc/driver/nvidia/gpus/0000:01:00.0/power` and the PCIe root
port (`0000:00:01.1`) ACPI power resource `LNXPOWER:00`:

1. **At boot (`initramfs`):** `gpu_unlock` runs on the cold dGPU, elevates
   `NV_PFUSE_FEATURE_OVERRIDE_PLM` (`0x00823804`) from `0xffffff8f` to
   `0xffffffff`, writes `SS0 = 0x88888888` (`0x0082381c`) and `SS1 = 0x8`
   (`0x00823820`), verifies `NV_PFUSE_FEATURE_READOUT_1` (`0x00823818`) =
   `0x00000000` (`fields=0 0 0 0 0 0 0 0 0`, `full=1`) before and after PCIe
   FLR, and hands off the card.
2. **Initial `nvidia.ko` probe:** `nvidia.ko` initializes GSP-RM while the card
   is still in `D0`. GSP-RM samples `0x00823818` once during `OBJGR` construction
   and caches `fmla32=0, imla2=0, imla4=0` in its driver state structure.
3. **Desktop idle → Runtime `D3cold`:** Because Xorg/GNOME runs entirely on the
   AMD Radeon 760M iGPU (`0000:06:00.0`) and `nvidia_drm.modeset=0` is set, no
   process holds `/dev/nvidia0` open after login. Under Ubuntu's default
   `NVreg_DynamicPowerManagement=0x02` (`Fine-Grained`), `nvidia.ko` suspends
   the dGPU into `D3cold` (`Runtime D3 status: Enabled (fine-grained)`,
   `Video Memory: Off`), and ACPI turns `LNXPOWER:00` **off** (`in_use = 0`).
4. **Wake for benchmark (`D3cold` → `D0`):** Launching `read_sm_rates.py` or a
   CUDA binary wakes `0000:01:00.0` from cold power-off:
   - The physical silicon re-latches its factory fuses (`FUSE_SS_FMLA32=1`,
     `FUSE_SS_IMLA2=1`, `FUSE_SS_IMLA4=1`), resetting `FEAT_OVR_PLM` to
     `0xffffff8f`, `SS0` to `0x53123046`, `SS1` to `0x3`, and
     `FEAT_READOUT_1` (`0x00823818`) to `0x00410080` (`0 0 2 0 0 2 0 2 0` —
     half rate on FMLA32, IMLA2, IMLA4).
   - Meanwhile, `NVreg_PreserveVideoMemoryAllocations=1` restores GSP-RM's
     saved software state from step 2 without re-running `gpu_unlock` or
     re-sampling `0x00823818` for the RM control call. Consequently,
     `NV2080_CTRL_CMD_GR_GET_SM_ISSUE_RATE_MODIFIER` (`0x20801230`) continues
     reporting `0 0 0` (full rate) while the hardware SMs execute at `1.00×`.

## BAR0 register verification across states (`gpu_peek`)

Full 63-register BAR0 snapshots (`gpu_peek_stock_run4.txt` and
`dmesg_unlock_run4_nod3cold.txt`) confirm the exact hardware state across four
regimes:

1. **After `D3cold` wake (stock / `D3cold`-wiped, `driver=nvidia`, active CUDA P0 at 2505 MHz)**
2. **Cold `D0` after unbound `D3cold` → `D0` wake (`driver=none`, before `gpu_unlock`)**
3. **Immediately after `gpu_unlock` (`driver=none`, post-FLR)**
4. **Unlocked with `D3cold` blocked (`driver=nvidia`, `DynamicPowerManagement=0`, active CUDA compute)**

| BAR0 Register | Name / Function | 1. Post-`D3cold` (`1.00×` CUDA) | 2. Cold `D0` pre-unlock (`driver=none`) | 3. Post-`gpu_unlock` (`driver=none`) | 4. Unlocked + `D3cold` blocked (`2.00×` CUDA) |
| :--- | :--- | :--- | :--- | :--- | :--- |
| `0x00000000` | `PMC_BOOT_0` (chip ID) | `0x197000a1` | `0x197000a1` | `0x197000a1` | `0x197000a1` |
| `0x001fa824` | `WPR2_LO` | `0x01733000` | `0x1ffffe00` | `0x1ffffe00` | `0x01733000` |
| `0x001fa828` | `WPR2_HI` | `0x017fbe00` | **`0x00000000`** | **`0x00000000`** | `0x017fbe00` |
| `0x008207d8` | `FUSE_SS_FMLA32` (raw fuse) | `0x00000001` | `0x00000001` | `0x00000001` | `0x00000001` |
| `0x008207e4` | `FUSE_SS_IMLA2` (raw fuse) | `0x00000001` | `0x00000001` | `0x00000001` | `0x00000001` |
| `0x008207ec` | `FUSE_SS_IMLA4` (raw fuse) | `0x00000001` | `0x00000001` | `0x00000001` | `0x00000001` |
| `0x00823804` | `FEAT_OVR_PLM` | `0xffffff8f` | `0xffffff8f` | **`0xffffffff`** | **`0xffffffff`** |
| `0x00823818` | `FEAT_READOUT_1` (effective SS) | `0x00410080` (`0 0 2 0 0 2 0 2 0`) | `0x00410080` (`0 0 2 0 0 2 0 2 0`) | **`0x00000000` (`0 0 0 0 0 0 0 0 0`)** | **`0x00000000` (`0 0 0 0 0 0 0 0 0`)** |
| `0x0082381c` | `FEAT_OVR_SM_SPEED_SELECT` (`SS0`) | `0x53123046` | `0x53323046` | **`0x88888888`** | **`0x88888888`** |
| `0x00823820` | `FEAT_OVR_SM_SPEED_SELECT_1` (`SS1`) | `0x00000003` | `0x00000003` | **`0x00000008`** | **`0x00000008`** |
| `0x00504200` | `GPC0_TPC0_SM_0x504200` | `0x00010000` (active) | `0x00090000` (idle) | `0x00090000` (idle) | `0x00010000` (active) |
| `0x00504204` | `SM_ISSUE_RATE_MODIFIER` (TPC0) | `0x00000005` | `0x00000005` | `0x00000005` | `0x00000005` |
| `0x00504a04` | `SM_ISSUE_RATE_MODIFIER` (TPC1) | `0x00000005` | `0x00000005` | `0x00000005` | `0x00000005` |
| `0x00419e04` | `SM_ISSUE_RATE_MODIFIER` (BCAST) | `0x8004bd10` (active) | `0x00000000` (idle) | `0x00000000` (idle) | `0x8004bd10` (active) |

Key hardware observations from this table:

1. **Unbound `D3cold` → `D0` produces a virgin cold GPU state (`Column 2`):**
   Waking `0000:01:00.0` from `D3cold` while no driver is bound yields
   `WPR2_LO = 0x1ffffe00`, `WPR2_HI = 0x00000000`, `GFW PLM = 0x8b8f`,
   `GFW boot = 0x3ff`, `CPUCTL = 0x10`, and `SS0 = 0x53323046` — identical to a
   cold AC-off / EC-reset boot.
2. **GSP-RM alters nibble 5 of `SS0` on stock/reset init (`0x53323046` → `0x53123046`),
   but does NOT overwrite `0x88888888` when `gpu_unlock` has run and `D3cold` is
   blocked (`Column 4`):** When `FEAT_OVR_PLM` is `0xffffffff` and `SS0`/`SS1`
   are `0x88888888` / `0x8`, `nvidia.ko` leaves `SS0`, `SS1`, and
   `FEAT_READOUT_1 = 0x00000000` completely intact during driver initialization
   and active CUDA compute, provided the GPU never enters `D3cold`.

## Core benchmark results: Stock vs `D3cold`-wiped (`run2`/`run3`) vs Unlocked (`run4_nod3cold`)

All runs used the exact same compiled binaries (`binaries_sha256.txt`) and
`tensor_modes_issue_sm89.cubin` (`sha256: 33aa6db385aa9eb8a1f73bb2f74891aea84794326b7c40992bc642f93a3c614d`)
on the same kernel (`7.0.0-42-generic`) and open driver (`580.178.04`).

### 1. SM89 Tensor-mode driver probe (`tensor_modes_driver_runner.py`, `iters=20000, reps=3`)

| Mode | Stock `run2` | Stock `run3` | Unlock `run2` (`D3cold`-wiped) | Unlock `run3` (`D3cold`-wiped) | **Unlock `run4_nod3cold` (`D3cold` blocked)** | **Speedup (`run4` / Stock `run2`)** |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: |
| **`f16_f32` best (Top/s)** | 25.320 | 25.317 | 25.468 | 25.390 | **50.682** | **2.002× (+100.2%)** |
| `f16_f32` best time (ms) | 218.66 | 218.69 | 217.39 | 218.06 | **109.24** | **0.500×** |
| **`tf32` best (Top/s)** | 12.660 | 12.659 | 12.733 | 12.694 | **25.281** | **1.997× (+99.7%)** |
| `tf32` best time (ms) | 218.66 | 218.68 | 217.41 | 218.08 | **109.50** | **0.501×** |
| `u4_s32` control (Top/s) | 201.30 | 201.32 | 202.23 | 201.88 | **201.84** | **1.003× (+0.3%)** |
| `u4_s32` best time (ms) | 110.02 | 110.00 | 109.51 | 109.70 | **109.72** | **0.997×** |
| Output mismatches (`270,336` cells/mode) | 0 | 0 | 0 | 0 | **0** | — |

At ~2505 MHz across 20 SMs, `50.682 Top/s` corresponds to
$\frac{50.682 \times 10^{12}}{20 \times 2.505 \times 10^9} = 1011.6\text{ FLOP/clk/SM}$
(full hardware rate of $1024\text{ FLOP/clk/SM}$), doubling from the stock half
rate of $505.4\text{ FLOP/clk/SM}$ (`25.320 Top/s`).

### 2. Full instruction fingerprint (`mma_bench 20000 5`, best TF/s)

| Mode | Stock `run2` | Stock `run3` | Unlock `run2` (`D3cold`-wiped) | Unlock `run3` (`D3cold`-wiped) | **Unlock `run4_nod3cold` (`D3cold` blocked)** | **Speedup (`run4` / Stock `run2`)** |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: |
| `f16/f16` (control) | 50.9 | 50.9 | 50.9 | 50.9 | **50.9** (`50.9/50.9/50.9/50.9/50.9`) | **1.000×** |
| **`f16/f32`** | 25.5 | 25.3 | 25.5 | 25.5 | **50.9** (`50.9/50.9/50.9/50.9/50.9`) | **1.996×** |
| **`bf16/f32`** | 25.5 | 25.3 | 25.5 | 25.4 | **50.9** (`50.9/50.9/50.9/50.9/50.9`) | **1.996×** |
| `s8/s32` (control) | 101.8 | 101.2 | 96.8 | 95.6 | **101.8** (`101.8/101.8/101.8/101.8/101.8`) | **1.000×** |
| **`fp8/e4m3`** | 50.9 | 50.6 | 50.9 | 50.9 | **101.4** (`101.4/101.4/101.4/101.4/101.4`) | **1.992×** |
| `dp4a` (control) | 25.3 | 24.8 | 22.3 | 22.1 | **25.2** (`25.2/25.2/25.1/25.1/25.1`) | **0.996×** |
| `ffma` (control) | 11.8 | 11.6 | 10.5 | 10.3 | **11.8** (`11.8/11.8/11.8/11.8/11.8`) | **1.000×** |

Every FP32-accumulate tensor mode gated by `FMLA32` / `IMLA4` (`f16/f32`,
`bf16/f32`, and `fp8/e4m3`) doubles to match its full-rate counterpart
(`50.9 TF/s` for FP16/BF16 and `101.4 TF/s` for FP8 E4M3), while all four
non-gated controls (`f16/f16`, `s8/s32`, `dp4a`, `ffma`) remain at `1.00×`.

### 3. cuBLAS large matrix multiplication (`gemm_bench`, 5 reps, 10 warmups, 20 iterations)

| Shape ($M=N=K$) | Mode | Stock `run2` median / best (TF/s) | Stock `run3` median / best (TF/s) | Unlock `run2` (`D3cold`-wiped) median / best | Unlock `run3` (`D3cold`-wiped) median / best | **Unlock `run4_nod3cold` median / best (TF/s)** | **Δ median / Δ best (`run4` vs Stock `run2`)** | Wiped `run2` median vs Stock `run2` | Failures |
| :--- | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| $1024^3$ | FP16/FP32 | 20.65 / 20.69 | 20.18 / 20.22 | 20.37 / 20.44 | 20.53 / 20.64 | **23.13 / 23.34** | **+12.0% / +12.8%** | -1.4% | 0 |
| $1024^3$ | BF16/FP32 | 20.57 / 20.63 | 20.10 / 20.31 | 20.51 / 20.55 | 20.38 / 20.57 | **23.23 / 23.52** | **+12.9% / +14.0%** | -0.3% | 0 |
| $4096^3$ | FP16/FP32 | 22.47 / 22.55 | 21.99 / 22.03 | 18.74 / 18.85 | 18.50 / 18.55 | **30.54 / 30.72** | **+35.9% / +36.2%** | -16.6% | 0 |
| $4096^3$ | BF16/FP32 | 21.48 / 21.50 | 21.05 / 21.13 | 17.39 / 17.41 | 17.26 / 17.29 | **31.85 / 34.75** | **+48.3% / +61.6%** | -19.0% | 0 |
| $8192^3$ | FP16/FP32 | 20.42 / 20.48 | 19.85 / 20.03 | 17.62 / 17.72 | 17.46 / 17.48 | **27.69 / 27.92** | **+35.6% / +36.3%** | -13.7% | 0 |
| $8192^3$ | BF16/FP32 | 19.76 / 19.84 | 19.49 / 19.54 | 17.28 / 17.35 | 17.12 / 17.21 | **27.35 / 27.44** | **+38.4% / +38.3%** | -12.6% | 0 |

> **Correction (2026-11-03):** an earlier version of this table showed values for Stock `run3`
> and Unlock `run2` that did not match `gemm_stock_run3.jsonl` and `gemm_unlock_run2.jsonl`.
> Every cell above is now generated directly from the five `gemm_*.jsonl` files (all `failures=0`);
> the Stock `run2` and `run4_nod3cold` columns, and therefore the headline speedups, were already correct.
> The raw files show the `D3cold`-wiped runs were **not** neutral for cuBLAS: at $4096^3$ and $8192^3$
> they ran **13–19 % below stock** (`run2`), while the tensor probes stayed at `1.00×` and the non-gated
> `mma_bench` controls (`s8/s32`, `dp4a`, `ffma`) were also 5–12 % lower. The cause of that slowdown
> (clocks, thermals or power state after the rail cut) was not measured and is `NOT_DEMONSTRATED`.

### 4. Seeded numerical validation (`validate_gemm`)

All 6 seeded FP16/FP32 and BF16/FP32 GEMM geometries (`256×256×256`,
`64×96×128`, `127×65×95`) matched the CPU FP64 reference with `failures=0` and
`max_abs=0` across all stock and unlocked runs (`numerical_unlock_run4_nod3cold.txt`).

## Updated hypothesis ledger

| ID | Hypothesis | Final Status |
| :--- | :--- | :--- |
| **H11 (Root cause)** | **Runtime `D3cold` (`NVreg_DynamicPowerManagement=0x02`) cuts the dGPU power rail (`LNXPOWER:00=0`) when the desktop runs on the iGPU, resetting `SS0`/`SS1`/`FEAT_OVR_PLM` to half-rate fuses on wake while `NVreg_PreserveVideoMemoryAllocations=1` preserves GSP-RM's cached `0 0 0` getter value** | **DEMONSTRATED** — directly observed via BAR0 `gpu_peek` (`0x00823818 = 0x00410080` after `D3cold` vs `0x00000000` with `D3cold` blocked) and confirmed by the immediate jump from `1.00×` to `2.00×` upon inhibiting `D3cold` |
| H1 | VBIOS image differences in the HP `10de:28a1` image block the rate change | **REFUTED** — unmodified HP VBIOS `95.07.40.40.FA` yields `2.00×` once `D3cold` is prevented |
| H2 | A second HP EC/firmware gate blocks full-rate execution | **REFUTED** |
| H3 | Driver-version dependence (`580.178.04` vs `615.x`) prevents full-rate execution on AD107M | **REFUTED** — `580.178.04` delivers `2.002×` on `10de:28a1` |
| H4 | GSP firmware overrides `SS0`/`SS1` during CUDA execution | **REFUTED** — `gpu_peek` during active CUDA compute confirms GSP-RM leaves `SS0=0x88888888`, `SS1=0x8`, and `FEAT_READOUT_1=0x00000000` untouched |
| H5 | Power/clock/thermal limitation of the rate | **REFUTED** |
| H6 | Hardware MUX is required for BAR0 VBIOS exposure or full-rate issue | **REFUTED** — both VBIOS PROM exposure (`+0x300000`) and `2.00×` tensor execution work on this MUX-less AMD hybrid chassis |
| H10 | Board-level fuse configuration prevents full-rate issue regardless of the PLM payload | **REFUTED** |

## Reproduction on `10de:28a1` (and other Optimus / hybrid laptops)

### Method A: Persistent boot-time unlock (recommended)

1. Install the package with `10de:28a1` enabled and ensure `gpu_unlock` is in
   the initramfs.
2. Prevent runtime `D3cold` from cutting the dGPU power rail after boot. On
   Ubuntu/Debian (`/etc/modprobe.d/nvidia-kms.conf` or a drop-in such as
   `/etc/modprobe.d/nvidia-gpu-unlock-nod3cold.conf`), override
   `NVreg_DynamicPowerManagement`:
   ```text
   options nvidia NVreg_DynamicPowerManagement=0x00
   ```
   *(Note: Rely on `NVreg_DynamicPowerManagement=0x00` rather than forcing PCI
   `power/control="on"` via udev. `DynamicPowerManagement=0x00` disables runtime
   D3cold inside the NVIDIA driver while still allowing GSP-RM to clock down to
   P8 idle [210 MHz / ~1.6 W / silent fans]. Forcing `power/control="on"` at the
   PCI sysfs level locks GSP-RM in P3 [1755 MHz / ~9.7 W] and causes
   `GPS ACPI DSM` / `NV_ERR_NO_MEMORY` warning logs whenever NVML clients open
   `/dev/nvidia0`.)*
3. Rebuild initramfs (`sudo update-initramfs -u`) and boot from a cold power-off
   (or after an unbound `D3cold` cycle) with:
   ```text
   gpu_unlock.targets=0000:01:00.0 gpu_unlock.ad107_experimental=1
   ```
4. Verify both `journalctl -k -b | grep "gpu_unlock: unlocked"` and actual
   tensor throughput (`mma_bench` or `tensor_modes_driver_runner.py`). Do **not**
   rely on `tools/read_sm_rates.py` alone on Optimus laptops, as `0x20801230`
   reflects GSP-RM's cached boot-time readout even if a subsequent `D3cold`
   cycle reset the hardware registers.

### Method B: Live no-reboot unlock via ACPI `D3cold` → `D0` cold rail reset

On Optimus laptops where the display server runs on the iGPU (`nvidia_drm`
refcount `0`) and the PCIe root port exposes an ACPI power resource
(`LNXPOWER:00`), a cold hardware reset (`WPR2_HI=0x0`) can be achieved without
rebooting:

1. Ensure no process holds `/dev/nvidia*` open, unbind `0000:01:00.0` from
   `nvidia`, and allow `/sys/bus/pci/devices/0000:01:00.0/power/control = auto`
   so the unbound device enters `D3cold` (`cat /sys/bus/pci/devices/0000:01:00.0/power_state` → `D3cold`, `LNXPOWER:00/in_use` → `0`).
2. Unload the idle `nvidia_drm`, `nvidia_modeset`, `nvidia_uvm`, and `nvidia`
   modules.
3. Write `on` to `/sys/bus/pci/devices/0000:01:00.0/power/control` to wake the
   dGPU to a fresh cold `D0` state (`WPR2_HI=0x00000000`) and lock `D3cold` off.
4. Run `modprobe gpu_unlock targets=0000:01:00.0 ad107_experimental=1`, confirm
   `gpu_unlock: unlocked` in `dmesg`, then `rmmod gpu_unlock`.
5. Load the NVIDIA driver with `D3cold` disabled:
   `modprobe nvidia NVreg_DynamicPowerManagement=0x00 && modprobe nvidia_uvm && modprobe nvidia_modeset && modprobe nvidia_drm`.
