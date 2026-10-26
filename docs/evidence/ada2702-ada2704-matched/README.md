# Matched RTX 4080 + RTX 4080 SUPER run, one host, Ubuntu 24.04 / driver 595.99.02

Two AD103 cards in one machine, unlocked in the same cold boot, each measured
against its own stock state from a separate boot of the same kernel and driver.
This is the first report in this repository with **two AD103 cards in one host**
and the first on driver **595.99.02** (older than the 610/615 drivers in the
existing reports).

## Hardware and software

| | card 0 | card 1 |
|---|---|---|
| Model | RTX 4080 | RTX 4080 SUPER |
| PCI ID | `10de:2704` | `10de:2702` |
| BDF | `0000:04:00.0` (PCIe x4) | `0000:2b:00.0` |
| VBIOS | `95.03.1E.00.3F` | `95.03.44.00.DB` |
| SMs | 76 | 80 |

- Ubuntu 24.04.4, kernel `6.8.0-147-generic`, NVIDIA `595.99.02` (proprietary),
  headless, Secure Boot off, no `nvidia.ko` in the initramfs.
- Power limit 250 W on both cards for every measurement below.
- Build: `gpu-unlock-ada` v1.1.1 with two local changes:
  `0x2704`/`0x2702` added to the supported-ID table, and the AD103 guard changed
  from "exactly one PCI address" to "an explicit comma-separated list of exact
  addresses" (`targets=all` is still refused). Both changes are only the card
  routing; the transaction, gates and payload are unchanged.
- Kernel command line, unlocked boot:
  `gpu_unlock.targets=0000:04:00.0,0000:2b:00.0 gpu_unlock.ad103_experimental=1`
- Kernel command line, stock boot: same kernel, parameters absent.

## Transaction (both cards, one cold boot)

`FEAT[0x823804]` reads `0xffffff8f` before the payload and `0xffffffff` after it;
marker `0x67b00001`, `halt=0`, then `ss0=0x88888888`, `ss1=0x8`. After the FLR the
rate getter still reports `readout1=0x0`, `full=1`, and the stock driver binds.
See `transaction-excerpt.txt` (68 lines, both BDFs) and the stock boot's log,
which contains no transaction at all.

## Instruction rates (register-resident, `mma.sync`, 20 000 iterations)

RTX 4080 (`0000:04:00.0`):

| Mode | stock | unlocked | ratio |
|---|---:|---:|---:|
| FP16 → FP32 acc | 107.0 TF/s | 213.7 TF/s | 2.00× |
| BF16 → FP32 acc | 106.9 TF/s | 213.6 TF/s | 2.00× |
| FP16 → FP16 acc | 214.2 TF/s | 213.9 TF/s | — |
| INT8 → INT32 | 427.1 TOPS | 425.7 TOPS | — |
| `dp4a` | 106.0 TOPS | 105.8 TOPS | — |
| scalar FP32 FMA | 50.8 TF/s | 50.7 TF/s | — |

RTX 4080 SUPER (`0000:2b:00.0`):

| Mode | stock | unlocked | ratio |
|---|---:|---:|---:|
| FP16 → FP32 acc | 104.8 TF/s | 209.7 TF/s | 2.00× |
| BF16 → FP32 acc | 104.8 TF/s | 209.3 TF/s | 2.00× |
| FP16 → FP16 acc | 210.1 TF/s | 210.4 TF/s | — |
| INT8 → INT32 | 418.9 TOPS | 417.0 TOPS | — |
| `dp4a` | 103.9 TOPS | 103.8 TOPS | — |
| scalar FP32 FMA | 49.4 TF/s | 49.7 TF/s | — |

Exactly the limited modes move; FP16-accumulate, INT8, `dp4a` and scalar FP32
do not, which is the signature of the FMLA32 rate field and not of a clock
change.

## cuBLAS GEMM (`cublasGemmEx`, FP16 in, FP32 out, `CUBLAS_COMPUTE_32F`)

| Card | Shape | stock | unlocked | ratio |
|---|---|---:|---:|---:|
| 4080 | 4096³ | 100.7 TF/s | 194.9 TF/s | 1.94× |
| 4080 | 8192³ | 102.5 TF/s | 164.9 TF/s | 1.61× |
| 4080 SUPER | 4096³ | 98.7 TF/s | 190.7 TF/s | 1.93× |
| 4080 SUPER | 8192³ | 100.9 TF/s | 177.7 TF/s | 1.76× |

The 8192³ gap is smaller than the 4096³ gap because at 250 W the sustained
shape is power-limited, not issue-limited. A separate sweep on the same card
with the cap lifted: 8192³ gave 127.9 TF/s at 180 W, 192.1 TF/s at 250 W and
188.3 TF/s at 320 W (the last one hits the clock ceiling, not the cap). That
sweep is a single run per cap and is not part of the matched pair above.

## Application-level check (not a general speedup claim)

Strata 0.2.1 serving a Qwen3.8-Flash-Next Q4 MoE model on both cards, 13 020-token
prompt, four runs per boot, unique prompt each run so no prefix cache is reused:

| State | prefill | TTFT |
|---|---:|---:|
| stock | 981.5 tok/s | 13.27 s |
| unlocked | 1015–1104 tok/s | 11.8–12.8 s |

That is +3…13 % across four boots. The spread comes from the host page cache and
the engine's expert cache being cold after every reboot, which is larger than the
effect itself, so this is reported as a range and not as a precise delta. Decode
throughput did not change (~146 tok/s). The engine's own layer split assigns the
x4 card only 6 of 48 layers, which is why unlocking it adds little there.

## Limits of this evidence

- One machine, one kernel, one driver version; two cold boots (one stock, one
  unlocked) plus ordinary warm reboots.
- Instruction rates are register-resident issue rates, not application speedups.
- The cuBLAS numbers are one binary, one run per shape; they are not a benchmark
  suite.
- No extended stability run: the machine was up for about an hour in the unlocked
  state with no Xid and no NVRM errors in the kernel log.
- A warm reboot returns the card to stock; the module has to run again at the next
  boot (the reports' "persistence" refers to surviving FLR and driver init within
  a boot, not across power cycles).

## Files

```
stock/       same kernel and driver, no parameters: mma, cuBLAS, host, kernel log
unlocked/    same kernel and driver, both cards targeted: same set
matched-results.json   the numbers above, machine-readable
transaction-excerpt.txt  kernel log of the unlocked boot, both BDFs
SHA256SUMS   hashes of everything here
```

GPU UUIDs were replaced with `<redacted-N>` and host names removed before
publication; nothing else was changed.
