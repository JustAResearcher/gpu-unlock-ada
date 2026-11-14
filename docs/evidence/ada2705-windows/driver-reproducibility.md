# RTX 4070 Ti SUPER (10de:2705): ratio stability across reboots and driver versions

The soak and ComfyUI evidence in this folder each use one boot, one driver,
one run per state. This page adds a second, lighter-weight measurement
repeated across multiple full power-off/reboot cycles and three separate
NVIDIA driver releases, to check whether the FP32-accumulate/FP8 throttle
signature the soak measures is specific to that one boot.

## Method

Same decision metric as the soak's "f16/f32 divided by f16/f16" line:
for each short run, `mma_bench.exe` (this repository's
[`diagnostics/bench/mma_bench.cu`](../../../diagnostics/bench/mma_bench.cu),
built with `nvcc -O3 -arch=sm_89`, CUDA 13.5) measures FP16-input throughput
with FP16 accumulate (the control), FP32 accumulate and FP8 accumulate in
the same run, then takes `fp16_fp32 / fp16_fp16` and `fp8 / fp16_fp16`. Both
ratios cancel clock/driver-level throughput drift because the control and
the measured mode are timed together.

14 short sessions, 5 separate cold boots (full power-off, not a warm
restart), 3 NVIDIA driver releases:

| State | Driver | Boot (UTC) | Sessions | fp16/fp32 ÷ fp16/fp16 | fp8 ÷ fp16/fp16 |
|---|---|---|---:|---|---|
| Locked (stock) | 610.88 | 2026-11-13 06:15 | 3 | 0.499 – 0.506 | 0.997 – 1.016 |
| Locked (stock) | 621.24 | 2026-11-13 06:59 | 3 | 0.500 – 0.502 | 0.995 – 1.002 |
| Unlocked | 621.14 | 2026-11-12 05:45 | 4 | 0.971 – 0.999 | 1.986 – 2.034 |
| Unlocked | 621.24 | 2026-11-13 13:37 | 3 | 0.954 – 1.046 | 1.886 – 2.072 |
| Unlocked | 621.24 | 2026-11-14 03:49 | 1 | 0.995 | 2.007 |

Pooled: every locked session falls in 0.499–0.506 / 0.995–1.016; every
unlocked session falls in 0.954–1.046 / 1.886–2.072. The two bands do not
overlap (a gap of at least 0.3 on both ratios) on any of the three driver
versions, across five independent boots.

## What this does and does not show

- It confirms the soak's ratio-based decision metric reproduces after a full
  power cycle and after two driver updates (610.88 to 621.14 to 621.24), not
  just within the single boot session the soak and ComfyUI tables were
  measured on.
- It is **not** a repeat of the full 5-minute soak (no cuBLAS, no sustained
  telemetry, no controls beyond f16/f16) and does not re-measure the
  ComfyUI job times. Treat the soak's TF/s table and the ComfyUI table as
  the single-boot measurements they are; this page only shows that the
  ratio those tables use as their pass/fail line is stable, not that the
  absolute throughput or wall-clock job times are.
- Same single physical card and motherboard as the rest of this evidence
  set; it does not establish support for other AD103 boards.
