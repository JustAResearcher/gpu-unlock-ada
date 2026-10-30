# RTX 4080 Laptop (`10de:27e0`, HP `103c:8bad`) evidence

One HP laptop GPU, A1 revision, 58 SMs, 12282 MiB; Ubuntu 24.04.1,
Linux `6.8.0-51-generic`, NVIDIA proprietary `590.48.01`. These are the stock
and first unlocked benchmark sessions from the same card. The unlocked boot
entered with `wpr2_hi=0x0`. There was one tested unlock boot; the three timing
rounds are immediate repeats within that boot, not three boot tests.
Experimental source revision: `568d915074e4cd9fc90bf3452903738baf6c9dfb`;
AD104M adaptation: `7a9c81f23f397baa223d9913b282e8244ce06f7d`.
This package records that experiment, not a fresh test of a later PR revision.

Both sessions used active `nvidia-powerd`, AC power, the performance platform
profile, an active desktop and the same benchmark binaries/libraries. The 18
source/binary/library SHA-256 entries in `manifest.json` matched exactly in the
two original manifests. The benchmark collector issued no manual clock or power
control. Dynamic Boost
and OEM policy remained active: this is **not a fixed-power comparison**.
Detailed current-limit samples have medians **114.18 W stock / 119.42 W
unlocked**; sampling includes idle periods and phase gaps. The CSV query's
`power.limit` field was N/A, so the separate detailed query is retained.

The all-sample medians show FP16/FP32 and BF16/FP32 MMA issue throughput
53.9 -> 108.3 TFLOP/s, and E4M3 FP8 107.0 -> 214.8 TFLOP/s. The checked
TF32 probe is approximately 2x, while its INT4 control is approximately
unchanged. The six cuBLAS size/type combinations improve by 50.7–63.2%.
These are instruction/compute measurements; no whole-model speedup is claimed.
FP8 is an issue-throughput observation with **no FP8 numerical validation**.
All supplied checked tensor results, checked GEMM matrices and six seeded
FP16/BF16 cases in each state report zero errors. This is a short experiment,
not a long-duration reliability or repeated cold/warm boot study.

## Files and provenance

- `mma-{stock,unlocked}.txt`: complete stdout from `mma_bench 20000 5`,
  three rounds of five observations for each mode. The original `best` column
  remains visible, but reported comparisons use all 15 observations.
- `tensor-{stock,unlocked}.txt`: complete JSON stdout from the CUDA-driver
  runner, three rounds of three observations per mode. Includes timings,
  cubin hash, expected values, checked-cell counts and mismatches.
- `gemm-{stock,unlocked}.txt`: complete JSON-lines stdout, three rounds of
  five timing batches per size/type, ten warmups and 20 GEMMs per batch.
  Compute each batch's TFLOP/s as `2*n**3/(times_ms*1e9)` and take the median
  of all 15 rates. Rounded per-round display fields are retained, not used
  as the pooled median.
- `numerical-{stock,unlocked}.txt`: complete seeded validator stdout.
- `sm-rates-{stock,unlocked}.json`: original getter objects for both the
  `before` and `after` readbacks in each session. `fmla32` is 1/1 stock,
  0/0 unlocked; the other rates are retained as observed.
- `telemetry-*.csv`, `power-limits-*.csv`: every original sample and field,
  with calendar timestamps replaced by elapsed seconds from that file's
  first sample. Each file has its own zero; do not align the two monitors by
  zero alone. Sampling cadence and total duration remain available.
- `run-records.json`: original session command/exit/duration records with
  start calendar times removed; selected post-run health/profile/powerd/GPU
  queries and unlocked power settings. The health query returned exit 1 and
  `-- No entries --`, rather than a fabricated success status.
- `transaction-excerpt.txt`: ordered kernel excerpt from this target's
  `entry:` through `gpu_unlock: unlocked`, followed by its postboot lspci
  driver-binding output. This is not a complete kernel log. It retains the
  FWSEC result, payload/HALT/marker, permission readbacks, selector writes,
  FLR, reset release, empty handoff resources and restored DMA/PCI state.
  `AD102` is the experimental implementation's diagnostic label, not a claim
  that the tested AD104M card is AD102. Post-FLR WPR permissions are retained
  as logged; they are not all claimed to retain their payload-stage values.
- `manifest.json`: identical build manifest, normalized path labels,
  commands, toolchain versions and source/binary/library hashes. `lab/` and
  `system/` are relative location labels, not literal executable paths.
- `summary.json`: explicitly **offline-derived** statistics from these
  observations, including all sample counts, minima, maxima, medians and
  stock/unlocked comparisons; it is not original benchmark stdout.
- `verify.py`: stdlib-only offline verifier and median recomputation.
- `SHA256SUMS`: covers every other file in this directory, without including
  itself. It checks package integrity, not independent authenticity of the
  original private collection.

Benchmark paths and compile commands are recorded in the manifest. The MMA,
seeded validator, tensor runner and GEMM sources are the versions identified
by that manifest's source hashes; the GEMM source is
`docs/evidence/rtx4090-pr1-stock/gemm_bench.cu` at the experiment revision.
Compiler: CUDA Toolkit 12.8.1, nvcc 12.8.93, `g++-12`, target `sm_89`.

## Privacy and offline verification

Original stdout numbers, timings, sample ordering and failure counts are
unchanged. Text edits replace only device identity, PCI location and private
path fields; three-round files add explicit round boundaries. JSON grouping
and formatting are normalized. Calendar times are removed or converted to
relative time; measured durations are retained. The transaction and post-run
query files are explicitly selected excerpts as described above, not full
system logs. GPU UUID is `<gpu-uuid>`, every observed PCI location is
`<target>`, and private lab paths become `<lab>/` or `lab/` labels. Commands
for another machine must use its own verified `<target>` placeholder.
No host/user identity, personal email, boot/disk UUID, network identifier,
signing key/certificate/fingerprint, firmware/ROM payload, module binary or
package is distributed. Firmware filenames and non-secret firmware geometry
in transaction text describe observed processing; they are not firmware files.

From this directory:

```sh
python3 verify.py
sha256sum -c SHA256SUMS
```

On macOS the equivalent checksum command is `shasum -a 256 -c SHA256SUMS`.
The verifier checks exhaustive hash coverage, sample counts, all numerical
error fields, rate transitions, timing medians, manifest structure and common
private identity patterns. It does not operate the GPU. `--write-summary`
is a maintainer-only recomputation mode; it does not update checksums.
