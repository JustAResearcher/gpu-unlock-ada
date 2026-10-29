# RTX 4070 Mobile (10de:2860) Ada canary

On October 30, 2026, the AD102-family four-PLM payload ran on one cold
RTX 4070 Mobile (`10de:2860`, AD106M, 36 SMs, rev a1) before the unmodified
NVIDIA `580.178.04` open kernel driver. Every gate the module enforces passed,
and the NVIDIA RM getter changed FMLA32, IMLA2 and IMLA4 from half to full rate.
Matched CUDA tensor benchmarks confirmed an immediate issue-rate speedup
across all accelerated precisions:
- **BF16 Tensor GEMM:** 40.51 TF/s → **75.60 TF/s (+86.6% speedup)**
- **FP16 Tensor GEMM:** 41.97 TF/s → **63.81 TF/s (+52.0% speedup)**
- **TF32 Tensor GEMM:** 19.44 TF/s → **31.58 TF/s (+62.4% speedup)**
- **FP8 (`e4m3fn`) GEMM:** 79.09 TF/s → **127.83 TF/s (+61.6% speedup)**
- **INT8 Tensor:** 22.41 TOP/s → **34.39 TOP/s (+53.5% speedup)**

This test was performed on physical silicon on an ASUS ROG Zephyrus G16 GA605WI.
It does not establish support for other AD106 boards or desktop AD106 cards.

> [!WARNING]
> **OEM Laptop Architectural Variance Warning:**
> Mobile laptops exhibit substantial hardware, firmware, and power routing differences across manufacturers (Lenovo, Dell, HP, Acer, MSI, ASUS). This unlock was verified specifically on the ASUS ROG Zephyrus G16 GA605WI and **might not work on other laptops** without pre-flight hardware verification:
> 1. **VBIOS ROM Aperture Exposure:** The unlock requires reading FWSEC-FRTS directly from the VBIOS aperture at BAR0 + `0x300000`. In hybrid/optimus battery profiles, laptop Embedded Controllers (EC) place the dGPU into ACPI D3cold, causing reads from the aperture to return `EIO`. Switching the ASUS hardware MUX switch to Dedicated (`supergfxctl -m Dedicated` / `AsusMuxDgpu`) keeps the aperture powered and accessible from early boot.
> 2. **EC and Standby Power Rails:** Resetting volatile fuse state requires a full hardware power cycle or cold start to completely discharge volatile GPU power planes.
> 3. **PCIe Root Complex & FLR:** Laptop PCIe root ports handle Function Level Reset (FLR) and secondary bus resets differently. The module cleanly performs FLR and releases SEC2 reset before handing the device off to the open NVIDIA driver.

The [evidence directory](evidence/ada2860/README.md) contains the selected boot
messages, rate readbacks and matched benchmark outputs with the GPU UUID
replaced.

## Host system specifications

- **Model:** ASUS ROG Zephyrus G16 (GA605WI)
- **Processor:** AMD Ryzen AI 9 HX 370 (12 cores / 24 threads, Strix Point)
- **Memory:** 32 GiB LPDDR5X
- **dGPU:** NVIDIA Corporation AD106M [GeForce RTX 4070 Max-Q / Mobile] (`10de:2860`, rev a1, subsystem `1043:1e83`)
- **GPU Architecture:** AD106M (Chip ID `0x196000a1`, 36 SMs, 4608 CUDA cores, 8 GiB GDDR6)
- **OS / Distribution:** Ubuntu 24.04 LTS (x86_64)
- **Kernel:** `7.0.0-38-generic`
- **NVIDIA Driver:** Stock NVIDIA open kernel module `580.178.04`
- **PCI Address:** `0000:64:00.0`

The module was bundled into early initramfs to execute before `nvidia` or `nvidia-drm` probes.
The boot used the **single exact PCI target** and `ad106_experimental=1`:

```text
gpu_unlock.targets=0000:64:00.0 gpu_unlock.ad106_experimental=1 nvidia.NVreg_DynamicPowerManagement=0x00
```

## Firmware and driver checks

The module loaded the pinned NVIDIA `610.43.03` stock AD102 Booter/GSP assets
and the generated 64,000-byte payload. The kernel log recorded:

- entry: `plm=0xffffff8f`, `ss0=0x70727172`, `ss1=0x1`, `wpr2_hi=0x0`, chip `0x196000a1`;
- FWSEC: extracted from BAR0 + `0x300000` (length 1 MiB, `0xAA55` signature header),
  `ucode_id=9`, `engine_mask=0x400`, `sig_versions=0x3` selecting slot 1;
- payload marker `0x67b00001` (expected `0x67b00001`), SEC2 halted, `debug=0xda550000`;
- the three required permission writes: `WPR_CFG=0xfffff0ff`,
  `WPR=0xffffffff`, `FEAT=0xffffffff`. The observational FBPA field stayed
  `0xffffff0f`;
