# Latence.app experimental RTX 4090 FE Windows port

A manual UEFI port for **one RTX 4090 Founders Edition on an MSI MAG B650 TOMAHAWK WIFI**. It copies the [AD104 port](../ad104-msi/README.md) and replaces every hardware pin with values from two cold-boot captures of this machine. It runs before the unmodified Windows NVIDIA driver. It does not add targets to the Linux module and is not a general RTX 4090 installer.

**Status.** One cold manual launch on this machine (RUN0001) ended with TRANSACTION OK and Windows started. The repository's tensor issue-rate probe, run with the same cubin before and after, went from 149.2 to 297.6 T/s for FP16 with FP32 accumulation and from 81.6 to 162.3 T/s for TF32, while the INT4 control stayed at 1292 T/s. The small cuBLAS FP16/BF16 checks reported 0 failures in both states. This is one short run, not long-duration or application qualification. With the real PROM, the captured driver code and the generated assets, the host simulations pass for the transaction (90 scenarios), admission (17), display release (47), application flow (15) and boot I/O (8, with a private copy of the pinned ESP boot manager).

## Exact profile and containment

The profile was captured on an RTX 4090 Founders Edition `10de:2684`, subsystem `10de:16f4`, revision A1, VBIOS 95.02.47.00.01, in an MSI MAG B650 TOMAHAWK WIFI (MS-7D75) with BIOS 1.P3, a Ryzen 9 7950X and Windows 11 build 26100. The Ryzen integrated GPU is enabled but has no GOP, so the 4090 drives the preboot display. In UEFI, BAR0 is 16 MiB at `0xf5000000` and BAR1 is 32 GiB at `0xf000000000`.

The ROM hash, FWSEC geometry, asset hashes, loaded console-driver code, device paths, PCI config and BARs, cold registers and memory geometry are pinned. A PCI ID match alone does not pass admission. Do not remove guards to make another system boot. This application writes GPU registers and deliberately halts on failure after it releases the display.

Selector SS0 differed between the two cold captures (`0x03704054` and `0x03304054`), so the build uses the entry-latched selector profile. It records SS0 at entry and requires it to stay unchanged, instead of comparing it with a fixed stock value.

The adapter calls the Stop callbacks of the exact TextOut, GraphicsConsole and NVIDIA GOP owners, takes an exclusive PCI claim and restores only the original IO/MEM decode bits that went missing. It never patches driver code or guessed globals. The keyboard and mouse on this board sit below the xHCI at `PciRoot(0)/Pci(8,1)/Pci(0,3)`, and USB additions or removals are accepted only below that host. GPU and host ownership, the virtual console callbacks and all non-USB relationships must stay unchanged. The core requires payload completion, HALT, permissions, single selector writes, one FLR and a full-rate readback after the FLR. DMA mappings the firmware might still see are kept until power-off. Windows starts only after cleanup succeeds and fresh final checks pass.

## Source-only build

The build needs Windows x64, GCC/MinGW-w64 with `ar` and `nm`, Python 3 and the MSVC x64 linker (VS 2022 Build Tools). It reads the EDK II headers from `../ad104-msi/vendor/edk2`. `vswhere` finds MSVC; pass `-VcVars64` to name its setup script instead. Building and testing never touch the GPU, the internal ESP or boot variables.

The ESP GUID and the boot-manager pin stay out of Git in `local/boot_profile.h`:

```powershell
New-Item -ItemType Directory -Force local
Copy-Item boot_profile.example.h local/boot_profile.h
```

Enter the reviewed GUID of the Windows ESP and the size and SHA-256 of `\EFI\Microsoft\Boot\bootmgfw.efi` read from that ESP. The application also requires the ESP to be partition 1, starting at LBA 2048, with 204,800 sectors. In an administrator PowerShell, these commands mount the ESP, print the size and hash, and unmount it again:

```powershell
mountvol S: /S
(Get-Item S:\EFI\Microsoft\Boot\bootmgfw.efi).Length
(Get-FileHash S:\EFI\Microsoft\Boot\bootmgfw.efi).Hash
mountvol S: /D
```

