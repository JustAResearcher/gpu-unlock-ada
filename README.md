# GPU unlock 1.1.1 — RTX 4090 and experimental Ada GPUs

New to this project? Start with the [copy-paste Quick Start](QUICKSTART.md).

This Linux kernel module enables the SM speed-select override before the stock
NVIDIA driver starts. RTX 4090 (`10de:2684`, AD102) support has demonstrated
approximately **2× FP16 and BF16 tensor throughput with FP32 accumulation**:

| Mode | Before | After |
|---|---:|---:|
| FP16 → FP32 accumulate | 165.6 TF/s | 331.6 TF/s |
| BF16 → FP32 accumulate | 165.5 TF/s | 330.6 TF/s |

The override survived PCIe Function Level Reset (FLR), stock NVIDIA `615.78.08`
initialization and CUDA benchmarks. INT8, `dp4a` and scalar FP32 stayed near their
baseline rates. The original transaction preceded the repository cleanup; see
the [technical report](rtx4090-register-report.md) and
[original evidence](docs/evidence/rtx4090/README.md).

An independent owner run of revision `8855e7b` also completed the RTX 4090
transaction and NVIDIA driver handoff on Linux `7.2.11-arch1-1`. Matched
stock/unlocked measurements with NVIDIA `615.78.08` and the same 450 W cap
found FP16/FP32 instruction throughput **160.4 → 320.4 TF/s** and BF16/FP32
**159.7 → 320.2 TF/s**. All exercised numerical checks passed. Real cuBLAS
GEMM median throughput varied by shape: FP16 gained 12–34%, BF16 gained
16–20% at two shapes and was essentially unchanged at 4096³. These are one
matched run pair with the desktop active, not whole-application speedups or
an extended stability test. See the [matched evidence and limits](docs/evidence/rtx4090-pr1-matched/README.md)
and [testing workflow](docs/testing.md).

A modified 48 GB RTX 4090 (`10de:2684`, AD102) on Ubuntu 26.04 with Linux
`7.0.0-38-generic` and NVIDIA `610.57.04` open kernel modules also completed the
transaction and driver handoff on 2026-10-25: `fmla32` went from `1` to `0`,
FP16/FP32 instruction throughput **164.9 → 328.9 TF/s**, BF16/FP32
**164.8 → 328.7 TF/s**, with INT8, `dp4a` and scalar unchanged and no Xid. A
quantized mixture-of-experts serving workload on the same card showed **no**
gain (8K prefill 0.983×), which is consistent with quantized matmuls not
consuming the FP32-accumulate tensor path. That is one card, one cold boot and
one workload. See the [modified-card evidence and limits](docs/evidence/rtx4090-48g-modded/README.md).
Further matched owner reproductions used NVIDIA [610.57.04](docs/evidence/rtx4090-610.57.04/README.md)
and [595.91.07](docs/evidence/rtx4090-595.91.07/README.md).

