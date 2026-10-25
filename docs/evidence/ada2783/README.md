# RTX 4070 SUPER (10de:2783) evidence

Sanitized outputs behind the [2783 report](../../ada-2783-report.md). The GPU
UUID, PCI location, host name, boot/root identifiers and private paths were
replaced or removed; numbers are unedited. `SHA256SUMS` covers every file here.

- `stock-*` — kernel without `gpu_unlock`, RM getter half rate.
- `unlocked-*` — first boot with the opt-in, RM getter full rate.
- `warm-*` — ordinary warm reboot with the opt-in, same readbacks.
- `*-sm-rates.json` — read-only RM getter (`tools/read_sm_rates.py`).
- `*-numerical.txt` — `validate_gemm`; `*-mma.txt` — `mma_bench`;
  `*-gemm.jsonl` — `gemm_bench`; `*-tensor-modes.json` — SM89 tensor-mode probe.
- `*-llama-bench.md` — llama.cpp build 11552, 27B IQ2_XS, no difference.
- `transaction-excerpt.txt`, `warm-reboot-excerpt.txt` — module log lines.
  ROM/FWSEC parsing lines and engine-reset detail were dropped.
- `telemetry-summary.json` — peak clock/power/temperature of the two batches.

Binaries (built with CUDA 12.0, `-arch=sm_89`): `mma_bench`
`6efea219…0255`, `validate_gemm` `53c06d48…eb69e`, `gemm_bench`
`c3c68354…7ee0`; tensor-mode cubin `fc0efe68…c599d4`.
