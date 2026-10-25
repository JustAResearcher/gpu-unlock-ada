# Experimental Windows pre-boot port

## Hardware result: 0.6.4, 2026-10-25

The submitted RUN12 report and matching Windows benchmarks support a successful
unlock on the captured MSI RTX 4070 Ti SUPER / Z690 system. FP16-to-FP32 tensor
throughput increased from 63.283 to 140.663 TFLOP/s; BF16-to-FP32 increased from
63.873 to 140.663 TFLOP/s. Scalar FP32 remained 33.272 TFLOP/s. All benchmark
output checks passed with identical GPU, driver, VBIOS and workload metadata.

The preboot report records one successful FLR, zero remaining DMA mappings and
successful display restoration with the unlocked selectors preserved. Its final
handoff marker is pending; the completed Windows measurement supplies the runtime
evidence. Four tensor timing-spread warnings limit precision. This is one successful
test pair, not validation of repeated boots, prolonged stability or application
speedups. Security restoration has not yet been reported. See the
[complete result and evidence hashes](HARDWARE-RESULT-2026-10-25.md).

The tested 0.6.4 ZIP and EFI are unchanged. Historical sections below describe
what was known at each release; the result above supersedes their unverified
status only for this particular manual staged build and captured system.

## Same-path unused USB child replacement: 0.6.4

REPORT10 from 0.6.3 stops at stage-0 preflight with six missing USB host-child
relationships and six new relationships on the same host and bus driver.
No GPU writes, graphics-driver release or unlock attempt occurred. The six
missing children had passed unused-leaf classification. Their replacements'
paths were not logged, so the report alone cannot prove same-path USB
re-enumeration or authorize accepting the new handles.

0.6.4 permits a one-to-one replacement only after verifying that the fresh
child handle has the exact full path frozen for a previously unused leaf,
with the same host and driver. It rejects new paths, reused baseline handles,
active consumers, descendants, source-volume overlap and host identity changes.
GPU, console, input, storage, PCI state and transaction checks remain in force.
Every graph difference is checked; diagnostics print at most 64 graph entries
and count omitted entries. This bounded output does not truncate verification.

The payload, single reset and firmware graphics restoration/Windows handoff
sequence are unchanged. The subsequent RUN12 test above supplies the first
successful hardware result for this build. The updater accepts only
the exact released 0.6.3 EFI, keeps a 0.6.3 backup and preserves existing assets
and reports. Follow [START-HERE.txt](usb-staged/START-HERE.txt) for the matched
benchmark and security-restoration steps.

## Historical USB leaf retirement correction: 0.6.3

REPORT9 from 0.6.2 shows the completed unlock transaction, verified readbacks,
clean DMA teardown and successful exclusive-claim close. At the stage-4
intent boundary, the next graph check found a missing `EFI_USB2_HC_PROTOCOL`
`BY_CHILD_CONTROLLER` relationship. It refused before NVIDIA's `Supported`
or `Start` callbacks; no graphics restoration or Windows handoff was attempted.
The old logger reported only the first mismatch, so the report does not prove
this was the only graph change or identify the physical USB device.

0.6.3 admits retirement only of a baseline USB child classified as unused
within the current boot. The USB host and its driver ownership remain exact;
GPU, console, input and storage relationships remain protected. Other removals
and new relationships remain refusals. A coherent bounded snapshot masks
firmware event callbacks while collecting the graph, without logging or file
I/O at raised TPL. Every graph difference is checked; diagnostics print at most
64 graph entries and count omitted entries.
This is a narrowly scoped candidate correction, not a general USB bypass.

The GPU payload, PCIe status policy introduced in 0.6.2, single reset and
firmware graphics restoration sequence are unchanged. The fork's direct
Windows handoff and continuation after failed hardware verification are not
used. **This build has not been tested on physical hardware; Windows handoff
with the unlock remains unverified.** The updater accepts only the exact
released 0.6.2 EFI and preserves existing assets, reports and backups.
Follow [START-HERE.txt](usb-staged/START-HERE.txt), including the existing
stock/unlocked benchmark and security-restoration steps.

