# 10de:28a1 follow-up — repetition and D3cold diagnosis (2026-11-03/04)

Same HP Victus RTX 4050 Laptop (AD107M), kernel `7.0.0-42-generic`, open driver
`580.178.04`, AC power, and the same three benchmark binaries as
[`../ada28a1/`](../ada28a1/README.md) (`hashes.txt` in every run verifies
`mma_bench`, `validate_gemm` and `gemm_bench` against `build/matched-bench/SHA256SUMS`).
The GPU UUID is replaced with `GPU-REDACTED-AD107M`.

This follow-up answers two open questions from the original report:

1. Was the retained 2× unlock a one-off (`run4_nod3cold` was a single run)?
2. Why were the original `D3cold`-wiped runs 13–19 % *below* stock on cuBLAS?

## Groups

| Group | Runs | How it was booted |
|---|---|---|
| Unlocked, `D3cold` blocked | `run5_perf`, `run6_perf`, `run7_perf`, `diag_balanced_1`, `diag_power-saver_1` | normal boot: `gpu_unlock.targets=0000:01:00.0 gpu_unlock.ad107_experimental=1 nvidia.NVreg_DynamicPowerManagement=0x00` |
| `D3cold`-wiped | `boot_wiped_1..3` | one-off GRUB edit: `NVreg_DynamicPowerManagement=0x02`; the script waited until `power_state=D3cold` before running |
| Stock | `boot_stock_1..3` | one-off GRUB edit: `gpu_unlock.*` parameters removed (module present, no target) |

Each run is one pass of `scripts/collect_run.sh` (same steps as
`build/matched-bench/collect.sh`) plus a 100 ms `nvidia-smi` sampler with
timestamps, P-state, SM/memory clocks, power, enforced power limit, temperature
and throttle reasons (`telemetry.csv`, step boundaries in `steps.txt`).
`SUMMARY.txt` is the output of `scripts/summarize.py` over all runs.

## Results (median across runs of each group)

| Group | n | Tensor `f16_f32` best (Top/s) | cuBLAS 4096³ FP16 / BF16 median (TF/s) | cuBLAS 8192³ FP16 / BF16 | `ffma` / `dp4a` | RM getter `fmla32` |
|---|---:|---:|---:|---:|---:|---|
| Stock | 3 | 25.46 | 21.90 / 21.58 | 20.72 / 20.12 | 11.9 / 25.3 | `1` (half rate) |
| `D3cold`-wiped | 3 | 25.40 (**0.998×**) | 22.49 / 21.66 (**+2.7 % / +0.4 %**) | 20.72 / 20.14 (**±0 %**) | 11.9 / 25.3 | `0` (**reports full rate**) |
| Unlocked, `D3cold` blocked | 5 | 50.70 (**1.992×**) | 31.03 / 32.40 (**+41.7 % / +50.2 %**) | 28.15 / 27.83 (**+35.8 % / +38.3 %**) | 11.9 / 25.2 | `0` (full rate) |

All 11 runs: `validate_gemm` 6/6 shapes `failures=0`, `gemm_bench` `failures=0`,
tensor probe `mismatches=0`.

## Findings

- **The retained unlock reproduces.** 5 of 5 runs reach ~2× on the tensor probe
  and on `mma_bench` `f16/f32`, `bf16/f32` and `fp8/e4m3`; non-gated controls stay flat.
  Changing the CPU platform profile (`performance`, `balanced`, `power-saver`) has
  no effect: the GPU's enforced limit stays at 40 W in all three.
- **The D3cold false positive reproduces.** In every wiped run the RM getter returns
  `fmla32=0` (full rate) while measured throughput is stock.
- **The original 13–19 % wiped-run slowdown does NOT reproduce.** Wiped and stock are
  equal within noise for cuBLAS and every control. That slowdown was specific to the
  2026-10-31 session (cold EC-reset boots with only partial telemetry) and is **not**
  a property of the `D3cold` state; its cause remains `NOT_DEMONSTRATED`.
- **cuBLAS is power-limited on this laptop.** During `gemm_bench` the GPU sits at its
  40 W enforced limit (`SW_POWER_CAP` in most samples) and the SM clock drops to
  ~1.9–2.1 GHz, which is why cuBLAS gains (+36 % to +50 %) are smaller than the 2×
  instruction-rate gain.
- **Measurement caveats.** The first tensor-probe repetition is sometimes low
  (39.1–47.1 Top/s) while clocks ramp; use best or later repetitions. `1024³`
  cuBLAS is bimodal (~24 or ~34 TF/s depending on whether clocks were already up)
  and should not be used for conclusions.

## Not demonstrated

Long-duration (>1 min) thermal stability, battery operation, other laptops or
SKUs, and the cause of the 2026-10-31 slowdown. The unlocked group was measured
in a different boot from the stock and wiped groups (same day, same AC power and
platform profile).

## Files

- `<run>/` — `env.txt`, `smi_before.txt`, `smi_after.txt`, `hashes.txt`,
  `sm-rates.json`, `numerical.txt`, `mma.txt`, `gemm.jsonl`, `tensor.json`,
  `telemetry.csv`, `steps.txt`; `SHA256SUMS.local-unredacted` lists the hashes of
  the owner's unredacted originals.
- `scripts/` — `collect_run.sh`, `diag_boot.sh` (the per-boot driver, with
  state checks that refuse a mislabelled boot), `summarize.py`, and `GUIA.md`
  (the Spanish step-by-step the operator followed).
- `SHA256SUMS` — hashes of every published file in this directory.
