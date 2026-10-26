# Experimental MSI AD103 Windows reference

This source-only snapshot preserves the manual USB UEFI unlock tested on one
MSI RTX 4070 Ti SUPER (`10de:2705`, subsystem `1462:e130`) and MSI MPG Z690 EDGE
WIFI DDR4 system. Version 0.6.4 completed the preboot transaction and graphics
restoration, followed by a matched Windows benchmark pair on NVIDIA 621.01.

| Instruction workload | Stock TFLOP/s | After TFLOP/s | Ratio |
|---|---:|---:|---:|
| FP16 with FP32 accumulation | 63.283 | 140.663 | 2.223x |
| BF16 with FP32 accumulation | 63.873 | 140.663 | 2.202x |
| Scalar FP32 control | 33.272 | 33.272 | 1.000x |

All checked arithmetic outputs passed. Tensor timing variation was substantial;
these are measurements of one run pair, not a promise of the exact gain. Repeat
boot reliability, prolonged stability and application speedups remain untested.
See the [hardware result and evidence hashes](windows/HARDWARE-RESULT-2026-10-25.md).
The report's `windows_handoff=pending` line is a preboot checkpoint; the separate
Windows measurements supply the runtime evidence.

## Scope

The staged image uses captured board, ROM, firmware-driver, device-path and
Windows-loader identities. A matching GPU model alone does not authorize a
different system. It requires a manual `U`, performs one transaction and one
FLR, checks cleanup and restores the captured graphics drivers before Windows.
The tested fix admits only unused USB child replacements with exact frozen
paths and ownership; it does not broadly ignore topology changes.

The process writes GPU registers and invokes firmware callbacks. Earlier
candidates black-screened or refused to continue. No native Windows driver,
VBIOS flashing, replacement NVIDIA driver or persistent boot schedule is part
of this staged image. Secure Boot must be off for the unsigned test; BitLocker
handling and subsequent security restoration remain manual. This is a reference
for review, not a general-purpose Windows installer.

## Pinned source layout

This directory is a self-contained source root. Its nested `windows/`,
`kernel/gpu_unlock/`, `firmware/` and `tests/` paths preserve the tested release's
relative build layout. The pinned core is intentionally independent of the live
Linux core above this subtree. No Linux source or existing AD104 Windows port
is modified by this snapshot.

[provenance.json](provenance.json) identifies the source revision and original
file hashes. All compiled C/header inputs are retained byte-for-byte. The
benchmark's public interface now requires an explicitly selected GPU UUID;
the original evidence retains its original runner hash. Changing that interface
does not rewrite or re-attest the historical measurements.

The source includes the earlier diagnostic/rehearsal and C session-helper files
because the existing build modes and host test suite refer to them. The staged
build defines `GU_SESSION_HELPER=0` and does not link session machinery. The
unused Windows boot installer and firmware-variable writer have been removed;
the session protocol test uses synthetic packets written only to its temporary
test directory. No automatic boot installer is distributed. The withdrawn
Windows kernel filter is not included. The unused
`GU_NATIVE_WINDOWS` preprocessor branch in the pinned diagnostic header is
preserved only for source fidelity; it is not enabled by this build.

## Build and validate

Run the existing commands from this directory, not the repository root.
The [port documentation](windows/README.md) describes diagnostics, private
capture prerequisites and the staged validation command. The
[staged test instructions](windows/usb-staged/START-HERE.txt) are preserved as
the original 0.6.4 release instructions; statements there about first testing
describe their publication date. Use the hardware-result document for current
evidence.

The public benchmark launchers now prompt for an explicit full GPU UUID from
`nvidia-smi -L`; select the same GPU for both measurements. This supersedes the
original package's implicit target selection in the preserved instructions.
See the [current benchmark guide](windows/benchmark/README.md) for the command
line and PowerShell parameters.

The staged builder needs Zig 0.14.1, exact verified firmware assets and the
private board/topology/restoration/rehearsal captures. Full validation also
needs PowerShell and QEMU/OVMF. Captures, generated headers, firmware binaries,
ROM dumps and EFI executables are deliberately excluded from Git. Missing
captures are not grounds to disable identity checks or invent a profile.

Rebuilding with the original private inputs produced the exact tested image:

```text
0.6.4-unlock-test BOOTX64.EFI: 918528 bytes
SHA256: 21999ec27045a9f441cf25dcac396de69d79fca2b11a92abb1baa36231c63da5
```

See [integration-validation.json](integration-validation.json) for the recorded
host/USB/VM results and reproducibility check. Those automated checks simulate
hardware or test refusal paths; they do not establish additional physical boots.

## Attribution and license

The code retains its per-file licenses; the application and core use
[GPL-2.0-only](LICENSE). [Port attribution](windows/THIRD_PARTY_NOTICES.md),
[firmware provenance](firmware/NOTICE.md), EDK2 and NVIDIA/CMP40HX notices are
included. The port infrastructure derives from TOTORONG's
`AD103_win_unlocker` revision `51df9c17ec89eb8486df52cc9079dde4bd1f9974`;
the transaction derives from this project's pinned core. No endorsement by
those authors is implied.
