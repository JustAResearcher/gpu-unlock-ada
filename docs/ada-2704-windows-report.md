# RTX 4080 16 GB (10de:2704) on Windows 11 via UEFI application

On 2026-10-30, one retail RTX 4080 16 GB (`10de:2704`, subsystem `6688:2704`, 76 SMs)
on an ASRock Z390 Taichi was unlocked from a **UEFI application** — not the Linux
kernel module — before any OS driver loaded, and the result was verified across
three cold boots. FP16 and BF16 tensor instructions with FP32 accumulation issued
at roughly twice their stock rates afterwards, measured under native Windows
PyTorch. This is one physical card on one board. It does not establish support for
other AD103 boards, other VBIOS revisions, or other firmware.

The unlock lineage is upstream work (`TOTORONG/AD103_win_unlocker`,
`JustAResearcher/gpu-unlock-ada`, `Zagitta/gpu-unlock`). The contribution here is the
port to this machine, the failure taxonomy, and the measurements.

Full notes, patch and bilingual documentation:
<https://github.com/pepsiboy87/ad103-win-uefi-port>
Discussion thread: `JustAResearcher/gpu-unlock-ada#115`

## Host

| Item | Value |
|---|---|
| GPU | RTX 4080 16 GB, `10de:2704`, subsystem `6688:2704`, chip `193000a1` |
| BDF | `0000:04:00.0` (second CPU PEG port, `00:01.1`) |
| BAR0 | `0xB1000000`, length `0x1000000` |
| Power limit | 340 W default (max 400 W) |
| CPU | 6-core / 12-thread Intel engineering sample, no iGPU |
| Board | ASRock Z390 Taichi, CSM disabled, primary graphics = PCIE |
| Console GPU | Radeon RX Vega (`1002:687f`) |
| OS / driver | Windows 11 (26100) / NVIDIA 617.42 |

## Firmware and transaction checks

Selectors were written by the UEFI application in one boot phase, then verified to
survive a PCIe FLR:

```
cold  : FEAT_PLM=0xffffff8f  SS0=0x54110026  SS1=0x0  WPR2_HI=0
after : SS0=0x88888888       SS1=0x8        matched=1 (both)
PCIe FLR returned: result=0
POST-FLR SS0=0x88888888 SS1=0x00000008
FINAL status=0 stage=post-unlock-handoff
```

`FBPA` did not reach its requested value (`matched=0`) and is not counted as an
unlocked permission, consistent with the observational behaviour described for
`10de:2705`. `WPR_CFG`, `WPR` and `FEAT` all matched.

The live PROM view bound each run to the card
(`BAR0 + 0x300000`, `0xd6c00` bytes,
SHA-256 `95cb1464e5312e03ffe904b73ce8ab33a1766520d0849b403aa595570670ed9f`).
That value is a measurement, not a distributable artifact.

## Measurements

Native Windows PyTorch (`torch.mm`, 4096³, median of three reps), same session:

| Test | stock (TFLOPS) | unlocked (TFLOPS) | ratio |
|---|---:|---:|---:|
| FP32 (negative control) | 35.49 | 35.48 | 1.00× |
| FP16 → FP32 accumulate | 91.93 | 174.35 | 1.90× |
| BF16 → FP32 accumulate | 91.94 | 176.20 | 1.92× |

Re-measured after a later cold boot: FP16 175.17 / BF16 174.68. SM clocks were not
locked in these runs; the ratio is slightly below the 1.97× measured elsewhere with
`nvidia-smi -lgc`, and the card sits on its 340 W default during sustained GEMM.

## Reproducibility and the display-cable rule

Three power cycles, same machine:

| Run | Display cable | Outcome |
|---|---|---|
| RUN10 | on the Radeon Vega | stops at `stage=display`, `GOP physical=1 target=0` |
| RUN11 | on the Radeon Vega | identical failure |
| RUN12 | on the 4080 | full chain passes, selectors rewritten, benchmark reproduced |

Cold-state `SS0` reads back `0x54110026` on every boot and is rewritten to
`0x88888888` afterwards, so the unlock is reproducible per cold boot rather than a
one-off. The display preflight requires exactly one GOP on the target card.

## What does not improve

A 19.9 GB fp8-quantised checkpoint on this 16 GB card did not speed up across
unlocked boots: 312.5 s warm versus 270.9–279.9 s recorded before the unlock on the
same machine. The gain is confined to compute-bound paths (GEMM, LoRA training,
long-context prefill); fp8/int8/GGUF inference remains transfer-bound here.

## Correction worth recording

A Turbo LoRA reported earlier as cutting a workflow from 312 s to 146 s was **not
loaded** for this model: `minimax_h3_turbo_v4_step600_ema.safetensors` lacks the
`diffusion_model.` key prefix and produced shape errors on the `adaln_proj` layers,
which the loader logs and continues past. The 146 s figure was 8 steps without it.
The matching LoRA — `minimax_h3_ref2v_turbo_4step_v0.1_comfyui_bf16.safetensors`,
whose metadata names `minimax_h3_fl2va_bf16` as base model — runs 4 steps in
104.6 s with no shape errors. Check metadata, not filenames.

## The Windows-specific blocker

The expensive part of this port was not the selector transaction but device
ownership:

- the firmware initialises the target GPU and keeps a `BY_DRIVER` open on the PCI
  handle, so an "expect no owners" admission gate can never pass;
- `DisconnectController(handle, NULL, NULL)` also tears down the PCI bus binding and
  hangs the firmware — it has to be the owning agent only, under `TPL_NOTIFY`;
- the disconnect destroys or replaces the handle, so `PciIo` must be re-acquired by
  BDF by re-enumerating handles before the transaction can be armed;
- an unstaged attempt returns `0xbadf5620` from FWSEC with `WPR2` still zero, the
  same signature reported for `#54`.

Neither binaries nor NVIDIA firmware assets are distributed with this report; the
five assets must be generated locally from a pinned driver package.
