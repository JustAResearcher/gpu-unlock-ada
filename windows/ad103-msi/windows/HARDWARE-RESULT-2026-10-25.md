# Windows 0.6.4 hardware result — 2026-10-25

One submitted preboot run and one stock/unlocked benchmark pair support a
successful unlock on the specific system below. Tensor microbenchmark throughput
approximately doubled while the scalar control was unchanged. This is a single
successful pair, not evidence of repeatable boot reliability or prolonged stability.

## Evidence provenance

These are the exact supplied filenames and SHA-256 digests. Raw reports are not
committed because they contain device identifiers and firmware handle details.

| Source filename | SHA-256 |
| --- | --- |
| `stock-20261025T160606908263Z-e118a47d.json` | `6f0765a5b765da81aa7765b7c16a2ccbfd6a1dc6d1c490e7ead64e2ecb4211f0` |
| `unlocked-20261025T172041861960Z-53d9a86d.json` | `2b7976b4653d19dd97190f801f60cd683cf25ded0dcf471d3e04e7ad11791e0f` |
| `REPORT(20261025-172117).TXT` | `3392eb1a1d387a3ac7860f947eb47f731c6c7658917aaa8921faacc13e5db41e` |

## Tested configuration

| Item | Value |
| --- | --- |
| GPU | MSI NVIDIA GeForce RTX 4070 Ti SUPER, AD103, 16 GB, 66 SMs |
| PCI device / subsystem / revision | `10de:2705` / `1462:e130` / `a1` |
| Motherboard | MSI MPG Z690 EDGE WIFI DDR4 / MS-7D31, BIOS 1.N0; previously captured target system |
| VBIOS | `95.03.45.80.0b` |
| NVIDIA Windows driver | `621.01` |
| Windows / Python | Windows 11 build 26300 / Python 3.12.10 |
| Reported power limit | 304.95 W, identical in both benchmarks |
| Preboot application | `0.6.4-unlock-test`, `RUN12`, staged unlock mode |

The comparator accepted both reports and their identical full metadata contracts:
GPU identity, PCI address, compute capability, SM count, CUDA driver API, driver,
VBIOS, power limit, workload settings, Python/Windows environment and source hashes.
The device UUID was compared locally and is intentionally omitted here.
The hashes also match the trusted repository files used for this comparison:

- `benchmark/run_benchmark.py`: `eda57810b19d3ec7c287bdd8c656d59e9cea9542231b83d1664fb43771980048`
- `benchmark/kernels.ptx`: `1255faacf28a3c8b5a63e49f9522cda7d01848c96546ba0486b63e9621982e7e`

## Windows measurements

Stock was recorded at 16:06:02–16:06:06 UTC; unlocked at 17:20:37–17:20:41 UTC.
Each mode used 264 blocks, 128 threads, 4,096 main iterations and nine timed samples.

| Workload | Stock median ms | Unlocked median ms | Stock TFLOP/s | Unlocked TFLOP/s | Throughput ratio |
| --- | ---: | ---: | ---: | ---: | ---: |
| FP16 → FP32 tensor | 2.239680 | 1.007616 | 63.283 | 140.663 | 2.223× |
| BF16 → FP32 tensor | 2.219008 | 1.007616 | 63.873 | 140.663 | 2.202× |
| Scalar FP32 control | 0.066560 | 0.066560 | 33.272 | 33.272 | 1.000× |

Both reports completed and recorded correct outputs for all 16 launches per mode
(four probes, three warmups, nine samples). Each tensor launch checked 1,081,344
output cells; each scalar launch checked 270,336. Report validation independently
recomputed the medians and throughput from the recorded samples and workload.

All four tensor datasets triggered the comparator's timing-spread warning:

| Dataset | Minimum ms | Maximum ms | (Maximum − minimum) / median |
| --- | ---: | ---: | ---: |
| Stock FP16 | 2.097152 | 3.029184 | 41.6% |
| Unlocked FP16 | 1.005568 | 1.296384 | 28.9% |
| Stock BF16 | 2.102272 | 2.665472 | 25.4% |
| Unlocked BF16 | 0.794656 | 1.500160 | 70.0% |

Every unlocked tensor sample was faster than every stock sample of the same mode.
The noise limits precision: these are measured ratios for this pair, not a promise
of a consistent 2.2× gain. The scalar timing spread was approximately 3% in each run.

End-of-run telemetry was 2,670 MHz SM / 10,501 MHz memory in both measurements;
temperature was 35.30°C stock and 39.74°C unlocked. Power draw was 48.96/49.21 W,
utilization 7%/5%, and P-state P0/P0. These are endpoint snapshots, not continuous
measurements during the kernels. No metadata, scalar-control or telemetry warning
was emitted; the four timing-spread warnings remained.

## Preboot result and limits

- All 23 explicit `CHECK` entries returned status 0.
- The single GPU transaction returned 0, with one FLR and 1,488 application MMIO writes.
- Cleanup recorded `quiesced=1`, `fault=0`, `retained=0`, `maps=0`; final `dma=0`.
- Selectors `SS0=0x88888888`, `SS1=0x8` survived FLR and display restoration;
  `restored-unlock` returned 0 and `UNLOCK_VERIFIED` was recorded.
- All three reconnect stages (4, 5 and 6) recorded Supported/return status 0 and
  verified ownership graphs. The six eligible USB child replacements were accepted
  under the bounded 0.6.4 matching policy.
- The final status was 0 at `windows-handoff`, with `windows_handoff=pending`.
  That log alone does not prove Windows booted: it ends before the transfer.
  The accompanying completed Windows measurement and tensor-specific gain support
  successful operation after handoff, without an independent boot-session attestation.

The JSON labels are user-supplied; both retain `hardware_unlock_verified=false`.
This result makes no claim about gaming, application performance, other boards,
long stress tests or repeat boots. A cold-start stock baseline cannot be attested
from these files alone. Secure Boot was disabled in the preboot report; restoration
of Secure Boot and BitLocker after this test has not yet been reported.
