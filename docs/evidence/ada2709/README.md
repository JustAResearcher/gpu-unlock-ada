# RTX 4070 AD103 10de:2709 evidence

All files come from one card at its 200 W default cap with NVIDIA 595.99.02
and kernel `7.0.0-38-generic`. The GPU UUID is replaced with
`GPU-UUID-OMITTED`; nothing else is edited.

`*-stock.*` were collected in the boot before the package was installed and
`*-unlocked.*` in the first boot with `gpu_unlock.targets=` and
`gpu_unlock.ad103_experimental=1`. Both used the same binaries and cubin.

- `sm-rates-*.json`: read-only `tools/read_sm_rates.py`.
- `tensor-modes-*.json`: `diagnostics/bench/tensor_modes_driver_runner.py`
  stdout for cubin SHA-256
  `b7e23c0d7922311c4319352640255e84caad02642fcdc901270c97a0cee4eac1`.
- `mma-*.txt`: `diagnostics/bench/mma_bench 20000 5`.
- `numerical-*.txt`: `diagnostics/bench/validate_gemm`.
- `gemm-*.jsonl`: `docs/evidence/rtx4090-pr1-stock/gemm_bench.cu`.
- `transaction-excerpt.txt`: this card's kernel messages from the `entry:`
  line to `gpu_unlock: unlocked`, with ROM/FWSEC parsing, asset loading and
  DMA lines removed. It is an excerpt, not a complete kernel log, and contains
  no host name.

`SHA256SUMS` covers every other file in this directory.
