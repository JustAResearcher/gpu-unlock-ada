# RTX 4060 Ti 16GB 10de:2803 evidence

`tensor-modes-stock.json` and `tensor-modes-unlocked.json` are unedited stdout
from `diagnostics/bench/tensor_modes_driver_runner.py` on the same physical
card, same generated cubin (SHA-256
`33aa6db385aa9eb8a1f73bb2f74891aea84794326b7c40992bc642f93a3c614d`), same 165
W default cap and same NVIDIA
`615.78.08` driver, except that the GPU UUID is replaced with
`GPU-UUID-OMITTED`. The stock run predates the unlock; the unlocked run is the
same cold boot's post-transaction state. `tensor-modes-warmboot.json` is the
same card after an ordinary warm reboot re-ran the transaction.

`sm-rates-unlocked.json` is output from the read-only
`tools/read_sm_rates.py` with the UUID replaced the same way. On driver
`615.78.08` the getter reports `3` for FMLA32, IMLA2 and IMLA4 after the
unlock, not `0`: the encoding of the flipped state is driver-version-dependent
(the 2805 canary read `0` on driver `595.99.02`). The module's own post-FLR
readout (`full=1`) and the matched benchmark are the rate evidence here. The
getter fails outright on this host's second GPU (GB206, RM status `0x1b`), so
any successful read comes from the 4060 Ti.

`sm-rates-stock.json` is re-serialized from the recorded stock-boot output
(FMLA32, IMLA2 and IMLA4 at half rate, all other fields full); the original
stdout file was lost to a host `/tmp` cleanup. The module's own
before-firmware readout in `transaction-excerpt.txt` independently records the
stock register state.

`transaction-excerpt.txt` contains this card's current-boot kernel messages
from the `entry:` line to `gpu_unlock: unlocked`, with ROM/FWSEC parsing,
asset loading and Falcon DMA lines removed. It is an excerpt, not a complete
kernel log, and contains no host name.

`SHA256SUMS` covers every other file in this directory.
