# ASUS RTX 4070 Ti SUPER / X670E Windows UEFI report

Reported by [karl-kaefer](https://github.com/karl-kaefer), 31 October 2026.

A local adaptation of the Windows UEFI port completed its firmware transaction
on one ASUS TUF RTX 4070 Ti SUPER and returned to Windows. Matched cuBLAS tests
measured **+78.4% FP16/FP32 and +78.5% BF16/FP32 GEMM throughput**, with matching
numerical checks and output hashes. The FP32 control changed by -0.5%.

This is another hardware report, not a new unlock method or general ASUS
support. The Windows session resumed its kernel; a fresh cold Windows driver
initialization was not established. The source-only hardening contribution is
separate from this historical hardware run.

## Hardware and software

| Component | Observed configuration |
| --- | --- |
| GPU | ASUS TUF RTX 4070 Ti SUPER OC, AD103, 16 GB |
| PCI identity | `10de:2705`, subsystem `1043:8957`, revision A1 |
| VBIOS | `95.03.45.00.9a` |
| Motherboard | ASUS TUF GAMING X670E-PLUS WIFI, BIOS 2613 |
| CPU | AMD Ryzen 7 7800X3D |
| Display during trial | Motherboard AMD integrated graphics; monitors moved off NVIDIA |
| Windows NVIDIA driver | `621.01`, unmodified |
| Configured power limit | 285 W in both phases |
| Benchmark stack | Python 3.13.2, PyTorch 2.11.0+cu128, NumPy 2.3.5, cuBLAS 120804 |
| Launch | Manual unsigned UEFI application on a separate FAT32 USB; Secure Boot off |

No VBIOS flash, power-limit change, Windows boot-file replacement, or permanent
boot-entry installation was part of this trial. The recorded gain concerns
Tensor throughput, not additional physical cores.

## Source lineage and local adaptations

The direct starting point was
[A-XIANGQAQ/gpu-unlock-ada, branch ad103-2705, commit 517930f](https://github.com/A-XIANGQAQ/gpu-unlock-ada/tree/517930f5ab50ab83c06a9f3a8ff79fb828e6f36f/windows/ad103-2705).
That work builds on this repository's Latence.app/MSI Windows reference port,
the UEFI work credited to impacifist/Windows-GPU-UNLOCK, and the Ada firmware
integration derived from Zagitta/gpu-unlock and TOTORONG's published payload.
The unlock method and benchmark workload are inherited work.

The local adaptation added this board's PROM/FWSEC and register profile,
reviewed NVIDIA firmware-driver release while retaining the AMD console,
exclusive PCI ownership and checked IO/MEM decode restoration with bus
mastering off. It also added exact-success status handling, failed reset-delay
containment, and combined synthetic transaction tests. These are a description
of the local implementation, not a portable installer or a claim that all
these changes are novel upstream fixes.

The EFI, private ownership captures, firmware/ROM/driver bytes, Windows image,
and machine-specific proof chain are not distributed here. This report does
not let another owner rebuild or run the exact historical EFI. Public source
changes require their own review and validation; a PCI ID match alone does
not qualify another board.

## Physical transaction

The sanitized [RUN0003 log](evidence/ad103-asus-x670e-windows/RUN0003.sanitized.txt)
records:

1. Successful NVIDIA firmware-driver Stop and exclusive PCI claim, with AMD
   retained. IO/MEM decode was restored to command `3`, attributes `0x300`,
   with bus mastering off.
2. Successful PROM/FWSEC parsing and firmware execution. The payload marker was
   `0x67b00001`; HALT and required permission checks passed.
3. Entry selectors `SS0=0x41455066`, `SS1=1`; exact readback after writing
   `SS0=0x88888888`, `SS1=8`. The effective readout changed from `0x410080` to `0`.
4. One successful PCIe FLR. Selectors and the full-rate readout survived the
   reset. WPR2 was disabled, GSP/SEC2 quiescence passed, and bus mastering was off.
5. Successful cleanup and exclusive-claim close:

```text
Transaction result=0 safe=1 retained=0 started=1 quiesced=1 flrs=1 DMA_live=0
ASUS close-exclusive return=0
Transaction readbacks and containment passed; Windows start intent; benchmark still required
```

The two logged pre-reset polling timeouts are the existing narrowly tolerated
`base+0xf4` bit-31 path annotated for HW3419321. Subsequent mandatory reset,
payload and HALT checks passed. They are not post-FLR delay failures.

The log ends at Windows-start intent, before the final unlogged boundary check
and `StartImage`. The owner's observed return to Windows and subsequent Windows
activity support the handoff; the log alone is not end-to-end boot attestation.
Windows events indicated boot type `0x1` and a resumed kernel. The benchmark
therefore establishes compatibility for this observed session, not a verified
full cold driver initialization.

A previous invocation refused before driver release/GPU mutation because entry
`SS1` was `3` instead of `1`, despite the owner reporting a PSU-off start. The
local revision admits those two observed values and latches the exact pair for
all same-boot checks. **Only entry `SS1=1` completed a physical unlock.** The
`SS1=3` success path has synthetic coverage only; `{1,3}` is not a universal
admission rule.

## Matched benchmark

Each phase used three fresh processes, 4096-square GEMMs, 15 timing samples of
five calls per mode, and a 0.75-second warmup. The local bounded runner used a
180-second per-process watchdog. Math mode, seed, workload, benchmark source,
packages, cuBLAS/cuBLASLt hashes, GPU identity, driver, VBIOS and the 285 W cap
matched across phases. No clock or power setting was changed.

| Mode | Stock median TFLOP/s | After median TFLOP/s | Change |
| --- | ---: | ---: | ---: |
| FP16 input, FP32 accumulation/output | 87.82 | 156.63 | +78.4% |
| BF16 input, FP32 accumulation/output | 87.66 | 156.50 | +78.5% |
| FP32 pedantic control | 32.02 | 31.87 | -0.5% |

All processes passed full 256-square exact checks, 1,024 sampled large-output
checks and a finite check over the entire large output. These deterministic
dyadic inputs produced zero measured error. All 18 mode outputs across both
phases had the same SHA-256:
`0e3b4c1a854bc95d06fb91f688b1f4f739fe7e76972c889fa1bbeb042ac7c1bc`.
This is the exercised numerical test, not proof for arbitrary inputs.

| After-run TFLOP/s | Run 1 | Run 2 | Run 3 | Run-to-run CV |
| --- | ---: | ---: | ---: | ---: |
| FP16/FP32 | 156.63 | 158.43 | 151.63 | 1.85% |
| BF16/FP32 | 157.58 | 143.65 | 156.50 | 4.15% |
| FP32 pedantic | 32.03 | 31.87 | 31.85 | 0.25% |

The slower second BF16 run is included. Its cause is not established from the
sparse telemetry. Tensor-mode SM clock samples were lower after unlocking
(2,415-2,535 MHz versus 2,730-2,745 MHz stock), while sampled Tensor-mode power
rose from roughly 193-209 W to 230-238 W. The power cap remained 285 W. These
before/after-mode samples are not continuous kernel telemetry.

The [evidence directory](evidence/ad103-asus-x670e-windows/README.md) contains
sanitized measurements, their checksums, the measurement code, and offline
verification. Original measurements remain unchanged privately. Exported
hashes identify the sanitized copies, not the original private bytes.

## Limits

- One successful combined transaction on this exact card and motherboard.
- One short matched stock/unlocked pair, with three processes in each phase;
  no cold A/B/A repetition or long-duration stability qualification.
- Resumed Windows kernel; fresh cold driver initialization remains untested.
- No tested persistence through reboot, shutdown, driver reload or power loss.
- No application, game, FP8, TF32 or INT8 performance claim from this trial.
- No support claim for other boards, BIOS/VBIOS versions, display arrangements
  or driver versions, or for physically successful entry from `SS1=3`.

## Historical identities

| Item | SHA-256 / identity |
| --- | --- |
| Local version | AD103 UNLOCK 0.3.1 |
| Build identity | `257e82e0d428dcf8262a50d5be6917328ea88a6db9ac79840dd34bc974cb0073` |
| Historical EFI (not distributed) | `a98da8272698aa07dec0e1187906c578802aceb852817a0b1cdac2d63bef4dbf` |
| Original private RUN0003 log | `028b3863b6179e102ee9bd9c43aad3268c11563c1b234d33edd385c87ef597df` |

These identities provide provenance; hashes of omitted private inputs do not
make those inputs publicly reproducible or authorize their redistribution.
