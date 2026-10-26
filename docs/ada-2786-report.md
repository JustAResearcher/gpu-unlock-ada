# Gigabyte RTX 4070 (10de:2786): first native Linux unlock

On 2026-10-24, this Gigabyte RTX 4070 completed a native Linux cold-boot unlock on CachyOS kernel 7.3.0-5-cachyos with NVIDIA open 615.78.08. FP16/FP32 throughput increased from 67.0 to 133.8 TF/s (+99.7%) and BF16/FP32 from 66.9 to 133.7 TF/s (+99.9%) using the same sm_89 binary and 20,000 iterations / three repetitions. Control modes remained essentially unchanged. The independent RM getter changed FMLA32, IMLA2 and IMLA4 from 1 to 0.

The user reported full shutdown and boot into the dedicated GRUB test entry. Kernel logs confirm the payload marker, SEC2 halt, required permissions, post-FLR selector persistence, zero effective readout and unlock completion. A post-benchmark current-boot scan found no NVRM Xid, GPU-fallen-off or retained-DMA errors. This is one successful cold transaction on one board; cold A/B/A reproduction, warm reboot reliability, application gains and long-term stability remain unqualified.

See [the matched comparison](evidence/ada2786/comparison.md) and [evidence inventory](evidence/ada2786/README.md). The sections below preserve initial stock characterization and offline ROM analysis, followed by the native test result. The running kernel also logged a module signing-key trust warning: the signature was valid against the existing DKMS certificate, but the kernel did not accept its trust chain. Secure Boot was already disabled and remains so; Secure-Boot-enabled operation is not validated.

## Identity

| Source | Observed identity |
|---|---|
| PCI | NVIDIA 10de:2786, revision a1 |
| Subsystem | Gigabyte 1458:40ee |
| Live PMC_BOOT_0 | 0x194000a1 (AD104 family) |
| CUDA | RTX 4070, 46 SMs, sm_89 |
| VBIOS identity string | GV-N4070WF3OC-12GD/F31/0F0C |
| VBIOS version string | 95.04.3E.08.BE; matches live NVML version |
| Kernel | 7.3.0-5-cachyos, CONFIG_CC_IS_CLANG=y, CONFIG_LTO_CLANG_THIN=y |
| NVIDIA packages | nvidia-utils 615.78.08-2; prebuilt linux-cachyos-nvidia-open 7.3.0-5 |

The board was described by its owner as AD104-250. The measurements identify the AD104 family and exact PCI/SKU; they do not independently read the physical die-marking suffix.

## Live stock registers and rates

| Register | Offset | Value |
|---|---|---|
| FEAT_OVR_PLM | 0x00823804 | 0xffffff8f |
| FEAT_READOUT_0 | 0x00823814 | 0x00000233 |
| FEAT_READOUT_1 | 0x00823818 | 0x00410080 |
| SS0 | 0x0082381c | 0x20022042 |
| SS1 | 0x00823820 | 0x00000003 |
| SEC2 Booter ucode 3 fuse | 0x00824148 | 0x00000001 |
| FWSEC ucode 9 fuse | 0x008241e0 | 0x00000001 |
| WPR2 low / high | 0x001fa824 / 0x001fa828 | 0x02f5d000 / 0x02ff8e00 |

RM getter 0x20801230 returned FMLA32=1, IMLA2=1, IMLA4=1; the other six selectors were 0. In the RM enum, 1 is half-rate and 0 is full-rate. FEAT_READOUT_1's raw 3-bit fields are `0 0 2 0 0 2 0 2 0`; do not treat its raw field indices/encodings as established equivalents of the RM enum. The AD102 register report explicitly leaves that mapping unestablished.

The nvidia driver was bound, GPU runtime-active, and WPR2 enabled during sampling. These values are post-driver stock state, not cold pre-driver state.

Four accesses returned inaccessible values:

| Offset | Register | Result |
|---|---|---|
| 0x00110100 | GFW/GSP falcon CPUCTL | 0xbadf5720 |
| 0x0082007c | DISABLE_FUSE_PROGRAM | 0xbadf5040 |
| 0x00409664 | FECS selector copy | 0xbadf5040 |
| 0x00409668 | FECS readout copy | 0xbadf5040 |

