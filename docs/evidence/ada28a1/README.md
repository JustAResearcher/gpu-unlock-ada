# Evidence bundle — 10de:28a1 HP Victus Ada canary (2026-10-31)

Selected evidence for the `docs/ada-28a1-report.md` claims. GPU UUID,
machine id, hostname and disk/journal UUIDs replaced with
`*-REDACTED` values. Original unredacted files stay in the owner's
local parity harness (`build/matched-bench/parity/{stock,unlock}/run{1,2,3}/`,
never deleted); this bundle is the publication-safe subset.

## Boot timeline

`boots.txt` — `journalctl --list-boots` for the session: one warm unlock
boot, one warm stock baseline boot, two cold unlock cycles (HP EC reset).

## Transaction evidence

- `dmesg_unlock_run2.txt` — full kernel log slice of cold cycle 1
  (boot `267d0cf7…`): ROM/FWSEC chain, permission writes, FLR
  `result=0`, post-FLR GFW `error=0 pci=0x28a110de`, `gpu_unlock: unlocked`.
- `dmesg_unlock_run3.txt` — same slice for cold cycle 2 (boot `7569fe75…`),
  captured live with `dmesg` in that boot.

Note: the systemd journal did not retain `_TRANSPORT=kernel` lines for the
cold-cycle-1 boot (`journalctl -b <id> -k` empty); the lines were recovered
from rsyslog's `/var/log/kern.log`, which persists kernel messages across
boots. Timestamps in `dmesg_unlock_run2.txt` are rsyslog wall-clock, the
other file uses seconds-since-boot from a live `dmesg`.

## Selector state (read-only RM getter, `tools/read_sm_rates.py`)

- `selectors_stock_run2.json`, `selectors_stock_run3.json` — clean stock
  boot: `fmla32=1 imla2=1 imla4=1` (half rate).
- `selectors_unlock_run2.json`, `selectors_unlock_run3.json` — both cold
  unlock boots: all zero (full rate).

## Matched benchmark outputs

All runs used the same three binaries and the same tensor cubin
(binaries are hashed with `sha256sum` in the build directory, see
`binaries_sha256.txt`; the cubin rebuild is byte-identical,
`33aa6db3…`):

- `mma_*.txt` — 7 instruction modes, 5 reps, best TF/s.
- `tensor_*.json` — SM89 tensor probe (f16_f32, tf32, u4_s32), 3 reps,
  raw timings + rates + mismatch counts.
- `numerical_*.txt` — `validate_gemm` results (6 shapes, GPU vs CPU
  double reference, failures=0 in every run).
- `gemm_*.jsonl` — raw `gemm_bench` rows (6 runs per collect).

Run1 artifacts (legacy pre-final harness) exist locally but are excluded
from the canonical matched pair; they are referenced in the report only
for the warm-boot fingerprint cross-check.

## Telemetry

`telemetry_summary.txt` — per-run max SM clock, max power and throttle
reason histogram from the `nvidia-smi --loop-ms=100` sampler (coverage
partial: stdout buffering on signal-loss; treat maxima as lower bounds).
