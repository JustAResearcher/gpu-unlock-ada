# RTX 3050 8GB GA106 10de:2507 evidence

All files come from one card (MSI `1462:c978`, VBIOS `94.06.37.00.86`) at its
130 W default (and maximum) cap with NVIDIA 615.71.09 (open kernel module) and
kernel `7.2.6-arch2-1`. See `host.txt` for the host, the boots and the CPU
power regime of each state. The GPU UUID is replaced with `GPU-UUID-OMITTED`,
the root filesystem UUID with `OMITTED`, and home paths with `~`; nothing else
is edited.

Four states, each in its own boot except the last:

- `*-baseline.*`: stock driver, collected first. The CPU regime differed
  (tuned `realtime-virtual-host`), which matters for the Ollama probe only.
- `*-unlocked.*`: the boot with `gpu_unlock.targets=0000:01:00.0`.
- `*-stock.*`: the next boot, stock, in the same CPU regime as the unlocked
  boot (power-profiles-daemon `performance`). **This is the matched stock
  state for every comparison in the report.**
- `ollama-stock-tuned.jsonl`: same boot as `stock`, Ollama only, after
  switching from power-profiles-daemon to tuned `realtime-virtual-host`.

All states used the same binaries and cubin, built with CUDA 13.4 for
`sm_86` (`binaries.sha256`; `sha256sum -c` passed before each state):

```text
d04edcddcf1ea3a9ad37bc043b163b10749f650ea7236339de65d1b2f3c3f7bd  mma_bench
ddc8341021283068bc74f855c05cfec9236c1472063d870ddc143b38103b1c7d  validate_gemm
54581391e5216b51626ec63e199fa0db99b20d87942ac00c6e3bb1028de4e004  gemm_bench
431f0489c702d5713592bfe0d6949b7da7e725dadf7cc264a37d324a44386092  tensor_modes_issue_sm86.cubin
```

- `sm-rates-*.json`: read-only `tools/read_sm_rates.py`. `sm-rates-stock-boot.json`
  was read right after the stock boot, before any workload.
- `mma-*-pass{1,2,3}.txt`: `diagnostics/bench/mma_bench 20000 5`, three passes.
- `tensor-modes-*-pass{1,2,3}.txt`: `diagnostics/bench/tensor_modes_driver_runner.py`
  stdout for the cubin above, `--expect-power-limit 130`, three passes.
- `numerical-*.txt`: `diagnostics/bench/validate_gemm`.
- `gemm-*.jsonl`: `docs/evidence/rtx4090-pr1-stock/gemm_bench.cu`.
- `torch-gemm-*.jsonl`: `torch_gemm.py` in this directory (PyTorch AMP, FP16
  and BF16 with FP32 accumulation, 10 warm-up and 3 × 50 event-timed matmuls,
  checked against an FP32 reference with TF32 disabled).
- `ollama-*.jsonl`: `ollama_bench.py` in this directory (llama3.2:3b Q4_K_M,
  about 2,020-token prompt with a per-request nonce, temperature 0, seed 42,
  one warm-up and five measured requests).
- `telemetry/`: `nvidia-smi --query-gpu=... -lms 200` samples taken by
  `sampled.sh` during each run, plus `*-summary.txt` from `telemetry_summary.py`
  (samples with utilization ≥ 50%).
- `transaction-excerpt.txt`: this card's kernel messages (dmesg, so the
  timestamps are the kernel's) from the `entry:` line to `gpu_unlock: unlocked`,
  with the `Falcon DMA:` lines removed. It is an excerpt, not a complete kernel
  log.
- `stock-boot-gpu_unlock.txt`: every `gpu_unlock` line of the stock boot. The
  module loaded from the initramfs and did nothing without the option.

The last line of each run's output, starting with `#`, was appended by
`sampled.sh` and records the start and end times, exit status and command. The
four scripts are included exactly as they ran, under the repository's
GPL-2.0-only licence.

`SHA256SUMS` covers every other file in this directory and `telemetry/`.