Thus the requested blanket zero-0xbadf validation fails. However, [the existing AD102 register reference](../rtx4090-register-report.md#5-register-reference-and-earlier-observations) records FECS 0xbadf5040 cold and driver-bound on its tested card. These FECS results alone are not evidence that this AD104 cannot use the unlock. CPUCTL/other inaccessible registers still require contextual investigation; no permissions were bypassed or registers written to investigate them. The sample does not prove permanent hardware faults. The captured kernel window has no additional Xid/AER entry.

## Supplied ROM layout

The supplied file is 2,048,000 bytes, begins with `NVGI`, and has SHA-256:

`ad166cab91ee607b3e5cf2f2198c617a857de5908972770ae40b5ca81d08854f`

It is not a raw 1 MiB PROM capture and must not be passed directly as one. The kernel parser correctly rejects the whole file with -EINVAL due to its input limit/layout. No parser guard was relaxed. A structural scan finds two complete option-ROM chains starting at container offsets 0x9400 and 0xe9400. Each is 0x99c00 bytes (629,760), includes code types 0, 3, 0xe0, 0xe0, identifies PCI 10de:2786, and parses successfully. The two logical ROM chains are byte-identical:

`bc49205f188f1409b1bec45acadd125488a74d4a36265e4b22c7181f24d3ba2a`

Chain bounds were determined from the PCI/NVIDIA image headers and final-image flag, not arbitrary truncation or zero padding. The NVGI container's full metadata/selection policy was not reverse engineered. Matching PCI/board/version strongly associates the supplied image with this board, but does not prove byte-for-byte equality to the live PROM.

## FWSEC and Booter compatibility

All offsets below are relative to an embedded logical ROM, not the outer file.

| FWSEC property | Parsed value |
|---|---|
| Descriptor offset | 0x4555c |
| Descriptor version | 3 |
| Firmware image offset / size | 0x45888 / 0x10500 |
| IMEM size / DMEM size | 0xf700 / 0xd80 |
| IMEM PA / VA; DMEM PA | 0 / 0; 0 |
| Data offset | 0xf700 |
| PKC offset / interface offset | 0xb24 / 0x1c |
| Engine mask | 0x400 (accepted GSP mask) |
| ucode_id | 9 |
| Signature versions / count | 0x3 / 2; versions 0 and 1 |
| Signature table offset / bytes | 0x45588 / 0x300 |
| Selected signature for fuse version 1 | table offset 0x180, ROM offset 0x45708, 384 bytes |
| FRTS mapper | version 3, 64 bytes; command offset 0xd40, size 64 |

FWSEC image SHA-256: `49fc3b561ad6087f597d19da8aadaccf0d967c18b76f464c8a39a01911e5bfaa`.

Selected FWSEC signature SHA-256: `04e022518e4782d4e4dc7b4f79cff4a84c5f3b4e239b905dc300ada10779d0f5`.

The actual ucode_id confirms the measured FWSEC fuse address: 0x008241c0 + (9-1)*4 = 0x008241e0. Raw fuse 1 becomes version 1 under gpu_unlock_fuse_version(). Bit 1 is present in sig_versions=3. The unmodified gpu_unlock_fwsec_sig_offset() returns success with table offset 384. The engine mask and rounded DMA spans also satisfy the existing unlock checks.

The unmodified gpu_unlock_fwsec_patch_frts() successfully prepared an image copy in host RAM using a synthetic aligned test address 0x100000. This validates the interface/mapper layout only. No image was saved, uploaded or executed; no FRTS hardware allocation was made.

The pinned 610.43.03 booter_load_ad102.bin has ucode_id=3, two signatures and SHA-256 `6ee05b20e1dc244c08c052f36ef28469b8d09f4955d1fe33e48a179e11a7fd49`. Its matching fuse address is 0x00824148. Raw fuse 1 selects version 1 / Booter signature slot 0 (reverse ordering). gpu_unlock_booter_validate(GU_BOOTER_AD10X) and gpu_unlock_booter_patch() both succeed against an in-memory copy.

**Conclusion: the supplied board FWSEC and pinned Booter pass the project's version-selection/layout checks against the live fuse values.** This does not cryptographically authenticate signatures, execute firmware or establish runtime unlock success. No change to vbios_610.c is needed for this image.

The existing Windows AD104 evidence is for a different MSI RTX 4070 Ti (10de:2782, subsystem 1462:5132), with FWSEC image offset 0x45488 and IMEM size 0xf500. Its board-specific pins must not be reused for this Gigabyte 10de:2786 card. Both descriptors use version mask 3 and DMEM size 0xd80, but their board firmware bytes/geometry differ.

## Baseline and project state

Native register-resident benchmark: 20,000 iterations, three repetitions, nvcc -O3 -arch=sm_89.

| Mode | Best TF/s (integer modes count operations) |
|---|---:|
| f16/f16 | 133.9 |
| f16/f32 | 67.0 |
| bf16/f32 | 66.9 |
| s8/s32 | 267.4 |
| dp4a | 66.6 |
| ffma | 32.1 |

The local feat/ad104-rtx4070 branch admits exact PCI 0x2786 behind ad103_experimental=1 and a single exact BDF, selects the AD102-family firmware path, passed 30/30 tests with real pinned assets/source at the hardware test. The publication suite adds a boot-option regression test and passes 31/31 with zero skips; both Arch packages also build. A mocked AD104 transaction passes; it does not use this supplied board ROM as a hardware execution fixture. A user-authorized isolated test initramfs and optional GRUB entry were installed with a copied stock kernel and the existing DKMS signing key. The user subsequently selected that entry after a full shutdown, and the first native unlock and benchmark passed. No global gpu-unlock package, module-tree installation, normal preset or default entry change was made.

The cold test has now supplied the required pre-driver access, marker/HALT/permission, post-FLR selector/readout and independent RM/throughput evidence on this exact board. Cold entry SS0 was 0x20022043 versus 0x20022042 in the earlier driver-bound sampler; record timing/state rather than treating these as interchangeable. The broad stock sampler still returned inaccessible observational registers, but the actual unlock required checks passed. Next useful project evidence is a cold A/B/A reproduction, a one-time warm reboot repeat and representative application/stability measurements, with approval before further boot changes.

## Tested initramfs setup

The test used a dedicated GRUB entry and an isolated image with a copied stock
kernel; normal presets/defaults were preserved. Only the test entry specified:

```text
gpu_unlock.targets=0000:01:00.0 gpu_unlock.ad103_experimental=1
```

The merged module admits 2786 under `gpu_unlock.ad104_experimental=1`
instead, alongside the other AD104 IDs; `ad103_experimental` no longer
selects it.

The systemd initramfs explicitly loaded `gpu_unlock nvidia nvidia_modeset
nvidia_uvm nvidia_drm` in that order. An image-only drop-in for
`systemd-udev-trigger.service` ensured coldplug did not race explicit module
loading:

```ini
[Unit]
After=systemd-modules-load.service
```

This ordering was part of the hardware test setup, not a new globally installed
hook in this PR. AMD Raphael iGPU rendering remained enabled; the boot log
identifies the RTX 4070 as boot VGA, and the owner reported a normal desktop
return with healthy nvidia-smi. Secure Boot was disabled before testing and was
not changed; the DKMS certificate was not trusted by the kernel. The signed
module's out-of-tree/signature taints are retained in the log excerpt.

## Software checks and offline analysis limits

Generate the pinned inputs and run the full suite from the repository root:

```bash
make assets CACHE="$PWD/build/validation"
make test-full CACHE="$PWD/build/validation"
make -C kernel/gpu_unlock LLVM=1
```

PyCryptodome is optional for the offline decoder tests; the publication validation
used an isolated virtual environment with it installed. No NVIDIA library or
system Python package was modified. Arch packages were built without installation.
The local PKGBUILD now supplies the extracted NVIDIA source directory to
`make test-full`, so real-source checks cannot silently skip.

The supplied ROM was opened read-only, and its hash remained unchanged. Offline
checks called the unmodified `gpu_unlock_vbios_fwsec()`,
`gpu_unlock_fwsec_sig_offset()`, `gpu_unlock_fwsec_patch_frts()`,
`gpu_unlock_booter_validate()` and `gpu_unlock_booter_patch()` from the Linux
source, against host-memory copies only. The parse diagnostics and outputs are
retained in the evidence directory. No ROM, NVIDIA firmware, compiled module,
EFI binary, signing key or benchmark executable is included.

The container selection policy and byte-for-byte equality to live PROM were not
established by offline inspection. The later native boot independently parsed
FWSEC from live PROM with the same descriptor offset and geometry and completed
the firmware transaction. A matching layout does not establish full-ROM equality.

## Native cold-boot result

See [cold-boot-validation.json](evidence/ada2786/cold-boot-validation.json) for all six before/after modes and individual runs. Baseline and after-test configured power limit/clock point samples match at 216 W, 2100 MHz graphics and 10501 MHz memory; no continuous telemetry trace was collected. After-test pre-benchmark temperature was 34 C. The stock benchmark binary SHA-256 matches exactly. Only one invocation per state was collected; each invocation contains three timed repetitions per mode.

## Publication provenance

The hardware-tested source was commit `0eb54148094f26e1ae49cdc894db555639ca4172`.
[software-validation.json](evidence/ada2786/software-validation.json) records the
signed hardware module hash and source-version identity, final software test
coverage and the separate publication module build. After hardware testing,
kernel changes are comments, module description and parameter help only;
admission and transaction behavior are unchanged. No new cold boot of the
publication metadata build is claimed.
