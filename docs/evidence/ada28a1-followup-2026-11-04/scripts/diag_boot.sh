#!/bin/bash
# Run after a one-off GRUB boot. Usage: bash diag_boot.sh stock|wiped
set -euo pipefail
MODE="${1:?usage: diag_boot.sh stock|wiped}"
F=/home/danny/Development/gpu-unlock-hp/gpu-unlock-ada/build/matched-bench/followup-2026-11-04
PCI=/sys/bus/pci/devices/0000:01:00.0
DPM=$(awk -F': ' '/^DynamicPowerManagement:/{print $2}' /proc/driver/nvidia/params 2>/dev/null || echo "?")
echo "cmdline: $(cat /proc/cmdline)"
echo "DynamicPowerManagement=$DPM  gpu_unlock_loaded=$([ -d /sys/module/gpu_unlock ] && echo yes || echo no)  power_state=$(cat $PCI/power_state)"
case "$MODE" in
  stock)
    grep -q 'gpu_unlock.targets' /proc/cmdline && { echo "STOP: gpu_unlock.targets is still on the cmdline; this is not a stock boot"; exit 2; }
    ;;
  wiped)
    [ "$DPM" = "2" ] || { echo "STOP: DynamicPowerManagement is $DPM, expected 2"; exit 2; }
    echo "Waiting (max 3 min) for the dGPU to reach D3cold; do not open GPU apps..."
    for i in $(seq 1 90); do [ "$(cat $PCI/power_state)" = "D3cold" ] && break; sleep 2; done
    echo "power_state=$(cat $PCI/power_state)"
    [ "$(cat $PCI/power_state)" = "D3cold" ] || { echo "STOP: never reached D3cold"; exit 2; }
    ;;
  *) echo "bad mode"; exit 2;;
esac
powerprofilesctl set performance
for n in 1 2 3; do
  for i in $(seq 1 90); do t=$(nvidia-smi --query-gpu=temperature.gpu --format=csv,noheader); [ "$t" -le 52 ] && break; sleep 2; done
  bash "$F/collect_run.sh" "boot_${MODE}_$n"
done
python3 "$F/summarize.py" "$F"/boot_${MODE}_*
echo "LISTO: dile a Claude 'ya corri $MODE'"
