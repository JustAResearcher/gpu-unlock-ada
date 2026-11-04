# Evidence bundle — 10de:28a1 HP Victus Ada canary (2026-10-31 – 2026-11-01)

Selected evidence for the [`docs/ada-28a1-report.md`](../../ada-28a1-report.md)
claims. GPU UUID, machine ID, hostname and disk/journal UUIDs are replaced with
`*-REDACTED` values (`GPU-REDACTED-AD107M`). Original unredacted files stay in
the owner's local parity harness (`build/matched-bench/parity/`, never deleted);
this bundle is the publication-safe subset.

The bundle preserves both experimental phases:

1. **Phase 1 (`run2`, `run3` — `D3cold`-wiped negative control):** Boot-time
   unlock with Ubuntu's default `NVreg_DynamicPowerManagement=0x02`
   (fine-grained Runtime `D3cold`) and `NVreg_PreserveVideoMemoryAllocations=1`.
   The transaction succeeded at boot (`gpu_unlock: unlocked`) and
   `tools/read_sm_rates.py` (`0x20801230`) continued returning `0 0 0` from
   GSP-RM's cached boot-time `OBJGR` state, while runtime `D3cold` (`ACPI
   LNXPOWER:00 = 0`, `Video Memory: Off`) wiped the physical BAR0 unlock
   registers back to half-rate fuses (`1.00×` throughput).
2. **Phase 2 (`run4_nod3cold` — `D3cold` inhibited, `2.00×` unlocked):**
   Full BAR0 register verification via `gpu_peek` and matched benchmark suite
   with `D3cold` blocked (`power/control = on`,
   `NVreg_DynamicPowerManagement=0x00`), demonstrating the **2.00× tensor
   issue-rate speedup** (`25.320 → 50.682 Top/s` FP16/FP32, `12.660 → 25.281
   Top/s` TF32, `25.5 → 50.9 TF/s` MMA FP16/BF16, `50.9 → 101.4 TF/s` FP8 E4M3,
   and median `+35.9%` FP16 / `+48.3%` BF16 cuBLAS $4096^3$ GEMM; best-to-best
   `+36.2%` / `+61.6%`).

## Boot timeline

- `boots.txt` — `journalctl --list-boots` for Phase 1: one warm unlock boot,
  one warm stock baseline boot, two cold unlock cycles (HP EC reset).

## Transaction & BAR0 register evidence (`dmesg` + `gpu_peek`)

- `dmesg_unlock_run2.txt` — full kernel log slice of Phase 1 cold cycle 1
  (boot `267d0cf7…`): ROM/FWSEC chain, permission writes, FLR `result=0`,
  post-FLR GFW `error=0 pci=0x28a110de`, `gpu_unlock: unlocked`.
- `dmesg_unlock_run3.txt` — kernel log summary for Phase 1 cold cycle 2
  (boot `7569fe75…`).
- `gpu_peek_stock_run4.txt` — 63-register BAR0 `gpu_peek` snapshot after a
  runtime `D3cold` wake (`driver=nvidia`), taken both at idle (`P3`, 1755 MHz)
  and during active CUDA tensor execution (`P4`, 2505 MHz), showing
  `FEAT_OVR_PLM=0xffffff8f`, `SS0=0x53123046`, `SS1=0x3`, and
  `FEAT_READOUT_1=0x00410080` (`0 0 2 0 0 2 0 2 0`, half rate).
- `dmesg_unlock_run4_nod3cold.txt` — complete kernel log and 63-register BAR0
  `gpu_peek` trace across the live `D3cold` → `D0` cold reset and unlock with
  `D3cold` blocked:
  1. Unbound `D3cold` → `D0` wake (`WPR2_HI=0x00000000`, `SS0=0x53323046`).
  2. Full `gpu_unlock` transaction (`gpu_unlock: unlocked`).
  3. Immediate post-unlock BAR0 `gpu_peek` (`driver=none`:
     `FEAT_OVR_PLM=0xffffffff`, `SS0=0x88888888`, `SS1=0x8`,
     `FEAT_READOUT_1=0x00000000`).
  4. Post-`nvidia.ko` load (`DynamicPowerManagement=0`) BAR0 `gpu_peek` during
     active CUDA execution (`driver=nvidia`: `FEAT_OVR_PLM=0xffffffff`,
     `SS0=0x88888888`, `SS1=0x8`, `FEAT_READOUT_1=0x00000000` preserved).

## Selector state (read-only RM getter, `tools/read_sm_rates.py`)

- `selectors_stock_run2.json`, `selectors_stock_run3.json` — clean stock boot:
  `fmla32=1 imla2=1 imla4=1` (half rate).
- `selectors_unlock_run2.json`, `selectors_unlock_run3.json` — Phase 1 cold
  unlock boots after runtime `D3cold`: cached RM getter still returns all zero
  (`0 0 0`, false positive) even though hardware `0x00823818` reset to
  `0x00410080`.
- `selectors_unlock_run4_nod3cold.json` — Phase 2 unlocked state with `D3cold`
  blocked: RM getter returns all zero (`0 0 0`) and matches hardware
  `0x00823818 = 0x00000000`.

## Matched benchmark outputs

All runs used the same three binaries (`binaries_sha256.txt`) and the same
`tensor_modes_issue_sm89.cubin` (`sha256: 33aa6db385aa9eb8a1f73bb2f74891aea84794326b7c40992bc642f93a3c614d`):

- `mma_stock_run2.txt`, `mma_stock_run3.txt` — stock baseline (`iters=20000 reps=5`, `f16/f32 = 25.5 / 25.3 TF/s`).
- `mma_unlock_run2.txt`, `mma_unlock_run3.txt` — Phase 1 `D3cold`-wiped negative control (`25.5 TF/s`, `1.00×`).
- `mma_unlock_run4_nod3cold.txt` — **Phase 2 unlocked with `D3cold` blocked** (`iters=20000 reps=5`: `f16/f32 = 50.9 TF/s`, `bf16/f32 = 50.9 TF/s`, `fp8/e4m3 = 101.4 TF/s`, **2.00×**).
- `tensor_stock_run2.json`, `tensor_stock_run3.json` — stock SM89 tensor probe (`f16_f32 = 25.320 Top/s`, `tf32 = 12.660 Top/s`, `u4_s32 = 201.30 Top/s`).
- `tensor_unlock_run2.json`, `tensor_unlock_run3.json` — Phase 1 `D3cold`-wiped negative control (`25.468 / 25.390 Top/s`, `1.00×`).
- `tensor_unlock_run4_nod3cold.json` — **Phase 2 unlocked with `D3cold` blocked** (`f16_f32 = 50.682 Top/s` [**2.002×**], `tf32 = 25.281 Top/s` [**1.997×**], `u4_s32 = 201.84 Top/s` [**1.003×**], `mismatches = 0`).
- `gemm_stock_run2.jsonl`, `gemm_stock_run3.jsonl` — stock cuBLAS GEMM ($1024^3, 4096^3, 8192^3$ FP16/FP32 and BF16/FP32).
- `gemm_unlock_run2.jsonl`, `gemm_unlock_run3.jsonl` — Phase 1 `D3cold`-wiped negative control.
- `gemm_unlock_run4_nod3cold.jsonl` — **Phase 2 unlocked with `D3cold` blocked** (median vs stock `run2`: $4096^3$ FP16/FP32 `+35.9%`, BF16/FP32 `+48.3%`; $8192^3$ FP16/FP32 `+35.6%`, BF16/FP32 `+38.4%`; best-to-best `+36.2%` / `+61.6%` / `+36.3%` / `+38.3%`; `failures = 0`).
- `numerical_stock_run2.txt`, `numerical_stock_run3.txt`, `numerical_unlock_run2.txt`, `numerical_unlock_run3.txt`, `numerical_unlock_run4_nod3cold.txt` — `validate_gemm` results (6 seeded shapes vs CPU FP64 reference, `failures=0, max_abs=0` in every run).

## Telemetry

- `telemetry_summary.txt` — per-run max SM clock, max power and throttle-reason
  histogram from the `nvidia-smi --loop-ms=100` sampler.
