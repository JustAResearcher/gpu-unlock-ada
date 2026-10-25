# Windows USB firmware rehearsal 0.5.2

This test temporarily releases the firmware drivers that provide the boot
display, reconnects them, verifies the resulting state, and then starts the
existing Windows Boot Manager. **It does not attempt the GPU unlock.**

This is an **active firmware test**, not another read-only capture. Driver
callbacks can change hardware state and cause a black screen or hang. The
0.5.2 package has not been validated on physical GPU hardware by its authors.
`VALIDATION.json` records the checks actually run; simulated firmware tests do
not establish that a physical release, restoration or Windows handoff will work.

The image is bound to the supplied capture of the MSI MPG Z690 EDGE WIFI DDR4
with the MSI RTX 4070 Ti SUPER. It checks exact GPU, ROM, output-path and firmware
driver identities before release. Keep the current GPU, monitor and cable
arrangement. Do not change hardware or weaken a check to get past a refusal.

## What this version changes

The 0.5.1 report identified the stage-4 refusal: NVIDIA's firmware driver had
recreated the graphics interface, but its framebuffer was still unset. The
reported mode was 15 with 14 available modes, while the display description and
callbacks matched the original state. The test stopped before the graphics
console driver had an opportunity to initialize the display, and before Windows
started. No unlock was attempted.

Version 0.5.2 tests a correction to that restoration sequence. Only at stage 4,
it accepts the exact intermediate state recorded in that report and lets the
already identified graphics-console driver start. The display must return to
its fully active, original state immediately after that step; the program must
also restore and verify the remaining console state before starting Windows.
Other mismatches still stop the test. It does not add a direct GPU mode-setting
call or enable the unlock payload.

This is an unproven physical-hardware fix. It may progress further, but it can
still leave a black screen or hang. Keep the same monitor and cable arrangement,
use one test attempt, and follow the recovery procedure if it stops. Do not
repeat the older builds. The ready prompt allows 120 seconds before cancellation.

## 1. Update the existing diagnostic or rehearsal USB

1. Extract the complete ZIP into a new normal folder in Windows.
2. Connect the existing **FAT32 USB** containing diagnostic 0.4.2 or rehearsal 0.5.0/0.5.1.
3. Run `Update-USB.cmd` and enter that USB's drive letter. It only accepts the
   exact released 0.4.2, 0.5.0 or 0.5.1 EFI, checks that the disk is USB and is not a Windows
   system/boot disk, preserves the reports, and retains the old EFI as a `.bak`.
   It does not format, repartition, or install a Windows driver.
4. Wait for the success message. If the updater refuses or reports a partial
   update, keep the error and do not boot it or manually overwrite another EFI.

Security settings remain enabled while preparing the USB. Updating the files
does not start the experiment or grant permission to bypass its runtime checks.

## 2. Prepare for one physical test

Keep this guide available on another device. Save your work and leave enough
time to restore the settings in section 4, even if you cancel the test.

1. Locate the matching **48-digit BitLocker recovery key** and confirm you can
   access it from another device or printout. Do not proceed without it. Do not
   send the key in chat or store its only copy on this PC or the test USB.
2. Open **Windows PowerShell as administrator** and check:

   ```powershell
   Confirm-SecureBootUEFI
   Get-BitLockerVolume -MountPoint $env:SystemDrive |
       Select-Object MountPoint, VolumeStatus, EncryptionPercentage, ProtectionStatus
   ```

   Expect `True`, `FullyEncrypted`, `100`, and `On`. If a command fails or the
   drive is encrypting/decrypting, stop before changing settings.
3. Immediately before the test, suspend BitLocker protection:

   ```powershell
   Suspend-BitLocker -MountPoint $env:SystemDrive -RebootCount 0
   Get-BitLockerVolume -MountPoint $env:SystemDrive |
       Select-Object MountPoint, VolumeStatus, EncryptionPercentage, ProtectionStatus
   ```

   Expect `FullyEncrypted`, `100`, and `Off`. The drive stays encrypted, but
   suspension makes an unlock key available on disk. `-RebootCount 0` leaves
   protection suspended until you explicitly resume it. Finish section 4 in
   the same sitting, including after cancellation. Do not use `Disable-BitLocker`.

## 3. Boot the rehearsal once

1. Perform a full Windows shutdown:

   ```powershell
   shutdown.exe /s /t 0
   ```

   After shutdown finishes, switch the power supply off or disconnect its power
   cord. Wait for the motherboard standby lights to go out, then restore power.
   Do not remove power while Windows is shutting down.
2. Power on and tap **Delete** for MSI setup. **F7** selects Advanced mode.
   Under **Settings → Security → Secure Boot**, change only the Secure Boot
   enable/disable setting to **Disabled**. Menu names may vary with BIOS 1.N0.
   Use **F10**, review the change list, and save only the intended change.
   Leave TPM/PTT, keys, Standard/Custom mode, CSM/UEFI mode, and boot priority
   unchanged. Do not clear the TPM, reset keys or defaults, or flash firmware.