A separate cold test of the Ada port on one RTX 4070 Ti SUPER
(`10de:2705`) with NVIDIA `580.178.04` measured FP16/FP32 tensor issue rate
at **95.0 → 189.9 TF/s** and BF16/FP32 at **95.0 → 189.7 TF/s** under the
same 240 W power cap. INT8 stayed at 379.5 TOP/s. The stock driver loaded,
the NVIDIA RM getter reported full FMLA32 rate, deterministic FP16/BF16 GEMM
outputs matched exactly, and owner mining recovered with fresh accepted shares.
A second matched instruction benchmark found **TF32/FP32 47.49 → 94.51 TF/s**
(1.99×), with its FP16/FP32 control also near 2×. Unsigned INT4/INT32 stayed
at approximately **754.8 TOP/s** in both states. These are register-resident
issue rates, not application GEMM or inference speedups.
A matched, verified E4M3 FP8/FP32 CUTLASS GEMM measured **59.326 TF/s stock →
59.495 TF/s unlocked** on this card, a 0.3% difference. That kernel reaches only
about 17% of this card's FP8 peak, so it cannot resolve an issue-rate change.
Other measurements do put FP8 in the affected group: an 8192³ E4M3 GEMM on a
Windows RTX 4080 SUPER measured **199.4 → 310.4 TF/s (+55.7%)** and
**199.4 → 298.7 TF/s (+49.8%)** in two runs (see
[goldlionren/gpu-unlock-4080s](https://github.com/Goldlionren/gpu-unlock-4080s);
its author flags FP8 variance and keeps FP8 out of that repo's own pass/fail
gate), and register-resident E4M3 probes read **603.1 TF/s** unlocked on the
modified 48 GB RTX 4090 (see [that evidence directory](docs/evidence/rtx4090-48g-modded/README.md))
and **165.45 → 323.95 TF/s (1.96×)** on a Windows RTX 4070 Ti SUPER (see
[A-XIANGQAQ/rtx4070tis-ad103-tensor-unlock](https://github.com/A-XIANGQAQ/rtx4070tis-ad103-tensor-unlock)).
The 4090 figure is 91% of its 660.6 TF/s full FP8 rate; NVIDIA rates a stock
RTX 4090 at 330.3 TF/s for FP8 with FP32 accumulate and 660.6 with FP16
accumulate.
So FP8 does move at the issue-rate level; a matched AD103 pair is 1.96×. At a
fixed power limit the application GEMM stays below that: an
unlocked RTX 4090 peaked at 346 W against 313 W stock and spent both MMA and
GEMM passes under SW Power Cap (see
[the 610.57.04 evidence](docs/evidence/rtx4090-610.57.04/README.md)).
This is one card; the unlock succeeded on two cold boots and two ordinary
warm reboots, the last from a persistent EFI default. Throughput was measured
on the first cold boot. See the [2705 canary report](docs/ada-2705-report.md)
and its [sanitized raw benchmark output](docs/evidence/ada2705/README.md).

The AD102 payload and its debug-booter analysis come from
**[TOTORONG/AD103_win_unlocker](https://huggingface.co/TOTORONG/AD103_win_unlocker)**.
This fork integrates that published payload into
**[Zagitta/gpu-unlock](https://github.com/Zagitta/gpu-unlock)**'s Linux module,
firmware generation and boot packaging. The payload is essential to the result.

## Experimental Windows contribution — Latence.app

A separate [MSI RTX 4070 Ti AD104 Windows UEFI reference port](windows/ad104-msi/README.md)
records one successful manual boot with NVIDIA 616.56 and matched cuBLAS FP16/FP32
**+80.1%**, BF16/FP32 **+76.4%** measurements. It is tied to one captured MSI Z490
profile; it does **not** add AD104 admission to the Linux module. See the
[report and limitations](docs/ad104-windows-latence-app.md). No firmware/Windows/ROM
binaries or automatic boot installer are distributed. Contribution by **Latence.app**;
original upstream credits and licences are retained.

## Supported targets

The module selects the AD102 firmware and payload automatically for RTX 4090
`10de:2684`. The original GeForce/CMP GA102/GA104 device list and GA102 payload
are retained, along with the newer GA103/GA106 device list. Upstream reported
RTX 3090 success; this branch does not revalidate every legacy board.
GA107 RTX 3050 6GB (`10de:2584`) uses the same GA102 Booter path and was
unlocked on one card ([2584 report](docs/ga107-2584-report.md)).

### Experimental Ada targets

Each ID below needs its own opt-in flag. Rows marked *exact* must also be
named by their exact PCI address in `gpu_unlock.targets=`, alone or in a
comma-separated list, and `targets=all` never selects them. All of them use the RTX 4090's pinned
AD102-family firmware and payload. Results come from one or a few cards per ID;
other boards, memory configurations and laptop models remain unverified.
`tools/print_boot_option.py` prints the exact option for any card listed here.

| PCI ID | Card | Die | Opt-in | Tested | Details |
|---|---|---|---|---|---|
| `2685` | RTX 4090 D | AD102 | `rtx4090d_experimental` | eight cards in one host | [#7](https://github.com/JustAResearcher/gpu-unlock-ada/pull/7) |
| `2689` | RTX 4070 Ti SUPER | AD102 | `rtx4070tis_ad102_experimental`, exact | one card, 30-minute soak | [#19](https://github.com/JustAResearcher/gpu-unlock-ada/pull/19) |
| `2702` | RTX 4080 SUPER | AD103 | `ad103_experimental`, exact | one card, matched mma and cuBLAS, alongside a 2704 in one host | [matched evidence](docs/evidence/ada2702-ada2704-matched/README.md), [#53](https://github.com/JustAResearcher/gpu-unlock-ada/issues/53) |
| `2704` | RTX 4080 | AD103 | `rtx4080_experimental`, exact | one modified 32 GiB card; a second card in [matched evidence](docs/evidence/ada2702-ada2704-matched/README.md) | [report](docs/rtx4080-hardware-report.md) |
| `2705` | RTX 4070 Ti SUPER | AD103 | `ad103_experimental`, exact | one card, matched stock/unlocked | [report](docs/ada-2705-report.md) |
| `2709` | RTX 4070 | AD103 | `ad103_experimental`, exact | one card, matched | [report](docs/ada-2709-report.md) |
| `2757` | RTX 4090 Laptop GPU | AD103M | `ad103m_experimental`, exact | Clevo: unlocked, no throughput change; Lenovo: 2× on two cold boots | [report](docs/ada-2757-report.md), [Lenovo](https://github.com/JustAResearcher/gpu-unlock-ada/issues/4#issuecomment-6397961415) |
| `27e0` | RTX 4080 Laptop GPU | AD104M | `ad104m_experimental`, exact | one HP laptop, one cold-entry boot; 2.01x FP16/BF16 issue, same-policy GEMM comparison | [report](docs/ada-27e0-report.md) |
| `2782` | RTX 4070 Ti | AD104 | `ad104_experimental`, exact | one card in a VFIO guest, matched; one bare-metal owner report | [report](docs/ada-2782-report.md), [#15](https://github.com/JustAResearcher/gpu-unlock-ada/pull/15) |
| `2783` | RTX 4070 SUPER | AD104 | `ad104_experimental`, exact | one card, matched | [report](docs/ada-2783-report.md) |
| `2786` | RTX 4070 | AD104 | `ad104_experimental`, exact | one card; three owner reports | [report](docs/ada-2786-report.md), [#11](https://github.com/JustAResearcher/gpu-unlock-ada/pull/11) |
| `2803` | RTX 4060 Ti | AD106 | `ad106_experimental`, exact | one 16 GB card, matched; two more boards in #34 | [report](docs/ada-2803-report.md), [#34](https://github.com/JustAResearcher/gpu-unlock-ada/pull/34) |
| `2805` | RTX 4060 Ti 16GB | AD106 | `ad106_experimental`, exact | one card, no matched throughput run | [report](docs/ada-2805-report.md) |
| `2882` | RTX 4060 | AD107 | `ad107_experimental`, exact | one card, matched cuBLAS; cold boot and warm reboot | [report](docs/ada-2882-report.md) |
| `28a0` | RTX 4060 Laptop GPU | AD107 | `ad107_experimental`, exact | one card, unlocked state only | [report](docs/ada-28a0-report.md) |
| `28e0` | RTX 4060 Laptop GPU | AD107 | `ad107_experimental`, exact | one owner report | [#10](https://github.com/JustAResearcher/gpu-unlock-ada/pull/10) |
| `28e1` | RTX 4050 Laptop GPU | AD107 | `ad107_experimental`, exact | one laptop, matched | [report](docs/ada-28e1-report.md) |

Laptops differ in VBIOS ROM exposure, embedded-controller power rails and FLR
handling, so a result on one model does not carry over to another. Other Ada
IDs, professional RTX A-series and data-centre parts are not accepted. Shared
firmware across Ada chips does not establish support for untested cards.

The RTX 4090 path runs the signed firmware, checks the payload's completion marker and
required permissions, writes SS0 `0x88888888` and SS1 `0x8`, then performs FLR
before the unmodified NVIDIA driver binds. It requires the expected
effective speed-select readout before and after FLR. No patched NVIDIA driver or UEFI
application is needed. The payload uses instruction addresses derived from
published decrypted debug-booter analysis; the production booter is authenticated
and decrypted inside the GPU.

## Build and install

Clone the GPL-2.0-only source and build the packages locally so the pinned
NVIDIA firmware is downloaded and generated on your own machine:

```sh
git clone https://github.com/JustAResearcher/gpu-unlock-ada.git
cd gpu-unlock-ada
make install
```

`make install` detects the supported distribution family. Its explicit targets
are:

```sh
make install-arch   # makepkg -sif
make install-deb    # apt-get build-dep, dpkg-buildpackage, apt-get install
make install-rpm    # dnf builddep, rpmbuild, dnf install
```

`make arch`, `make deb` and `make rpm` build without installing. Packages are
built from the committed `HEAD`; Debian and RPM outputs go to `build/deb` and
`build/rpm`. Kernel headers and a working DKMS toolchain are required.

Builds fetch the pinned NVIDIA `610.43.03` source and binary inputs over HTTPS
and verify their SHA-256 hashes. `make inputs` prepares the cache;
`make firmware` generates assets from cached inputs; `make assets` does both.
The default cache is `$HOME/.cache/gpu-unlock` and can be overridden with
`CACHE=`, `INPUTS=` and `ASSETS=`. A clean-chroot Debian build needs
`GPU_UNLOCK_INPUTS` pointing to the input cache because `dpkg-buildpackage`
does not fetch inputs itself.

The source archive produced by `make dist`, the Debian source package and the
`.nosrc.rpm` contain no NVIDIA archives or generated firmware. See
[firmware/README.md](firmware/README.md) for the input and asset layout.
Prebuilt firmware packages are not distributed by this repository.

## Enable at boot

The package installs the module and firmware into the initramfs so the unlock
runs before NVIDIA binds the card. It remains inactive unless the kernel
command line opts in:

```text
gpu_unlock.targets=0000:03:00.0
gpu_unlock.targets=0000:03:00.0,0000:04:00.0
gpu_unlock.targets=all
gpu_unlock.targets=0000:01:00.0 gpu_unlock.ad103_experimental=1
```

These PCI addresses are examples. Use the full PCI names printed by `lspci -D`.
`all` selects every accepted unbound card, including legacy GA102/GA104 targets;
an explicit address is useful when selecting one RTX 4090. Do not mix `all` with
addresses. Every experimental Ada target requires its own flag from the
[experimental target table](#experimental-ada-targets), and the exact ones must
be named by their exact PCI address; `all` never selects them. Invalid entries
are logged and ignored.

The packages support these initramfs generators:

- **mkinitcpio:** `/etc/mkinitcpio.conf.d/zz-gpu-unlock.conf` puts `gpu_unlock`
  immediately before `nvidia` in `MODULES`, or last if NVIDIA is absent. Images
  are rebuilt on installation. A custom configuration selected with `-c` or a
  preset `_config` does not read drop-ins; add `gpu_unlock` before `nvidia` in
  that configuration's `MODULES` yourself.
- **initramfs-tools:**
  `/usr/share/initramfs-tools/modules.d/gpu-unlock.conf`.
- **dracut:** `/usr/lib/dracut/dracut.conf.d/gpu-unlock.conf`.

There is no `softdep nvidia pre:` rule because kmod uses only the first matching
soft dependency line, which can conflict with other packages.

**Power the machine off and start it again for the first attempt.** The module
needs a card whose firmware state is fresh before NVIDIA loads. A warm reboot
is not guaranteed to provide that state: it did on the tested
RTX 4070 Ti SUPER, where the pre-firmware WPR2 readback returned to zero and
the unlock succeeded again without an AC cut. Other boards may retain WPR2;
the module refuses to rerun the firmware sequence in that state. It also skips
cards that are already bound or powered down. If board firmware left bus
mastering enabled, the module clears it and verifies that it stayed cleared;
the later WPR2 check still rejects an already initialized card.
Loading the module manually after NVIDIA has bound the card cannot unlock it.

After validating a cold boot and a one-time warm reboot on the exact card, a
persistent bootloader entry can select the same early initramfs and exact
`gpu_unlock.*` options on every boot. A one-shot entry by itself does not
persist. Keep an unmodified stock entry available, and verify
`gpu_unlock: unlocked`, the rate readback and GPU health after each boot.
The tested canary uses a kernel-version-specific image; regenerate and
revalidate that image before removing its pinned kernel during an upgrade.

Avoid `nvidia-smi`, CUDA, NVML and `nvidia-modprobe` before a manual cold-card
attempt: they can load NVIDIA by name. Ordinary modprobe blacklist rules do not
block explicit named loads. The packaged initramfs flow avoids the need for
manual firmware staging and research parameters.

The original display-card handoff is retained: the firmware framebuffer is
removed just before FLR, and the display stays blank until `nvidia-drm` takes
over. Display handoff and multi-card operation were not part of the RTX 4090
validation.

## Check and disable

Inspect the kernel log after boot:

```sh
journalctl -k -b | grep gpu_unlock
lspci -nnk -s 0000:03:00.0  # replace the example address
```

A loaded module or successful `modprobe` return alone does not prove an unlock:
look for the selected card's `gpu_unlock: unlocked` message and the payload,
selector and post-FLR checks. The report describes independent register and
performance confirmation. The optional read-only sampler lives in
`diagnostics/peek`; the CUDA instruction benchmark is in `diagnostics/bench`.
Never load a second BAR0 sampler while `gpu_unlock` is performing its firmware
transaction.

If the module reports that firmware did not stop and its DMA buffers are being
kept, power off before trying again. The module intentionally retains buffers
that firmware may still use. Do not force-unload it in this state.

Remove `gpu_unlock.targets=` from the kernel command line to disable future
attempts. It can be removed for one boot in the bootloader editor. Removing the
packages removes the module and initramfs integration; power off to return to a
fresh hardware state.

## Development and credits

`make test` runs the host-side suite without accessing a GPU. Missing generated
firmware is reported as an explicit skip of the full transaction tests. After
`make assets`, use `make test-full` for a strict check that requires both the
pinned source and all generated assets. Override `ASSETS=` and `NVIDIA_SOURCE=`
for inputs built elsewhere. These tests mock firmware execution and device
behavior; they do not replace a hardware repeat.
See [testing and interpreting results](docs/testing.md) for the validation
levels, matched benchmark commands and recorded hardware evidence.

The kernel verifies each loaded asset against its pinned SHA-256 before claiming
the card or starting firmware DMA, including files staged into an initramfs.
The kernel's SHA-256 library (`CONFIG_CRYPTO_LIB_SHA256`) must be available.
`make -C kernel/gpu_unlock` builds against the running kernel's headers; use
`LLVM=1` when those headers support a Clang build.

The Linux module and packaging derive from
[Zagitta/gpu-unlock](https://github.com/Zagitta/gpu-unlock). The AD102 four-PLM
payload derives from
[TOTORONG/AD103_win_unlocker](https://huggingface.co/TOTORONG/AD103_win_unlocker),
including its published payload generator and debug-booter analysis. The GA102
V67 chain derives from
[WildFlash1st/cmp90hx-unlock](https://github.com/WildFlash1st/cmp90hx-unlock).
The earlier CMP unlock work, NVIDIA's published loader code, Nouveau and
[envytools](https://github.com/envytools/envytools) also underpin the loader and
investigation.

The kernel module, packaging and payload integration are GPL-2.0-only. NVIDIA
firmware remains under its own terms. See [firmware/NOTICE.md](firmware/NOTICE.md)
and [licenses/](licenses/) for provenance and preserved license notices.
