# ASUS AD103 / X670E Windows benchmark evidence

Contributed by **karl-kaefer**. See the [hardware report](../../ad103-asus-x670e-windows-report.md)
for the tested machine, firmware transaction and interpretation.

This directory contains six sanitized measurements: three stock processes and
three processes from the Windows session following the 0.3.1 transaction.
All measurements used 4096-square cuBLAS GEMM, 15 samples of five calls per
mode, a 0.75-second bounded warmup, and the same software and 285 W power limit.
`unlocked` is the recorded phase label, not an automatic state detector.

| Mode | Stock median TFLOP/s | After median TFLOP/s | Change |
| --- | ---: | ---: | ---: |
| FP16 inputs / FP32 accumulation and output | 87.82 | 156.63 | +78.4% |
| BF16 inputs / FP32 accumulation and output | 87.66 | 156.50 | +78.5% |
| FP32 pedantic control | 32.02 | 31.87 | -0.5% |

The slower second BF16 process is retained. After-run ranges were
151.63–158.43 TFLOP/s for FP16 and 143.65–157.58 TFLOP/s for BF16.
Full 256-square CPU-reference checks and 1,024 sampled large-output checks had
zero error. Every large output passed the finite check in the measurement
program, and all 18 mode outputs across both phases have the same full-output
SHA-256. These checks do not independently validate every large-output value.

## Offline verification

From the repository root, using Python 3.11 or later:

```sh
python -B docs/evidence/ad103-asus-x670e-windows/verify.py
```

This uses only the Python standard library and recorded files. It verifies
the complete `SHA256SUMS` inventory, the six run checksums, exact source hashes,
matched software/device/settings, numerical-check fields and full output hashes.
It independently recalculates every timing median, TFLOP/s value, variability,
and the before/after gains. It does not load CUDA, access a GPU, or execute the
firmware log. A successful result verifies the exported records' consistency;
it is not an independent attestation of their collection or the firmware run.

The original comparator also works offline:

```sh
python -B docs/evidence/ad103-asus-x670e-windows/compare.py --before docs/evidence/ad103-asus-x670e-windows/stock --after docs/evidence/ad103-asus-x670e-windows/after
```

`benchmark.py`, `run_stock.py` and `compare.py` are byte-for-byte copies of the
measurement sources. Their inherited GPL-2.0-only provenance is retained;
the license is included as `LICENSE`. `benchmark.py` declares its derivation
from the upstream Windows AD103 benchmark. The scripts are included for method
inspection and reproducibility, not as a portable hardware-qualification tool.
The measurement runner specifically requires its isolated Windows environment,
this device/BDF, idle checks and three bounded processes with a 180-second
timeout each. The measurement code only loads PyTorch/CUDA when its measurement
entry point is explicitly invoked; the verification commands above do not.

## Redaction and integrity

Private originals remain immutable. In each exported run JSON, exactly these
nine string fields were replaced, consistently across both phases:

- `executable` becomes `<benchmark-venv>/Scripts/python.exe`.
- GPU `uuid` becomes `GPU-REDACTED-SAME-DEVICE` in initial telemetry and the six
  before/after mode telemetry records.
- `process_environment.CUDA_VISIBLE_DEVICES` uses that same device alias.

Every other parsed field is unchanged, including timestamps, raw timing arrays,
software and DLL hashes, device model/PCI identity, driver/VBIOS versions,
compute settings, all telemetry values, correctness fields and output hashes.
The exported `summary.json` files are minimal indexes generated from these
sanitized runs; their `original_sha256` entries identify the private original
bytes, while `sha256` identifies the exported bytes.

`RUN0003.sanitized.txt` preserves log order, registers, results and transaction
messages. Its five transient DMA addresses are replaced by ordered aliases;
the private boot-proof line and prior selector-evidence log hash are marked
as redacted. Line endings are normalized to LF. The hardware report identifies
the original private log hash separately.

`SHA256SUMS` was generated **after** sanitization. These exported checksums
are distinct from the private originals' checksums. There are no ROM images,
NVIDIA payloads, Windows boot files, raw driver captures or EFI executables in
this directory. The captured output SHA-256 is a checksum, not output bytes.

## Limits

The physical successful entry had SS1=1. A different initial SS1=3 was observed
in an earlier pre-mutation refusal and tested in simulation; successful hardware
execution from SS1=3 is not established. The observed Windows session resumed
its kernel, so fresh cold driver initialization is also not established.

Telemetry is sampled around workloads, not continuous kernel-time tracing.
Keep all three processes and inspect the slower BF16 run and the FP32 control.
The measured gain concerns this short GEMM workload and Windows session. It
does not establish application/game performance, universal correctness,
long-duration stability, or persistence across reset, driver reload or power
removal. No additional GPU workload was run to prepare this export.
