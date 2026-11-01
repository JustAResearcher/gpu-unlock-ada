# RTX 4090 Laptop 10de:2757 - Alienware m18 R1 Windows UEFI cross-check

Windows UEFI pre-driver unlock via
`physicsdolphin/nv-uefi-unlocker` on an Alienware m18 R1, with a matched
stock/unlocked `mma_bench` pair.

This is a **Windows UEFI cross-tool data point**, not the `gpu-unlock-ada`
Linux kernel module. It does not replace the same-kernel Linux stock/unlocked
pair requested in #4/#76; that work remains pending.

## Result

| mode | stock TF/s | unlocked TF/s | ratio |
|---|---:|---:|---:|
| f16/f16 | 177.3 | 177.7 | 1.0023 |
| **f16/f32** | **88.8** | **177.4** | **1.9977** |
| **bf16/f32** | **88.7** | **176.0** | **1.9842** |
| s8/s32 | 354.2 | 353.9 | 0.9992 |
| fp8/e4m3 | 177.3 | 352.6 | 1.9887 |
| dp4a | 88.0 | 87.9 | 0.9989 |
| ffma | 42.2 | 42.3 | 1.0024 |

Controls `f16/f16`, `s8/s32`, `dp4a` and `ffma` are flat; FP8 also scales
about 2x. The 2757 Clevo report (#4) had a green transaction with no
throughput change; this m18 R1 and the m18 R2 (#76) are positive. The result
is board-specific.

## Hardware

| Item | Value |
|---|---|
| Laptop | Alienware m18 R1 |
| GPU | RTX 4090 Laptop GPU `10de:2757` (AD103M), 76 SMs |
| Subsystem | `1028:0c49` |
| VBIOS | `95.03.2a.00.47` |
| BIOS | `1.35.0` |
| OS | Windows 11 Pro for Workstations 25H2, build 26220.9757 |
| Driver | NVIDIA 596.49 |
| Power | 125 W default / 175 W max |
| Display | Hybrid / Advanced Optimus; UEFI entry dGPU D0 (PMCSR=0x0008) |
| Power plan | Windows Balanced; no NVOC, no power-limit/overclock mod |

## Tool and loader

- UEFI tool: `physicsdolphin/nv-uefi-unlocker` v0.2.0 release assets, with
  `BOOTX64.EFI` built from `main` via `cargo efi-tx`.
- `make-usbroot.ps1 -Family ad102`; `v67-ad102-four-plm.bin` present
  (unlock mode). No `tx-select-bdf.txt`, no `i_know_what_i_am_doing.txt`.
- Local loader SHA-256:
  `6707B44EC20239301EE52ED09FF950D12D07E429C25D8E67228387ED7F516188`.
  This local build changes only `keywait.rs` to ignore a stuck `Z` key
  (`0x1a` / `'z'` / `'Z'`) while waiting for Space; the transaction code is
  unchanged. Standard loader backup SHA-256:
  `ECA8248A891EA74D21E208B07F0B4BEA72E918FCDC2B4D89EB6B09EC3A58A708`.
  The laptop's Z key is stuck at boot; this is a machine-specific
  confirmation-key workaround, not part of the transaction.
- Benchmark: `diagnostics/bench/mma_bench.cu` from this repository,
  compiled with CUDA 13.3 + MSVC, `20000` iterations x `5` reps. Binary
  SHA-256:
  `1F8509FA5CB3B8FFE8D714759DDD1FCF3C6CB5F67CAB4DA11C4E8143FFA7C5C3`.
- No nvoc/power-limit/overclock test was run.

## Transaction

Key lines from `loader.log` (full log in this directory):

```text
TX: device 10de:2757 -> asset family 'ad102'
TX: V67 present - unlock mode (family-specific write chain)
SCRUB: handoff DONE - SEC2/GSP domains un-gated
FWSEC: WPR2 verified
SEC2 MAILBOX0=0x67b00001
POST-BOOT: FEAT_PLM=0xffffffff
SELECTOR RESULT: SS0=0x88888888 SS1=0x8
POST-FLR: verified (selectors survive AON)
HANDOFF-OUT: ss0=0x88888888 ss1=0x8 readout1=0x00000000 feat_plm=0xffffffff wpr2_hi=0x0 pmcsr=0x0008 dstate=D0 | ss0_unlock=true ss1_unlock=true plm_open=true
FINAL: transaction complete, all DMA released
```

## Matched benchmark

Same `mma_bench.exe`, same driver, same Windows power plan. Stock run:
full shutdown, AC unplugged 30 s, Windows boot. Unlocked run: UEFI boot
with `v67` present, then Windows.

Raw outputs are `mma-stock.txt` and `mma-unlocked.txt` in this directory.

## Caveats

- One machine, one stock/unlocked pair; Windows/UEFI, not this repo's Linux
  kernel module. The official 2757 Linux same-kernel pair is still pending.
- `mma_bench` is register-resident issue rate; no cuBLAS GEMM was run because
  the CUDA 12 cuBLAS runtime is not installed on this Windows host.
- The Z-key keywait patch is local and machine-specific; transaction code is
  unchanged. Standard loader is backed up locally.
- The Clevo 2757 report (#4) showed the same green transaction with no
  throughput change; the m18 R1 (this report) and m18 R2 (#76) are positive.
- No NVIDIA firmware, generated payload, UUID or hostname is included. The
  `loader.log` contains PCI addresses and register values, as in the other
  evidence directories.

## Reproduce

Build and run the benchmark on Windows:

```powershell
nvcc -O3 -arch=sm_89 -std=c++17 -o mma_bench.exe diagnostics/bench/mma_bench.cu
.\mma_bench.exe 20000 5
```

The UEFI side uses `nv-uefi-unlocker` v0.2.0 assets with a loader built from
`main` via `cargo efi-tx` and a FAT32 test disk containing
`v67-ad102-four-plm.bin`. The local loader keywait patch is only for the
stuck-Z confirmation collision described in #110.

## References

- Issue: https://github.com/JustAResearcher/gpu-unlock-ada/issues/132
- Clevo/Lenovo 2757 thread: https://github.com/JustAResearcher/gpu-unlock-ada/issues/4
- Alienware m18 R2 result: https://github.com/JustAResearcher/gpu-unlock-ada/issues/76
- UEFI tool: https://github.com/physicsdolphin/nv-uefi-unlocker
- UEFI tool stuck-Z issue: https://github.com/physicsdolphin/nv-uefi-unlocker/issues/110
- Windows evidence precedent: `docs/evidence/ad104-windows/`

## Evidence files

| File | SHA-256 |
|---|---|
| `mma-stock.txt` | `86DD71947518246AD522B986C1A761873BDF7412E01F0D3497ACEE64E3A8B152` |
| `mma-unlocked.txt` | `D7EE76D472FAA50994AECA602906706BA2022D7FFD542823738E472912670984` |
| `loader.log` | `F554BCF506ED2B79C1DCF89550CA83AA8D8464B7342A49DC0EBE24CA79C13934` |
| `mma_bench.exe` | `1F8509FA5CB3B8FFE8D714759DDD1FCF3C6CB5F67CAB4DA11C4E8143FFA7C5C3` |
| local loader | `6707B44EC20239301EE52ED09FF950D12D07E429C25D8E67228387ED7F516188` |
| standard loader backup | `ECA8248A891EA74D21E208B07F0B4BEA72E918FCDC2B4D89EB6B09EC3A58A708` |

`SHA256SUMS` covers the text evidence files in this directory; the benchmark
binary and loader are not redistributed.
