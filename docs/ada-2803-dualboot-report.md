# RTX 4060 Ti 16GB (10de:2803) and RTX 3060 (10de:2504) unlocked in one boot

On October 27, 2026, one cold boot with
`gpu_unlock.targets=0000:04:00.0,0000:05:00.0 gpu_unlock.ad106_experimental=1`
unlocked an RTX 3060 12GB (`10de:2504`, GA106, 28 SMs) and an RTX 4060 Ti
16GB (`10de:2803`, AD106, 34 SMs) simultaneously. Both cards reported
`gpu_unlock: unlocked`; the AD102-family four-PLM payload ran on the 4060 Ti
with `valid=1 full=1`. The stock NVIDIA `595.99.02` driver then bound to both
cards and completed CUDA work on each.

The host is a consumer X99 board running Linux Mint 21.3, kernel
`5.15.0-190-generic`, with the 3060 as the boot VGA device driving the displays
and the 4060 Ti as a compute-only second card. The DKMS module source was
commit `d28c485` (version `1.1.1`). Firmware was the pinned `610.43.03` set,
five GA102 and four AD102 assets.

No prior report of a mixed Ada+Ampere unlock boot was found in `docs/` or
`docs/evidence/`. This is one board, one card of each type, and two cold starts;
it does not establish that the combination is safe on other hardware.

## Boot evidence

Both cold starts unlocked both targets. The kernel log for the measured boot:

```
pci 0000:05:00.0: AD102 selectors-applied: readout1=0x0 fields=0 0 0 0 0 0 0 0 0 valid=1 full=1
pci 0000:05:00.0: gpu_unlock: unlocked
pci 0000:04:00.0: gpu_unlock: unlocked
```

Each target is processed independently: an `unlocked` line appears once per
slot, and a failure on one slot does not abort the other. The 3060 used the
GA10x path (`selector result: matched=1`, `SS0=0x88888888`), the 4060 Ti the
AD102 payload plus `ad106_experimental=1`. Both ended at `SS0=88888888`,
`SS1=00000008` after the FLR. No NVRM Xid, GPU-fallen-off or GSP failure
appeared in the boot in which the measurements below were taken.

## Matched issue-rate comparison

The repository's `diagnostics/bench/tensor_modes_issue.cu` was built for
`sm_89` with CUDA 13.0 and run through `tensor_modes_driver_runner.py` with
20,000 iterations and three repetitions. The cubin SHA-256 was
`e90cc66d2af181e1741ab8845bcb192b63e1d1ad37a23d3fca2108d7bb8565ca`; the same
cubin, driver, card and cap were used in both states. All 270,336 output cells
per mode were checked and matched the expected value in every run.

| Mode | Stock | Unlocked | Ratio | Mismatches |
| --- | ---: | ---: | ---: | ---: |
| FP16 → FP32 accumulate | 45.87 TF/s | 91.37 TF/s | 1.99× | 0 |
| TF32 → FP32 accumulate | 22.93 TF/s | 45.69 TF/s | 1.99× | 0 |
| Unsigned INT4 → INT32 | 364.78 TOP/s | 364.78 TOP/s | 1.00× | 0 |

Kernel time halved exactly, from 120.7 ms to 60.6 ms per 20,000 iterations.
These figures agree with the [2803 canary](ada-2803-report.md) (46.4 → 92.3
FP16, 23.2 → 46.2 TF32, driver `615.78.08`) within 1 %, on a different driver
and a different board.

## The power cap does not bind the issue rate

The probe was repeated at 150 W and 165 W in both states:

| Mode | Stock 150 W | Stock 165 W | Unlocked 150 W | Unlocked 165 W |
| --- | ---: | ---: | ---: | ---: |
| FP16 → FP32 | 45.87 | 45.87 | 91.37 | 91.37 |
| TF32 → FP32 | 22.93 | 22.93 | 45.69 | 45.69 |
| INT4 → INT32 | 364.78 | 364.78 | 364.79 | 364.78 |

The probe is register-resident and issues `mma.sync` from registers with no
meaningful memory traffic, so within this range it measures issue rate and
nothing else. A power cap in this range neither delivers nor costs issue rate,
in either state.

## A GEMM converts less of it

A sustained cuBLAS GEMM on the same boot is the more conservative view of what
an application sees. `torch.mm`, 8192³, FP32 accumulation, warmed for ~2.5 s
and then measured over twelve 50-repetition batches (median of the last six,
spread under 1 %):

| Card / cap | Mode | Stock | Unlocked | Ratio |
| --- | --- | ---: | ---: | ---: |
| 3060 @120 W | FP16 | 24.91 | 40.18 | 1.61× |
| 3060 @120 W | BF16 | 25.20 | 41.54 | 1.65× |
| 3060 @120 W | TF32 | 12.24 | 20.49 | 1.67× |
| 4060 Ti @150 W | FP16 | 45.85 | 73.17 | 1.60× |
| 4060 Ti @150 W | BF16 | 45.56 | 72.10 | 1.58× |
| 4060 Ti @150 W | TF32 | 20.97 | 31.09 | 1.48× |

The instruction rate doubles; a stock GEMM converts roughly 1.6× of the gain on
both cards, because at this shape it is bound by memory traffic and tile
selection rather than by mma issue. The 3060 figures match the independently
measured Ampere result (24.86 → 40.30 FP16 at 120 W), which is the control for
the method.

Two cautions on benchmarking this card, both of which produced wrong numbers
here before they were caught:

- A short run measures boost, not steady state. A single 8192³ FP16 matmul is
  about 16 ms, so a five-repetition median measures 80 ms of work and is not
  reproducible; the same configuration read 83.3 TF/s on one run and 68.2 on
  the next. Warm up for seconds and report the min/max spread.
- `tensor_modes_driver_runner.py` asserts a single CUDA device and parses an
  unfiltered `nvidia-smi`. On a two-GPU host it must be given
  `CUDA_VISIBLE_DEVICES=<uuid of the target>` and an `nvidia-smi` wrapper that
  passes `-i <uuid>`. Note that CUDA's default device order is fastest-first,
  so without `CUDA_DEVICE_ORDER=PCI_BUS_ID` the 4060 Ti is `cuda:0` on this
  host even though `nvidia-smi` lists it second.

## Install notes for a mixed Ada+Ampere host

Three failures occurred during this install and are reproducible:

- Installing `dh-dkms` from the NVIDIA CUDA repository removes `dkms`, which
  cascades to `nvidia-dkms-*` and `nvidia-driver-*` and deletes every built
  module. The running desktop survives on the loaded module and the next boot
  has no driver. Build with plain DKMS instead of the debhelper path.
- `sudo make` resolves `$(HOME)` to `/root`, so the firmware install looks for
  assets under `/root/.cache/gpu-unlock` and fails. Use
  `sudo HOME=<user> make install-firmware`.
- `dkms remove <old version>` deletes the shared module path, so remove the
  superseded version before installing the new one.

`grub-mkconfig` executes every executable file in `/etc/grub.d/` regardless of
name, so a `cp -a` backup of a custom entry keeps mode 755 and is sourced
alongside the live one, duplicating its entries. Backups need `chmod 644`.

## Not yet done

- more than two cold starts, and any long-run stability testing;
- other 2803 and 2504 boards, and the 4060 Ti 8 GB variant;
- application workloads (image/video generation), which the GEMM figures above
  only bound loosely;
- whether an unlock failure on one target can leave the other in a bad state.
  It was not observed here, but both targets succeeded in both boots, so the
  path was never exercised.
