# RTX 4060 10de:2882 evidence

`transaction-excerpt.txt` contains this card's kernel messages from the
`entry:` line to `gpu_unlock: unlocked` for the first boot after a full
power-off. `warm-reboot-excerpt.txt` is the same range for a later warm
reboot. Both have ROM/FWSEC parsing, FWSEC result, asset loading and Falcon
DMA lines removed. They are excerpts, not complete kernel logs, and contain
no host name.

`sm-rates-stock.json` and `sm-rates-unlocked.json` are output from the
read-only `tools/read_sm_rates.py`, with the GPU UUID replaced with
`GPU-UUID-OMITTED`. The stock reading was taken before the module was
installed. The unlocked reading was taken after the warm reboot.

`tensor-modes-unlocked.json` is stdout from
`diagnostics/bench/tensor_modes_driver_runner.py` at the card's 115 W default
cap after the warm reboot, with the UUID replaced the same way. The cubin was
built with CUDA 13.4.92 following `diagnostics/bench/README.md`; its SHA-256
matches the cubin used for the 2805 report. No same-card stock run of this
probe has been made.

`torch-gemm-stock.txt` and `torch-gemm-unlocked.txt` are unedited stdout of
`torch-gemm.py`, run in a stock boot and in the cold unlocked boot on the same
card, driver, power cap and PyTorch build. `torch-gemm-check-unlocked.txt` is
unedited stdout of `torch-gemm-check.py` in the cold unlocked boot.

`SHA256SUMS` covers every other file in this directory.
