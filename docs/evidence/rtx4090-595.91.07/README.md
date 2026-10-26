# RTX 4090 matched evidence on NVIDIA 595.91.07

Additional owner-controlled AD102 reproduction using source revision `4e7fefcc4923f9e083d5324dc75a7d2f906af77f`, Ubuntu 24.04/kernel6.8 and the open 595.91.07 driver/GSP. 450 W cap and clock policy unchanged; dynamic GPU Boost, no overclock. Experimental firmware assets are the pinned 610.43.03 inputs, not the installed driver version.

Official RM issue-rate getter: FMLA32/IMLA2/IMLA4 **1/1/1 stock -> 0/0/0 unlocked**, including after NVIDIA initialization and workloads. A later cold stock boot restored half rate; a separately authorized subsequent unlock reproduced full rate. FLR/warm reboot are not proven rollback.

| Case | Stock | Unlocked | Observation |
|---|---:|---:|---|
| 8192³ FP16 / FP32 accumulation |162.59 TFLOPS|180.46 TFLOPS|+10.99% throughput|
| 8192³ BF16 / FP32 accumulation |148.45 TFLOPS|150.21 TFLOPS|+1.19%, small/control-scale variation|
| 8192³ TF32 |88.64 TFLOPS|153.48 TFLOPS|+73.15% throughput|
| Frozen H3 BF16 generation |309.6 s|267.3 s|13.66% less time; identical outputs|
| Qwen3.6-27B prefill4096 |2869.565 tokens/s|2911.127 tokens/s|No meaningful gain demonstrated|
| Qwen3.6-27B decode128 |46.419 tokens/s|46.523 tokens/s|No meaningful gain demonstrated|

All 30 measured GEMM outputs matched stock per mode. Five H3 forward outputs and final AVI/WAV bytes matched. H3 used BF16 linears/offload, not INT4. Its matched forward profile had the same 70,629 kernel names/counts: FlexAttention **13.323 -> 8.542 s** (+55.98% observed throughput), explaining 68.78% of net kernel-time saving; actual BF16 GEMMs explained 30.73%. The square GEMM and synthetic TF32 result do not describe this workload.

## Contents

- `metadata.json`, `comparison.json`: environment, exact methods/results and limitations.
- `stock-gemm.jsonl`, `unlocked-gemm.jsonl`: all numeric samples; original process IDs and private library location omitted.
- `stock-sm-rates.json`, `unlocked-sm-rates.json`: getter fields without local device identity.
- `h3-forwards.json`: per-forward timing and complete output digests.
- `unlocked-boot-excerpt.txt`: selected real transaction rows, target/time/host prefix omitted; register values unchanged.
- `bounded-canary-protocol.md`, `cpu-kit/`: one-shot controls and CPU-only log replay tests, not a turnkey hardware installer.
- `SHA256SUMS`: integrity of these sanitized evidence files, not of unavailable private originals.

Two harness findings informed the protocol: a cold-vs-initialized PROM tail difference, investigated by persisting bytes and independently pinning the cold image/ROM-chain/descriptor/FWSEC; and a false-negative parser that rejected `0x0` as a spelling of zero. Neither was resolved by dropping transaction guards. The CPU draft additionally rejects repeated key/value fields in a row; this later hardening is not claimed to have run in the completed trials.

## Limitations and risk

One board, few boots; internal samples and sequential takes are not independent boot-level statistics. Clocks were dynamic. One profile per state, no hardware-counter/SASS attribution. Power/temperature windows and stock RAM/swap pressure do not establish energy efficiency. No LLM significance estimate; matching tokens are not matching logits. Private H3 inputs/media and raw LLM responses are not redistributed. The later FP16-conversion experiment is a separate question and is not included here.

Privileged experimental firmware/module execution can hang a GPU/host and cause data loss or hardware damage. A one-shot boot, cold rollback, exact pins, external observer, bounded cgroups and raw validation reduce exposure, not eliminate it. No automatic unlock installer or firmware binary is included.

Upstream discussion: https://github.com/Zagitta/gpu-unlock/issues/4#issuecomment-6401377964
