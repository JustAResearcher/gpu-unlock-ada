#!/bin/bash
# Same steps/binaries as build/matched-bench/collect.sh, but a fresh output dir per run
# and richer telemetry. Usage: collect_run.sh <label>
set -euo pipefail
LABEL="${1:?label}"
REPO=/home/danny/Development/gpu-unlock-hp/gpu-unlock-ada
OUT="$REPO/build/matched-bench/followup-2026-11-04/$LABEL"
[ -e "$OUT" ] && { echo "refusing to reuse $OUT"; exit 2; }
UUID=$(nvidia-smi --query-gpu=uuid --format=csv,noheader | head -1)
CUBIN_SHA=33aa6db385aa9eb8a1f73bb2f74891aea84794326b7c40992bc642f93a3c614d
CUBIN="$REPO/build/matched-bench/tensor_modes_issue_sm89.cubin"
cd "$REPO"; mkdir -p "$OUT"; export CUDA_VISIBLE_DEVICES="$UUID"
{ date -Is; uname -r; echo "profile=$(powerprofilesctl get)"; echo "ac_online=$(cat /sys/class/power_supply/ACAD/online)";
  echo "dpm=$(grep DynamicPowerManagement: /proc/driver/nvidia/params)"; echo "pci_power_state=$(cat /sys/bus/pci/devices/0000:01:00.0/power_state)";
  echo "gpu_unlock_loaded=$([ -d /sys/module/gpu_unlock ] && echo yes || echo no)"; } > "$OUT/env.txt"
nvidia-smi -q -d CLOCK,PERFORMANCE,POWER,TEMPERATURE > "$OUT/smi_before.txt"
sha256sum -c build/matched-bench/SHA256SUMS > "$OUT/hashes.txt"
python3 tools/read_sm_rates.py --expect-uuid "$UUID" > "$OUT/sm-rates.json"
stdbuf -oL nvidia-smi --query-gpu=timestamp,pstate,clocks.sm,clocks.mem,power.draw,enforced.power.limit,temperature.gpu,clocks_throttle_reasons.active \
  --format=csv,noheader --loop-ms=100 > "$OUT/telemetry.csv" 2>&1 &
TELE=$!; trap 'kill $TELE 2>/dev/null || true' EXIT; sleep 1
step(){ echo "$(date +%s.%N) START $1" >> "$OUT/steps.txt"; }
step validate_gemm; build/matched-bench/validate_gemm > "$OUT/numerical.txt"
step mma; build/matched-bench/mma_bench 20000 5 > "$OUT/mma.txt"
step gemm; build/matched-bench/gemm_bench > "$OUT/gemm.jsonl"
step tensor; python3 diagnostics/bench/tensor_modes_driver_runner.py "$CUBIN" --sha256 "$CUBIN_SHA" --expect-uuid "$UUID" --expect-power-limit N/A > "$OUT/tensor.json"
step end; sleep 1; kill $TELE; wait $TELE 2>/dev/null || true; trap - EXIT
nvidia-smi -q -d CLOCK,PERFORMANCE,POWER,TEMPERATURE > "$OUT/smi_after.txt"
(cd "$OUT" && sha256sum * > SHA256SUMS)
echo "DONE $OUT"
