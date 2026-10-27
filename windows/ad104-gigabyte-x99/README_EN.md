**English version** | [Русская версия](README.md)

---

# Experimental RTX 4070 Ti Windows Port — Gigabyte / Huananzhi X99

A manual UEFI port for the **Gigabyte GeForce RTX 4070 Ti (AD104-400 / Subsystem 1458:40e8)** on **Huananzhi X99-F8D Plus (Intel C612 / X99, AMI Aptio IV/V UEFI BIOS)**, based on [JustAResearcher/gpu-unlock-ada](https://github.com/JustAResearcher/gpu-unlock-ada) and [impacifist/Windows-GPU-UNLOCK](https://github.com/impacifist/Windows-GPU-UNLOCK). It executes before the unmodified Windows NVIDIA driver loads.

## Performance Comparison: Stock vs. UEFI Unlock (cuBLAS GEMM)
 
Measured on physical hardware under Windows 11 x64 with unmodified official NVIDIA driver 616.92 using PyTorch cuBLAS:

| Workload / Precision | Stock Windows (No Unlock) | Unlocked UEFI Loader | Relative Gain / Impact |
|---|---:|---:|:---:|
| **BF16 Tensor Cores (4096² GEMM)** | **73.19 TFLOP/s** (37.2 W) | **143.76 TFLOP/s** (284.9 W) | **+96.4% (~2x Throughput)** 🔥 |
| **BF16 Tensor Cores (16384² Peak)** | **80.85 TFLOP/s** (240.4 W) | **~145+ TFLOP/s** (284.9 W) | **+79.3% Peak Gain** |
| **FP16 Tensor Cores (4096² GEMM)** | **73.58 TFLOP/s** (220.2 W) | **140.10 TFLOP/s** (278.0 W) | **+90.4% Gain** |
| **FP16 Tensor Cores (16384² Peak)** | **80.67 TFLOP/s** (256.5 W) | **~142+ TFLOP/s** (278.0 W) | **+76.1% Peak Gain** |
| **FP32 CUDA Cores (Dense Vector)** | **26.82 TFLOP/s** (283.7 W) | **27.29 TFLOP/s** (~185 W) | **Parity** (scalar ALUs unthrottled) |

> [!NOTE]
> **Silicon & Firmware Mechanics**:  
> In stock Windows, the official GeForce driver imposes an artificial duty-cycle throttle on Tensor operations (under 4096² GEMM, power draw drops to just 37.2 W due to artificial pipeline starvation).  
> Standard OS-level tuning (MSI Afterburner, undervolting curves, or core clock offsets up to 2775 MHz) **cannot bypass this cap**, remaining hard-pinned at ~80.8 TFLOP/s.  
> The `AD104-EXPERIMENTAL.EFI` pre-boot handshake successfully unbinds the hardware scheduler, allowing Tensor Cores to run at full physical capability (**143.76 TFLOP/s**) while saturating the entire 285 W board power budget (**284.9 W**).

---

## Exact Profile and Platform Constraints

- **GPU**: Gigabyte GeForce RTX 4070 Ti (AD104-400, PCI ID `10de:2782`, Subsystem `1458:40e8`, Stepping A1)
- **VBIOS**: Version `95.04.3c.c0.d2`
  - Payload Size: 626,176 bytes (`0x98e00`)
  - Payload SHA-256: `35a6c1ad3225b1266cfacaf6ea014f137f32c54888eb676e230a33a8ac9d19c2`
  - Padded MMIO Aperture (1 MiB): `4891c171e39da7839677b115c7acd6df77720a53b23c7d8ad5226d2c6f42ba05`
- **Host Motherboard**: Huananzhi X99-F8D Plus (Dual Socket LGA2011-3, Intel Wellsburg C612 / X99 PCH, AMI Aptio IV/V UEFI BIOS)
- **BAR0 Base**: `0xc6000000ULL` (16 MiB aperture)
- **Subsystem Latch Targets**: Entry `SS0=0x41145131` / `0x60145131`, `SS1=0x4` $\rightarrow$ Unlocked Latch `SS0=0x88888888`, `SS1=0x8`
- **Operating System**: Windows 11 x64 (Driver 616.92)

ROM, FWSEC signature versions, asset hashes, loaded graphics driver code, device paths, PCI/BARs, cold registers and memory geometry are pinned. A PCI ID match alone is insufficient. Do not remove guards to make another system boot. This application writes GPU registers and deliberately halts on failure after display release.

---

## Dynamic GOP Controller Detachment Architecture

Unlike single-board reference implementations that rely on fixed handle topologies or hardcoded USB node enumeration:
1. **Dynamic Driver Discovery**: `x99_release.c` locates the active GOP driver binding dynamically via `OpenProtocolInformation()` on `EFI_PCI_IO_PROTOCOL_GUID`.
2. **Safe Controller Disconnect**: Disconnects the console driver (`gBS->DisconnectController`) without corrupting unrelated platform devices or PCIe root ports.
3. **Exclusive Protocol Ownership**: Acquires `EFI_OPEN_PROTOCOL_EXCLUSIVE` on the GPU handle to guarantee mutual exclusion during Falcon/SEC2 firmware injection and GSP init.
4. **Decode Restoration**: Restores Bus Master, Memory, and I/O decode bits in the PCI Command register (`0x04`) if cleared by the UEFI driver stack during disconnection.
5. **Clean Release**: Upon successful GSP transaction, FLR verification, and latch readback, `CloseProtocol()` releases exclusive control so the Windows kernel graphics subsystem can attach cleanly without driver crash or Code 43.

---

## Building

### Prerequisites
- Windows x64 host
- GCC / MinGW-w64 toolchain with `ar` and `ld` (e.g., `w64devkit` or MSVC Build Tools)
- Python 3.10+

### Option A: Using the Standalone Python Builder (Recommended)
```powershell
# In windows/ad104-gigabyte-x99:
python build.py

# Optional: build and deploy directly to a USB drive:
python build.py --deploy G:
```

### Option B: Using PowerShell Scripts
```powershell
# Create ignored local header with your ESP partition GUID
New-Item -ItemType Directory -Force local
Copy-Item boot_profile.example.h local/boot_profile.h
# Edit local/boot_profile.h with your actual Windows ESP partition GUID

# Build using GNU ld:
.\Build-Experimental.ps1 -UseLd

# Or build using MSVC link if Visual Studio tools are installed:
.\Build-Experimental.ps1 -Python python
```

Output binary: `build/AD104-EXPERIMENTAL.EFI`.
The build automatically verifies 64-bit EFI headers, zero imports/TLS/CLR, per-function stack limits ($\le 4096$ bytes), and runs `verify_experimental.py`.

---

## Firmware Assets and USB Staging

This repository does **not** distribute proprietary NVIDIA firmware binaries. Generate the required AD102 payload assets locally:
```sh
# Required assets placed in AD104-ASSETS/ on your FAT32 USB drive:
# - booter_load_ad102.bin
# - v67-ad102-610.43.03-four-plm.bin
# - gsp_ga10x.bin
# - gsp_boot_ad102.bin
# - gsp_boot_desc_ad102.bin
```

Format a FAT32 USB drive and stage the files:
```text
USB_ROOT/
├── EFI/
│   └── BOOT/
│       └── BOOTX64.EFI          <-- built AD104-EXPERIMENTAL.EFI
└── AD104-ASSETS/
    ├── booter_load_ad102.bin
    ├── v67-ad102-610.43.03-four-plm.bin
    ├── gsp_ga10x.bin
    ├── gsp_boot_ad102.bin
    └── gsp_boot_desc_ad102.bin
```

---

## Boot Procedure
 
> [!IMPORTANT]
> **Strict Cold Boot Required!**  
> Do not use BIOS `Boot Override` from the graphical setup utility — doing so pre-initializes the GPU and activates PCI Bus Mastering, causing the safety preconditions in `checks.c` to reject the unlock transaction.

**Recommended Procedure:**
1. Disable **Secure Boot** in your UEFI BIOS.
2. In the **`Boot`** tab, set your USB flash drive as **Boot Option #1** and save settings (`F10`).
3. **Perform a complete cold shutdown** (switch off the PSU or disconnect power cord for 10 seconds to allow capacitor discharge and guarantee hardware cold reset).
4. Power on the PC — the system will automatically cold-boot directly into `BOOTX64.EFI` without keypresses.
5. The loader menu appears. Press **`U`** once to authorize the one-shot unlock transaction.  
   *(Pressing `S`, `Esc`, or waiting 30 seconds boots stock Windows without touching GPU state).*
6. The display will freeze for 2-3 seconds during GOP detachment and GSP handover, followed by standard Windows 11 boot.
7. The complete trace log is written to `AD104-UNLOCK/RUNxxxx.TXT` on the USB drive.

*(Alternative: If the USB is not set as primary boot drive, power on from cold and tap **`F7`** repeatedly to access the one-time BBS Boot Menu).*

---

## Verification & Benchmarks

Once booted into Windows 11, verify GPU status:
- Open Device Manager $\rightarrow$ Display Adapters $\rightarrow$ NVIDIA GeForce RTX 4070 Ti (status should be: *"This device is working properly"*, no Code 43).
- Run PyTorch GEMM benchmark to verify Tensor Core operation:
```bash
python tools/porting/benchmark_bf16.py
```
Expected output:
- BF16 Tensor GEMM: ~140–144 TFLOPS
- FP16 Tensor GEMM: ~138–142 TFLOPS
- FP32 Vector GEMM: ~27–30 TFLOPS

---

## License

GPL-2.0-only. Retain all original copyright notices, third-party notices, and EDK II license files.
