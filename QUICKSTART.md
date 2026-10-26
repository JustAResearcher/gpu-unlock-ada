# GPU Unlock: quick start

This guide is for a Linux machine you control. RTX 4090 `10de:2684` has a
separate [single-card report](rtx4090-register-report.md). Every other Ada ID in
the README's [experimental target table](README.md#experimental-ada-targets) is
experimental, was tested on one or a few cards, and needs its own opt-in flag.
Other Ada IDs remain disabled. The measured instruction-rate gain is for
FP16, BF16, TF32 and FP8 tensor work with FP32 accumulation. INT8 and unsigned
INT4 did not speed up in the 2705 tests. One matched FP8/FP32 GEMM on the 2705
was effectively unchanged (59.326 → 59.495 TF/s), but it ran far below the FP8
peak; the README lists the FP8 issue-rate results.

## 1. Find your GPU's PCI address

```sh
lspci -Dnn | grep -i nvidia
```

Find the line for your GPU. The full address is the first field, such as
`0000:01:00.0`. Confirm the device ID is `10de:2684` for the RTX 4090 path or
appears in the [experimental target table](README.md#experimental-ada-targets).
Keep this address for step 3. On a multi-GPU host, identify the exact intended card; do not use
`targets=all` for an Ada test.

## 2. Install from open source

Install Git, your running kernel's headers, a C build toolchain, DKMS, Python 3,
and `zstd` using your distribution's package manager. Then:

```sh
git clone https://github.com/JustAResearcher/gpu-unlock-ada.git
cd gpu-unlock-ada
make install
```

`make install` selects the Arch, Debian/Ubuntu, or Fedora/RHEL package path.
The build downloads **SHA-pinned NVIDIA 610.43.03 inputs** and generates the
firmware locally. The downloadable source contains no NVIDIA firmware binary.
The build may take several minutes and asks for `sudo` when installing.

If your distribution cannot resolve a build dependency automatically, use
`make install-arch`, `make install-deb`, or `make install-rpm` as appropriate;
the [README](README.md#build-and-install) explains the generated package paths.

## 3. Enable one exact card

From the cloned folder, run this read-only helper to print the exact option
for each eligible GPU it finds:

```sh
python3 tools/print_boot_option.py
```

Choose **one** printed line for the first cold test. Cards that each passed
alone can later be listed together, comma-separated. To enter it manually,
use the card's opt-in flag from the
[experimental target table](README.md#experimental-ada-targets). For an
RTX 4070 Ti SUPER `10de:2705`, append these **two** options to your kernel
command line, replacing the sample address with yours:

```text
gpu_unlock.targets=0000:01:00.0 gpu_unlock.ad103_experimental=1
```

For an RTX 4090 `10de:2684`, only the first option is needed:

```text
gpu_unlock.targets=0000:01:00.0
```

Use the bootloader method your machine already has:

- **Ubuntu/Debian with GRUB:** Edit `GRUB_CMDLINE_LINUX_DEFAULT` in
  `/etc/default/grub`, append the options *inside its existing quotes*, save,
  then run `sudo update-grub`.
- **Arch with GRUB:** Make the same edit, then run
  `sudo grub-mkconfig -o /boot/grub/grub.cfg`.
- **Fedora/RHEL with GRUB:** Run
  `sudo grubby --update-kernel=ALL --args="gpu_unlock.targets=0000:01:00.0 gpu_unlock.ad103_experimental=1"`,
  substituting your PCI address and omitting the experimental flag for 4090.
- **systemd-boot:** Run `bootctl status` to find the active entry. Add the
  options to that entry's `options` line. Make sure its `initrd` points to the
  newly generated initramfs containing `gpu_unlock`; some distributions copy
  `/boot/initrd.img-*` to the EFI partition separately. Keep a copy of the
  original entry so you can select it if needed.

**Keep a normal boot entry available.** Do not copy a sample PCI address.
The package does nothing until these options are present.

## 4. Start from full power-off

Shut down cleanly. Cut AC power to the machine, wait about 30 seconds, then
restore power and start it. This gives the first test a fresh card state.
If the GPU has a customer or rented workload, wait until it is fully released
before doing this.

## 5. Check the result

After the stock NVIDIA driver loads, run:

```sh
journalctl -k -b --no-pager | grep -E 'gpu_unlock: unlocked|gpu_unlock: already unlocked|gpu_unlock: unlock failed|post-FLR SEC2'
nvidia-smi
```

Success requires the **exact selected card's** `gpu_unlock: unlocked` line,
healthy `nvidia-smi`, and a workload check. A loaded module alone is not
proof. The [2705 report](docs/ada-2705-report.md) shows the expected marker,
permission/selector readbacks, matched performance and numerical checks from
the tested card. For a single-GPU host, the read-only rate getter is:

```sh
python3 tools/read_sm_rates.py --expect-uuid GPU-PASTE-YOUR-EXACT-UUID
```

Get the UUID from `nvidia-smi --query-gpu=uuid --format=csv,noheader` after
boot. Do not use a UUID from another card.

For a stock/unlocked comparison, collect the stock measurements before enabling
the test and use the same running kernel, driver, binaries and power cap in both
states. Check `uname -r` and `/proc/cmdline`; a boot-menu title alone does not
identify the kernel or prove that the test entry ran. The [testing guide](docs/testing.md)
includes benchmark commands and the [matched RTX 4090 results](docs/evidence/rtx4090-pr1-matched/README.md).

## Keep it enabled across boots

First test an ordinary reboot with the same early initramfs and exact target
options selected for **one boot**. Check for `gpu_unlock: unlocked`, healthy
`nvidia-smi`, and full rate readbacks again. On the tested 4070 Ti SUPER, a
warm reboot returned the card to a fresh enough state and the unlock
succeeded; other boards may retain firmware state and be rejected. A loaded
module alone does not prove the unlock succeeded.

After that test succeeds, leave the target options in your persistent GRUB
entry. With systemd-boot, make the *tested* Ada entry persistent using
`sudo bootctl set-default YOUR-ADA-ENTRY.conf`, replacing the entry ID with
the one shown by `bootctl status`. Keep a separate stock entry in the boot
menu. `bootctl set-oneshot` selects only the next boot and will not persist.
After a kernel update, verify that the selected entry points to a rebuilt
initramfs containing `gpu_unlock` before booting the new kernel.

## Roll back

Remove the `gpu_unlock.*` kernel-command-line options and boot the normal
entry. If the firmware transaction did not stop cleanly, power off completely
before retrying; do not force-unload the module while it may retain DMA buffers.
Uninstall the generated packages if you no longer want the module or firmware.
If you used `bootctl set-default` for a separate Ada entry, run
`sudo bootctl set-default YOUR-STOCK-ENTRY.conf` and confirm the selected
default with `bootctl status` before rebooting.

For licensing, provenance and distribution limits, see
[firmware/NOTICE.md](firmware/NOTICE.md) and [the release notes](docs/release-1.1.1.md).
