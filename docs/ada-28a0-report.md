# RTX 4060 Laptop GPU (10de:28a0) Ada canary

On October 25, 2026, the AD102-family four-PLM payload ran on one
RTX 4060 Laptop GPU (`10de:28a0`, AD107M, 8 GB, Max-Q) before the unmodified
NVIDIA `615.78.08` driver on kernel `7.3.0-4-cachyos`. Every gate the module
enforces passed, and the module's own effective speed-select readback changed
from fields `0 0 2 0 0 2 0 2 0` (`full=0`) to all zero (`full=1`) within the
same boot, with the RM getter reporting full rate on every field afterwards.
This is one physical card and one transaction. No same-card stock
instruction-rate run has been made, so the report does not show a matched
stock/unlocked benchmark pair; the same-boot selector readback and the RM
readout are the rate evidence. It does not establish support for other AD107
boards or IDs.
The [evidence directory](evidence/ada28a0/README.md) contains the selected
boot messages, rate readbacks and unedited benchmark stdout with the GPU UUID
replaced.

The host ran CachyOS with kernel `7.3.0-4-cachyos`; the module was built by
DKMS from this branch and shipped in the initramfs by the mkinitcpio drop-in,
signed with the host's local MOK key, so its `.ko` hash is host-specific and
not recorded here. The boot used the **single exact PCI target**
`gpu_unlock.targets=0000:01:00.0` and `gpu_unlock.ad107_experimental=1`.

## Warm-state caveat

Unlike the 2705 and 2805 canaries this transaction did not start from a cold
boot. The card had been fully initialized by the stock driver about three
minutes earlier; the driver chain (`nvidia`, `nvidia_modeset`, `nvidia_drm`,
`nvidia_uvm`) was then unloaded, every GPU-holding process was terminated, and
the module was loaded with the experimental opt-in. An earlier attempt the
same evening, where the stock driver was still bound, was refused by the
module with `-EBUSY` and left the card untouched. The card completed the
transaction from this driver-initialized warm state, produced a healthy
`nvidia-smi` and a running desktop session after the driver was reloaded, and
survived subsequent driver unload/reload cycles. A cold-boot reproduction on a
second card is still the required standard.

## Firmware and driver checks

The module loaded the pinned NVIDIA `610.43.03` stock AD102 Booter/GSP assets
and the generated 64,000-byte payload. The kernel log recorded:

- entry: `plm=0xffffff8f`, `ss0=0x21025116`, `ss1=0x7`, `wpr2_hi=0x0`, chip
  `0x197000a1`;
- payload marker `0x67b00001` (expected `0x67b00001`), SEC2 halted,
  `debug=0xda550000`;
- permission readbacks: `WPR_CFG[0x1fa7cc]=0x4cb8f`, `WPR[0x1fa7c4]=0x4cb8f`,
  `FEAT[0x823804]` `0xffffff8f` → `0xffffffff` across the transaction, with
  the observational `FBPA[0x9a0148]=0xffffff0f` unchanged, as on the 2705 and
  2805;
- selector writes read back `SS0=0x88888888` (`matched=1`) and `SS1=0x8`
  (`matched=1`), and the effective readout changed from fields
  `0 0 2 0 0 2 0 2 0` (`full=0`) to all zero (`full=1`);
- after PCIe FLR: BAR ready with the same chip value, post-FLR GFW
  `error=0`, SEC2 reset released, GSP halted, `WPR2=1ffffe00:00000000`
  (HI.VAL=0 as the stock driver requires), selectors still
  `SS0=88888888 SS1=00000008`;
- `gpu_unlock: unlocked`.

The stock driver then loaded (`nvidia-drm` on minor 0, framebuffer device
created) and CUDA kernels completed. The current-boot kernel log contains no
Xid and no GPU-fallen-off message.

The read-only RM getter reported every field at 0 (full):

| Field | After |
| --- | ---: |
| FMLA16, FMLA32, IMLA0-IMLA4, DP, FFMA | 0 (full) |

No stock reading was taken on this card. The same-boot readout transition
`full=0` → `full=1` and the stock field pattern `0 0 2 0 0 2 0 2 0` (identical
to the 2705's pre-unlock readout) stand in for it.

## Unlocked instruction rates

`diagnostics/bench/tensor_modes_issue.cu` was built with CUDA 13.4 for
`sm_89` (cubin SHA-256
`267963e693e2beba7aba9b29a6f517478cffe676851a3eb3154011c160f0b194`) and run
through `tensor_modes_driver_runner.py` with 20,000 iterations and three
repetitions (1056 blocks, 256 threads, eight accumulator chains per warp).
The runner's exact-power-cap assertion was bypassed because this Max-Q part
reports no enforced power limit (`[N/A]`; the SBIOS manages the budget
dynamically). All 270,336 output cells per mode matched the all-ones
reference:

| Mode | Best rate | Stock expectation |
| --- | ---: | ---: |
| FP16 → FP32 accumulate (`f16_f32`) | 57.10 Top/s | half rate (~28.5) |
| TF32 → FP32 accumulate (`tf32`) | 28.65 Top/s | half rate (~14.3) |
| unsigned INT4 → INT32 (`u4_s32`) | 225.11 Top/s | unchanged |

The stock expectations are structural, not measured: on stock Ada consumer
silicon the FP32-accumulate tensor rate is exactly half the FP16-accumulate
rate, and the module's own readback shows the half-rate selector state
(`full=0`, fields `0 0 2 0 0 2 0 2 0`) before the transaction and the
full-rate state (`full=1`, all zero) after it in the same boot. INT4 stayed at
its baseline, as on the 2705 and 2805.

A supplementary cuBLAS probe (`cublasGemmEx`, FP16 in/out,
`CUBLAS_COMPUTE_32F`, 20 timed iterations after three warm-ups) measured
33.56 TF/s at 4096³, 32.48 TF/s at 8192³ and 31.97 TF/s at 12288³ at a
sustained 2475 MHz SM clock, roughly twice the rates commonly reported for
this board in its stock state. This probe is not part of the repository's
harness and no same-card stock run was made; it is recorded as context only.

`validate_gemm.cu` (cuBLAS against a CPU reference, three seeded FP16/BF16
input sets with square, rectangular and non-tile-aligned shapes) reported
`failures=0, max_abs=0` on all six cases, i.e. bit-exact FP32-accumulate
outputs.

## Limitations

- One card, one transaction, warm entry state; no cold-boot reproduction.
- No same-card stock instruction-rate run; the rate evidence is the module's
  same-boot selector/readout transition and the RM getter.
- The exact-power-cap assertion of the runner was bypassed for this Max-Q
  board (no enforced limit is exposed).
- Other AD107 IDs (desktop `10de:2882` variants, other mobile bins) and other
  Ada chips are not covered by this report.
