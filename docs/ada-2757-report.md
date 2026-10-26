# RTX 4090 Laptop GPU (10de:2757, AD103M): unlock runs, throughput unchanged

**Status: inconclusive, one card.** On one RTX 4090 Laptop GPU (`10de:2757`),
the AD102-family boot payload completed, every permission and selector
readback matched, and the RM rate getter changed from stock to full. A
matched CUDA run before and after showed **no change** in FP16/BF16 tensor
or GEMM throughput. This report records what was observed so maintainers can
judge whether anything further is possible on this part. It does not claim a
speedup.

## Hardware and software

| Item | Value |
|---|---|
| GPU | NVIDIA GeForce RTX 4090 Laptop GPU, `10de:2757` rev a1, CLEVO/KAPOK subsystem `1558:3702` |
| Driver | NVIDIA `580.178.04` (open-source module build, DKMS) |
| Boot assets | Pinned NVIDIA 610.43.03 inputs, AD102 four-PLM payload |
| Option | `gpu_unlock.targets=<exact address> gpu_unlock.ad103m_experimental=1` |
| Display | Intel iGPU drives the display; dGPU in compute mode |
| Stock kernel | `7.3.0-2-cachyos-bore-lto` |
| Unlocked kernel | `7.2.11-zen1-1-zen` |
| Fork revision | commit `6dfb7d2` (see "A routing bug found on the way") |

The stock and unlocked runs used different kernels. The driver, binaries,
GPU, launch settings and power profile were the same. A strict pair on one
kernel has not been done. `power.limit` reads `[N/A]` on this mobile part
(`power.default_limit` is 150 W), so the cap was not recorded numerically.

## What the unlock reported

From the kernel journal of the unlocked boot (PCI addresses and UUIDs
omitted):

- Assets requested were the AD102 set: `v67-ad102-...-four-plm.bin`,
  `gsp_boot_ad102.bin`, `gsp_boot_desc_ad102.bin`, `booter_load_ad102.bin`.
- FWSEC-FRTS ran: `scratch=0x0`, WPR2 low `0x3ff6000`, high `0x3ff6e00`,
  matching the expected page.
- AD102 permission registers before and after the payload:

| Register | Before | After | Expected |
|---|---|---|---|
| WPR_CFG `0x1fa7cc` | `0x4cb8f` | `0xfffff0ff` | `0xfffff0ff` |
| WPR `0x1fa7c4` | `0x4cb8f` | `0xffffffff` | `0xffffffff` |
| FEAT `0x823804` | `0xffffff8f` | `0xffffffff` | `0xffffffff` |
| FBPA `0x9a0148` (not required) | `0xffffff0f` | `0xffffff0f` | `0xffffffff` |

- Payload marker `0x67b00001` matched; the falcon halted.
- Selectors written and verified: `SS0=0x88888888`, `SS1=0x8`.
- Readout after the selector write: all nine fields `0`, `full=1`.
- Final line: `gpu_unlock: unlocked`. No `NVRM: Xid` was logged.

RM rate getter (`tools/read_sm_rates.py`, `0=full, 1=half`):

| Rate | Stock | Unlocked |
|---|---|---|
| fmla32 | 1 | 0 |
| imla2 | 1 | 0 |
| imla4 | 1 | 0 |
| all others | 0 | 0 |

## What the workloads showed

Same binaries (hash-checked), same exact GPU UUID, performance power profile.
Stock is `stock-compute-perf`, unlocked is `unlocked-zen`.

| Probe | Stock | Unlocked |
|---|---|---|
| `mma_bench` f16/f32 (TF/s) | 90.7 | 90.7 |
| `mma_bench` bf16/f32 | 90.7 | 90.7 |
| `mma_bench` s8/s32 | 362.7 | 362.7 |
| `mma_bench` dp4a | 90.3 | 90.3 |
| `mma_bench` ffma | 43.6 | 43.6 |
| GEMM FP16/FP32 n=8192 median | 87.48 | 87.46 |
| GEMM BF16/FP32 n=8192 median | 87.43 | 87.31 |
| GEMM FP16/FP32 n=4096 median | 85.69 | 85.71 |
| `validate_gemm` | 6 cases, 0 failures | 6 cases, 0 failures |
| Peak sampled SM clock / power | 2355 MHz, 169.9 W | 2355 MHz, 163.7 W |

Hybrid-mode and compute-mode stock runs gave the same throughput as well, so
the stock figures do not depend on display mode or power profile here.

## Why this looks like "still half rate"

This is an estimate, not a measurement of the rate setting. The part has an
expected 76 SMs (not read from the device in this run). At the half
FP16/FP32-accumulate rate of 512 FLOP per SM per clock and the 2355 MHz seen
in the benchmark:

76 x 512 x 2.355 GHz = about 91.6 TFLOPS, versus 90.7 measured.

A full-rate result would be roughly twice that. The measured value is the
half-rate figure both before and after. Lower-binned parts are expected to
have fewer SMs or lower clocks, not a different per-SM rate, so binning does
not explain an unchanged rate by itself.

## Open questions

1. Does the AD102 payload write the register or fuse selector that actually
   governs tensor issue rate on AD103M, or only the one the RM getter reads?
   On this card the getter changed and the hardware behaviour did not.
2. Does AD103M need different payload offsets or a different Booter image
   than the AD102 family? The 2705 (AD103) canary used the same AD102 assets
   and did show a change, so the difference may be specific to the mobile
   part.
3. Is the half-rate limit on this part enforced by something outside the
   boot-time transaction, such as VBIOS, EC, or a driver-side check?
4. The FBPA `0x9a0148` readback stayed `0xffffff0f` rather than the
   `0xffffffff` the check lists as expected (it is not a required register);
   is that normal on AD103M?

## Not done

- The tensor-mode probe in `diagnostics/bench/` (FP16/TF32 and unsigned INT4
  controls) was not run, so TF32 and INT4 behaviour is unknown.
- No strict same-kernel pair, no repeat boot, no long-run stability.
- `nvidia-smi` briefly showed power draw around 573-593 W during an idle
  query (limit 150 W) while the benchmark telemetry peaked at 163.7 W. Treat
  the high readings as unexplained; they were not logged to a file.
- No comparison against a Windows/other-OS boot of the same card.

## A routing bug found on the way

Commit `16978f9` added the `0x2757` ID and parameter but did not add it to
`is_ad102()` or `unlock_assets()`, so the first attempt used the GA102
firmware and path. It failed with `unlock failed (-5)`, SEC2 registers
reading `0xbadf5620`, and left BAR0 claimed so the NVIDIA driver could not
probe until a full power-off. Commit `6dfb7d2` routes 2757 through the AD102
family; this report covers the run after that fix. A failed attempt on this
card required a cold power cycle to recover.

## Reproduce

```sh
python3 tools/print_boot_option.py          # prints the exact option
# boot with the printed option, cold start, then:
python3 tools/read_sm_rates.py --expect-uuid GPU-...
```

Then follow [testing.md](testing.md) for the stock-then-unlocked matched
pair. Remove UUIDs, PCI addresses and hostnames before sharing logs.
