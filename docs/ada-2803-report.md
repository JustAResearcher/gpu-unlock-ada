# RTX 4060 Ti 16GB (10de:2803) Ada canary with a matched stock/unlocked benchmark

On October 25, 2026, the AD102-family four-PLM payload ran on one RTX 4060 Ti
16GB (`10de:2803`, AD106, 34 SMs) before the unmodified NVIDIA `615.78.08`
driver. Every gate the module enforces passed, and a matched same-card
benchmark measured FP16/FP32 and TF32/FP32 tensor issue rate at exactly twice
the stock rate. This is one physical card, one cold boot, and one ordinary
warm reboot that re-ran the transaction. It does not establish support for
other AD106 boards or the 8 GB variant; if `10de:2803` is shared with an 8 GB
board, that variant remains untested.
The [evidence directory](evidence/ada2803/README.md) contains the selected
boot messages, rate readbacks and unedited benchmark stdout with the GPU UUID
replaced.

This card reports `10de:2803`, not the `10de:2805` ID of the
[2805 canary](ada-2805-report.md). A second 16 GB 4060 Ti reporting `2803` is
described in issue #22. Unlike the 2805 canary, this report includes a
same-card stock throughput run, so it shows a matched speedup.

The host runs Garuda Linux with kernel `7.2.11-zen1-1-zen` (dracut initramfs,
DKMS-built module) and the open `nvidia-open-dkms`/`nvidia-utils` `615.78.08`
packages. The host has a second GPU (RTX 5060 Ti, GB206 `10de:2d04`), which
the module never selects: the experimental AD106 target requires both
`ad106_experimental=1` and the card's single exact PCI address on the kernel
command line, and the device-ID gate cannot match the GB206. The boot used the
**single exact PCI target** and `ad106_experimental=1`. The card owner
performed a full power-off before the attempt and confirmed it was a cold
boot; the module's entry readback reported WPR2 high `0x0`.

## Firmware and driver checks

The module loaded the pinned NVIDIA `610.43.03` stock AD102 Booter/GSP assets
and the generated 64,000-byte payload. The kernel log recorded:

- entry: `SS0=0x16672044`, `SS1=0x7`, WPR2 high `0x0`, chip `0x196000a1`;
- payload marker `0x67b00001` (expected `0x67b00001`), SEC2 halted;
- the three required permission writes: `WPR_CFG=0xfffff0ff`,
  `WPR=0xffffffff`, `FEAT=0xffffffff`. The observational FBPA field stayed
  `0xffffff0f`, as on the 2705 and 2805;
- selector writes read back `SS0=0x88888888` and `SS1=0x8`, and the effective
  readout changed from fields `0 0 2 0 0 2 0 2 0` to all zero;
- after PCIe FLR the selectors were still `SS0=88888888 SS1=00000008`;
- `gpu_unlock: unlocked`.

The stock driver then loaded and CUDA kernels completed. No Xid was logged.
An ordinary warm reboot re-ran the transaction from the initramfs with the
same entry readback (`WPR2` high `0x0`), the same gates and the same
`gpu_unlock: unlocked` verdict.

## Matched throughput

Both runs used the same generated `tensor_modes_issue_sm89.cubin`
(SHA-256 `33aa6db385aa9eb8a1f73bb2f74891aea84794326b7c40992bc642f93a3c614d`),
20,000 iterations per rep, 3 reps, the card's 165 W default power cap, and
the same `615.78.08` driver. All 270,336 cells per mode matched (0
mismatches) in both states.

| Mode | Stock | Unlocked | Ratio |
| --- | ---: | ---: | ---: |
| FP16/FP32 accumulate | 46.63 TF/s | 93.28 TF/s | 2.00x |
| TF32/FP32 accumulate | 23.31 TF/s | 46.45 TF/s | 1.99x |
| unsigned INT4/INT32 | 370.86 TOP/s | 370.86 TOP/s | 1.00x |

The FP16/FP32 mode ramps: the first benchmark invocation after a boot reads
about 87 TF/s with all repetitions at 64.9 ms, and later invocations converge
to 92.9-93.3 TF/s at 59.6 ms. The warm-boot run's sustained rate was 92.89
TF/s. TF32/FP32 and INT4/INT32 hit identical times on every boot from the
first repetition.

## RM getter caveat

The read-only RM getter (`tools/read_sm_rates.py`, public RM call
`0x20801230`) read `fmla32=1, imla2=1, imla4=1` (half rate) before the
unlock, matching the 2805 canary's stock profile. After the unlock it reads
`3` for those fields on this driver, not `0`: the encoding of the flipped
state is driver-version-dependent (the 2805 canary read `0` on driver
`595.99.02`). The getter also fails outright on the host's GB206 second GPU
(RM status `0x1b`), so any successful read comes from the 4060 Ti. The
module's own post-FLR readout (`full=1`) and the matched benchmark above are
the rate evidence for this card.

## Scope

This is one card with two successful unlock transactions (one cold boot, one
warm reboot). Independent cold-boot reproduction and application-level
numerical validation remain pending, as on the other canaries. Shared
firmware across Ada chips does not establish support for untested cards.
