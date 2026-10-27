**English version** | [Русская версия](README.md)

---

# Experimental RTX 4070 Ti Windows Port — Gigabyte / Huananzhi X99

A manual UEFI port for the **Gigabyte GeForce RTX 4070 Ti (AD104-400 / Subsystem 1458:40e8)** on **Huananzhi X99-F8D Plus (Intel C612 / X99, AMI Aptio IV/V UEFI BIOS)**, based on [JustAResearcher/gpu-unlock-ada](https://github.com/JustAResearcher/gpu-unlock-ada) and [impacifist/Windows-GPU-UNLOCK](https://github.com/impacifist/Windows-GPU-UNLOCK). It executes before the unmodified Windows NVIDIA driver loads.

| cuBLAS 4096² GEMM | Measured TFLOP/s | GPU Power | Clock | Notes |
|---|---:|---:|---:|---|
| FP32 Dense SIMD | 27.29 | ~185 W | 2520 MHz | Physical dense vector rate |
| FP16 Tensor Core | 140.10 | 278 W | 2520 MHz | Unlocked Ada Tensor throughput |
| BF16 Tensor Core | **143.76** | 284.9 W | 2520 MHz | **>91% of physical dense silicon peak** |

Measured on hardware under Windows 11 with unmodified NVIDIA driver 616.92. The v0.5.4 firmware transaction and GSP latch completed successfully, returning to Windows without Code 43. Peak power under saturated matrix multiplication reached 284.9 W (matching the 285 W board limit).

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

1. Disable Secure Boot in your UEFI BIOS.
2. Completely power off the system (cold shutdown; wait 10 seconds).
3. Power on and open the UEFI Boot Menu (e.g., `F11` or `F12`), select your USB drive.
4. The loader opens and performs read-only cold checks. Press **`U`** once to authorize the one-shot unlock transaction.
   - Pressing **`S`**, **`Esc`**, or waiting 30 seconds boots stock Windows without touching GPU state.
5. The display will detach, firmware injection and GSP configuration run, and the loader hands execution over to Windows Boot Manager (`bootmgfw.efi`).
6. Inspect `AD104-UNLOCK/RUNxxxx.TXT` on the USB drive to review the full register and transaction trace.

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
