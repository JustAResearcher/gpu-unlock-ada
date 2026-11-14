# RTX 4070 Ti SUPER (10de:2705): Windows UEFI port

[`windows/ad103-z790`](../windows/ad103-z790/README.md) adapts the
[`ad104-msi`](ad104-windows-latence-app.md) Windows UEFI port to one AD103
board: an RTX 4070 Ti SUPER in an MSI MPG Z790 EDGE WIFI, VBIOS
95.03.45.80.07, 285 W limit, NVIDIA 621.14, Windows 11.

It is a **manual, USB-only** flow: boot the stick, press `U` within 30 s for
one attempt; `S`, `Esc` or the timeout continues to normal Windows. It writes
no boot variables, opens the ESP read-only and needs Secure Boot off for the
unsigned build. The unlock is volatile: a warm reboot keeps it, a full
power-off clears it.

## Results

Two matched sets on the same card, stock vs unlocked, in the
[combined evidence](evidence/ada2705-windows/README.md) (SHA-256 manifest
included).

**ComfyUI production workloads**, median seconds, default vs unlocked,
**every pair bit-identical in output** (three video models, one image
model, one image-edit model):

| Workload | Default | Unlocked | Speedup |
|---|---:|---:|---:|
| Wan2.2 I2V A14B (GGUF Q4_K_S, 4-step LoRA), 832x480, 49 frames | 66.2 s | 46.2 s | **+43%** |
| FLUX.1 dev fp8, 1024x1024, 20 steps | 22.1 s | 16.1 s | **+37%** |
| Qwen-Image-Edit 2511 (fp8, 4-step LoRA), ~1 MP | 12.1 s | 10.1 s | **+20%** |
| MiniMax H3 I2V (turbo, 6 steps), 832x480, 56 frames | 28.1 s | 24.1 s | **+17%** |
| LTX-2.5 22B distilled int8 I2V, 832x480, 49 frames | 14.1 s | 12.4 s | **+14%** |

The two heaviest matmul-bound workloads (Wan2.2, FLUX) gain the most; full
raw timings, GPU telemetry and caveats (notably a ComfyUI core-version
difference between the two sets) are in
[`comfyui-workloads/BENCHMARKS.md`](evidence/ada2705-windows/comfyui-workloads/BENCHMARKS.md).

5-minute sustained tensor-core/GEMM soak, median of per-loop best:

| Test | Stock TF/s | Unlocked TF/s |
|---|---:|---:|
| mma f16, f32 accumulate | 88.8 | 195.7 (2.20x) |
| mma bf16, f32 accumulate | 90.4 | 195.4 |
| mma fp8 e4m3, f32 accumulate | 182.1 | 392.2 |
| cuBLAS fp16, f32 accumulate | 84.5 | 135.2 |
| mma f16, f16 accumulate (control) | 182.6 | 195.4 |
| mma s8 (control) | 368.6 | 393.6 |
| FP32 FFMA (control) | 45.2 | 47.2 |

f16/f32 divided by f16/f16 goes from 0.493 to 0.999. Controls move 3-7%,
consistent with clock and power differences (peak 277-278 W, SM average
2725 vs 2542 MHz). This same ratio, re-measured across 5 cold boots and 3
driver releases (610.88, 621.14, 621.24; 14 sessions,
[`driver-reproducibility.md`](evidence/ada2705-windows/driver-reproducibility.md)),
lands in the same two non-overlapping bands every time.

## Limits

- The soak and ComfyUI tables are one run per state on one boot. A
  lighter ratio-only check (the soak's decision metric) was repeated
  across 5 separate cold boots and 3 driver releases and landed in the
  same two non-overlapping bands every time — see
  [`driver-reproducibility.md`](evidence/ada2705-windows/driver-reproducibility.md).
  The full soak and the ComfyUI wall-clock numbers themselves were not
  repeated across boots.
- The ComfyUI sets used different ComfyUI core versions; the soak is the
  cleaner hardware measurement.
- This does not establish support for other AD103 boards or any other
  motherboard.
- Host tests (`Build.ps1 -RunTests`: 17 admission cases, 90 simulated transaction
  scenarios) pass, but they run on an OS-level VBIOS dump with a test-only hash pin, not
  the live pre-driver capture, and all hardware-side behaviour is simulated.
- A resident, automatic variant exists separately and is not part of this
  contribution.