## Historical PCIe status correction and matched benchmark: 0.6.2

The 0.6.1 hardware report captures all 64 configuration DWORDs before and after
the successful GPU transaction. The sole difference is `0x00092930` to
`0x00002930` at offset `0x80`: PCIe Device Status changed from `0x0009` to zero,
while Device Control stayed `0x2930`. The two cleared flags are Correctable
Error Detected and Unsupported Request Detected. Interface identity, PCI I/O
attributes, selector readback and DMA cleanup passed. The application halted
at its exact configuration comparison before restoring graphics or starting
Windows. This identifies an overly strict check, not a failed Windows boot.

0.6.2 uses the same comparison in the restoration adapter and the app. Only
after a clean completed single-FLR transaction may those two previously set
flags clear. Every other configuration bit must still match. All four current
PCIe error flags and Transactions Pending must be zero on every post-reset
comparison, including when the raw value equals the original baseline; a
cleared flag reappearing is rejected. Both capability chains are validated.
Pre-transaction and rehearsal comparisons remain exact. No PCI register writes,
payload changes, additional resets or error-clearing writes were added.

This is a candidate fix for the observed refusal. **Display restoration and
unlock persistence into Windows are still unverified.** The updater accepts
only the released 0.6.1 EFI and preserves its assets, reports and backup.
Follow [START-HERE.txt](usb-staged/START-HERE.txt).

The package also includes three launchers: `Benchmark-Stock.cmd`,
`Benchmark-Unlocked.cmd` and `Compare-Benchmarks.cmd`. The user-mode benchmark
uses 64-bit Python and the installed NVIDIA CUDA driver, with no third-party
Python packages or CUDA Toolkit. The original package selected the captured GPU
by its full UUID. The public source requires an explicit full GPU UUID instead,
checks arithmetic outputs, records repeated FP16/BF16 tensor and scalar FP32
timings, and rejects mismatched comparison environments. Labels do not verify
the unlock. Instruction issue rate does not predict gaming, GEMM or inference
speedups; full application and numerical validation remain separate work.

