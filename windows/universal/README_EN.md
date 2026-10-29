[Русская версия](README.md) | **English version**

---

# Universal Multi-GPU Windows Unlock Port — Ampere & Ada Lovelace

Standalone UEFI bootloader for concurrent, volatile in-RAM unlock of NVIDIA graphics cards based on **Ada Lovelace (RTX 40xx)** and **Ampere (RTX 30xx)** architectures running under **Windows 10/11 x64 (NVIDIA Drivers)**.

The project is built on research from [JustAResearcher/gpu-unlock-ada](https://github.com/JustAResearcher/gpu-unlock-ada) and [impacifist/Windows-GPU-UNLOCK](https://github.com/impacifist/Windows-GPU-UNLOCK). The bootloader operates during early UEFI boot prior to Windows display driver initialization, eliminating «Code 43» and unlocking full hardware access to Tensor Cores at maximum silicon line rates.

100% Volatile In-RAM — the bootloader **NEVER flashes, modifies, or writes to physical SPI Flash EEPROMs**. All modifications occur strictly within Host RAM and MMIO BAR0. Rebooting without the USB drive leaves all graphics cards in their 100% factory stock condition.

---

## Performance Comparison: Stock vs UEFI-Unlock (cuBLAS GEMM)

Testing was conducted on physical hardware under **Windows 11 x64** on a dual-socket **Dual Xeon E5-2696 v3 (X99)** workstation with **both GPUs (Ada Lovelace + Ampere) unlocked and operating simultaneously**, using [`tools/benchmark_dual_gpu.py`](tools/benchmark_dual_gpu.py):

| GPU & Architecture | Benchmark (cuBLAS GEMM) | STOCK (Factory) | WITH OUR UEFI UNLOCK | Difference / Gain | Latch Status in RAM |
| :--- | :--- | :---:| :---:| :---:| :---:|
| **GPU 0: RTX 4070 Ti 12GB** <br>*(Ada Lovelace AD104, 60 SM)* | **BF16 Tensor Cores** <br>**FP16 Tensor Cores** <br>**FP32 CUDA Cores** | 75.0 TFLOPS <br>74.0 TFLOPS <br>28.0 TFLOPS | **146.94 TFLOPS** <br>**142.22 TFLOPS** <br>**28.30 TFLOPS** | **+95.9% (2x FASTER!)** 🔥 <br>**+92.2% gain** 🔥 <br>Parity (CUDA cores not throttled) | `SS0=0x88888888` <br>`SS1=0x8` |
| **GPU 1: RTX 3060 12GB** <br>*(Ampere GA106, 28 SM)* | **BF16 Tensor Cores** <br>**FP16 Tensor Cores** <br>**FP32 CUDA Cores** | 25.6 TFLOPS <br>25.6 TFLOPS <br>9.6 TFLOPS | **49.57 TFLOPS** <br>**48.07 TFLOPS** <br>**9.65 TFLOPS** | **+93.6% (2x FASTER!)** 🔥 <br>**+87.8% gain** 🔥 <br>Parity (CUDA cores not throttled) | `SS0=0x88888888` <br>`SS1=0x8` |

> [!NOTE]
> **Physical Mechanics & Throttle Removal (SS0 / SS1)**:
> - **On Ada Lovelace (RTX 4070 Ti)**, factory firmware activates a `1/2 Rate` divider (`SS0=0x40145131` / `0x60145131`), artificially halving the issue rate for tensor operations with FP32 accumulation. Our in-RAM unlock (`SS0=0x88888888`) lifts this divider, doubling performance to **146.9 TFLOPS** while consuming up to **284 W** (100% TDP saturation).
> - **On Ampere (RTX 3060)**, stock firmware (`SS0=0x20111021`) forces `FMLA16 / FMLA32` into `1/2 Rate` (~25.6 TFLOPS dense without sparsity). Unlocking to `0x88888888` transitions all 8 sub-fields to `Full Rate`, unleashing the dense non-sparse potential of GA106 silicon to **49.6 TFLOPS**!
> - In **FP32 mode (standard CUDA shaders)**, both GPUs run at identical rated speeds before and after the unlock, as standard vector ALUs are unconstrained by NVIDIA firmware.

---

## Physically Validated Hardware Testbed

- **Motherboard**: Huananzhi X99-F8D Plus (Dual LGA2011-3, Intel C612 / X99 chipset, AMI Aptio V UEFI).
- **Processors**: 2 × Intel Xeon E5-2696 v3 (Haswell-EP, 36 cores / 72 threads total).
- **Memory**: 128 GB DDR4 (8 × 16 GB, Quad-Channel with 64 GB per CPU socket).
- **GPU 0 (PCIe Slot / Bus 3:00.0, Socket 0)**: Gigabyte GeForce RTX 4070 Ti WINDFORCE OC (AD104-400, `10de:2782`, Subsystem `1458:40e8`, revision A1).
- **GPU 1 (PCIe Slot / Bus 130:00.0, Socket 1)**: Gigabyte GeForce RTX 3060 EAGLE OC (GA106-300, `10de:2504`, Subsystem `1458:40e2`, revision A1).
- **OS & Drivers**: Windows 11 Pro x64 (Build 26100), NVIDIA Driver 572.70 / 617.xx (both adapters report «This device is working properly»).

---

## Universal Bootloader Architecture

1. **Simultaneous Multi-GPU & Cross-Architecture Unlock**:
   - Complete dynamic PCIe bus scan (segments 0..255), independent of CPU sockets (Dual Xeon, Threadripper, EPYC).
   - Supports heterogeneous pairings (Ada + Ampere in the same machine).
   - Clean microcode dispatch: dynamically routes Booter/V67 containers (`AD102` for Ada and `GA102` for Ampere).
2. **Safe GOP Display Release**:
   - Standard `DisconnectController()` followed by explicit `PCI_COMMAND` restoration (`0x3`: IO + Memory Space decoding enabled, Bus Master disabled prior to claim transaction). Display blanking is reduced to 1–2 seconds before smooth Windows Boot Manager handoff.
3. **Hardware ID-Based VBIOS Addressing**:
   - ROM files are mapped strictly by PCI Subsystem ID: `\EFI\VBIOS\<VENDOR>_<DEVICE>_<SUBVENDOR>_<SUBDEVICE>.rom` (e.g., `10de_2782_1458_40e8.rom` and `10de_2504_1458_40e2.rom`). Eliminates filename collisions.
4. **Direct Physical Page Allocation (`AllocatePages`)**:
   - The large firmware blob `gsp_ga10x.bin` (84.3 MB) is loaded into contiguous physical memory pages, preventing UEFI pool fragmentation failures.

---

## Building from Source

Builds cleanly using the standalone w64devkit toolchain (GCC 16.2.0):

```powershell
python build.py
```

The build script compiles all C units with `-O2`, links the standalone PE32+ executable, and zeroes out the PE Import Directory to eliminate dynamic loading dependencies in UEFI ImageLoader.

Output binary: `dist/EFI/BOOT/BOOTX64.EFI` (~116 KB).

---

## USB Setup & Cold Boot Procedure

1. Format a USB drive as **FAT32**.
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
│       ├── 10de_2782_1458_40e8.rom  <-- Dump of GPU 0 (RTX 4070 Ti)
│       └── 10de_2504_1458_40e2.rom  <-- Dump of GPU 1 (RTX 3060)
├── copy_to_usb.bat                  <-- 1-click quick deploy
```
3. Dump your card's VBIOS directly from Windows using `dump_vbios.bat`.
4. Plug the USB drive into your PC and select it from the BIOS Boot Menu (F11 / F12).
5. The bootloader detects all supported cards and unlocks them in RAM within 30 seconds (or immediately upon pressing `U`), passing control smoothly to Windows.

---

## Performance Verification (Benchmark)

Run the included multi-GPU benchmark to test all installed cards simultaneously:
```powershell
python tools/benchmark_dual_gpu.py
```
This tool benchmarks BF16, FP16, and FP32 GEMM throughput across all available GPUs and displays a comparative speedup ratio.
