# RTX 4080 experimental hardware report

## Setup

- GPU: NVIDIA GeForce RTX 4080, PCI ID `10de:2704`.
- VRAM: modified 32 GiB configuration; the NVIDIA driver reported 32,760 MiB.
- Guest OS: Ubuntu 24.04, Linux `6.8.0-147-generic`; NVIDIA driver `580.178.04`.
- The GPU was passed through to a virtual machine.
- Application workload: public Qwen 27B EXL3 6.0 bpw checkpoint with ExLlamaV3 `1.6.2`; PyTorch `2.11.0+cu130`.
- Same runtime settings in both states: maximum sequence 98,304 tokens, total cache 196,608 tokens, 8-bit K/V cache, prefill chunk 2,048 tokens. Power limit remained 320 W; clocks were not locked.

## Source and boot provenance

The tested local commit was `0059fb58a413c29186cac956ad8486135de9e9a0`, based on upstream `4e7fefcc4923f9e083d5324dc75a7d2f906af77f`, packaged as `1.1.1+rtx4080.1`. This upstream contribution keeps the existing `1.1.1` package version; its kernel transaction code and pinned firmware are unchanged from the tested local port. The only runtime source difference is the `MODULE_VERSION` string. Comments, helper labels and documentation were adjusted for upstream publication.

The trial used one VM stop/start and a one-shot boot entry, with a separate original stock kernel/initramfs entry retained. The entry check observed `wpr2_hi=0`. No host AC power cycle or subsequent warm-reboot trial was performed. The opt-in was subsequently made persistent on this host, but another boot of that persistent configuration was not tested.

## Unlock and hardware readbacks

A guarded trial with the experimental option and one exact PCI target completed successfully on this single card. The kernel path reported `gpu_unlock: unlocked`; the firmware marker matched the expected value, required permissions were present, and the boot processor reached the expected halted state. The effective hardware readout reported full rates before and after function-level reset.

An independent NVIDIA resource-manager getter read showed the FMLA32, IMLA2, and IMLA4 encodings change from `1` to `0`. The driver enumerated 32,760 MiB of VRAM after the trial.

The [selected transaction excerpt](evidence/ada2704/transaction-excerpt.txt) preserves the entry, payload, selector and post-FLR register evidence with timestamps, host identifiers and PCI location removed. RM encodings are recorded separately in [matched-results.json](evidence/ada2704/matched-results.json); those readbacks do not establish INT8/INT4 throughput gains.

## CUDA numerical check and synthetic GEMM

FP16 and BF16 CUDA GEMMs (2048 x 2048, FP32 accumulation) produced bitwise-identical before/after outputs by full SHA-256. Relative Frobenius errors against FP64 were unchanged: `0.00035876` for FP16 and `0.00286997` for BF16. Median time changed from `0.21929 ms` to `0.11544 ms` for FP16 and from `0.21934 ms` to `0.11546 ms` for BF16, at the same 320 W power limit with clocks unlocked. This synthetic GEMM result is distinct from the application measurements below.

Inputs were CPU `torch.randn` float32 matrices scaled by 0.05 with seed 4080, then converted to FP16/BF16. TF32 and reduced-precision reductions were disabled. The FP64 CUDA reference used the original float32 matrices, so the reported error includes input quantization. Each timing is the median of five CUDA-event windows of twenty matmuls, after one untimed matmul. SHA-256 covers every output element after conversion to float32 CPU NumPy bytes; matching hashes establish equality for these deterministic inputs, not general numerical correctness.

## Native server prompt A/B

| Prompt tokens | Median tokens/s before | Median tokens/s after | Change | Median prompt time before | Median prompt time after |
|---:|---:|---:|---:|---:|---:|
| 4,089 | 1,785.59 | 2,140.84 | +19.90% | 2.29 s | 1.91 s |
| 16,377 | 1,629.55 | 2,002.08 | +22.86% | 10.05 s | 8.18 s |
| 65,529 | 1,284.38 | 1,630.48 | +26.95% | 51.02 s | 40.19 s |

These are native server prompt timings for one stock/unlocked boot pair using the same configuration and prompts. The targets were 4,096/16,384/65,536 tokens; the table reports the actual token counts returned by the tokenizer and completion usage. Each size had one warmup and three measured repeats. Prompts combined a deterministic per-repeat nonce with repeated neutral synthetic text; each corresponding prompt SHA-256 matched before and after, and `cached_tokens` was zero in all 12 requests per state. Generation used `max_tokens=1`, temperature 0, seed 42, and no added BOS token. Throughput is actual uncached prompt tokens divided by server prompt time. Cold model loading and output-token generation time are excluded from the prefill metric.

The sanitized numeric data is in [matched-results.json](evidence/ada2704/matched-results.json). The model returned to Ready, no Xid errors appeared after load or benchmark, and application health/configuration checks passed. Checksums for the evidence files are in [SHA256SUMS](evidence/ada2704/SHA256SUMS).

## Scope and limits

This is one modified-memory RTX 4080 trial. It does not establish compatibility for other RTX 4080 cards or memory configurations. The application A/B covers one boot pair with unlocked clocks; separate cold-boot and warm-reboot repeatability have not been tested. Decode throughput, answer quality and long-duration stability were not measured. Results describe this workload and setup and do not guarantee the same gains on other cards or models.
