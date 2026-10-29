[Русская версия](README.md) | **English version**

---

# Universal Multi-GPU Windows Unlock Port — Ampere & Ada Lovelace

Standalone UEFI bootloader for concurrent volatile in-RAM unlock of NVIDIA graphics cards based on **Ada Lovelace (RTX 40xx)** and **Ampere (RTX 30xx)** architectures running under **Windows 10/11 x64 (NVIDIA Drivers)**.

The project is based on the research and codebase of [JustAResearcher/gpu-unlock-ada](https://github.com/JustAResearcher/gpu-unlock-ada) and [impacifist/Windows-GPU-UNLOCK](https://github.com/impacifist/Windows-GPU-UNLOCK). The bootloader runs during the early UEFI boot phase prior to the Windows display driver initialization, enabling error-free startup without «Code 43» and unlocking full hardware access to Tensor Cores at maximum silicon speed.

100% Volatile In-RAM — the bootloader **NEVER flashes, modifies, or writes to the physical SPI Flash EEPROM**. All modifications occur strictly within Host RAM and MMIO BAR0. Rebooting without the USB drive leaves all graphics cards in their 100% factory stock condition.

---

## Performance Comparison: Stock vs UEFI-Unlock (cuBLAS GEMM)

Testing was conducted on physical hardware under Windows 11 x64 with official NVIDIA driver 616.92 using PyTorch cuBLAS:

| Test (cuBLAS GEMM) | STOCK (No Unlock) | WITH OUR UEFI UNLOCK | Difference / Gain | Power Draw |
|---|---:|---:|:---:|:---:|
| **BF16 Tensor Cores** | **73.19 TFLOP/s** | **143.76 TFLOP/s** | **+96.4% (2x FASTER!)** 🔥 | **284.9 W** (TDP 100%) |
| **FP16 Tensor Cores** | **73.58 TFLOP/s** | **140.10 TFLOP/s** | **+90.4% gain** | **278.0 W** |
| **FP32 CUDA Cores** | **26.82 TFLOP/s** | **27.29 TFLOP/s** | **Parity** (CUDA cores are not throttled) | **283.9 W** |

> [!NOTE]
> **Physical Mechanics & Power Consumption**:
> - In **FP32 (CUDA cores)** benchmarks, the GPU hits the board's maximum hardware power limit (**~284 W**) both in stock and unlocked modes, outputting identical rated **~27–28 TFLOP/s** because standard vector shaders are not restricted by NVIDIA firmware.
> - On **Tensor Cores (FP16/BF16)** in stock mode, the driver artificially throttles the hardware scheduler: the GPU hard-caps at **80.6–80.8 TFLOPS** even with core clocks boosted to 2775 MHz.
> - Our UEFI bootloader removes this firmware throttle: Tensor Cores reach their full silicon speed of **142+ TFLOP/s** while consuming **276–284 W**, fully saturating the board's hardware power delivery.

---

## Hardware Configuration & Parameters

- **Physically Validated Testbed**:
  - **Motherboard**: Huananzhi X99-F8D Plus (Dual LGA2011-3, Intel C612 / X99 chipset, AMI Aptio V UEFI).
  - **Processors**: 2 × Intel Xeon E5-2696 v3 (Haswell-EP, 36 cores / 72 threads).
  - **Graphics Card**: Gigabyte GeForce RTX 4070 Ti WINDFORCE OC (AD104-400, PCI ID `10de:2782`, Subsystem `1458:40e8`, revision A1).
- **Strict VBIOS Hardware ID Addressing**:
  - ROM files are mapped strictly by PCI Subsystem ID: `\EFI\VBIOS\<VENDOR>_<DEVICE>_<SUBVENDOR>_<SUBDEVICE>.rom` (e.g., `10de_2782_1458_40e8.rom`). No hardcoded paths.
  - If multiple identical cards are present, the VBIOS image is staged in RAM once and shared across all matching controllers.
- **Supported Generations**:
  - **Ada Lovelace**: AD102, AD103, AD104, AD106, AD107 (RTX 4060 — 4090).
  - **Ampere**: GA102, GA103, GA104, GA106, GA107 (RTX 3050 — 3090).
- **Subsystem Selectors**: Initial read `SS0=0x41145131` / `0x60145131`, `SS1=0x4` $\rightarrow$ Latching target: `SS0=0x88888888`, `SS1=0x8`.
- **Driver**: NVIDIA (clean startup, Windows Device Manager reports «This device is working properly»).

---

## Universal Bootloader Architecture

1. **Motherboard & ACPI Independence**:
   - Automated dynamic search for `bootmgfw.efi` across all ESP/FAT partitions without hardcoded disk GUIDs.
   - Clean GOP release via standard `DisconnectController()` with PCI Command register synchronization (IO + Memory Space decode enabled, Bus Master disabled prior to transaction claim).
2. **Multi-GPU & Role Separation**:
   - PCIe topology scanning across all segments, root bridges, and CPU sockets (Dual Xeon / EPYC / Threadripper).
   - Support for homogeneous or heterogeneous GPU setups (e.g., RTX 4070 Ti + RTX 3060).
   - Clean handling of primary display adapters (active GOP) and dedicated compute accelerators (Headless Compute).
3. **Direct Physical Page Allocation**:
   - Large microcode payloads (`gsp_ga10x.bin`, 84.3 MB) are loaded via direct physical page allocation (`gBS->AllocatePages`), preventing failures due to UEFI memory pool fragmentation.

---

## Building from Source

The project builds cleanly using the standalone w64devkit toolchain (GCC 16.2.0):

```powershell
python build.py
```

The script compiles all C units with `-O2`, links the executable, and zeroes out the PE Import Directory to eliminate dynamic loading dependencies in UEFI ImageLoader.

Output binary: `dist/EFI/BOOT/BOOTX64.EFI` (~116 KB, clean PE32+).

---

## USB Drive Setup & Cold Boot Procedure

1. Format a USB flash drive as **FAT32**.
2. Organize files according to the structure:
```text
USB_ROOT/
├── EFI/
│   ├── BOOT/
│   │   └── BOOTX64.EFI              <-- Compiled bootloader
│   ├── ASSETS/
│   │   ├── booter_load_ad102.bin    (57,672 bytes)
│   │   ├── booter_load_ga102.bin    (61,256 bytes)
│   │   ├── gsp_boot_ad102.bin       (36,864 bytes)
│   │   ├── gsp_boot_desc_ad102.bin  (84 bytes)
│   │   ├── gsp_boot_desc_ga102.bin  (84 bytes)
│   │   ├── gsp_boot_ga102.bin       (24,576 bytes)
│   │   ├── gsp_ga10x.bin            (84,277,400 bytes)
│   │   ├── v67-ad102-610.43.03-four-plm.bin (64,000 bytes)
│   │   ├── v67-ga102-610.43.03.bin  (64,000 bytes)
│   │   └── NVIDIA-LICENSE
│   └── VBIOS/
│       └── 10de_2782_1458_40e8.rom  <-- Dump of your graphics card
├── copy_to_usb.bat                  <-- 1-click quick deploy
├── dump_vbios.bat                   <-- 1-click Windows VBIOS dumper
└── nvflashk.exe
```

*(Alternatively, run `copy_to_usb.bat`, enter your flash drive letter, and it will deploy everything automatically).*

### Cold Boot Launch Procedure:

> [!IMPORTANT]
> **Physical Cold Boot is Required!**  
> Do not use `Boot Override` from the BIOS setup menu — in that case the GPU is already initialized by the motherboard firmware, and bootloader integrity checks will abort the transaction.

**Recommended Boot Sequence:**
1. Disable **Secure Boot** in BIOS settings.
2. In the **`Boot`** tab, set your USB flash drive as **Boot Option #1** and save settings (`F10`).
3. **Shut down the PC** — a standard system shutdown is sufficient (no need to unplug the power cord or flip the PSU switch).
4. Power on the PC via the power button — the motherboard will cold-boot directly into the USB bootloader.
5. The bootloader text menu will appear. Press **`U`** (once) to apply the unlock.  
   *(By default, unlock `[U]` is automatically applied after 30 seconds; pressing `S` boots stock Windows).*
6. The display will freeze for 2-3 seconds (GOP release, microcode injection, and handover to `bootmgfw.efi`), followed by normal Windows 11 startup.
7. Transaction execution logs are saved on the flash drive in `UNLOCK-LOGS/RUNxxxx.TXT`.

---

## Utilities & Scripts

* **`dump_vbios.bat`** / **`dump_vbios.ps1`** — Automatic discovery of installed GPUs and VBIOS dumping named by PCI Subsystem ID.
* **`copy_to_usb.bat`** / **`copy_to_usb.ps1`** — Automated deployment of bootloader, firmware assets, and VBIOS to a FAT32 USB drive.
* **`build.py`** — EFI application build script with PE import directory sanitization.
* **`tools/package_usb.py`** — Distribution packaging script.

---

## License

GPL-2.0-only. All original copyrights, EDK II licenses, and third-party notices are preserved.
Firmware binary microcodes (`*.bin`) remain the intellectual property of NVIDIA Corporation and are governed by `NVIDIA-LICENSE`.
