# RTX 4070 SUPER: successful Windows unlock and matched GEMM results

On 2026-10-25, a Gigabyte RTX 4070 SUPER (`10de:2783`, subsystem `1458:413b`)
completed the Tensor unlock, returned to Windows 11 and ran CUDA successfully.
With the power limit held at 220 W, cuBLAS FP16/FP32 GEMM throughput increased
49.5% and BF16/FP32 increased 43.1%. This adds a working Windows case for the
4070 SUPER to the existing AD104 port.

## Matrix measurements

| cuBLAS mode | Stock TFLOP/s | Unlocked TFLOP/s | Change |
|---|---:|---:|---:|
| FP32 pedantic | 26.02 | 26.18 | +0.6% |
| FP16 inputs, FP32 accumulation | 78.63 | 117.53 | +49.5% |
| BF16 inputs, FP32 accumulation | 78.97 | 113.00 | +43.1% |

Each cell is the median of three fresh-process runs. Each run used 4096-square
matrices, 15 timing samples of five products, one second of warmup, seed
20261024 and cuBLAS math mode 16. The same benchmark bytes, PyTorch/cuBLAS DLLs,
driver, VBIOS and 220 W power limit were used before and after the unlock.
The [six measurement files](evidence/ad104-4070-super/README.md) are published
unchanged, with every timing sample and their software/output hashes.

All six runs passed the full 256-square CPU reference and the 1,024 sampled
elements of each large product. Complete output SHA-256 hashes matched across
both states. FP32 stayed near its baseline; its 0.6% change is within ordinary
run variation. The desktop remained active, clocks varied, and FP16/BF16
post-unlock runs span 110–127 TFLOP/s. These are measured GEMM rates from one
physical trial, not an instruction-issue benchmark or a universal 2× result.
Repeated cold A/B/A and long-duration stability tests remain open.

## System and physical-run evidence

The system used an MSI MAG Z790 TOMAHAWK WIFI (MS-7D91), BIOS H.E0, an Intel
Core i5-13600KF, Windows 11 Pro build 26300, NVIDIA driver 617.42 and VBIOS
95.04.69.00.e7. The GPU reported 12,282 MiB. The benchmark environment was
Python 3.14.6, PyTorch 2.13.0+cu132, CUDA 13.2 and cuBLAS 130400.

The operator chose the existing internal ESP for convenience, avoiding the
need to prepare a USB drive, then cold-booted the EFI and pressed `U`. USB boot
has not been tested on this board.
The [transaction log](evidence/ad104-4070-super/RUN0001.sanitized.txt) records:

- The Windows image was staged from the original ESP and passed its SHA-256 check.
- Native firmware NTFS access read the 84,277,400-byte GSP file from the Windows partition.
- The text, graphics-console and NVIDIA firmware drivers detached successfully.
- The payload completed with marker `0x67b00001`; required permission and HALT checks passed.
- SS0 changed from `0x72543027` to `0x88888888`, and SS1 from `0x4` to `0x8`.
- Those selectors survived one FLR. The final rate readout was zero with `full=1`.
- The transaction completed with `safe=1`, `retained=0`, `quiesced=1`, `flrs=1` and `DMA_live=0`.

Windows then loaded its normal NVIDIA driver and completed the matrix runs.
Device Manager reported the GPU as OK with problem code zero. The post-benchmark
event query found no NVIDIA, Display or WHEA warnings/errors since that boot.
Sampled GPU temperature during the unlocked matrix runs peaked at 56°C.
The one-time firmware selection was consumed, Windows Boot Manager stayed
first, and its binary hash was unchanged.

The log ends before the last Windows launch check. The Windows/CUDA results
provide the subsequent handoff evidence. A Falcon pre-reset timeout in the log
follows the existing hardware-bug workaround; it is followed by successful
reset/completion checks. WPR permission registers returning to their original
values after FLR also follow the transaction's existing checks. The full log
retains these lines so the successful result can be inspected in context.

## What changed for this card

The base is `JustAResearcher/gpu-unlock-ada@4e7fefcc4923f9e083d5324dc75a7d2f906af77f`,
including Latence.app's `windows/ad104-msi` reference port. This contribution
adds a separate hash-pinned overlay and leaves that port's behavior unchanged.

**Cold-state profile.** The collector reads the physical 1 MiB PROM and all
4 KiB of PCI configuration. The cached GOP ROM alone was only 150,528 bytes
and lacked the full PROM layout needed by the FWSEC parser. The new profile
checks the captured FWSEC geometry, card identity, assets and 19 required cold
registers. SS1 starts at `4`; SS0 is captured and checked for consistency.

**Resizable BAR.** The captured extended capability has three controls at
`0xbb8`, `0xbc0` and `0xbc8`, for 16 MiB, 16 GiB and 32 MiB BARs. The port
restores and reads back all three controls before restoring BAR/decode state.
It uses the six-bit size field (`13:8`, mask `0x3f00`) from the EDK II PCIe
definition, preserves the other fields and rejects immutable-field drift.
Bus mastering stays clear throughout post-FLR restoration.

**Firmware driver ownership.** The MSI Z790 capture supplied three different
code hashes and a 121-edge ownership graph. The port identifies the text,
graphics-console and NVIDIA driver instances at runtime and checks their
code/entry-point profiles before releasing them. It preserves the USB input
path while the display drivers detach. No raw handle address is baked into
the implementation.

**Internal-disk boot (Experimental).** The EFI stages the pinned Windows image
from the ESP, reads the large GSP through the firmware's native NTFS filesystem protocol,
and writes a new log under its own ESP directory. It has no boot-variable
writer. The Windows-side setup creates a persistent firmware menu entry and
selects it for one boot with `bootsequence`; consuming that selection leaves
the entry and on-disk files in place. The operator controls the cold boot and
unlock key. The current profile requires its pinned internal launch volume
and rejects USB volumes. A USB variant would need a reviewed boot-I/O profile
and physical validation.

## Rebuild and reproduce

The [workflow](../windows/ad104-4070-super/README.md) covers source assembly,
read-only capture, local firmware generation, profile configuration, host
checks, manual boot and matched matrix measurements. It includes a disk-only
route, chosen for convenience, with no ESP resizing on the tested layout.
The [rollback section](../windows/ad104-4070-super/README.md#8-rollback-and-remove-the-experimental-setup)
covers deleting the trial boot entry, EFI files, captures and the GSP file,
and restoring firmware settings.

The physical run used a 98,816-byte EFI with SHA-256
`b20394d76f7b7dc706e7d7ba893a4ca56b958db66232b7b22ed60a63d28acf84`.
Rebuilding the public source with the author's excluded local partition/path
header reproduces those exact bytes. The source, private-input hashes, host
test results and evidence transformations are recorded in the
[evidence manifest](evidence/ad104-4070-super/README.md).

The original EFI's “hardware unvalidated” banner describes its state before
this first physical run. It remains in the source to preserve binary identity;
this report records the completed run. Each fresh build still records whether
that newly produced artifact itself has been booted.

Credit belongs to Latence.app for the original AD104 Windows port,
JustAResearcher and Zagitta for the upstream transaction work, and TOTORONG
for the payload and debug-booter analysis. The overlay retains their code and
licence notices. The contribution publishes source and measured matrix data;
the ROM, firmware, driver-code and Windows binaries stay with their owner.