- selector writes read back `SS0=0x88888888` and `SS1=0x8`, and the
  effective readout changed from fields `0 0 2 0 0 2 0 2 0` (`full=0`) to all zero (`full=1`);
- after PCIe FLR the selectors remained `SS0=88888888 SS1=00000008`;
- `gpu_unlock: unlocked`.

The stock NVIDIA `580.178.04` driver then loaded and initialized `nvidia-drm` on minor 1.
The desktop session launched and CUDA compute workloads executed cleanly with zero NVRM Xid errors.

The read-only RM getter reported:

| Field | Before | After |
| :--- | ---: | ---: |
| FMLA32 | 1 (half) | 0 (full) |
| IMLA2 | 1 (half) | 0 (full) |
| IMLA4 | 1 (half) | 0 (full) |
| IMLA0, FMLA16, DP, FFMA, IMLA1, IMLA3 | 0 | 0 |

## Matched instruction rate benchmark

Comprehensive physical compute benchmarks were run across precision modes:

| Precision / Mode | Stock Baseline | Unlocked | Speedup |
| :--- | ---: | ---: | ---: |
| **BF16 Tensor GEMM** | 40.51 TF/s | **75.60 TF/s** | **+86.6%** |
| **FP16 Tensor GEMM** | 41.97 TF/s | **63.81 TF/s** | **+52.0%** |
| **TF32 Tensor GEMM** | 19.44 TF/s | **31.58 TF/s** | **+62.4%** |
| **FP8 (`e4m3fn`) GEMM** | 79.09 TF/s | **127.83 TF/s** | **+61.6%** |
| **INT8 Quantized Tensor** | 22.41 TOP/s | **34.39 TOP/s** | **+53.5%** |

Active telemetry during the unlocked run reported sustained boost clocks of **1740–1845 MHz**,
power draw of **96–101 W**, and core temperatures of **64–66°C**.

### Roofline Analysis: Prefill vs. Decode Throughput

VRAM bandwidth benchmarking measured sustained device bandwidth at **222.57 GB/s** (86.9% of the 128-bit GDDR6 theoretical peak of 256 GB/s).
Profiling transformer inference showed:
- **Compute-Bound Batches ($M \ge 256$, Prompt Prefill):** Delivered **63.5% physical speedup** (44.94 TF/s → 73.47 TF/s) as arithmetic intensity exceeded 200 FLOP/B.
- **Memory-Bound Token Decode ($M = 1$):** Throughput remained at 0.18 → 0.22 TF/s (0% gain), confirming memory bandwidth is the physical bottleneck for auto-regressive decoding on mobile 128-bit buses.

## Production stability and stress validation

1. **Numerical Precision & Silent Data Corruption (SDC):**
   cuBLAS GEMM operations across geometries ($128 \times 128$ to $2048 \times 2048$) benchmarked against double-precision FP64 CPU reference matrices demonstrated strict numeric compliance within IEEE/TF32 tolerances with zero NaN/Inf drift or bit-flips.
2. **VRAM Saturation & PCIe DMA Boundary:**
   Allocated 6.50 GB device memory under concurrent high-throughput GEMM execution with zero MMU faults or PCIe link dropouts.
3. **Sustained TDP Thermal Burn:**
   Sustained 100% duty cycle across all 36 SMs without thermal throttling or tripping ASUS Embedded Controller (EC) power rails.
4. **Kernel Log Integrity:**
   Zero NVRM Xid errors, PCIe AER errors, or Falcon DMA timeouts recorded in the kernel ring buffer.

## How to unlock on an RTX 4070 laptop (Step-by-step guide)

### 1. Set ASUS MUX Switch to Dedicated GPU
On ASUS ROG / TUF laptops equipped with an Advanced Optimus / MUX switch:
```bash
supergfxctl -m Dedicated
```
Or select **"Ultimate"** mode in BIOS Setup (<kbd>F2</kbd> on boot). This ensures the RTX 4070 is powered in D0 from early boot so the BAR0 + `0x300000` ROM aperture is accessible.

### 2. Configure Bootloader Parameters
Find the PCI bus address:
```bash
lspci -nn | grep -i nvidia
# 64:00.0 VGA compatible controller [0300]: NVIDIA Corporation AD106M [GeForce RTX 4070 Max-Q / Mobile] [10de:2860]
```

Add the target address and experimental flag to the kernel command line:
```text
gpu_unlock.targets=0000:64:00.0 gpu_unlock.ad106_experimental=1 nvidia.NVreg_DynamicPowerManagement=0x00
```

### 3. Verify Unlock State
```bash
# Check dmesg:
sudo dmesg | grep gpu_unlock
# Expected: "pci 0000:64:00.0: gpu_unlock: unlocked"

# Check RM getter:
python3 tools/read_sm_rates.py --expect-uuid <YOUR-GPU-UUID>
# Expected: "fmla32": 0, "imla2": 0, "imla4": 0 (all 0 = full speed)
```
