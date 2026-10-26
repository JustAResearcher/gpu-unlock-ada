# RTX 4070 SUPER on Windows (Experimental)

The Tensor unlock succeeded on a Gigabyte RTX 4070 SUPER (`10de:2783`,
`1458:413b`) running Windows 11. At the same 220 W power limit, matched cuBLAS
GEMM throughput rose from 78.63 to 117.53 TFLOP/s for FP16/FP32 and from 78.97
to 113.00 TFLOP/s for BF16/FP32. The GPU remained the desktop display device.

This directory contains the source adaptation, a read-only cold-state
collector and the reproduction workflow. The [run report](../../docs/ad104-4070-super-windows.md)
explains the changes and results; the [evidence directory](../../docs/evidence/ad104-4070-super/README.md)
contains all six matrix measurements and the sanitized EFI log.

## Tested configuration

| Component | Tested value |
|---|---|
| GPU | RTX 4070 SUPER, PCI `10de:2783`, subsystem `1458:413b`, revision `a1` |
| VBIOS / memory | `95.04.69.00.e7` / 12,282 MiB reported by NVIDIA |
| Board / BIOS | MSI MAG Z790 TOMAHAWK WIFI (MS-7D91) / H.E0 |
| CPU | Intel Core i5-13600KF |
| OS / driver | Windows 11 Pro, build 26300 / NVIDIA 617.42 |
| GPU power limit | 220 W before and after |
| Boot method | Existing internal ESP, persistent menu entry selected for one boot, operator presses `U` |

We used the internal ESP for convenience, so the trial did not need a prepared
USB drive. USB boot has not been tested on this board; the trial does not show
that the board requires internal-disk boot. The current unlock profile checks
that it was launched from the pinned internal ESP and rejects USB volumes.
A USB-launched variant would need a separate, reviewed boot-I/O profile and
physical validation; copying this unlock EFI onto a USB drive is insufficient.

The source retains the successful run's exact GPU, firmware, driver-code and
boot checks. Another system needs its own capture and a reviewed profile if
any of those checks differ. This is one successful physical trial with short
matrix benchmarks; repeated cold A/B/A runs and long-duration stability remain
open. The original `windows/ad104-msi` port and Linux target admission are unchanged.

## 1. Assemble the source

Use x64 Windows, Python 3.13 or later, MinGW-w64 GCC/binutils and Visual Studio
2022's x64 C++ tools. Run the following from the repository root:

```powershell
python windows/ad104-4070-super/prepare.py --out build/4070-super
python -m unittest discover -s windows/ad104-4070-super/tests -v
```

`prepare.py` checks every file against `source-files.json` before writing a new
directory. It reuses unchanged files from `windows/ad104-msi` at base commit
`4e7fefcc4923f9e083d5324dc75a7d2f906af77f` and applies this directory's overlay.
Choose a new output directory for each assembly; existing output is refused.
The repository's `.gitattributes` preserves the bytes used by the hash catalog.

All commands below use the assembled tree. Keep captures, local configuration,
firmware and executable outputs under the ignored `build/` directory.

## 2. Capture the card before Windows loads (Experimental internal-ESP setup)

Build the collector from PowerShell:

```powershell
$vcvars = 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat'
& ./build/4070-super/diagnostic/Build.ps1 -VcVars $vcvars -Python python
```

The build runs host tests and PE checks, then writes
`diagnostic/build/AD104-PROBE.EFI` and its manifest. The collector records the
1 MiB physical PROM, 4 KiB PCI configuration, fixed cold registers and firmware
driver ownership/code. Its only persistent output is a new
`AD104-PROBE/CAPxxxx.JSON` on the volume from which it started.

This experimental workflow uses the existing internal FAT32 EFI System
Partition. In an administrator PowerShell, first verify that `S:` is unused
and mount the system ESP with `mountvol S: /S`. Check the disk/partition identity and available space;
leave at least 8 MiB for each capture. Create an **unused** `S:\EFI\4070S-PROBE`
directory and copy the collector there as `COLD.EFI`. Keep Windows Boot Manager
at `\EFI\Microsoft\Boot\bootmgfw.efi` intact.

Before changing the boot entry, save `bcdedit /enum firmware /v` and export the
BCD store with `bcdedit /export <private-backup-file>`. The following commands
create a separate entry. Run each command individually and stop on an error:

