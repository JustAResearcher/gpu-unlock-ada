# RTX 3050 6GB GA107 10de:2584 evidence

All files come from one card at its 70 W default (and maximum) cap with NVIDIA
595.84 (open kernel module) and kernel `6.8.0-146-generic`. The GPU UUID is
replaced with `GPU-UUID-OMITTED`; nothing else is edited.

`*-stock.*` were collected with the stock driver before the module touched the
card. `*-unlocked.*` were collected right after the live sequence (unload
`nvidia`, PCIe FLR, `modprobe gpu_unlock targets=<address>`, reload `nvidia`)
in the same boot. Both used the same binaries and cubin, built with CUDA 13.3
for `sm_86`:

```text
ce143fc8a5a99f72fd2f1cb2bff15ba4f6c43b2aa743f00cb58906ae084531c0  mma_bench
d0825f911a646564a10b7a5f36dbbdbe66b142e8ff78afd0a069d87aac9725c2  validate_gemm
692e069c52726d02ce4574bd6d029778b9ee09441e099928b03ce6c1d723d748  gemm_bench
de6ac71f7488359ad1e63ba79ee1e2ea75be7748f2b9fcdff19a5138f866af52  tensor_modes_issue_sm86.cubin
```

- `sm-rates-*.json`: read-only `tools/read_sm_rates.py`.
- `tensor-modes-*.json`: `diagnostics/bench/tensor_modes_driver_runner.py`
  stdout for the cubin above.
- `mma-*.txt`: `diagnostics/bench/mma_bench 20000 5`.
- `numerical-*.txt`: `diagnostics/bench/validate_gemm` (accepts `sm_86` on
  this branch).
- `gemm-*.jsonl`: `docs/evidence/rtx4090-pr1-stock/gemm_bench.cu`.
  `gemm-unlocked-r2/r3.jsonl` are two immediate repeats in the unlocked state.
- `transaction-excerpt.txt`: this card's kernel messages from the `entry:`
  line to `gpu_unlock: unlocked`, with FWSEC ROM parsing and DMA lines removed.
  It is an excerpt, not a complete kernel log, and contains no host name.
- `transaction-excerpt-boot.txt` and `sm-rates-boot.json`: the same excerpt
  and readback from the next boot (warm `systemctl reboot`, kernel
  `6.8.0-147-generic`, module loaded from initramfs with
  `gpu_unlock.targets=` on the kernel command line).

`SHA256SUMS` covers every other file in this directory.
