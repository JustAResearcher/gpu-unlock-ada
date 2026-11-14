# AD103/Z790 combined diagnostic probe

A read-only UEFI application that supersedes `../probe/` and
`../ownership_probe/`, doing both captures in one boot instead of two. They
were always disjoint, non-overlapping, one-time snapshots of the same
pre-driver machine state -- `../probe/` never walked the protocol-ownership
graph, `../ownership_probe/` never read PCI config space or BAR0 -- so there
was never a reason they needed separate cold boots. This halves the number
of cold-boot cycles the discovery phase for a new board needs.

## What it does and does not do

**Does**, in one run: finds the GPU over the PCI IO protocol; reads its
256-byte config space, capability list, and the ~19 BAR0 register offsets
`../profile.c`'s `Registers()` checks (including the SS0/SS1 selector pair,
printed as-is, never gated here -- see `../SS1_FINDINGS.md` for
why); reads 1 MiB from BAR0+0x300000 (the VBIOS aperture) and hashes it
against the pinned PROM hash, saving the raw bytes on a mismatch; then walks
the firmware's protocol-ownership graph for the GPU's and the physical GOP's
device paths, the keyboard USB host controller's device path, and the
identity (image base/size, file path, per-section code hashes) of the
drivers bound to console-output, GOP and PCI-IO. Everything goes to one log
file (`AD103-COMBINED-<n>.TXT`, numbered so repeated runs never collide) on
the USB stick it booted from, plus the console. The log's first lines report
which numbered run this is, as a plain reminder that a single run never
establishes anything stable -- confirm across >=2 independently confirmed
full cold boots (true power off, Fast Startup/`HiberbootEnabled` off) before
treating any register or graph value as settled.

**Does not:** write to PCI config space or BAR0, perform FLR, allocate DMA,
open any protocol exclusively or `BY_DRIVER`, call
`DisconnectController`/`ConnectController`, touch the internal disk, the
ESP, or `bootmgfw.efi`, or start Windows. It returns to the firmware boot
menu when done, every time, whether it succeeds or fails.
`GuWrite32`/`GuFlr`/`GuEnableMaster`/`GuDmaAllocate` and the selector-latch
functions do not exist anywhere in this tree (see `port_probe.h/.c`, copied
unmodified from `../probe/`), so there is no code path that could do any of
that even by mistake.

## Build and run

```powershell
.\Build-CombinedProbe.ps1
```

Produces `build\AD103-COMBINED-PROBE.EFI`. Copy it to the USB stick (same
one the real transaction `.EFI` boots from) and run it from the firmware
boot menu like any other probe. Needs GCC/MinGW-w64 and MSVC x64 linker
tools -- same toolchain as every other `.EFI` in this port; see
`Build-CombinedProbe.ps1`'s header for exact requirements.

## Provenance

`port_probe.c/.h`, `checks_probe.c/.h`, `format.c/.h`, `runtime.c/.h`,
`sha256.c/.h` copied byte-identical from `../probe/` (confirmed by `cmp` when
copied). `devpath.c/.h`, `graph.c/.h`, `pehash.c/.h` copied byte-identical
from `../ownership_probe/` (same). `probe_app.c` is new: the two source
probes' `UefiMain` bodies merged into one run, plus the run-counter reminder
and a `WriteBin`/`OpenBootVolumeRoot` pair restored from `../probe/` for the
PROM-mismatch capture path (the ownership probe's streaming-log `OpenLog`
pattern is used for the main log since it is strictly safer -- nothing is
lost if a long walk hangs partway, same reasoning as `boot_io.c`'s `GuLog`).
