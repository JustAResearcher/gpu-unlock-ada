#!/usr/bin/env bash
# Usage: sampled.sh <outdir> <name> <command...>
# Runs the command, writing stdout+stderr to <outdir>/<name>.txt and 200 ms GPU
# telemetry to <outdir>/<name>.telemetry.csv. GPU UUIDs are masked in both files.
set -uo pipefail
out=$1 name=$2; shift 2
mkdir -p "$out"
mask() { sed -E 's/GPU-[0-9a-fA-F-]{36}/GPU-<UUID-MASQUE>/g'; }
q=timestamp,power.draw,power.limit,clocks.sm,clocks.mem,temperature.gpu,utilization.gpu,pstate,clocks_event_reasons.active,clocks_event_reasons.sw_power_cap,clocks_event_reasons.hw_slowdown,clocks_event_reasons.sw_thermal_slowdown,clocks_event_reasons.hw_thermal_slowdown,clocks_event_reasons.hw_power_brake_slowdown
nvidia-smi --query-gpu=$q --format=csv -lms 200 > "$out/$name.telemetry.csv" &
smi=$!
sleep 1
start=$(date -Is)
"$@" 2>&1 | mask > "$out/$name.txt"
rc=${PIPESTATUS[0]}
sleep 1
kill $smi; wait $smi 2>/dev/null
printf '# start=%s end=%s exit=%s cmd=%s\n' "$start" "$(date -Is)" "$rc" "$*" | mask >> "$out/$name.txt"
exit $rc
