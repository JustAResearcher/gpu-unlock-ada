#!/bin/sh
# DRAFT CPU-only log validator for the measured AD102 transaction format.
# No module loading, MMIO, boot changes, reboot or driver actions are provided.
# Kernel printk may print zero as 0 or 0x0. Exact target must be supplied.
GU_BDF=${GU_BDF:-}

gu_validate() {
  gu_log=$1
  printf '%s\n' "$GU_BDF" | grep -Eq '^[0-9a-f]{4}:[0-9a-f]{2}:[0-9a-f]{2}\.[0-7]$' || return 1
  [ -s "$gu_log" ] || return 1
  if grep -Eq 'gpu_unlock: (unlock failed|firmware load failed|no supported card)|failure unlock_610.c:|firmware did not stop|keeping its DMA buffers|card already initialised|Kernel panic|BUG:|NVRM: Xid' "$gu_log"; then
    return 1
  fi
  gu_rows=$(grep -F "$GU_BDF:" "$gu_log") || return 1
  # Treat a repeated key in one row as malformed, including a later conflicting
  # value that would otherwise hide behind a positive regex prefix.
  printf '%s\n' "$gu_rows" | awk '
    { for (key in seen) delete seen[key]
      for (i=1; i<=NF; i++) if ($i ~ /^[a-zA-Z_][a-zA-Z_0-9]*=/) {
        key=$i; sub(/=.*/, "", key)
        if (seen[key]++) exit 1
      }
    }' || return 1
  gu_one() {
    gu_count=$(printf '%s\n' "$gu_rows" | grep -Ec "$1")
    [ "$gu_count" = 1 ]
  }
  # Count stage prefixes, not only positive rows: a valid plus contradictory
  # row must never authorize the driver. The module emits one per stage.
  gu_one 'AD102 payload result:' || return 1
  gu_one 'PCIe FLR returned:' || return 1
  gu_one 'post-FLR GFW result:' || return 1
  gu_one 'post-FLR SEC2 reset released,' || return 1
  gu_one 'gpu_unlock: unlocked$' || return 1
  for gu_phase in after-payload post-FLR; do
    for gu_permission in WPR_CFG WPR FEAT; do
      gu_one "AD102 permission $gu_phase: $gu_permission\\[" || return 1
    done
  done
  for gu_phase in selectors-applied pre-FLR post-FLR; do
    gu_one "AD102 $gu_phase: readout1=" || return 1
  done
  gu_one 'AD102 payload result: boot=0 halt=0 marker=0x67b00001 expected=0x67b00001 marker_ok=1 required_permissions=1 cpuctl=0x[0-9a-f]+ halted=1 ' || return 1
  gu_one 'AD102 permission after-payload: WPR_CFG\[0x1fa7cc\]=0xfffff0ff .*expected=0xfffff0ff required=1 valid=1 ' || return 1
  gu_one 'AD102 permission after-payload: WPR\[0x1fa7c4\]=0xffffffff .*expected=0xffffffff required=1 valid=1 ' || return 1
  gu_one 'AD102 permission after-payload: FEAT\[0x823804\]=0xffffffff .*expected=0xffffffff required=1 valid=1 ' || return 1
  for gu_phase in selectors-applied pre-FLR post-FLR; do
    gu_one "AD102 $gu_phase: readout1=(0|0x0) fields=0 0 0 0 0 0 0 0 0 valid=1 full=1$" || return 1
  done
  gu_one 'PCIe FLR returned: result=0$' || return 1
  gu_one 'post-FLR GFW result: error=0 pci=0x268410de plm=(0|0x[0-9a-f]{1,8}) boot=(0|0x[0-9a-f]{1,8}) cpuctl=(0|0x[0-9a-f]{1,8}) budget_us=2050000$' || return 1
  gu_gfw=$(printf '%s\n' "$gu_rows" | grep 'post-FLR GFW result:') || return 1
  gu_plm= gu_boot= gu_cpuctl=
  for gu_field in $gu_gfw; do
    case "$gu_field" in plm=*) gu_plm=${gu_field#*=};; boot=*) gu_boot=${gu_field#*=};; cpuctl=*) gu_cpuctl=${gu_field#*=};; esac
  done
  # Same invariants as pinned gfw_eval.c, no invented exact register values.
  [ "$((gu_plm))" -lt 4294967295 ] && [ "$((gu_plm & 1))" = 1 ] || return 1
  [ "$((gu_boot))" -lt 4294967295 ] && [ "$((gu_boot & 255))" = 255 ] || return 1
  [ "$((gu_cpuctl))" -lt 4294967295 ] && [ "$((gu_cpuctl & 16))" = 16 ] || return 1
  gu_one 'post-FLR SEC2 reset released, GSP halted, BME=0 pending=0 WPR2=[0-9a-f]{8}:00000000 SS0=88888888 SS1=00000008$' || return 1
  # WPR permissions must be re-locked to the board's cold values after FLR;
  # FEAT alone stays open. Upstream's 'expected=' column still shows its earlier
  # payload target, so do not mistake that column for the post-FLR requirement.
  gu_one 'AD102 permission post-FLR: WPR_CFG\[0x1fa7cc\]=0x4cb8f before=0x4cb8f .*required=1 valid=1 pci=0x268410de chip=0x192000a1$' || return 1
  gu_one 'AD102 permission post-FLR: WPR\[0x1fa7c4\]=0x4cb8f before=0x4cb8f .*required=1 valid=1 pci=0x268410de chip=0x192000a1$' || return 1
  gu_one 'AD102 permission post-FLR: FEAT\[0x823804\]=0xffffffff .*required=1 valid=1 pci=0x268410de chip=0x192000a1$' || return 1
  gu_one 'gpu_unlock: unlocked$' || return 1
  return 0
}

# Deliberately no action runner. Valid log syntax does not authorize hardware.
