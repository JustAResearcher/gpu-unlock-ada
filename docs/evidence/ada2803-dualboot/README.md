# RTX 4060 Ti 16GB + RTX 3060 dual-unlock evidence

The [dual-boot report](../../ada-2803-dualboot-report.md) describes one board
where `10de:2803` (AD106) and `10de:2504` (GA106) were unlocked in a single
cold boot on October 27, 2026, kernel `5.15.0-190-generic`, driver `595.99.02`,
module `1.1.1` at commit `d28c485`.

`metadata.json` records the matching inputs, boot options, benchmark commands,
cubin hash and test limits.

`tensor-modes-stock-150w.json`, `tensor-modes-stock-165w.json`,
`tensor-modes-unlocked-150w.json` and `tensor-modes-unlocked-165w.json` contain
stdout from `diagnostics/bench/tensor_modes_driver_runner.py` for the 4060 Ti at
both caps in both states. All four runs checked all 270,336 output cells per
mode with zero mismatches.

`gemm-sustained.txt` contains the sustained cuBLAS GEMM arm for both cards,
including the reason short-run GEMM numbers from this host are not quoted.

`transaction-excerpt.txt` contains selected unlock messages from the measured
boot, ending with one `gpu_unlock: unlocked` line per target.

GPU UUIDs are replaced with `GPU-UUID-OMITTED`, PCI locations with
`<target-4060ti>` and `<target-3060>`, the root filesystem UUID with
`<root-fs-uuid>`, and private build paths with `bin/`. Numeric output is
unchanged.

`SHA256SUMS` covers every other file in this directory.
