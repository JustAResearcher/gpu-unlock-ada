# RTX 4060 Ti 16GB 10de:2803 evidence

The [2803 report](../../ada-2803-report.md) describes one card tested on
October 25, 2026 at its 165 W cap. One matched pair used kernel
`6.18.58-1-lts` in both states, measured unlocked first and stock second.
`metadata.json` records the matching inputs, benchmark commands, hashes
and test limits.

`tensor-modes-stock.json` and `tensor-modes-unlocked.json` contain stdout
from `diagnostics/bench/tensor_modes_driver_runner.py`. `stock-mma.txt` and
`unlocked-mma.txt` contain instruction rates; `stock-numeric.txt` and
`unlocked-numeric.txt` contain the seeded numerical checks.

`sm-rates-stock.json` and `sm-rates-unlocked.json` contain the final
read-only `tools/read_sm_rates.py` output after the workloads.
`transaction-excerpt.txt` contains selected unlock messages from the
observed boot, ending with `gpu_unlock: unlocked`.

GPU UUIDs are replaced with `GPU-UUID-OMITTED`, PCI locations with
`<target>`, and private build paths with `bin/`. Numeric output is unchanged.

`SHA256SUMS` covers every other file in this directory.