```powershell
bcdedit /copy "{bootmgr}" /d "4070 SUPER capture"
# Paste the new identifier printed by /copy, including its braces:
$entry = '{NEW-ENTRY-GUID}'
bcdedit /set $entry device partition=S:
bcdedit /set $entry path '\EFI\4070S-PROBE\COLD.EFI'
bcdedit /set "{fwbootmgr}" displayorder $entry /addlast
bcdedit /set "{fwbootmgr}" bootsequence $entry
bcdedit /enum firmware /v
```

The entry and its `displayorder` membership persist. Only `bootsequence` is
one-shot; consuming it does not delete the entry or files. The collector writes
captures to `S:\AD104-PROBE\`, while the unlock writes logs under
`S:\EFI\4070S-PROBE\LOGS\`. [Section 8](#8-rollback-and-remove-the-experimental-setup)
covers removal of both the entry and the on-disk files.

Confirm that Windows remains the first default entry, the previous entries
retain their order, and only the new entry is selected for the next boot.
Save the new entry's identifier with the private boot backup; assign it to
`$entry` again in each administrator shell after restarting Windows.
The commands use Microsoft's documented [UEFI boot entries](https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/bcd-system-store-settings-for-uefi?view=windows-11)
and [display-order options](https://learn.microsoft.com/en-us/windows-hardware/drivers/devtest/bcdedit--displayorder).
`mountvol S: /D` removes the temporary drive letter after file work is complete.

The unsigned collector requires Secure Boot disabled. The native GSP read path
requires a firmware-readable NTFS volume and cannot read a locked BitLocker
volume. Check recovery access before changing firmware settings.
On the tested MSI board, `Delete` opens BIOS and `F11` opens the boot menu.
Use a full shutdown and power-off, then boot the capture entry once. After the
collector returns to firmware, select Windows Boot Manager.

Copy the resulting capture into `build/private/` and inspect it with the
manifest from the collector you actually ran:

```powershell
python build/4070-super/diagnostic/tools/analyze.py build/private/CAP0002.JSON --manifest build/4070-super/diagnostic/build/manifest.json
python build/4070-super/tools/extract_private_capture.py build/private/CAP0002.JSON --out build/private/capture-inputs
```

Use your actual capture filename. The extractor accepts schema 5 and this
profile's exact identity, PROM and driver-code hashes. It writes five private
inputs, with no overwrite. Labels `310`, `315` and `406` identify the captured
code profiles; live UEFI handle addresses are discovered at runtime. Keep the
raw capture private because it contains ROM/code bytes and local device paths.

## 3. Generate the firmware locally

Use the repository's existing [firmware workflow](../../firmware/NOTICE.md).
For example, from a Linux or WSL shell at the repository root, with Python,
GNU make, tar, xz and zstd available:

```sh
make assets INPUTS="$PWD/build/4070s-inputs" ASSETS="$PWD/build/4070s-assets"
```

Use those dedicated paths: the existing Makefile recreates the corresponding
`4070s-assets.src` extraction directory. Generation verifies the upstream
610.43.03 inputs and derived assets. This step only prepares files; it does not
load a Linux module or touch the GPU. Copy the five AD102 assets to a local
Windows directory if generated elsewhere. Their sizes and SHA-256 values are
listed in `profile.c` and [firmware/ada-610.43.03.json](../../firmware/ada-610.43.03.json).

| Asset | Bytes | Location used by the EFI |
|---|---:|---|
| `booter_load_ad102.bin` | 57,672 | ESP `EFI/4070S-PROBE/ASSETS/` |
| `v67-ad102-610.43.03-four-plm.bin` | 64,000 | Same ESP directory |
| `gsp_boot_ad102.bin` | 36,864 | Same ESP directory |
| `gsp_boot_desc_ad102.bin` | 84 | Same ESP directory |
| `gsp_ga10x.bin` | 84,277,400 | Pinned path on the Windows NTFS partition |

The tested ESP was 100 MiB. Reading the large GSP file through the firmware's
native NTFS `EFI_SIMPLE_FILE_SYSTEM_PROTOCOL` kept it off that small partition.
The actual run verified this path; no additional NTFS driver was loaded.

## 4. Configure the local boot profile and run host checks

Create `build/4070-super/local/boot_profile.h` from `boot_profile.example.h`.
Set the ESP and Windows NTFS partition GUIDs and the absolute GSP path **within
that NTFS volume**, such as `L"\\gpu-unlock\\gsp_ga10x.bin"` with no drive letter.
Obtain the GUIDs and geometry from `Get-Partition`; encode GUIDs as `EFI_GUID`
initializers, not raw big-endian byte strings. Never commit this local header.

`boot_io.c` also pins the layout and the Windows executable:

| Volume | Partition number | Start LBA | Size in 512-byte sectors |
|---|---:|---:|---:|
| ESP | 1 | 2,048 | 204,800 |
| NTFS asset volume | 3 | 239,616 | 3,998,527,488 |

The tested `bootmgfw.efi` was 3,086,680 bytes, SHA-256
`2d60a2203865122dad22772136b3d905167d9dcf926d602bb5f48b3b6668abec`.
Copy your own ESP's Windows image to the private input directory for the host
checks. A different layout or Windows image requires reviewing and updating
the corresponding source and fixtures; changing just the GUIDs is insufficient.
The collector's PCI, PROM and loaded-driver evidence must also match the profile.

Set `$assets` to the absolute directory containing the five generated files
and run these commands from `build/4070-super`:

```powershell
$vcvars = 'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat'
$inputs = (Resolve-Path ../private/capture-inputs).Path
$windows = (Resolve-Path ../private/bootmgfw.efi).Path
./Test-Core4070S.ps1 -Prom "$inputs/physical-prom.bin" -Assets $assets
./Test-Ownership4070S.ps1 -DriverCode $inputs
./Test-Boot4070S.ps1 -Assets $assets -WindowsImage $windows
./Build-Experimental.ps1 -VcVars64 $vcvars
```

Put MinGW-w64's `gcc`, `ar` and `nm` on `PATH`; the scripts also accept explicit
compiler/Python paths. All suites must pass. The core suite includes a deliberate
transient-BME fault whose expected nonzero exit confirms that its assertion fires.
The ownership fixtures preserve the reviewed capture graph; the public runner
checks the external code files by length and hash. The fixture's relocation
statement records the original capture review, not an additional raw-capture check.

The output is `build/AD104-EXPERIMENTAL.EFI`. Using the author's excluded local
header, the public source rebuilds the hardware-tested 98,816-byte binary:
`b20394d76f7b7dc706e7d7ba893a4ca56b958db66232b7b22ed60a63d28acf84`.
A different local header changes the binary hash. The build manifest records
the new artifact separately from the prior physical run. The original EFI
banner says “hardware unvalidated”; it is retained to preserve the tested bytes.

## 5. Measure the stock state

Use the same CUDA-enabled Python environment for both phases. The published
runs used Python 3.14.6, PyTorch 2.13.0+cu132 and CUDA 13.2, with cuBLAS version
130400. `benchmark.py` loads the cuBLAS DLLs from that PyTorch environment.

From the assembled source directory, run three separate processes before the
unlock. Keep the desktop idle and background applications consistent:

```powershell
python run.py --name stock-run1 --label stock
python run.py --name stock-run2 --label stock
python run.py --name stock-run3 --label stock
```

The runner uses below-normal priority and four CPU threads, records NVIDIA
telemetry and stops on its temperature/time/memory limits. Each run multiplies
4096-by-4096 matrices in FP32, FP16/FP32 and BF16/FP32 modes, with 15 samples of
five products after a one-second warmup. The benchmark verifies a full 256-square
CPU reference, 1,024 elements of the large result, and the complete output hash.

## 6. Run the manual unlock once (Experimental)

Mount the same ESP. Copy your verified EFI as
`S:\EFI\4070S-PROBE\UNLOCK.EFI` and the four small assets into `ASSETS/`.
Place `gsp_ga10x.bin` at the exact NTFS path configured in your private header.
Compare the deployed files' hashes with their build/asset hashes.

Update only the entry created for this workflow, then select it for one boot:

```powershell
bcdedit /set $entry description '4070 SUPER manual trial'
bcdedit /set $entry path '\EFI\4070S-PROBE\UNLOCK.EFI'
bcdedit /set "{fwbootmgr}" bootsequence $entry
bcdedit /enum firmware /v
```

After a full shutdown and power-off, boot the entry and press `U` for one unlock
attempt. `S`, Escape or a 30-second timeout selects stock Windows after preflight.
The display may freeze while the firmware display drivers are released. If
Windows has not appeared after four minutes, power off fully, wait ten seconds
and choose Windows Boot Manager. There is no automatic retry.

## 7. Verify the return to Windows and compare

Save the new `EFI/4070S-PROBE/LOGS/RUNxxxx.TXT`, check NVIDIA/Device Manager status
and review Display, NVIDIA and WHEA events from this boot. The successful run
recorded all three driver detach stages, payload completion, full selector
readbacks after one FLR and:

```text
Transaction result=0 safe=1 retained=0 started=1 quiesced=1 flrs=1 DMA_live=0
```

Windows must also return and execute CUDA correctly; the transaction log ends
before the final Windows launch. Confirm that the one-time boot sequence has
been consumed and the ordinary Windows entry is still first. Run:

```powershell
python run.py --name after-run1 --label after-trial
python run.py --name after-run2 --label after-trial
python run.py --name after-run3 --label after-trial
python compare.py --before stock-run1.json stock-run2.json stock-run3.json --after after-run1.json after-run2.json after-run3.json
```

The comparator rejects differences in software hashes, GPU identity, driver,
VBIOS, power limit, dimensions or output hashes. Keep all runs, including slow
ones, and inspect clocks and temperature alongside median throughput.
Share the matrix JSON, sanitized transaction log and exact tested source/binary
hashes. Raw captures and generated firmware remain local.

## 8. Rollback and remove the experimental setup

Deleting the boot entry or files does not reset an already unlocked GPU.
End the unlocked session with a full shutdown and power-off, then boot Windows
Boot Manager directly. Run the cleanup below from administrator PowerShell.

1. Restore `$entry` from the identifier saved in section 2. Inspect it first
   and confirm its description and EFI path belong to this trial:

   ```powershell
   $entry = '{NEW-ENTRY-GUID}'
   bcdedit /enum $entry /v
   ```

   Use only the entry created for this workflow. Then remove it and its menu
   references with the documented `/cleanup` option:

   ```powershell
   bcdedit /delete $entry /cleanup
   bcdedit /enum firmware /v
   ```

   Stop on an error. Verify that the trial entry is absent, Windows Boot
   Manager remains first, and the other entries retain their previous order.
   `bcdedit /? DELETE` describes `/cleanup`, which removes references to the
   deleted entry, including a pending one-time selection. A full BCD import
   is unnecessary for this removal and would replace unrelated entries too.

2. Verify that `S:` is unused, mount the system ESP with `mountvol S: /S`,
   and compare its disk/partition identity with the private setup notes.
   Inspect the exact directory before removing it:

   ```powershell
   Get-Item -LiteralPath 'S:\EFI\4070S-PROBE'
   Get-ChildItem -LiteralPath 'S:\EFI\4070S-PROBE' -Force
   ```

   After saving any wanted logs privately and confirming that this is the
   dedicated directory created in section 2, remove it:

   ```powershell
   Remove-Item -LiteralPath 'S:\EFI\4070S-PROBE' -Recurse
   Test-Path -LiteralPath 'S:\EFI\4070S-PROBE'
   ```

   `Test-Path` should return `False`. This removes the collector/unlock EFI,
   four small assets and unlock logs. Keep `S:\EFI\Microsoft` and all other
   bootloader directories intact.

3. The collector's captures are separate: inspect `S:\AD104-PROBE\` and
   delete only the `CAPxxxx.JSON` files created by this trial. Remove that
   directory only if it was created for the trial and is now empty. Remove
   the temporary drive letter with `mountvol S: /D` when ESP cleanup is done.

4. On `C:`, remove the exact GSP file configured in the private header.
   For the example path in section 4, that is
   `Remove-Item -LiteralPath 'C:\gpu-unlock\gsp_ga10x.bin'`; substitute the
   actual configured path and leave unrelated files in its parent directory.
   Also remove unwanted capture copies and extracted PROM/driver-code inputs
   under `build/private/`, the local `boot_profile.h`, generated assets and
   trial build outputs. Inspect each location before deletion and retain any
   wanted evidence in private storage. Raw captures, BCD exports and local
   partition identifiers must stay out of commits and public replies.

Restore any firmware settings changed for the trial, including Secure Boot,
to their previous values. No rollback or deletion command above is run by the
build scripts; cleanup remains an explicit operator action.

## Credits

This adaptation builds on [Latence.app's AD104 Windows port](../ad104-msi/README.md),
[JustAResearcher's Ada integration](https://github.com/JustAResearcher/gpu-unlock-ada),
[Zagitta/gpu-unlock](https://github.com/Zagitta/gpu-unlock) and
[TOTORONG's AD103 unlocker/payload](https://huggingface.co/TOTORONG/AD103_win_unlocker).
The original source, EDK II headers and all corresponding licence notices are
retained. Firmware, PROM, loaded driver code and Windows executables are not
included in this contribution.