Field references:
- [Microsoft PCIe Device Status layout](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/miniport/ns-miniport-_pci_express_device_status_register)
- [NVIDIA PTX ISA](https://docs.nvidia.com/cuda/parallel-thread-execution/index.html)

## Historical PCI-state diagnostic update 0.6.1

The supplied 0.6.0 report records actual FWSEC/SEC2 payload completion, selectors
`88888888/8`, and effective readout zero surviving one FLR. DMA cleanup and the
independent post-state checks passed. The application then stopped at the
`exclusive-release` PCI-state comparison before closing its exclusive claim,
restoring graphics, or starting Windows. The user recovered by powering off and
removing the USB, and confirmed normal Windows boot.

That report lacks the exact differing configuration value or refusal branch.
0.6.1 adds bounded diagnostics from observations already made by the checks:
interface identity, read status, attributes, and baseline/current configuration
differences. It preserves the payload, reset procedure, comparison policy and
halt behavior. It is **an evidence-gathering update, not a claimed fix**; another
run may stop at the same point. No status mask or speculative configuration
write has been added. The full-config comparator in the later app-level check
also records its exact differences.

The updater accepts only the released 0.6.0 EFI, verifies and reuses its existing
assets without downloading dependencies, and preserves all reports and backups.
Use [the current instructions](usb-staged/START-HERE.txt). Physical execution of
this new build and restoration into Windows after unlocking remain unverified.

## Staged unlock test 0.6.0

The user confirmed that the 0.5.2 rehearsal restored the display and booted
Windows. Its completed report records all six stages, restoration of the 179
ownership relationships, and unchanged PCI/cold state, with zero unlock
attempts or application MMIO writes. That establishes this board's rehearsal
result; it does not establish that the actual unlock works.

The separate `build_staged.py` inserts one shared unlock transaction between
that release/restoration sequence. It requires the original 0.4.2 capture,
the observed 0.5.1 deferred-framebuffer report, the complete 0.5.2 success
report, and the user's separate confirmation of Windows boot. It pins the
Windows loader bytes observed by the rehearsal. Updates to those inputs can
cause a preflight refusal; no pin is automatically relaxed.

This is a **first actual hardware unlock test**, armed only by `U`. It performs
GPU MMIO writes, DMA and one function-level reset. Exact ownership, complete
DMA cleanup, BME containment, selectors and effective readout are checked
before and through display restoration and once more after USB logging closes.
A failure after driver release halts for power-off; it does not retry or enter
Windows. The legacy release/reconnect path and session helper are excluded.

Read [the complete test and recovery instructions](usb-staged/START-HERE.txt).
The updater accepts only the released 0.5.2 USB image, preserves reports and a
backup, and prepares the hash-pinned NVIDIA assets locally with their notices.
No VBIOS flashing, Windows driver installation or permanent boot scheduling is
performed. Secure Boot and BitLocker changes remain explicit manual test steps.

```text
python3 windows/validate_staged.py --zig /path/to/zig --pwsh /path/to/pwsh --qemu-tool-root /path/to/tools --profile REPORT-042.TXT --topology TOPOLOGY.TXT --restoration-report REPORT-051.TXT --rehearsal-report REPORT-052.TXT --windows-boot-confirmed --assets /path/to/verified-assets --output windows/build/new-staged-validation
python3 windows/package_staged.py --output unlock-test.zip --validation windows/build/new-staged-validation/VALIDATION.json --vm-validation windows/build/new-staged-validation/vm/vm-validation.json
```

Release validation requires the actual pinned assets to pass through the real
asset loader and shared core/Windows adapter host tests. GPU responses and EFI
services are simulated. QEMU validates loading and refusal only, not the unlock.
Physical unlock execution, persistence into the NVIDIA Windows driver, and any
performance improvement remain unverified. No PR is submitted before that test.

## Firmware rehearsal 0.5.2

The separate [rehearsal package](rehearsal/README.md) uses the completed 0.4.2
metadata capture to pin the exact board, both output paths, and three firmware
drivers. It requires `R` before staged driver release/restoration and a Windows
handoff. This actively invokes firmware callbacks and can blank or hang the
display. It contains no unlock payload, Windows kernel driver, direct GPU-write
transaction, DMA setup, GPU reset or persistent boot scheduling.

Restoration invokes only the verified binding's `Supported` and `Start`
callbacks. UEFI `ConnectController` takes a priority list, not an exclusive
driver allowlist. The adapter checks complete ownership relationships between
stages, preserving unrelated keyboard/console relationships and re-resolving
replacement output handles by exact paths. Unknown changes stop the attempt.
Its observations are sequential; fingerprints do not certify firmware behavior.
Any uncertain partial failure halts without a blind retry or Windows handoff.

The 0.5.2 rehearsal completed on the captured machine; actual unlocking remains
unverified. The old unlock eligibility parser is unchanged: a 0.4.2 report does
not authorize it. The default build remains the read-only diagnostic.

To reproduce a reviewed capture-bound rehearsal:

```text
python3 windows/validate_rehearsal.py --zig /path/to/zig --pwsh /path/to/pwsh --qemu-tool-root /path/to/extracted-tools --profile REPORT.TXT --topology TOPOLOGY.TXT --restoration-report REPORT-051.TXT --output windows/build/new-rehearsal-validation
python3 windows/package_rehearsal.py --output rehearsal.zip --validation windows/build/new-rehearsal-validation/VALIDATION.json --vm-validation windows/build/new-rehearsal-validation/vm/vm-validation.json
```

Host tests use synthetic firmware and storage. QEMU tests only absent-target and
storage refusal/return, since it has no target Ada GPU. The package includes the
exact validation scope, generated identity headers, and corresponding source.

## USB diagnostic 0.4.2

0.4.2 captures the missing firmware ownership and driver-identity evidence in
`TOPOLOGY.TXT`, alongside `REPORT.TXT`. It runs the new capture before the old
physical-display/child admission checks, so an EDID-only child does not prevent
the capture. It records two bounded inventories of non-passive ownership edges,
the related output and virtual-console endpoints and paths, PCI ancestry, and
driver-binding/loaded-image metadata. Executable PE sections are hashed, with
supported DIR64 relocations normalized in private scratch memory. Unsupported
formats or unreadable ranges are explicitly incomplete evidence. No driver
callback is invoked by the capture, and no code binary is dumped.

These are sequential observations, not an atomic snapshot or trusted driver
certification. `stable_active_graph=0` preserves both graphs for review; it never
authorizes release. Even a fully completed 0.4.2 diagnostic has `eligible=0`.
The unlock/profile policy remains blocked pending review and implementation.
`Update-USB.cmd` accepts either exact released 0.4 or 0.4.1 EFI and preserves
reports and a backup. That historical diagnostic package's separate guide is
not included in this pinned source snapshot; the current staged procedure is
documented in [START-HERE.txt](usb-staged/START-HERE.txt).

### Previous diagnostic findings

0.4.1 adds exact child-admission failure reasons and bounded read-only protocol
and consumer observations. A real 0.4 run passed target, memory, cold state,
PROM and physical-display checks but refused a second firmware child. That
status alone did not identify the child. The update preserves that refusal
policy and cannot be used to authorize an experimental unlock. `Update-USB.cmd`
originally updated only the exact previous 0.4 diagnostic.

A subsequent 0.4.1 capture identifies the rejected child as EDID-only and also
reveals downstream console consumers on the physical display. The offline
ownership analysis informed the later staged restoration checks described
above. `analyze_diagnostic.py` interprets existing
0.4.1 reports without hardware access. Its observations never authorize unlocking;
the failed report remains ineligible for an experimental profile.

The historical separate USB test was prepared with `Prepare-USB.cmd`.
It handles the user's existing Secure Boot and BitLocker configuration explicitly;
the copy tool never changes either setting. BitLocker need not be decrypted for
this manual diagnostic procedure. Its temporary suspension and resumption remain
physical-machine test steps, not capabilities validated by our emulation.

The diagnostic build now excludes the shared unlock transaction, firmware loader,
and session source. Its write adapters and controller-claim/asset-load functions
also deny activation. Separate mocked EFI harnesses exercise the diagnostic
core, metadata capture, PE parser and capture writer, plus hardware refusals and
logging failures. Actual QEMU/OVMF boots verify
the absent-GPU refusal, rejection of missing log storage/non-USB devices, and
return to firmware. These tests do not establish real GPU compatibility.

To reproduce the tested release, provide Zig 0.14.1, PowerShell 7, and QEMU/OVMF
plus mtools/dosfstools (system packages or an extracted tool directory):

```text
python3 windows/validate_diagnostic.py --zig /path/to/zig --pwsh /path/to/pwsh --qemu-tool-root /path/to/extracted-tools --output windows/build/new-validation
```

Historical diagnostic packaging references additional presentation documents
not retained here. Use the staged validation and packaging workflow above for
this source snapshot; the legacy diagnostic packaging command is not advertised
as reproducible from this reduced tree.

## Control Center and experimental unlock source

The earlier Control Center preview is not included. Its two protocol helper
files remain because the retained host suite references them. Automatic session
handoff has not been hardware validated, and the staged build neither enables
nor links it. The manual staged build's result above does not validate automation.

This is an experimental Windows port for **RTX 4070 Ti SUPER, PCI ID
`10de:2705` (AD103, 16 GiB)**. It runs as an x64 UEFI application from a USB
before Windows starts, then hands off to the existing Windows Boot Manager and
stock NVIDIA driver. It is not a Windows kernel driver or a WSL unlocker.
Only the captured manual 0.6.4 staged configuration has the hardware evidence
described above; this does not establish support for other configurations.

The UEFI infrastructure is adapted from
[TOTORONG/AD103_win_unlocker](https://huggingface.co/TOTORONG/AD103_win_unlocker/tree/51df9c17ec89eb8486df52cc9079dde4bd1f9974/source).
The transaction, VBIOS parser, firmware layout and payload come from this
repository's shared Linux implementation. See [attribution](THIRD_PARTY_NOTICES.md).

## First test

The default build is a **read-only GPU diagnostic**. GPU writes, DMA allocation,
GPU reset, firmware execution and driver detach cannot be reached in this mode.
It writes its report to the boot USB. It does not flash a VBIOS or change boot
variables, the Windows partition, Secure Boot, BitLocker, or the installed driver.

Use [START-HERE.txt](START-HERE.txt) with the prepared test ZIP. Boot from an
existing FAT32 USB using the one-time UEFI menu. The included PowerShell helper
only copies the diagnostic to a checked USB destination; it never formats a disk
or overwrites an existing removable-media bootloader. Manual copying is also
supported. The unsigned EFI normally requires Secure Boot off. Have your
BitLocker/device-encryption recovery key before changing firmware security
settings; these changes can trigger recovery. Do not clear the TPM or Secure
Boot keys.

Return `GPU-UNLOCK-ADA/RUNxx/REPORT.TXT` and `TOPOLOGY.TXT` even on a refusal. They contain
PCI configuration, board identifiers, register samples, ROM SHA-256 hashes and
UEFI driver metadata, paths, code hashes and callback addresses. They contain no
ROM/driver-code dump or Windows recovery key.

This initial port requires one target GPU, exactly one physical GOP display on
that target, and an unambiguous firmware driver owner. A second active GPU/GOP,
an inaccessible option ROM, an unfamiliar capability layout, or an already
initialized GPU can cause refusal. Multiple Windows boot volumes also block the
experimental handoff. These restrictions are deliberate; do not edit identifiers
or remove checks just to get past a refusal. A successful diagnostic establishes
an observed profile, not proof that an unlock will work.

## Build on Windows or Linux

Install Python 3.10+ and **Zig 0.14.1**. No EDK2 toolchain install is needed; the
small transitive set of EDK2 headers is included with its license. From the
repository root (use `python3` instead of `py` on Linux):

```powershell
py windows/build.py --zig C:\tools\zig-0.14.1\zig.exe
```

Output is `windows/build/diagnostic/BOOTX64.EFI` with `build-result.json` containing
the image and source hashes. Copy the EFI to `EFI/BOOT/BOOTX64.EFI` on a FAT32 USB
and create `GPU-UNLOCK-ADA` at the USB root. The diagnostic needs no NVIDIA assets.

After reviewing a **completed eligible diagnostic from the exact board**, the
separate experimental build is explicit:

The current 0.4.2 capture intentionally never produces such an eligible report.
The following is research-source documentation, not a next test step; the
experimental release path still needs the console-ownership adapter described
in `OWNERSHIP-ANALYSIS.md`.

```powershell
py windows/build.py --zig C:\tools\zig-0.14.1\zig.exe --experimental --profile REPORT.TXT
```

The parser rejects missing/duplicate fields, nonzero GPU writes, failed reports,
other devices and unsupported layouts. The resulting image pins the subsystem,
BDF, BAR0 mapping, chip ID, VRAM size, live PROM hash and EFI option-ROM hash.
Those properties are rechecked before and after the target's firmware-driver
release. The report is not authenticated; never substitute someone else's report
or the synthetic test fixture. Hardware, firmware or PCI layout changes require a
new diagnostic and review. Experimental images built with test fixtures are not
usable on hardware and must never be distributed as an unlock test.

The experimental image additionally needs the exact five assets under
`GPU-UNLOCK-ADA/ASSETS` on the USB. Prepare them locally with:

```powershell
py -m pip install zstandard==0.23.0
py windows/prepare_assets.py --output E:\GPU-UNLOCK-ADA\ASSETS
```

This downloads the existing pinned source and binary archives, checks SHA-256,
and extracts only named regular members. It does not execute or install their
contents. The output directory must be empty. `--offline` uses already verified
cached inputs. NVIDIA binary firmware is not included in the test ZIP; its
license and the repository's provenance notice accompany local extraction.

## Experimental transaction

Before asking for `U`, the app verifies the target, display/owner, PCI capability
layout, cold state, PROM, firmware hashes and a single preloaded Windows Boot
Manager. Escape or a 30-second timeout cancels. Only an explicit `U` begins the
driver-release path. The app releases the exact observed firmware driver, claims
the target exclusively, checks it again and calls the shared unlock transaction.

Fallible UEFI PCI/MMIO operations latch a fault. DMA lifetime is tied to the
shared transaction's start/quiesce callbacks. Failed containment or uncertain
cleanup retains memory and halts instead of continuing into Windows. On success,
the app requires the full-rate selectors, effective readout and FEAT permission
state to survive FLR and GOP restoration before starting Windows. It makes no
persistent boot entry, flash or physical-fuse write.

An experimental attempt can hang or black-screen. If it stops after driver
release, power off fully, remove the USB and boot normally. This is a recovery
procedure, not a guarantee against hardware damage. The read-only first test does
not run that transaction.

## Hardware acceptance before a PR

Do not describe the port as working based on compilation, the diagnostic, or a
single register log. The user's explicit test comes before publishing a PR.
Required evidence is a successful board-matched transaction, Windows driver
startup, numerical correctness, and a comparable stock/unlocked benchmark on
the same GPU and driver. Preserve logs from both boots and restore stock with a
full power-off and normal boot without the USB.

The repository's existing CUDA programs can be built from an x64 Visual Studio
developer command prompt with a matching CUDA toolkit. They are provided as
source; Windows compilation and execution still require verification on the
test machine. First select the target by its UUID from `nvidia-smi -L` so a second
GPU is not accidentally benchmarked:

```bat
set CUDA_VISIBLE_DEVICES=GPU-REPLACE-WITH-YOUR-4070-TI-SUPER-UUID
nvcc -O3 -arch=sm_89 diagnostics\bench\mma_bench.cu -o mma_bench.exe
nvcc -O3 -std=c++17 -arch=sm_89 diagnostics\bench\validate_gemm.cu -lcublas -o validate_gemm.exe
validate_gemm.exe
mma_bench.exe 1000 3
```

Record the printed GPU name, driver, clocks, temperature and power before and
after with the same settings. Windows display-driver timeouts are possible with
long-running kernels; do not raise the benchmark iteration count blindly or
disable Windows timeout protections. FP16/BF16 issue-rate gains are not a claim
of general application, gaming or FP8 gains.

## Host validation

```sh
make test
python3 -m unittest discover -s windows/tests -v
```

The Windows tests compile the real UEFI adapters and handoff checks against fake
EFI services in both modes. They cover write gates, wrong target/profile,
transport failures, post-reset selector loss, in-flight DMA retention and unmap
failure. Report tests cover malformed and failed inputs. Both PE images are
checked for the x64 EFI subsystem, relocations, NX/ASLR and absence of imports.
The diagnostic was also booted under QEMU/OVMF with an emulated FAT32 USB and
correctly logged `stage=target`, unsupported, zero GPU writes. This VM test
does not emulate an Ada GPU or validate Windows handoff.

These host/VM checks do not establish hardware success or guarantee recovery.
The separate hardware result above records the observed Windows test. Source
and binary hashes plus the exact local checks are included in the prepared
test package.
