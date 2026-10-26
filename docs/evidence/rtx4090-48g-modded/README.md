# Modified 48 GB RTX 4090 (`10de:2684`): transaction, rate readback and workload result

A Chinese-market **modified RTX 4090 with 48 GB of memory** (AD102, `10de:2684`, VBIOS
`95.02.3C.00.02`) completed the reviewed module's firmware transaction and handed the card to the
stock NVIDIA driver. This answers [issue #17](https://github.com/JustAResearcher/gpu-unlock-ada/issues/17)
and adds the one measurement the existing RTX 4090 evidence does not have: what the unlock does to a
real quantized inference workload.

Executed module source: revision `4e7fefcc4923f9e083d5324dc75a7d2f906af77f` (v1.1.1). Environment:
Ubuntu 26.04.1, Linux `7.0.0-38-generic`, NVIDIA `610.57.04` open kernel modules (DKMS), CUDA 13.2,
450 W power limit, stock clocks.

## Boot and correctness evidence

The module and all nine pinned firmware assets were installed into the regular-kernel initramfs for
`7.0.0-38-generic` and loaded before NVIDIA. The selected card reported the transaction and
`gpu_unlock: unlocked` ([excerpt](unlocked-boot-excerpt.txt)), and the public RM getter readback
changed from `fmla32=1, imla2=1, imla4=1` (half rate) to `0/0/0` (full rate). Both readbacks are
included. No `NVRM: Xid` lines appeared in the boot journal before or after the benchmark suite.
Six seeded FP16/BF16 numerical cases passed with `failures=0, max_abs=0` in both states.

## Instruction throughput (`mma_bench`, 20,000 iterations, best of five repetitions)

| Mode | Stock | Unlocked | Ratio |
|---|---:|---:|---:|
| `f16/f16` | 330.8 TF/s | 330.8 TF/s | 1.000× |
| `f16/f32` | 164.9 TF/s | 328.9 TF/s | 1.995× |
| `bf16/f32` | 164.8 TF/s | 328.7 TF/s | 1.995× |
| `s8/s32` | 659.4 TF/s | 657.7 TF/s | 0.997× |
| `dp4a` | 164.0 TF/s | 163.5 TF/s | 0.997× |
| `ffma` | 78.5 TF/s | 78.6 TF/s | 1.001× |

Only the FP16/BF16-input, FP32-accumulate modes change. `f16/f16` was already at full rate, and the
INT8, dp4a and scalar controls are unaffected, which is the expected signature of this limiter.

## cuBLAS (`gemm_bench`, 10 warm-ups, 20 iterations per timing, median of 5)

| Shape | Stock | Unlocked | Ratio |
|---|---:|---:|---:|
| FP16/FP32 1024³ | 103.8 | 131.5 | 1.266× |
| BF16/FP32 1024³ | 120.2 | 157.7 | 1.312× |
| FP16/FP32 4096³ | 154.3 | 216.3 | 1.401× |
| BF16/FP32 4096³ | 155.0 | 155.3 | 1.002× |
| FP16/FP32 8192³ | 173.6 | 189.6 | 1.092× |
| BF16/FP32 8192³ | 169.2 | 187.7 | 1.109× |

FP16 8192³ gains less than FP16 4096³, which is consistent with a memory-bandwidth roof at that
shape. BF16 4096³ is the one case that did not improve, reproducing at 155.0 and 157.4 TF/s in two
separate runs while FP16 4096³ reached 215-216 TF/s; no explanation is claimed here.

## FP8 (E4M3)

`mma.sync.aligned.m16n8k32.row.col.f32.e4m3.e4m3.f32`, issued from registers with eight independent
accumulator chains, measures **603.1 TF/s** (five repetitions, 602.7-603.1) on the unlocked card:
91% of the 660.6 TF/s nominal FP8 rate for this GPU and 1.82x the same-session FP16/FP32-accumulate
rate. The instruction is validated before timing: with all-ones E4M3 inputs every output element must
equal k=32, which it does. Source and raw output: [fp8-mma-bench.cu](fp8-mma-bench.cu),
[fp8-mma.txt](fp8-mma.txt).

This is an issue-rate probe, not a stock/unlocked pair -- the same probe was not run on a locked boot,
so no FP8 gain is claimed. It does show FP8 sitting at its nominal rate, and it covers a case this
repository lists as untested ("other FP8 kernels remain untested"). The existing matched CUTLASS FP8
result on AD103 (59.3 -> 59.5 TF/s) reached about 17% of that card's FP8 peak, so it could not have
resolved an issue-rate change either.

Cross-card context (added 2026-10-26, after this directory was merged). A register-resident E4M3 probe
running at 91% of the nominal rate is consistent with an unlocked, unsaturated FP8 issue path; it
does not show a stock/unlocked ratio for FP8. Two other sources put FP8 in the same group as FP16
and BF16:

- An 8192³ E4M3 GEMM on a Windows RTX 4080 SUPER went 199.4 -> 310.4 TF/s (+55.7%) and
  199.4 -> 298.7 TF/s (+49.8%) in two runs
  ([goldlionren/gpu-unlock-4080s](https://github.com/Goldlionren/gpu-unlock-4080s)); its author notes
  FP8 variance and excludes FP8 from that repository's own pass/fail gate.
- A matched issue-rate pair now exists on AD103: on a Windows RTX 4070 Ti SUPER (`10de:2705`, 66 SMs)
  the same `mma.m16n8k32` E4M3 probe measured **165.45 -> 323.95 TF/s median (1.96×)**, with the
  FP16/FP32 control at 88.0 -> 176.6 TF/s and `f16/f16`, `s8/s32`, `dp4a`, `ffma` unchanged
  ([A-XIANGQAQ/rtx4070tis-ad103-tensor-unlock](https://github.com/A-XIANGQAQ/rtx4070tis-ad103-tensor-unlock),
  `results/fp8-baseline.txt` and `results/fp8-unlocked.txt`). That is the stock side this 4090 probe
  lacks, on the same instruction.
- Instruction-level gains do not survive a fixed power limit intact: an unlocked RTX 4090 on driver
  610.57.04 peaked at 346 W against 313 W stock, throttled on SW Power Cap during both MMA and GEMM,
  and dropped to 2505 MHz minimum clock against 2655 MHz stock
  ([rtx4090-610.57.04](../rtx4090-610.57.04/README.md)).

So the honest reading is: FP8 is not exempt from the limiter, the issue rate can reach nominal when
unlocked, and an application-level FP8 GEMM will land between the two depending on power and clocks.

## Card safety, and the abort paths

- The transaction completed with `full=1`, `valid=1` on every AD102 permission readback before the
  driver bound the card; the rate readback after driver start is full rate.
- About 30 minutes of continuous load followed (four serving matrices, several GEMM sweeps) with
  zero `NVRM: Xid` lines, zero numerical mismatches, 46 °C board temperature.
- The machine was powered off for at least 30 s before the attempt. Physical AC removal was not
  independently observed, and a warm reboot was not tested.
- The module's rejection paths (VBIOS/FWSEC mismatch, already-bound card, stale firmware state) are
  covered by the host test suite (`make test-full`, 29 passed), not exercised on this silicon.
- If the module ever reports that firmware did not stop, power the machine off instead of unloading
  it: the module intentionally keeps DMA buffers that firmware may still use.

## Workload result: no gain on a quantized MoE serving stack

[Full table, method and the file's quantization mix](serving-workload.md). Summary: the model is a
mixed quantization whose F32/F16/BF16 tensors are 0.43% of elements, so there is very little
compute for the unlock to act on. Prefill at 8K and 32K is within 1–2% of stock,
decode spread exceeds any effect, and the CPU/GPU-expert overlap that dominates this model does not
use the unlocked tensor path. The unlock is real; this workload does not consume it.

## Limits

- One modified 48 GB board, one cold boot, one kernel/driver combination.
- About 30 minutes of load after the unlock; no long-duration stability run.
- No warm-reboot persistence test and no repeated cold-boot test were performed.
- The stock serving sample is a single run on a long-running engine; the unlocked side is a fresh engine measured over three passes. Expert-cache residency differs, so small-prompt differences are not attributable to the unlock.
- Decode throughput varied by about 12% between identical passes; no decode claim is made.
- No claim about unquantized (FP16/BF16) workloads, other engines, training, lifetime reliability, or maximum board power.

Raw payloads, hashes of the module, all nine firmware assets and the benchmark binaries are in
[`metadata.json`](metadata.json) and [`SHA256SUMS`](SHA256SUMS).
