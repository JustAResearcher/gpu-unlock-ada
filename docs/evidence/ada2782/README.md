# RTX 4070 Ti 10de:2782 evidence

These are from one ASUS card (`1043:88dd`), passed through by VFIO to a KVM guest, at its
285 W default cap.

- `stock-*` and `unlocked-*`: unedited stdout of the same binaries in a stock boot and in the
  unlocked cold boot. The GPU UUID is replaced with `GPU-UUID-OMITTED`.
  - `*-sm-rates.json`: `tools/read_sm_rates.py`
  - `*-mma.txt`: `mma_bench 20000 5`
  - `*-numerical.txt`: `validate_gemm`
  - `*-gemm.jsonl`: `docs/evidence/rtx4090-pr1-stock/gemm_bench.cu`
  - `*-tensor-modes.json`: `diagnostics/bench/tensor_modes_driver_runner.py`
- `bench-binaries.sha256`: the hashes of those binaries and the cubin, recorded before both runs.
- `transaction-excerpt.txt`: the cold boot's kernel messages from the `entry:` line to
  `gpu_unlock: unlocked`. The ROM/FWSEC parsing, asset loading and Falcon DMA lines are removed. It is an
  excerpt, not a complete kernel log, and contains no host name.
- `warm-reboot-excerpt.txt`: the module's lines from the next guest boot. The host stayed powered and
  VFIO reset the device.

`SHA256SUMS` covers every other file in this directory.
