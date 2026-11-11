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

5-minute sustained soak, median of per-loop best:

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
2725 vs 2542 MHz).

ComfyUI jobs, median seconds, default vs unlocked, bit-identical outputs:
Wan 2.2 I2V 66.2 to 46.2 (+43%), FLUX +37%, Qwen-Image-Edit +20%,
MiniMax H3 +17%, LTX +14%.

## Limits

- One card, one run per state, no repetition across cold boots.
- The ComfyUI sets used different ComfyUI core versions; the soak is the
  cleaner hardware measurement.
- This does not establish support for other AD103 boards or any other
  motherboard.
- Host tests (`Build.ps1 -RunTests`: 17 admission cases, 90 simulated transaction
  scenarios) pass, but they run on an OS-level VBIOS dump with a test-only hash pin, not
  the live pre-driver capture, and all hardware-side behaviour is simulated.
- A resident, automatic variant exists separately and is not part of this
  contribution.