3. On restart, tap **F11** and choose the **UEFI entry for the prepared USB**.
   This is a one-time boot selection. If the entry is absent or Secure Boot
   refuses it, stop and restore settings using section 4. If Windows boots
   instead, perform a full shutdown and power-off before the intended cold test.
4. The application must identify itself as a **firmware rehearsal**. It first
   checks this board, the exact firmware driver identities and ownership, and
   the Windows boot target. Refusal here should return without releasing drivers.
5. At the ready prompt, **press R** to run the test. Escape or the 120-second
   prompt timeout cancels before the first driver release. The release order is text-output
   consumer, graphics-console consumer, then NVIDIA firmware driver. Restoration
   checks the reverse sequence. The screen may temporarily go blank or retain
   the last frame. There is no unlock payload or `U` activation in this build.
6. On success, the test should restore the display and start Windows. Note
   whether Windows reaches the sign-in screen and whether display and keyboard
   work. A report ending immediately before Windows starts proves only that the
   application reached its handoff check; your observation establishes the boot.
7. A failure after a driver callback stops the sequence. The program does not
   guess at recovery or blindly continue to Windows. If it displays a stop
   message, or stays frozen for two minutes, follow section 5. Do not retry.

## 4. Restore security and return the report

Do this after success, cancellation, or a failed attempt.

1. Disconnect the test USB. Enter firmware with **Delete** (restart Windows
   first if it is running). Restore **Secure Boot = Enabled** through the same
   control, preserving the keys and other settings. Save with F10 and boot the
   existing Windows Boot Manager.
2. In administrator Windows PowerShell, verify Secure Boot before resuming
   BitLocker:

   ```powershell
   Confirm-SecureBootUEFI
   ```

   It must return `True`. If not, restore the original setting before proceeding.
3. Resume protection and verify:

   ```powershell
   Resume-BitLocker -MountPoint $env:SystemDrive
   Get-BitLockerVolume -MountPoint $env:SystemDrive |
       Select-Object MountPoint, VolumeStatus, EncryptionPercentage, ProtectionStatus
   ```

   Expect `FullyEncrypted`, `100`, and `On`.
4. Perform one normal Windows restart with the test USB disconnected. Keep the
   recovery key available. After signing in, repeat both status checks.
5. Reconnect the USB and send **`REPORT.TXT` from the newest `RUNxx` directory**,
   for example `GPU-UNLOCK-ADA\RUN03\REPORT.TXT`. Also state whether Windows
   started, the display returned, and the keyboard worked. Send refusal or
   partial reports too. If no report exists, send the last visible message.
   Do not repeat a failed attempt just to complete the report.

The report contains hardware and firmware identifiers, hashes, and diagnostic
state. Review it before sharing. It does not need your recovery key. Previous
`TOPOLOGY.TXT` captures remain on the USB; this rehearsal does not request a
repeat of that diagnostic.

## 5. If the screen hangs or Windows will not start

- Hold the physical power button until the PC switches off. Remove the USB.
  Disconnect PC power until standby lights go out, reconnect it, and enter
  firmware to restore Secure Boot. An interrupted report may be incomplete.
- If needed, select **F11 → the existing Windows Boot Manager**. This package
  does not install the withdrawn `AdaUnlockNative` filter or create a persistent
  boot entry; there is no new Windows driver-removal script to run.
- If BitLocker requests recovery, use the saved key matching the on-screen key
  ID. Never share the key. Finish section 4 after Windows starts. Do not clear
  the TPM, delete protectors, reset Windows, or format a drive to bypass recovery.
- If normal boot still fails with the USB removed and Secure Boot restored,
  stop testing and report the exact screen/error. These steps are a recovery
  procedure, not a guarantee against firmware failure or hardware damage.

## Scope and references

The build excludes the unlock payload, direct GPU-write transaction, DMA setup,
GPU reset routine and persistent boot scheduling. It does call the existing
firmware drivers to disconnect/reconnect them; those drivers can access hardware.
It does not replace Game Ready drivers or flash the VBIOS. Passing this rehearsal
is not proof that the subsequent unlock will work or remain stable in Windows.

- [Microsoft: BitLocker recovery keys](https://support.microsoft.com/en-us/windows/security/encryption/find-your-bitlocker-recovery-key)
- [Microsoft: suspend protection for firmware changes](https://learn.microsoft.com/en-us/troubleshoot/windows-client/windows-security/suspend-bitlocker-protection-non-microsoft-updates)
- [Microsoft: Suspend-BitLocker](https://learn.microsoft.com/en-us/powershell/module/bitlocker/suspend-bitlocker)
- [Microsoft: Resume-BitLocker](https://learn.microsoft.com/en-us/powershell/module/bitlocker/resume-bitlocker)
- [MSI motherboard manual](https://download.msi.com/archive/mnu_exe/mb/MPGZ690EDGEWIFIDDR4.pdf)
- [MSI Intel 600-series BIOS guide](https://download.msi.com/manual/mb/Intel600BIOS.pdf)
