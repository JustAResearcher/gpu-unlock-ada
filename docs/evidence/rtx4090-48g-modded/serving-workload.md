# Serving workload on the same card: no gain from the unlock

The microbenchmarks show the unlock works. This file records what it did to the workload the
card actually runs: a 3-bit quantized mixture-of-experts model (`IQ3_S`, 79 GB, experts partly
resident in host RAM) served by Strata 0.2.2 / engine 0.1.47 at a 262144-token context.

Method: the engine's own `/metrics` counters, driven by vLLM `v0.7.3` `benchmark_serving.py`
(vendored; adaptations documented in that repository), fixed seeds, one warm-up request per cell discarded,
five cells identical in both states. The desktop session was stopped before the unlocked runs
(`lightdm` inactive, no process on `/dev/nvidia*`).

| Cell | Metric | Stock | Unlocked (median of 3 passes) | Unlocked range | Ratio |
|---|---|---:|---:|---:|---:|
| P1K | prefill tok/s | 1351.4 | 1193.1 | 1110–1221 | 0.883× |
| P8K | prefill tok/s | 3518.8 | 3457.8 | 3382–3526 | 0.983× |
| P32K | prefill tok/s | 3916.6 | 3890.3 | 3847–3892 | 0.993× |
| D1K | prefill tok/s | 1279.6 | 1257.3 | 1169–1262 | 0.983× |
| D32K | prefill tok/s | 3894.2 | 3913.2 | 3892–3915 | 1.005× |
| P1K | TTFT ms | 783.8 | 886.2 | 865–950 | 1.131× |
| P8K | TTFT ms | 2356.7 | 2396.9 | 2351–2451 | 1.017× |
| P32K | TTFT ms | 8415.9 | 8470.6 | 8467–8567 | 1.007× |
| D1K | TTFT ms | 827.4 | 841.5 | 839–903 | 1.017× |
| D32K | TTFT ms | 8464.3 | 8421.1 | 8419–8468 | 0.995× |
| D1K | decode tok/s | 184.8 | 206.6 | 187–210 | 1.118× |
| D32K | decode tok/s | 164.0 | 154.3 | 154–168 | 0.941× |

Reading: prefill at 8K and 32K is within 1–2% of stock in both directions, and its run-to-run
spread is about 1–4%, so the unlock produces no measurable prefill gain on this workload.
Decode spread (±12%) is larger than any effect, so no decode conclusion is drawn. The 1K
prefill cell moves the most, which matches the confounder described under Limits: expert-cache
residency differs between a 3-day-old engine and a freshly started one.

Why this is consistent with a 2x tensor-core unlock: the file is a *mixed* quantization, so it
does contain higher-precision tensors -- they are simply not where the work is. Parsed from the
GGUF header ([model-quantization.json](model-quantization.json)): 78.09 GiB over 176.9 G elements, split by element count as
IQ4_NL 47.4%, IQ2_S 19.0%, IQ3_XXS 16.1%, IQ3_S 9.5%, one custom type 4.3%, the remainder 3.7%.
Every F32/F16/BF16 tensor together is 1.8% of bytes and 0.43% of elements,
and the largest of them is a 50 MiB projection: they are narrow per-layer
bottlenecks (`hc_*` 320-wide branches, MoE router gates, per-layer embeddings), not the
per-token GEMM. A perfect 2x on that 0.43% would move the numbers reported above by well
under a percent, which is inside the 1-4% spread.
The controls say the same thing:
`dp4a` 164.0 -> 163.5 TF/s and `s8/s32` 659.4 -> 657.7 TF/s.
The speculative decoder's draft model is not pure BF16 either: its manifest is 76.7% Q8_0 and
23.3% BF16 by element count, and the BF16 part is rank-320 hyper-connection mixers plus a
512x2560 gate -- skinny, bandwidth-bound projections rather than large GEMMs. The Q8_0 part is
the INT8 path that does not move with this unlock, and every accepted draft still costs one
quantized verification pass of the main model.
A BF16-heavy workload -- the BF16 vision projector that ships next to this model, or
unquantized weights -- is where the unlock should still pay off; neither was measured here.

## Limits

- One modified 48 GB board, one cold boot, one kernel/driver combination.
- About 30 minutes of load after the unlock; no long-duration stability run.
- No warm-reboot persistence test and no repeated cold-boot test were performed.
- The stock serving sample is a single run on a long-running engine; the unlocked side is a fresh engine measured over three passes. Expert-cache residency differs, so small-prompt differences are not attributable to the unlock.
- Decode throughput varied by about 12% between identical passes; no decode claim is made.
- No claim about unquantized (FP16/BF16) workloads, other engines, training, lifetime reliability, or maximum board power.
