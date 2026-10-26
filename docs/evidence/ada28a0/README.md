# RTX 4060 Laptop GPU 10de:28a0 evidence

`tensor-modes-unlocked.json` is unedited stdout from
`diagnostics/bench/tensor_modes_driver_runner.py` on one card, except that the
GPU UUID is replaced with `GPU-UUID-OMITTED` and the runner's exact-power-cap
assertion was bypassed because this Max-Q board exposes no enforced power
limit (`[N/A]`). No same-card stock run has been made.

`sm-rates-unlocked.json` is output from the read-only `tools/read_sm_rates.py`,
with the UUID replaced the same way. No stock reading was taken; the kernel
transaction excerpt below contains the module's own same-boot effective
speed-select readback before and after the transaction instead.

`validate-gemm-unlocked.txt` is unedited stdout from `validate_gemm.cu`
(cuBLAS against a CPU reference) on the unlocked card.

`transaction-excerpt.txt` contains this card's current-boot kernel messages
for the card's PCI address, from enumeration through `gpu_unlock: unlocked`
to the NVIDIA driver handoff, with ROM/FWSEC parsing, asset loading and Falcon
DMA lines removed. It is an excerpt, not a complete kernel log, and contains
no host name.

`SHA256SUMS` covers every other file in this directory.