Windows updates replace `bootmgfw.efi`. After an update the application refuses before any GPU access, and the pin must be read again. Then build:

```powershell
.\Build-Experimental.ps1 -Python python
```

The output is `build/AD102-FE-EXPERIMENTAL.EFI`. The build checks the x64 EFI entry, relocations, absence of imports/TLS/CLR and per-function stack frames. No EFI binary is distributed.

## Firmware and private tests

The tree contains no NVIDIA firmware or payload, ROM, loaded driver binary or Windows executable. Generate the assets locally with the upstream hash-pinned workflow in Linux/WSL:

```sh
# Repository root; use the pinned generators at 491925c1079435e260137b9f3526066e764de830.
make assets ASSETS=/your/private/ad102-assets
```

`profile.c` enforces the five AD102 asset names, sizes and hashes. The [read-only probe](../ad104-msi/diagnostic/README.md), built with `-Target ad102-fe`, produced the captures for this profile. Raw captures contain the ROM and driver code and must stay private.

```powershell
python tools/extract_private_capture.py CAP0001.JSON --out local/capture
.\Test-Experimental.ps1 -IncludeCore `
  -Prom local/capture/ad102-fe-physical-prom.bin `
  -Assets C:\private\ad102-assets `
  -DriverCode local/capture `
  -WindowsImage C:\private\bootmgfw.efi
```

The Windows image must be a private copy of the pinned boot manager from your own ESP. Tests simulate all GPU and EFI effects and perform no unlock. `tools/make_test_fixtures.py CAP0001.JSON` regenerates `tests/capture_graph.h`, `tests/capture_usb_paths.h` and `tests/captured_config.h`. Those files hold the ownership graph, USB paths and config space but no ROM or driver code.

## Manual experimental boot

This is a researcher-operated, board-specific reference. Use a separate FAT32 USB you control and keep its previous boot files. Copy the reviewed local EFI to `EFI/BOOT/BOOTX64.EFI` and the five generated AD102 assets to `AD102-ASSETS/` on that USB. Leave the internal ESP, BCD and BootOrder unchanged.

1. Turn off Secure Boot for the unsigned EFI and keep recovery media ready. Power off fully, wait ten seconds, then pick the UEFI USB from the MSI boot menu (F11).
2. Press **U once**. S, Esc or 30 seconds starts stock Windows without releasing the driver or touching the GPU.
3. The display may keep a frozen last frame. Do not touch any device. If Windows has not appeared after four minutes, power off fully, wait ten seconds and choose Windows Boot Manager.
4. Read `AD102-UNLOCK/RUNxxxx.TXT` on the USB, confirm that the Windows driver recovered, then check numerical correctness and throughput. A selector write, a frozen frame or a success line before the handoff does not show a Windows gain.

The application has no automatic retry, boot-variable edit or reconnect fallback. On the AD104 system a full power-off restored the stock state; this has not yet been checked on this machine.

## Credits and licence

**Latence.app** wrote the AD104/MSI Windows integration, admission profile, ownership/USB handling, tests and Windows evidence that this port copies. The AD102 FE / B650 profile replaces only the hardware pins and the test fixtures. Published through [Bizcott](https://github.com/Bizcott). This is an independent contribution, not upstream endorsement.

Ada code/payload integration: [JustAResearcher/gpu-unlock-ada](https://github.com/JustAResearcher/gpu-unlock-ada), building on [Zagitta/gpu-unlock](https://github.com/Zagitta/gpu-unlock) and [TOTORONG/AD103_win_unlocker](https://huggingface.co/TOTORONG/AD103_win_unlocker). UEFI strategy/compatibility: [impacifist/Windows-GPU-UNLOCK](https://github.com/impacifist/Windows-GPU-UNLOCK), pinned commits in `provenance.json`.

GPL-2.0-only unless a file states otherwise. Preserve [third-party notices](THIRD_PARTY_NOTICES.md), Windows upstream notices, EDK II licences, MIT diagnostic/PE-validator notices and original copyright headers.
