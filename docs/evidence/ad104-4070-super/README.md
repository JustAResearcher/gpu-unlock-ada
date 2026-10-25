# Windows RTX 4070 SUPER evidence

These files record the successful 2026-10-25 manual unlock on PCI `10de:2783`,
subsystem `1458:413b`, and its matched cuBLAS matrix measurements. The
[report](../../ad104-4070-super-windows.md) explains the result; the
[reproduction guide](../../../windows/ad104-4070-super/README.md) covers the
source, cold capture, internal-disk boot and benchmark procedure.

| Files | Contents |
|---|---|
| `gemm/stock-run1.json` through `stock-run3.json` | Three original stock matrix runs, byte-for-byte |
| `gemm/after-trial1-run1.json` through `after-trial1-run3.json` | Three original unlocked matrix runs, byte-for-byte |
| `gemm/*.execution.json` | Per-run monitor samples and process settings; local command paths replaced by labels |
| [gemm-comparison.json](gemm-comparison.json) | Medians, ratios, run values and numerical checks |
| [RUN0001.sanitized.txt](RUN0001.sanitized.txt) | Complete EFI log with volatile host addresses replaced by labels |
| [windows-driver-health.json](windows-driver-health.json) | Post-run GPU status and driver/display/WHEA event query |
| [boot-state-summary.json](boot-state-summary.json) | Selected boot-state fields, Windows image hash and deployed EFI hash |
| [validation.json](validation.json) | Public-source rebuild and host-test results |
| [evidence-manifest.json](evidence-manifest.json) | Original/published hashes and exact transformations |

The log replaces six UEFI handle addresses, four host DMA source addresses
and one DMA mailbox address. Register addresses/values, GPU memory layout,
status codes, ordering and all other log bytes are retained. The boot summary
omits local GUIDs, device paths and the machine's recorded security settings.
Raw captures, ROM/code bytes, firmware and Windows executables are kept outside
the repository.

To recompute the matrix table from the repository root, no CUDA installation
is needed:

```powershell
python windows/ad104-msi/compare.py --before docs/evidence/ad104-4070-super/gemm/stock-run1.json docs/evidence/ad104-4070-super/gemm/stock-run2.json docs/evidence/ad104-4070-super/gemm/stock-run3.json --after docs/evidence/ad104-4070-super/gemm/after-trial1-run1.json docs/evidence/ad104-4070-super/gemm/after-trial1-run2.json docs/evidence/ad104-4070-super/gemm/after-trial1-run3.json
```

Expected medians: FP32 26.02 → 26.18 TFLOP/s, FP16/FP32 78.63 → 117.53 TFLOP/s,
BF16/FP32 78.97 → 113.00 TFLOP/s. The comparator checks software/configuration
identity and output hashes before reporting changes.

The public-source rebuild produced the exact 98,816-byte EFI used on hardware:
`b20394d76f7b7dc706e7d7ba893a4ca56b958db66232b7b22ed60a63d28acf84`.
It used the author's private partition/path header. All 14 core expected
outcomes, seven ownership groups, 110 boot-I/O cases and 53 application-flow
cases passed. The collector passed 127,227 application checks, 179 platform
checks and 28 Python tests. The publication helpers passed 26 test methods;
four symlink subcases were skipped because this Windows host could not create
symlinks. Extraction from the actual schema-5 capture reproduced all five
private input hashes. These host checks exercise the code without another
hardware unlock.

SHA-256 values identify the artifacts and transformations; the physical-run
log and measurements are the author's recorded observations. The source
catalog is [source-files.json](../../../windows/ad104-4070-super/source-files.json),
and the capture/profile provenance is in
[provenance.json](../../../windows/ad104-4070-super/overlay/provenance.json).
